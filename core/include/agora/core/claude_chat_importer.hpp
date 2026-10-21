#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::claude_import {

/**
 * Port of data/ClaudeChatImporter.kt: the "import a Claude chat export" path.
 *
 * The Kotlin file has three public steps:
 *   extractAndParse — pick the conversations out of the export bytes (a ZIP archive or a raw JSON
 *       document) with the importer's own JSON configuration;
 *   preview — the per-conversation summary the import dialog shows;
 *   toImportFormat — the chat/message entities the database import writes.
 *
 * Decoding follows kotlinx's `Json { ignoreUnknownKeys = true; isLenient = true;
 * coerceInputValues = true }`: unknown keys are ignored, a primitive is accepted where a string is
 * expected (lenient), a null becomes the field's default (or null for a nullable field) and a
 * shape mismatch fails the whole document.
 */
struct ClaudeAttachment {
    std::string file_name;
    std::int64_t file_size = 0;
    std::string file_type;
    std::optional<std::string> extracted_content;
};

struct ClaudeFile {
    std::string file_uuid;
    std::string file_name;
    std::int64_t file_size = 0;
    std::string file_type;
    std::optional<std::string> extracted_content;
};

struct ClaudeContent {
    std::string type;
    std::string text;
    std::string thinking;
    std::vector<json::Value> citations;
};

struct ClaudeMessage {
    std::string uuid;
    std::string text;
    std::string sender;
    std::string created_at;
    std::optional<std::string> parent_message_uuid;
    std::vector<ClaudeAttachment> attachments;
    std::vector<ClaudeFile> files;
    std::vector<ClaudeContent> content;
};

struct ClaudeConversation {
    std::string uuid;
    std::string name;
    std::string summary;
    std::string created_at;
    std::string updated_at;
    std::vector<ClaudeMessage> chat_messages;
};

struct ClaudeConversations {
    std::vector<ClaudeConversation> conversations;
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
    int human_message_count = 0;
    int assistant_message_count = 0;
    bool has_attachments = false;
};

struct ImportChatEntity {
    std::string id;
    std::string title;
    std::int64_t last_updated = 0;
    /** The selected branches of the current path: the GPT importer sets it, Claude never does. */
    std::optional<std::string> selected_branches_json;
};

struct ImportMessageEntity {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_id;
    std::string text;
    std::vector<std::string> images;
    std::optional<std::string> thoughts;
    std::optional<std::string> thought_title;
    int token_count = 0;
    std::string status = "SUCCESS";
    std::string participant = "MODEL";
    std::int64_t timestamp = 0;
    std::optional<std::int64_t> thought_time_ms;
    std::optional<std::string> model_name;
    std::optional<std::string> tool_call_json;
    std::optional<std::string> attachment_meta;
};

struct ImportConversations {
    std::vector<ImportChatEntity> conversations;
    std::vector<ImportMessageEntity> messages;
};

/** A kotlinx SerializationException: the document does not match the model. */
class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** decodeFromStream<ClaudeConversations> (the wrapped object) and the bare-array form. */
std::optional<ClaudeConversations> decode_conversations(const json::Value& root);
std::optional<ClaudeConversations> decode_conversation_list(const json::Value& root);

/** preview(conversations). */
ImportPreview preview(const ClaudeConversations& conversations);

/** toImportFormat(conversations, selectedIds). */
ImportConversations to_import_format(
    const ClaudeConversations& conversations,
    const std::optional<std::vector<std::string>>& selected_ids = std::nullopt);

/** The failure of extractAndParse: the Kotlin exception category and its message. */
struct ExtractionFailure {
    std::string type;     // "decode", "Exception", "ZipException", ...
    std::string message;  // empty for the decode category
};

struct Extraction {
    bool ok = false;
    ClaudeConversations conversations;
    ExtractionFailure failure;
};

/** extractAndParse(openStream) over the export bytes (a ZIP archive or a raw JSON document). */
Extraction extract_and_parse(const std::string& bytes);

}  // namespace agora::core::claude_import
