// Port of data/ClaudeChatImporter.kt.
//
// Decoding mirrors kotlinx's lenient + coerceInputValues configuration, the preview and the
// import-format transform follow the Kotlin source line by line, and the archive step reuses the
// pinned ZIP reader (the JDK's ZipInputStream behaviour) plus ImportStreams' PK probe.
#include "agora/core/claude_chat_importer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/imported_citation_projection.hpp"
#include "agora/core/zip_reader.hpp"

namespace agora::core::claude_import {

namespace {

namespace json = agora::core::json;
using json::Kind;
using json::Value;

constexpr const char* kZeroUuid = "00000000-0000-4000-8000-000000000000";

// ------------------------------------------------------------------- lenient decoding

bool is_null_literal(const Value& value) {
    return value.kind == Kind::Literal && value.text == "null";
}

/**
 * kotlinx's lenient string decoding: any primitive contributes its content (so the number 42
 * becomes "42"), a null is coerced and an object/array is a decode error.
 */
std::optional<std::string> decode_string(const Value* value, bool nullable) {
    if (value == nullptr) {
        if (nullable) return std::nullopt;
        return std::string();  // the field's default
    }
    if (is_null_literal(*value)) {
        if (nullable) return std::nullopt;
        return std::string();
    }
    if (value->kind == Kind::String || value->kind == Kind::Literal) return value->text;
    throw DecodeError("expected a string");
}

std::optional<std::int64_t> decode_long(const Value* value) {
    if (value == nullptr || is_null_literal(*value)) return std::nullopt;
    if (value->kind != Kind::String && value->kind != Kind::Literal) {
        throw DecodeError("expected a number");
    }
    const std::string& text = value->text;
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '-' || text[index] == '+')) {
        negative = text[index] == '-';
        ++index;
    }
    if (index >= text.size()) throw DecodeError("expected a number");
    long long parsed = 0;
    for (; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') throw DecodeError("expected a number");
        parsed = parsed * 10 + (text[index] - '0');
        if (parsed > 9223372036854775807LL) throw DecodeError("number overflow");
    }
    return negative ? -parsed : parsed;
}

const Value* member(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = member(object, name);
    if (value == nullptr) throw DecodeError("missing field " + std::string(name));
    if (is_null_literal(*value)) throw DecodeError("null for " + std::string(name));
    if (value->kind == Kind::String || value->kind == Kind::Literal) return value->text;
    throw DecodeError("expected a string");
}

std::string optional_string(const Value& object, std::string_view name,
                            std::string fallback = std::string()) {
    const Value* value = member(object, name);
    if (value == nullptr) return fallback;
    if (is_null_literal(*value)) return fallback;
    if (value->kind == Kind::String || value->kind == Kind::Literal) return value->text;
    throw DecodeError("expected a string");
}

std::optional<std::string> nullable_string(const Value& object, std::string_view name) {
    const Value* value = member(object, name);
    if (value == nullptr || is_null_literal(*value)) return std::nullopt;
    if (value->kind == Kind::String || value->kind == Kind::Literal) return value->text;
    throw DecodeError("expected a string");
}

std::int64_t optional_long(const Value& object, std::string_view name, std::int64_t fallback) {
    const std::optional<std::int64_t> value = decode_long(member(object, name));
    return value.has_value() ? *value : fallback;
}

std::vector<Value> raw_list(const Value& object, std::string_view name) {
    const Value* value = member(object, name);
    if (value == nullptr || is_null_literal(*value)) return {};
    if (value->kind != Kind::Array) throw DecodeError("expected an array");
    return value->elements;
}

std::vector<ClaudeAttachment> decode_attachments(const Value& object) {
    std::vector<ClaudeAttachment> attachments;
    for (const Value& element : raw_list(object, "attachments")) {
        if (element.kind != Kind::Object) throw DecodeError("expected an attachment object");
        ClaudeAttachment attachment;
        attachment.file_name = optional_string(element, "file_name");
        attachment.file_size = optional_long(element, "file_size", 0);
        attachment.file_type = optional_string(element, "file_type");
        attachment.extracted_content = nullable_string(element, "extracted_content");
        attachments.push_back(std::move(attachment));
    }
    return attachments;
}

std::vector<ClaudeFile> decode_files(const Value& object) {
    std::vector<ClaudeFile> files;
    for (const Value& element : raw_list(object, "files")) {
        if (element.kind != Kind::Object) throw DecodeError("expected a file object");
        ClaudeFile file;
        file.file_uuid = optional_string(element, "file_uuid");
        file.file_name = optional_string(element, "file_name");
        file.file_size = optional_long(element, "file_size", 0);
        file.file_type = optional_string(element, "file_type");
        file.extracted_content = nullable_string(element, "extracted_content");
        files.push_back(std::move(file));
    }
    return files;
}

std::vector<ClaudeContent> decode_content(const Value& object) {
    std::vector<ClaudeContent> blocks;
    for (const Value& element : raw_list(object, "content")) {
        if (element.kind != Kind::Object) throw DecodeError("expected a content object");
        ClaudeContent block;
        block.type = optional_string(element, "type");
        block.text = optional_string(element, "text");
        block.thinking = optional_string(element, "thinking");
        for (const Value& citation : raw_list(element, "citations")) block.citations.push_back(citation);
        blocks.push_back(std::move(block));
    }
    return blocks;
}

ClaudeMessage decode_message(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a message object");
    ClaudeMessage message;
    message.uuid = required_string(value, "uuid");
    message.text = optional_string(value, "text");
    message.sender = optional_string(value, "sender");
    message.created_at = optional_string(value, "created_at");
    message.parent_message_uuid = nullable_string(value, "parent_message_uuid");
    message.attachments = decode_attachments(value);
    message.files = decode_files(value);
    message.content = decode_content(value);
    return message;
}

ClaudeConversation decode_conversation(const Value& value) {
    if (value.kind != Kind::Object) throw DecodeError("expected a conversation object");
    ClaudeConversation conversation;
    conversation.uuid = required_string(value, "uuid");
    conversation.name = optional_string(value, "name");
    conversation.summary = optional_string(value, "summary");
    conversation.created_at = optional_string(value, "created_at");
    conversation.updated_at = optional_string(value, "updated_at");
    for (const Value& element : raw_list(value, "chat_messages")) {
        conversation.chat_messages.push_back(decode_message(element));
    }
    return conversation;
}

// --------------------------------------------------------------------------- timestamps

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/** java.time.Instant.parse: the ISO instant format with a mandatory offset. */
std::optional<std::int64_t> parse_iso_instant(std::string_view text) {
    if (text.size() < 20) return std::nullopt;
    auto digits = [&](std::size_t offset, std::size_t count, int* out) {
        if (offset + count > text.size()) return false;
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text[offset + i];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        *out = value;
        return true;
    };
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!digits(0, 4, &year) || text[4] != '-' || !digits(5, 2, &month) || text[7] != '-' ||
        !digits(8, 2, &day) || (text[10] != 'T' && text[10] != 't') || !digits(11, 2, &hour) ||
        text[13] != ':' || !digits(14, 2, &minute) || text[16] != ':' ||
        !digits(17, 2, &second)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59) {
        return std::nullopt;
    }
    std::size_t cursor = 19;
    long long millis = 0;
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        std::size_t fraction_digits = 0;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            if (fraction_digits < 3) millis = millis * 10 + (text[cursor] - '0');
            ++fraction_digits;
            ++cursor;
            if (fraction_digits > 9) return std::nullopt;
        }
        if (fraction_digits == 0) return std::nullopt;
        for (std::size_t i = fraction_digits; i < 3; ++i) millis *= 10;
    }
    long long offset_seconds = 0;
    if (cursor >= text.size()) return std::nullopt;
    if (text[cursor] == 'Z' || text[cursor] == 'z') {
        ++cursor;
    } else if (text[cursor] == '+' || text[cursor] == '-') {
        const bool negative = text[cursor] == '-';
        ++cursor;
        int offset_hour = 0;
        int offset_minute = 0;
        if (!digits(cursor, 2, &offset_hour) || cursor + 2 >= text.size() ||
            text[cursor + 2] != ':' || !digits(cursor + 3, 2, &offset_minute)) {
            return std::nullopt;
        }
        cursor += 5;
        if (offset_hour > 23 || offset_minute > 59) return std::nullopt;
        offset_seconds = offset_hour * 3600 + offset_minute * 60;
        if (negative) offset_seconds = -offset_seconds;
    } else {
        return std::nullopt;
    }
    if (cursor != text.size()) return std::nullopt;

    const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month),
                                               static_cast<unsigned>(day));
    const std::int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
    return seconds * 1000 + millis;
}

/** iso8601ToMillis: the parsed instant, or the wall clock when the text is not an instant. */
std::int64_t iso8601_to_millis(const std::string& text) {
    const std::optional<std::int64_t> parsed = parse_iso_instant(text);
    if (parsed.has_value()) return *parsed;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// -------------------------------------------------------------------------- attachments

const char* const kTextExtensions[] = {
    ".txt", ".json", ".cpp", ".py",  ".js",  ".html", ".xml", ".md", ".java", ".kt",
    ".rs",  ".go",   ".ts",  ".c",   ".h",   ".yaml", ".yml", ".toml", ".cfg", ".ini",
    ".properties"};

const char* const kImageExtensions[] = {".png", ".jpg", ".jpeg", ".gif", ".webp",
                                        ".bmp", ".svg", ".heic", ".heif"};

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool ends_with_any(const std::string& name, const char* const* extensions, std::size_t count) {
    const std::string lowered = ascii_lower(name);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string extension = extensions[i];
        if (lowered.size() >= extension.size() &&
            lowered.compare(lowered.size() - extension.size(), extension.size(), extension) == 0) {
            return true;
        }
    }
    return false;
}

bool is_text_file(const std::string& name) {
    return ends_with_any(name, kTextExtensions, sizeof(kTextExtensions) / sizeof(char*));
}

bool is_image_file(const std::string& name) {
    return ends_with_any(name, kImageExtensions, sizeof(kImageExtensions) / sizeof(char*));
}

backup::AttachmentItem to_attachment_item(const std::string& file_name, const std::string& file_type,
                                          const std::optional<std::string>& extracted_content) {
    const bool is_image = file_type.rfind("image/", 0) == 0 || is_image_file(file_name);
    const bool is_text = !is_image && (is_text_file(file_name) || file_type.find("text") != std::string::npos);
    backup::AttachmentItem item;
    item.type = is_image ? "image" : "file";
    item.file_name = file_name;
    item.text_content = is_text ? extracted_content : std::nullopt;
    item.mime_type = file_type;
    return item;
}

std::optional<std::string> build_attachment_meta(const ClaudeMessage& message) {
    std::vector<backup::AttachmentItem> items;
    for (const ClaudeAttachment& attachment : message.attachments) {
        items.push_back(to_attachment_item(attachment.file_name, attachment.file_type,
                                           attachment.extracted_content));
    }
    for (const ClaudeFile& file : message.files) {
        items.push_back(to_attachment_item(file.file_name, file.file_type, file.extracted_content));
    }
    if (items.empty()) return std::nullopt;
    return backup::encode_attachment_meta(backup::AttachmentMeta{std::move(items)});
}

// ------------------------------------------------------------------------------ import

std::string conversation_title(const ClaudeConversation& conversation) {
    if (!conversation.name.empty()) return conversation.name;
    const std::string summary = conversation.summary.substr(0, 50);
    if (!summary.empty()) return summary;
    return "Untitled";
}

}  // namespace

std::optional<ClaudeConversations> decode_conversations(const json::Value& root) {
    if (root.kind != Kind::Object) return std::nullopt;
    try {
        ClaudeConversations conversations;
        for (const Value& element : raw_list(root, "conversations")) {
            conversations.conversations.push_back(decode_conversation(element));
        }
        return conversations;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

std::optional<ClaudeConversations> decode_conversation_list(const json::Value& root) {
    if (root.kind != Kind::Array) return std::nullopt;
    try {
        ClaudeConversations conversations;
        for (const Value& element : root.elements) {
            conversations.conversations.push_back(decode_conversation(element));
        }
        return conversations;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

ImportPreview preview(const ClaudeConversations& conversations) {
    ImportPreview result;
    result.conversation_count = static_cast<int>(conversations.conversations.size());

    std::vector<const ClaudeConversation*> ordered;
    ordered.reserve(conversations.conversations.size());
    for (const ClaudeConversation& conversation : conversations.conversations) {
        ordered.push_back(&conversation);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ClaudeConversation* left, const ClaudeConversation* right) {
                         return iso8601_to_millis(left->updated_at) >
                                iso8601_to_millis(right->updated_at);
                     });

    for (const ClaudeConversation* conversation : ordered) {
        ConversationSummary summary;
        summary.uuid = conversation->uuid;
        summary.title = conversation_title(*conversation);
        summary.message_count = static_cast<int>(conversation->chat_messages.size());
        result.conversations.push_back(std::move(summary));

        for (const ClaudeMessage& message : conversation->chat_messages) {
            ++result.total_message_count;
            if (message.sender == "human") ++result.human_message_count;
            if (message.sender == "assistant") ++result.assistant_message_count;
            if (!message.attachments.empty() || !message.files.empty()) result.has_attachments = true;
        }
    }
    return result;
}

ImportConversations to_import_format(const ClaudeConversations& conversations,
                                      const std::optional<std::vector<std::string>>& selected_ids) {
    ImportConversations result;
    for (const ClaudeConversation& conversation : conversations.conversations) {
        if (selected_ids.has_value()) {
            const auto& ids = *selected_ids;
            const bool selected = std::find(ids.begin(), ids.end(), conversation.uuid) != ids.end();
            if (!selected) continue;
        }
        ImportChatEntity chat;
        chat.id = conversation.uuid;
        chat.title = conversation_title(conversation);
        chat.last_updated = iso8601_to_millis(conversation.updated_at);
        result.conversations.push_back(std::move(chat));

        for (const ClaudeMessage& message : conversation.chat_messages) {
            std::vector<const ClaudeContent*> answer_blocks;
            for (const ClaudeContent& block : message.content) {
                if (block.type != "thinking") answer_blocks.push_back(&block);
            }
            std::string merged_text;
            if (!message.content.empty()) {
                for (const ClaudeContent* block : answer_blocks) merged_text += block->text;
            } else {
                merged_text = message.text;
            }

            std::vector<citations::CitationRecord> imported_citations;
            std::size_t answer_offset = 0;
            for (const ClaudeContent* block : answer_blocks) {
                for (const Value& citation : block->citations) {
                    const std::optional<citations::CitationRecord> record =
                        imported_citation::project_claude_citation(
                            citation, static_cast<std::int32_t>(answer_offset), block->text);
                    if (record.has_value()) imported_citations.push_back(*record);
                }
                answer_offset += json::utf16_length(block->text);
            }
            const std::optional<std::string> citation_json =
                imported_citation::encode_imported_citations(imported_citations, merged_text);

            ImportMessageEntity entity;
            entity.id = message.uuid;
            entity.conversation_id = conversation.uuid;
            if (message.parent_message_uuid.has_value() &&
                *message.parent_message_uuid != kZeroUuid) {
                entity.parent_id = message.parent_message_uuid;
            }
            entity.text = merged_text;
            for (const ClaudeContent& block : message.content) {
                if (block.type == "thinking") {
                    entity.thoughts = block.thinking;
                    break;
                }
            }
            entity.token_count = 0;
            entity.status = "SUCCESS";
            entity.participant = message.sender == "human" ? "USER" : "MODEL";
            entity.timestamp = iso8601_to_millis(message.created_at);
            entity.tool_call_json = citation_json;
            entity.attachment_meta = build_attachment_meta(message);
            result.messages.push_back(std::move(entity));
        }
    }
    return result;
}

Extraction extract_and_parse(const std::string& bytes) {
    Extraction extraction;
    // ImportStreams.isZip: the "PK" probe.
    const bool zipped = bytes.size() >= 2 && bytes[0] == 'P' && bytes[1] == 'K';
    if (zipped) {
        try {
            zip::Reader reader = zip::Reader::from_bytes(bytes);
            std::optional<ClaudeConversations> fallback;
            zip::Entry entry;
            while (reader.next_entry(entry)) {
                if (entry.name.empty() || entry.name.back() == '/') continue;
                if (entry.name.size() < 5 ||
                    entry.name.compare(entry.name.size() - 5, 5, ".json") != 0) {
                    continue;
                }
                const json::ParseResult parsed = json::parse(entry.content);
                if (!parsed.value) continue;
                std::optional<ClaudeConversations> conversations =
                    decode_conversations(*parsed.value);
                if (!conversations.has_value()) {
                    conversations = decode_conversation_list(*parsed.value);
                }
                if (!conversations.has_value() || conversations->conversations.empty()) continue;
                const std::string lowered = ascii_lower(entry.name);
                if (lowered.find("conversation") != std::string::npos) {
                    extraction.ok = true;
                    extraction.conversations = *conversations;
                    return extraction;
                }
                if (!fallback.has_value()) fallback = conversations;
            }
            if (fallback.has_value()) {
                extraction.ok = true;
                extraction.conversations = *fallback;
                return extraction;
            }
            extraction.failure = ExtractionFailure{"Exception", "No conversation data found in ZIP archive"};
            return extraction;
        } catch (const zip::ZipException& error) {
            extraction.failure = ExtractionFailure{"ZipException", error.what()};
            return extraction;
        }
    }

    // ImportStreams.peekFirstNonWhitespace: the wrapped-object vs bare-array shape.
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const unsigned char byte = static_cast<unsigned char>(bytes[cursor]);
        std::uint32_t code_point = byte;
        std::size_t width = 1;
        if (byte >= 0xF0) {
            width = 4;
            code_point = byte & 0x07U;
        } else if (byte >= 0xE0) {
            width = 3;
            code_point = byte & 0x0FU;
        } else if (byte >= 0xC0) {
            width = 2;
            code_point = byte & 0x1FU;
        }
        for (std::size_t i = 1; i < width; ++i) {
            if (cursor + i >= bytes.size()) break;
            code_point = (code_point << 6) |
                         (static_cast<unsigned char>(bytes[cursor + i]) & 0x3FU);
        }
        if (!json::is_kotlin_whitespace_code_point(code_point)) break;
        cursor += width;
    }
    if (cursor >= bytes.size()) {
        extraction.failure = ExtractionFailure{"Exception", "Unrecognized JSON format"};
        return extraction;
    }
    const char first = bytes[cursor];
    if (first != '{' && first != '[') {
        extraction.failure = ExtractionFailure{"Exception", "Unrecognized JSON format"};
        return extraction;
    }
    const json::ParseResult parsed = json::parse(bytes);
    if (!parsed.value) {
        extraction.failure = ExtractionFailure{"decode", ""};
        return extraction;
    }
    std::optional<ClaudeConversations> conversations;
    if (first == '{') {
        conversations = decode_conversations(*parsed.value);
    } else {
        conversations = decode_conversation_list(*parsed.value);
    }
    if (!conversations.has_value()) {
        extraction.failure = ExtractionFailure{"decode", ""};
        return extraction;
    }
    extraction.ok = true;
    extraction.conversations = *conversations;
    return extraction;
}

}  // namespace agora::core::claude_import
