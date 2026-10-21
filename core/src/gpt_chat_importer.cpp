// Port of data/GptChatImporter.kt.
//
// The message graph is rebuilt exactly like the Kotlin source: a validity pass over the mapping
// (cycles and dangling parents are dropped), a breadth-first order over the valid nodes with the
// JDK's PriorityQueue ordering, the current-node path in front of it, and the canonical node per
// message id. The import pass then merges the ephemeral ancestors (thoughts, reasoning recaps,
// code, tool output) into the message they belong to and cascades the parent references.
#include "agora/core/gpt_chat_importer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/imported_citation_projection.hpp"
#include "agora/core/zip_reader.hpp"

namespace agora::core::gpt_import {

namespace {

namespace json = agora::core::json;
using json::Kind;
using json::Value;

constexpr const char* kClientRoot = "client-created-root";

bool is_null_literal(const Value& value) {
    return value.kind == Kind::Literal && value.text == "null";
}

/** JsonObject iteration: first insertion position, last value for a duplicate key. */
std::vector<std::pair<std::string, const Value*>> object_entries(const Value& object) {
    std::vector<std::pair<std::string, const Value*>> entries;
    for (const json::Member& member : object.members) {
        bool replaced = false;
        for (auto& entry : entries) {
            if (entry.first == member.key) {
                entry.second = &member.value;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.emplace_back(member.key, &member.value);
    }
    return entries;
}

const Value* member(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const auto& entry : object_entries(object)) {
        if (entry.first == name) found = entry.second;
    }
    return found;
}

/** A non-nullable field: a missing key takes the declared default, a null is a decode error. */
std::string decode_string(const Value* value, const std::string& fallback = std::string()) {
    if (value == nullptr) return fallback;
    if (is_null_literal(*value)) throw DecodeError("null for a non-nullable string");
    if (value->kind == Kind::String || value->kind == Kind::Literal) return value->text;
    throw DecodeError("expected a string");
}

std::optional<std::string> decode_nullable_string(const Value* value) {
    if (value == nullptr || is_null_literal(*value)) return std::nullopt;
    if (value->kind == Kind::String || value->kind == Kind::Literal) return value->text;
    throw DecodeError("expected a string");
}

double decode_double(const Value* value, double fallback = 0.0) {
    if (value == nullptr) return fallback;
    if (is_null_literal(*value)) throw DecodeError("null for a non-nullable number");
    if (value->kind != Kind::String && value->kind != Kind::Literal) {
        throw DecodeError("expected a number");
    }
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value->text, &consumed);
        if (consumed != value->text.size()) throw DecodeError("expected a number");
        return parsed;
    } catch (const DecodeError&) {
        throw;
    } catch (const std::exception&) {
        throw DecodeError("expected a number");
    }
}

std::optional<double> decode_nullable_double(const Value* value) {
    if (value == nullptr || is_null_literal(*value)) return std::nullopt;
    return decode_double(value);
}

std::optional<bool> decode_nullable_bool(const Value* value) {
    if (value == nullptr || is_null_literal(*value)) return std::nullopt;
    if (value->kind != Kind::String && value->kind != Kind::Literal) {
        throw DecodeError("expected a boolean");
    }
    std::string lowered = value->text;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lowered == "true") return true;
    if (lowered == "false") return false;
    throw DecodeError("expected a boolean");
}

std::optional<int> decode_nullable_int(const Value* value) {
    if (value == nullptr || is_null_literal(*value)) return std::nullopt;
    if (value->kind != Kind::String && value->kind != Kind::Literal) {
        throw DecodeError("expected a number");
    }
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(value->text, &consumed);
        if (consumed != value->text.size()) throw DecodeError("expected a number");
        return static_cast<int>(parsed);
    } catch (const DecodeError&) {
        throw;
    } catch (const std::exception&) {
        throw DecodeError("expected a number");
    }
}

std::vector<Value> decode_elements(const Value* value) {
    if (value == nullptr) return {};
    if (is_null_literal(*value)) throw DecodeError("null for a non-nullable array");
    if (value->kind != Kind::Array) throw DecodeError("expected an array");
    return value->elements;
}

GptThought decode_thought(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a thought object");
    GptThought thought;
    thought.content = decode_string(member(value, "content"));
    thought.summary = decode_string(member(value, "summary"));
    return thought;
}

GptMetadata decode_metadata(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a metadata object");
    GptMetadata metadata;
    metadata.model_slug = decode_nullable_string(member(value, "model_slug"));
    metadata.parent_id = decode_nullable_string(member(value, "parent_id"));
    metadata.is_complete = decode_nullable_bool(member(value, "is_complete"));
    metadata.content_references = decode_elements(member(value, "content_references"));
    metadata.citations = decode_elements(member(value, "citations"));
    if (const Value* finish = member(value, "finish_details");
        finish != nullptr && !is_null_literal(*finish)) {
        if (finish->kind != Kind::Object) throw DecodeError("expected a finish details object");
        GptFinishDetails details;
        details.type = decode_string(member(*finish, "type"));
        metadata.finish_details = details;
    }
    metadata.finished_duration_sec = decode_nullable_int(member(value, "finished_duration_sec"));
    return metadata;
}

GptAuthor decode_author(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected an author object");
    GptAuthor author;
    author.role = decode_string(member(value, "role"));
    author.name = decode_nullable_string(member(value, "name"));
    return author;
}

GptContent decode_content(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a content object");
    GptContent content;
    content.content_type = decode_string(member(value, "content_type"));
    if (const Value* parts = member(value, "parts"); parts != nullptr && !is_null_literal(*parts)) {
        if (parts->kind != Kind::Array) throw DecodeError("expected an array");
        content.parts = parts->elements;
    }
    content.text = decode_nullable_string(member(value, "text"));
    content.result = decode_nullable_string(member(value, "result"));
    content.reasoning_text = decode_nullable_string(member(value, "content"));
    content.language = decode_nullable_string(member(value, "language"));
    if (const Value* thoughts = member(value, "thoughts");
        thoughts != nullptr && !is_null_literal(*thoughts)) {
        if (thoughts->kind != Kind::Array) throw DecodeError("expected an array");
        std::vector<GptThought> decoded;
        for (const Value& element : thoughts->elements) decoded.push_back(decode_thought(element));
        content.thoughts = std::move(decoded);
    }
    return content;
}

GptMessage decode_message(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a message object");
    GptMessage message;
    message.id = decode_string(member(value, "id"));
    if (const Value* author = member(value, "author");
        author != nullptr && !is_null_literal(*author)) {
        message.author = decode_author(*author);
    }
    if (const Value* content = member(value, "content");
        content != nullptr && !is_null_literal(*content)) {
        message.content = decode_content(*content);
    }
    message.create_time = decode_nullable_double(member(value, "create_time"));
    if (const Value* metadata = member(value, "metadata");
        metadata != nullptr && !is_null_literal(*metadata)) {
        message.metadata = decode_metadata(*metadata);
    }
    return message;
}

GptMappingNode decode_node(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a node object");
    GptMappingNode node;
    node.id = decode_string(member(value, "id"));
    if (const Value* message = member(value, "message");
        message != nullptr && !is_null_literal(*message)) {
        node.message = decode_message(*message);
    }
    node.parent = decode_nullable_string(member(value, "parent"));
    return node;
}

GptConversation decode_conversation(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a conversation object");
    GptConversation conversation;
    conversation.conversation_id = decode_string(member(value, "conversation_id"));
    conversation.title = decode_string(member(value, "title"));
    conversation.create_time = decode_double(member(value, "create_time"));
    conversation.update_time = decode_double(member(value, "update_time"));
    conversation.current_node = decode_nullable_string(member(value, "current_node"));
    const Value* mapping = member(value, "mapping");
    if (mapping != nullptr) {
        if (is_null_literal(*mapping)) throw DecodeError("null for a non-nullable map");
        if (mapping->kind != Kind::Object) throw DecodeError("expected a mapping object");
        for (const auto& entry : object_entries(*mapping)) {
            conversation.mapping.emplace_back(entry.first, decode_node(*entry.second));
        }
    }
    return conversation;
}

// ------------------------------------------------------------------------- message graph

struct Graph {
    std::vector<const GptMessage*> messages;
    std::vector<std::pair<std::string, std::optional<std::string>>> parent_by_message_id;
    std::vector<std::string> current_message_ids;
};

Graph build_message_graph(const GptConversation& conversation) {
    Graph graph;
    const auto& mapping = conversation.mapping;
    if (mapping.empty()) return graph;

    const auto find_node = [&](const std::string& id) -> const GptMappingNode* {
        for (const auto& entry : mapping) {
            if (entry.first == id) return &entry.second;
        }
        return nullptr;
    };

    // Validity: a node is valid when its whole ancestor chain exists and is acyclic.
    std::vector<std::string> validity_order;
    std::map<std::string, bool> validity;
    std::vector<std::string> starts;
    for (const auto& entry : mapping) starts.push_back(entry.first);
    std::sort(starts.begin(), starts.end());
    for (const std::string& start : starts) {
        if (start == kClientRoot || validity.count(start) != 0) continue;
        std::vector<std::string> path;
        std::map<std::string, std::size_t> positions;
        std::optional<std::string> cursor = start;
        bool valid = true;
        while (cursor.has_value() && *cursor != kClientRoot) {
            const std::string node_id = *cursor;
            const auto cached = validity.find(node_id);
            if (cached != validity.end()) {
                valid = cached->second;
                break;
            }
            const GptMappingNode* node = find_node(node_id);
            if (node == nullptr || positions.count(node_id) != 0) {
                valid = false;
                break;
            }
            positions[node_id] = path.size();
            path.push_back(node_id);
            cursor = node->parent;
        }
        for (const std::string& node_id : path) {
            if (validity.count(node_id) == 0) validity_order.push_back(node_id);
            validity[node_id] = valid;
        }
    }

    std::vector<std::string> valid_node_ids;
    for (const std::string& node_id : validity_order) {
        if (validity[node_id]) valid_node_ids.push_back(node_id);
    }

    std::map<std::string, std::vector<std::string>> children_by_parent;
    for (const std::string& node_id : valid_node_ids) {
        const GptMappingNode* node = find_node(node_id);
        if (node == nullptr || !node->parent.has_value()) continue;
        if (*node->parent == kClientRoot) continue;
        children_by_parent[*node->parent].push_back(node_id);
    }
    for (auto& entry : children_by_parent) std::sort(entry.second.begin(), entry.second.end());

    // java.util.PriorityQueue: the smallest node id is taken first.
    std::priority_queue<std::string, std::vector<std::string>, std::greater<std::string>> ready;
    for (const std::string& node_id : valid_node_ids) {
        const GptMappingNode* node = find_node(node_id);
        if (node == nullptr) continue;
        if (!node->parent.has_value() || *node->parent == kClientRoot) ready.push(node_id);
    }
    std::vector<std::string> ordered_node_ids;
    while (!ready.empty()) {
        const std::string node_id = ready.top();
        ready.pop();
        ordered_node_ids.push_back(node_id);
        const auto children = children_by_parent.find(node_id);
        if (children != children_by_parent.end()) {
            for (const std::string& child : children->second) ready.push(child);
        }
    }

    std::vector<std::string> current_node_path;
    if (conversation.current_node.has_value()) {
        const auto cached = validity.find(*conversation.current_node);
        if (cached != validity.end() && cached->second) {
            std::vector<std::string> path;
            std::optional<std::string> cursor = conversation.current_node;
            while (cursor.has_value() && *cursor != kClientRoot) {
                path.push_back(*cursor);
                const GptMappingNode* node = find_node(*cursor);
                if (node == nullptr) break;
                cursor = node->parent;
            }
            current_node_path.assign(path.rbegin(), path.rend());
        }
    }

    // The canonical node per message id: the current path wins, then the graph order.
    std::vector<std::pair<std::string, std::string>> canonical_node_by_message_id;
    auto canonical_for = [&](const std::string& message_id) -> const std::string* {
        for (const auto& entry : canonical_node_by_message_id) {
            if (entry.first == message_id) return &entry.second;
        }
        return nullptr;
    };
    std::vector<std::string> canonical_order;
    std::vector<std::string> canonical_nodes;
    auto remember = [&](const std::string& node_id) {
        const GptMappingNode* node = find_node(node_id);
        if (node == nullptr || !node->message.has_value()) return;
        const std::string message_id = node->message->id;
        if (message_id.empty()) return;
        if (canonical_for(message_id) != nullptr) return;
        canonical_node_by_message_id.emplace_back(message_id, node_id);
        canonical_order.push_back(message_id);
        canonical_nodes.push_back(node_id);
    };
    for (const std::string& node_id : current_node_path) remember(node_id);
    for (const std::string& node_id : ordered_node_ids) remember(node_id);

    auto nearest_ancestor_message_id = [&](const std::string& node_id,
                                           const std::string& message_id) -> std::optional<std::string> {
        const GptMappingNode* node = find_node(node_id);
        std::optional<std::string> parent_node_id = node != nullptr ? node->parent : std::nullopt;
        while (parent_node_id.has_value() && *parent_node_id != kClientRoot) {
            const GptMappingNode* parent = find_node(*parent_node_id);
            if (parent == nullptr) return std::nullopt;
            const std::string parent_message_id =
                parent->message.has_value() ? parent->message->id : std::string();
            const std::string* canonical = canonical_for(parent_message_id);
            if (!parent_message_id.empty() && parent_message_id != message_id &&
                canonical != nullptr && *canonical == *parent_node_id) {
                return parent_message_id;
            }
            parent_node_id = parent->parent;
        }
        return std::nullopt;
    };

    for (std::size_t index = 0; index < canonical_order.size(); ++index) {
        const std::string& message_id = canonical_order[index];
        const std::string& node_id = canonical_nodes[index];
        graph.parent_by_message_id.emplace_back(
            message_id, nearest_ancestor_message_id(node_id, message_id));
        const GptMappingNode* node = find_node(node_id);
        if (node == nullptr || !node->message.has_value()) {
            // Kotlin's checkNotNull: a canonical node always carries a message.
            throw std::logic_error("canonical node without a message");
        }
        graph.messages.push_back(&*node->message);
    }

    std::vector<std::string> current_message_ids;
    for (const std::string& node_id : current_node_path) {
        const GptMappingNode* node = find_node(node_id);
        if (node == nullptr || !node->message.has_value()) continue;
        const std::string& message_id = node->message->id;
        if (canonical_for(message_id) == nullptr) continue;
        if (std::find(current_message_ids.begin(), current_message_ids.end(), message_id) ==
            current_message_ids.end()) {
            current_message_ids.push_back(message_id);
        }
    }
    graph.current_message_ids = std::move(current_message_ids);
    return graph;
}

// --------------------------------------------------------------------------------- helpers

std::string extract_text_from_part(const Value& part) {
    try {
        if (part.kind == Kind::String) return part.text;
        if (part.kind == Kind::Literal) return std::string();
        if (part.kind != Kind::Object) return std::string();
        const Value* content_type = member(part, "content_type");
        const std::string type = content_type != nullptr &&
                                         (content_type->kind == Kind::String ||
                                          content_type->kind == Kind::Literal)
                                     ? content_type->text
                                     : std::string();
        if (type == "image_asset_pointer") return std::string();
        const Value* text = member(part, "text");
        if (text == nullptr) return std::string();
        if (text->kind == Kind::String || text->kind == Kind::Literal) return text->text;
        return std::string();
    } catch (...) {
        return std::string();
    }
}

std::int64_t unix_seconds_to_millis(double seconds) {
    return static_cast<std::int64_t>(seconds * 1000.0);
}

std::int64_t now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool matches_split_entry(const std::string& name) {
    if (name == "conversations.json") return true;
    if (name.size() < 18) return false;
    if (name.compare(0, 14, "conversations-") != 0) return false;
    if (name.compare(name.size() - 5, 5, ".json") != 0) return false;
    std::size_t index = 14;
    while (index < name.size() - 5) {
        const char c = name[index];
        if (c < '0' || c > '9') return false;
        ++index;
    }
    return index > 14;
}

}  // namespace

std::optional<std::vector<GptConversation>> decode_conversations(const json::Value& root) {
    if (root.kind != Kind::Array) return std::nullopt;
    try {
        std::vector<GptConversation> conversations;
        for (const Value& element : root.elements) {
            conversations.push_back(decode_conversation(element));
        }
        return conversations;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

ImportPreview preview(const std::vector<GptConversation>& conversations) {
    ImportPreview result;
    result.conversation_count = static_cast<int>(conversations.size());

    std::vector<const GptConversation*> ordered;
    ordered.reserve(conversations.size());
    for (const GptConversation& conversation : conversations) ordered.push_back(&conversation);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const GptConversation* left, const GptConversation* right) {
                         return left->update_time > right->update_time;
                     });

    for (const GptConversation* conversation : ordered) {
        ConversationSummary summary;
        summary.uuid = conversation->conversation_id;
        summary.title = conversation->title.empty() ? "Untitled" : conversation->title;
        summary.message_count = static_cast<int>(build_message_graph(*conversation).messages.size());
        result.conversations.push_back(std::move(summary));
    }

    for (const GptConversation& conversation : conversations) {
        const Graph graph = build_message_graph(conversation);
        for (const GptMessage* message : graph.messages) {
            ++result.total_message_count;
            const std::string role = message->author.has_value() ? message->author->role : std::string();
            if (role == "user") ++result.user_message_count;
            if (role == "assistant") ++result.assistant_message_count;
            bool attachments = false;
            if (message->content.has_value()) {
                const GptContent& content = *message->content;
                if (content.content_type == "multimodal_text") attachments = true;
                if (content.parts.has_value()) {
                    for (const Value& part : *content.parts) {
                        if (part.kind != Kind::Object) continue;
                        const Value* content_type = member(part, "content_type");
                        if (content_type == nullptr) continue;
                        if (content_type->kind != Kind::String &&
                            content_type->kind != Kind::Literal) {
                            continue;
                        }
                        if (content_type->text == "image_asset_pointer") attachments = true;
                    }
                }
            }
            if (attachments) result.has_attachments = true;
        }
    }
    return result;
}

claude_import::ImportConversations to_import_format(
    const std::vector<GptConversation>& conversations,
    const std::optional<std::vector<std::string>>& selected_ids) {
    using claude_import::ImportChatEntity;
    using claude_import::ImportMessageEntity;

    claude_import::ImportConversations result;

    struct RawMessage {
        std::string id;
        std::optional<std::string> raw_parent_id;
        std::string text;
        std::vector<citations::CitationRecord> citations;
        std::optional<std::string> thoughts;
        std::optional<std::string> thought_title;
        std::string content_type;
        std::string participant;
        std::string status;
        std::int64_t timestamp = 0;
        std::optional<std::int64_t> thought_time_ms;
        std::optional<std::string> model_name;
        std::string author_role;
    };

    for (const GptConversation& conversation : conversations) {
        if (selected_ids.has_value()) {
            const auto& ids = *selected_ids;
            const bool selected = std::find(ids.begin(), ids.end(), conversation.conversation_id) !=
                                  ids.end();
            if (!selected) continue;
        }
        const Graph graph = build_message_graph(conversation);
        if (graph.messages.empty()) continue;

        std::vector<RawMessage> raw_messages;
        raw_messages.reserve(graph.messages.size());
        for (std::size_t index = 0; index < graph.messages.size(); ++index) {
            const GptMessage* message = graph.messages[index];
            const std::string role = message->author.has_value() ? message->author->role : "user";
            std::string participant = "USER";
            if (role == "assistant" || role == "tool" || role == "system") participant = "MODEL";
            const std::string content_type =
                message->content.has_value() ? message->content->content_type : "text";

            std::string text;
            if (message->content.has_value()) {
                const GptContent& content = *message->content;
                const auto join_parts = [&content]() {
                    std::string joined;
                    if (content.parts.has_value()) {
                        for (const Value& part : *content.parts) joined += extract_text_from_part(part);
                    }
                    return joined;
                };
                if (content_type == "text" || content_type == "multimodal_text" ||
                    content_type == "user_editable_context") {
                    text = join_parts();
                } else if (content_type == "code") {
                    text = content.text.value_or("");
                } else if (content_type == "execution_output" ||
                           content_type == "tether_quote" ||
                           content_type == "tether_browsing_display") {
                    text = content.text.value_or(content.result.value_or(""));
                } else {
                    text = join_parts();
                }
            }

            std::vector<Value> references;
            if (message->metadata.has_value()) {
                for (const Value& reference : message->metadata->content_references) {
                    references.push_back(reference);
                }
                for (const Value& citation : message->metadata->citations) {
                    references.push_back(citation);
                }
            }
            const imported_citation::ProjectedText imported =
                imported_citation::project_chatgpt_citations(text, references);

            std::optional<std::string> thoughts;
            std::optional<std::string> thought_title;
            if (content_type == "thoughts" && message->content.has_value() &&
                message->content->thoughts.has_value()) {
                std::string joined;
                bool first = true;
                for (const GptThought& thought : *message->content->thoughts) {
                    if (!first) joined += "\n\n";
                    joined += thought.content;
                    first = false;
                }
                thoughts = joined;
                if (!message->content->thoughts->empty()) {
                    thought_title = message->content->thoughts->front().summary;
                }
            }
            if (content_type == "reasoning_recap" && message->content.has_value()) {
                thought_title = message->content->reasoning_text;
            }
            std::optional<std::int64_t> thought_time_ms;
            if (content_type == "reasoning_recap" && message->metadata.has_value() &&
                message->metadata->finished_duration_sec.has_value()) {
                thought_time_ms =
                    static_cast<std::int64_t>(*message->metadata->finished_duration_sec) * 1000;
            }
            std::optional<std::string> model_name;
            if (message->metadata.has_value() && message->metadata->model_slug.has_value()) {
                model_name = "OpenAI:" + *message->metadata->model_slug;
            }
            const std::int64_t timestamp = message->create_time.has_value()
                                              ? unix_seconds_to_millis(*message->create_time)
                                              : now_millis();
            std::string status = "SUCCESS";
            if (message->metadata.has_value()) {
                if (message->metadata->is_complete.has_value() &&
                    !*message->metadata->is_complete) {
                    status = "STOPPED";
                } else if (message->metadata->finish_details.has_value()) {
                    status = message->metadata->finish_details->type == "stop" ? "SUCCESS"
                                                                                : "STOPPED";
                }
            }

            RawMessage raw;
            raw.id = message->id;
            raw.raw_parent_id = graph.parent_by_message_id[index].second;
            raw.text = imported.text;
            raw.citations = imported.citations;
            raw.thoughts = thoughts;
            raw.thought_title = thought_title;
            raw.content_type = content_type;
            raw.participant = participant;
            raw.status = status;
            raw.timestamp = timestamp;
            raw.thought_time_ms = thought_time_ms;
            raw.model_name = model_name;
            raw.author_role = role;
            raw_messages.push_back(std::move(raw));
        }

        // The raw map (associateBy keeps the last entry of a duplicate id).
        std::map<std::string, const RawMessage*> raw_by_id;
        for (const RawMessage& raw : raw_messages) raw_by_id[raw.id] = &raw;

        const std::set<std::string> merge_types = {"thoughts",         "reasoning_recap",
                                                   "execution_output", "tether_quote",
                                                   "tether_browsing_display", "code"};
        const auto is_ephemeral = [&](const RawMessage& raw) {
            return merge_types.count(raw.content_type) != 0 || raw.author_role == "tool";
        };
        std::set<std::string> removed_ids;
        for (const RawMessage& raw : raw_messages) {
            if (is_ephemeral(raw)) removed_ids.insert(raw.id);
        }

        std::map<std::string, std::string> fallback_texts;
        for (const RawMessage& raw : raw_messages) {
            if (is_ephemeral(raw)) continue;
            if (!raw.text.empty() && !json::is_blank(raw.text)) continue;
            if (raw.content_type == "multimodal_text") fallback_texts[raw.id] = "[Image]";
        }

        std::map<std::string, std::string> merged_thoughts;
        std::map<std::string, std::string> merged_thought_title;
        std::map<std::string, std::int64_t> merged_thought_time_ms;
        std::map<std::string, std::string> merged_text_suffix;
        std::map<std::string, std::vector<citations::CitationRecord>> merged_citations;

        auto merge_into = [&](const RawMessage& target, const RawMessage& ephemeral) {
            if (!ephemeral.citations.empty()) {
                std::vector<citations::CitationRecord>& bucket = merged_citations[target.id];
                for (const citations::CitationRecord& citation : ephemeral.citations) {
                    citations::CitationRecord copy = citation;
                    copy.anchors.clear();
                    bucket.push_back(copy);
                }
            }
            if (ephemeral.content_type == "thoughts") {
                if (ephemeral.thoughts.has_value() && !json::is_blank(*ephemeral.thoughts)) {
                    merged_thoughts[target.id] = *ephemeral.thoughts;
                    if (ephemeral.thought_title.has_value()) {
                        merged_thought_title[target.id] = *ephemeral.thought_title;
                    }
                }
            } else if (ephemeral.content_type == "reasoning_recap") {
                if (ephemeral.thought_time_ms.has_value() && *ephemeral.thought_time_ms > 0) {
                    merged_thought_time_ms[target.id] = *ephemeral.thought_time_ms;
                }
                if (ephemeral.thought_title.has_value() &&
                    !json::is_blank(*ephemeral.thought_title)) {
                    merged_thought_title[target.id] = *ephemeral.thought_title;
                }
            } else if (ephemeral.content_type == "code") {
                if (!json::is_blank(ephemeral.text)) {
                    const auto existing = merged_thoughts.find(target.id);
                    if (existing != merged_thoughts.end()) {
                        merged_thoughts[target.id] = existing->second + "\n\n" + ephemeral.text;
                    } else {
                        merged_thoughts[target.id] = ephemeral.text;
                    }
                }
            } else {
                if (!json::is_blank(ephemeral.text)) {
                    std::string& suffix = merged_text_suffix[target.id];
                    if (!suffix.empty()) suffix += "\n\n";
                    suffix += ephemeral.text;
                }
            }
        };

        for (const RawMessage& target : raw_messages) {
            if (is_ephemeral(target)) continue;
            std::vector<const RawMessage*> ancestors;
            std::optional<std::string> parent_id = target.raw_parent_id;
            while (parent_id.has_value() && removed_ids.count(*parent_id) != 0) {
                const auto found = raw_by_id.find(*parent_id);
                if (found == raw_by_id.end()) break;
                ancestors.push_back(found->second);
                parent_id = found->second->raw_parent_id;
            }
            for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) merge_into(target, **it);
        }

        std::set<std::string> skipped_ids;
        for (const RawMessage& raw : raw_messages) {
            if (is_ephemeral(raw)) continue;
            const auto fallback = fallback_texts.find(raw.id);
            const std::string effective_text =
                fallback != fallback_texts.end() ? fallback->second : raw.text;
            const auto suffix = merged_text_suffix.find(raw.id);
            const auto thoughts = merged_thoughts.find(raw.id);
            const std::string effective_thoughts =
                thoughts != merged_thoughts.end() ? thoughts->second
                                                  : raw.thoughts.value_or(std::string());
            if (json::is_blank(effective_text) &&
                (suffix == merged_text_suffix.end() || suffix->second.empty()) &&
                json::is_blank(effective_thoughts)) {
                skipped_ids.insert(raw.id);
            }
        }
        std::set<std::string> all_removed = removed_ids;
        all_removed.insert(skipped_ids.begin(), skipped_ids.end());

        auto cascade_parent = [&](std::optional<std::string> id) -> std::optional<std::string> {
            std::optional<std::string> current = id;
            while (current.has_value() && all_removed.count(*current) != 0) {
                const auto found = raw_by_id.find(*current);
                if (found == raw_by_id.end()) {
                    current = std::nullopt;
                    break;
                }
                current = found->second->raw_parent_id;
            }
            return current;
        };

        int conversation_message_count = 0;
        for (const RawMessage& raw : raw_messages) {
            if (all_removed.count(raw.id) != 0) continue;
            const std::optional<std::string> parent_id = cascade_parent(raw.raw_parent_id);
            const auto fallback = fallback_texts.find(raw.id);
            const std::string base_text =
                fallback != fallback_texts.end() ? fallback->second : raw.text;
            const auto suffix = merged_text_suffix.find(raw.id);
            const bool has_suffix = suffix != merged_text_suffix.end();
            std::string final_text = base_text;
            if (has_suffix) {
                final_text = !json::is_blank(base_text)
                                 ? base_text + "\n\n" + suffix->second
                                 : suffix->second;
            }
            const auto thoughts = merged_thoughts.find(raw.id);
            const auto thought_title = merged_thought_title.find(raw.id);
            const auto thought_time = merged_thought_time_ms.find(raw.id);
            const auto extra_citations = merged_citations.find(raw.id);

            std::vector<citations::CitationRecord> citations = raw.citations;
            if (extra_citations != merged_citations.end()) {
                for (const citations::CitationRecord& citation : extra_citations->second) {
                    citations.push_back(citation);
                }
            }
            const std::optional<std::string> citation_json =
                imported_citation::encode_imported_citations(citations, final_text);

            ImportMessageEntity entity;
            entity.id = raw.id;
            entity.conversation_id = conversation.conversation_id;
            entity.parent_id = parent_id;
            entity.text = final_text;
            entity.thoughts = thoughts != merged_thoughts.end()
                                  ? std::optional<std::string>(thoughts->second)
                                  : raw.thoughts;
            entity.thought_title = thought_title != merged_thought_title.end()
                                       ? std::optional<std::string>(thought_title->second)
                                       : raw.thought_title;
            entity.thought_time_ms = thought_time != merged_thought_time_ms.end()
                                         ? std::optional<std::int64_t>(thought_time->second)
                                         : std::nullopt;
            entity.token_count = 0;
            entity.status = raw.status;
            entity.participant = raw.participant;
            entity.timestamp = raw.timestamp;
            entity.model_name = raw.model_name;
            entity.tool_call_json = citation_json;
            entity.attachment_meta = std::nullopt;
            result.messages.push_back(std::move(entity));
            ++conversation_message_count;
        }

        // The selected branches of the current path.
        std::vector<std::pair<std::optional<std::string>, std::string>> selected_branches;
        for (const std::string& message_id : graph.current_message_ids) {
            if (all_removed.count(message_id) != 0) continue;
            const auto found = raw_by_id.find(message_id);
            if (found == raw_by_id.end()) continue;
            const std::optional<std::string> parent = cascade_parent(found->second->raw_parent_id);
            bool replaced = false;
            for (auto& entry : selected_branches) {
                if (entry.first == parent) {
                    entry.second = message_id;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) selected_branches.emplace_back(parent, message_id);
        }

        if (conversation_message_count > 0) {
            ImportChatEntity chat;
            chat.id = conversation.conversation_id;
            chat.title = conversation.title.empty() ? "Untitled" : conversation.title;
            chat.last_updated = unix_seconds_to_millis(conversation.update_time);
            if (!selected_branches.empty()) {
                std::string json_text = "{";
                for (std::size_t index = 0; index < selected_branches.size(); ++index) {
                    if (index != 0) json_text.push_back(',');
                    json_text.push_back('"');
                    json::escape_to(json_text,
                                    selected_branches[index].first.has_value()
                                        ? *selected_branches[index].first
                                        : std::string("null"));
                    json_text += "\":\"";
                    json::escape_to(json_text, selected_branches[index].second);
                    json_text.push_back('"');
                }
                json_text.push_back('}');
                chat.selected_branches_json = json_text;
            }
            result.conversations.push_back(std::move(chat));
        }
    }

    return result;
}

Extraction extract_and_parse(const std::string& bytes) {
    Extraction extraction;
    const bool zipped = bytes.size() >= 2 && bytes[0] == 'P' && bytes[1] == 'K';
    if (zipped) {
        try {
            zip::Reader reader = zip::Reader::from_bytes(bytes);
            std::vector<GptConversation> all_conversations;
            zip::Entry entry;
            while (reader.next_entry(entry)) {
                if (!entry.name.empty() && entry.name.back() == '/') continue;
                if (!matches_split_entry(entry.name)) continue;
                const json::ParseResult parsed = json::parse(entry.content);
                if (!parsed.value) throw DecodeError("invalid JSON");
                const auto decoded = decode_conversations(*parsed.value);
                if (!decoded.has_value()) throw DecodeError("invalid conversation document");
                for (const GptConversation& conversation : *decoded) {
                    all_conversations.push_back(conversation);
                }
            }
            if (all_conversations.empty()) {
                extraction.failure =
                    ExtractionFailure{"Exception", "No conversation data found in ZIP archive"};
                return extraction;
            }
            extraction.ok = true;
            extraction.conversations = std::move(all_conversations);
            return extraction;
        } catch (const zip::ZipException& error) {
            extraction.failure = ExtractionFailure{"ZipException", error.what()};
            return extraction;
        } catch (const DecodeError&) {
            extraction.failure = ExtractionFailure{"decode", ""};
            return extraction;
        }
    }

    const json::ParseResult parsed = json::parse(bytes);
    if (!parsed.value) {
        extraction.failure = ExtractionFailure{"decode", ""};
        return extraction;
    }
    const auto decoded = decode_conversations(*parsed.value);
    if (!decoded.has_value()) {
        extraction.failure = ExtractionFailure{"decode", ""};
        return extraction;
    }
    if (decoded->empty()) {
        extraction.failure = ExtractionFailure{"Exception", "No conversations found in JSON"};
        return extraction;
    }
    extraction.ok = true;
    extraction.conversations = *decoded;
    return extraction;
}

}  // namespace agora::core::gpt_import
