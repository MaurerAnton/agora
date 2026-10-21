#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/claude_chat_importer.hpp"
#include "agora/core/json.hpp"

namespace agora::core::gpt_import {

/**
 * Port of data/GptChatImporter.kt: the "import a ChatGPT export" path.
 *
 * The Kotlin file has three public steps:
 *   extractAndParse — read the conversations out of the export bytes (a ZIP archive with
 *       conversations.json / conversations-N.json entries, or the raw array);
 *   preview — the per-conversation summary the import dialog shows;
 *   toImportFormat — the chat/message entities the database import writes (the same
 *       ClaudeChatImporter.ImportConversations the Claude importer produces).
 *
 * Decoding follows kotlinx's `Json { ignoreUnknownKeys = true; isLenient = true }` (no
 * coerceInputValues here: a null in a non-nullable field fails the document).
 */
struct GptThought {
    std::string content;
    std::string summary;
};

struct GptFinishDetails {
    std::string type;
};

struct GptMetadata {
    std::optional<std::string> model_slug;
    std::optional<std::string> parent_id;
    std::optional<bool> is_complete;
    std::vector<json::Value> content_references;
    std::vector<json::Value> citations;
    std::optional<GptFinishDetails> finish_details;
    std::optional<int> finished_duration_sec;
};

struct GptAuthor {
    std::string role;
    std::optional<std::string> name;
};

struct GptContent {
    std::string content_type;
    std::optional<std::vector<json::Value>> parts;
    std::optional<std::string> text;
    std::optional<std::string> result;
    std::optional<std::string> reasoning_text;
    std::optional<std::string> language;
    std::optional<std::vector<GptThought>> thoughts;
};

struct GptMessage {
    std::string id;
    std::optional<GptAuthor> author;
    std::optional<GptContent> content;
    std::optional<double> create_time;
    std::optional<GptMetadata> metadata;
};

struct GptMappingNode {
    std::string id;
    std::optional<GptMessage> message;
    std::optional<std::string> parent;
};

struct GptConversation {
    std::string conversation_id;
    std::string title;
    double create_time = 0.0;
    double update_time = 0.0;
    std::optional<std::string> current_node;
    /** The mapping in document order (Kotlin's LinkedHashMap). */
    std::vector<std::pair<std::string, GptMappingNode>> mapping;
};

struct ConversationSummary {
    std::string uuid;
    std::string title;
    int message_count = 0;
};

struct ImportPreview {
    std::vector<ConversationSummary> conversations;
    int conversation_count = 0;
    int total_message_count = 0;
    int user_message_count = 0;
    int assistant_message_count = 0;
    bool has_attachments = false;
};

/** A kotlinx SerializationException: the document does not match the model. */
class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** decodeFromStream<List<GptConversation>>. */
std::optional<std::vector<GptConversation>> decode_conversations(const json::Value& root);

/** preview(conversations). */
ImportPreview preview(const std::vector<GptConversation>& conversations);

/** toImportFormat(conversations, selectedIds) — the shared import entities. */
claude_import::ImportConversations to_import_format(
    const std::vector<GptConversation>& conversations,
    const std::optional<std::vector<std::string>>& selected_ids = std::nullopt);

struct ExtractionFailure {
    std::string type;     // "decode", "Exception", "ZipException", ...
    std::string message;  // empty for the decode category
};

struct Extraction {
    bool ok = false;
    std::vector<GptConversation> conversations;
    ExtractionFailure failure;
};

/** extractAndParse(openStream) over the export bytes. */
Extraction extract_and_parse(const std::string& bytes);

}  // namespace agora::core::gpt_import
