// Port of data/local/ChatMediaReferencesDao.kt.
//
// Five read-only projections over messages/conversations/new_chat_persist. The SQL text below is
// the annotation text unchanged; `pathToken` binds three times in the message page (IS NULL plus
// both instr calls) exactly like Room's generated adapter binds the named parameter once per
// occurrence.
#include "agora/core/chat_media_references_dao.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::media_references {

namespace {

namespace json = agora::core::json;

/** MessageConverters.toStringList: JSON array with the legacy "|||" format as fallback. */
std::vector<std::string> decode_string_list(const std::string& raw) {
    if (raw.empty()) return {};
    const json::ParseResult parsed = json::parse(raw);
    if (parsed.value.has_value() && parsed.value->kind == json::Kind::Array) {
        std::vector<std::string> out;
        for (const json::Value& element : parsed.value->elements) {
            if (element.kind != json::Kind::String) {
                throw KotlinError("SerializationException", "unexpected JSON element");
            }
            out.push_back(element.text);
        }
        return out;
    }
    // Backward compatibility: old format used "|||" delimiter.
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = raw.find("|||", start);
        if (at == std::string::npos) {
            parts.push_back(raw.substr(start));
            break;
        }
        parts.push_back(raw.substr(start, at - start));
        start = at + 3;
    }
    return parts;
}

SqlValue optional_text(const std::optional<std::string>& value) {
    return value.has_value() ? SqlValue::text_value(*value) : SqlValue::null();
}

std::vector<MessageAttachmentReference> attachment_rows(const std::vector<SqlRow>& rows) {
    std::vector<MessageAttachmentReference> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 3, "MessageAttachmentReference: row shape mismatch");
        MessageAttachmentReference reference;
        reference.id = row[0].as_text();
        reference.images = decode_string_list(row[1].as_text());
        reference.attachment_meta = row[2].text_or_null();
        out.push_back(std::move(reference));
    }
    return out;
}

}  // namespace

std::vector<MessageAttachmentReference>
ChatMediaReferencesDao::get_message_attachment_references_page(
    const std::optional<std::string>& after_id,
    int limit,
    const std::optional<std::string>& path_token) {
    const std::string sql =
        "SELECT id, images, attachmentMeta FROM messages WHERE (? IS NULL OR id > ?) AND (? IS "
        "NULL OR instr(images, ?) > 0 OR instr(attachmentMeta, ?) > 0) AND ((images != '' AND "
        "images != '[]') OR (attachmentMeta IS NOT NULL AND attachmentMeta != '')) ORDER BY id "
        "LIMIT ?";
    const SqlValue after = optional_text(after_id);
    const SqlValue path = optional_text(path_token);
    const std::vector<SqlRow> rows =
        sql_.query(sql, {after, after, path, path, path, SqlValue::integer_value(limit)});
    return attachment_rows(rows);
}

std::vector<MessageAttachmentReference>
ChatMediaReferencesDao::get_conversation_message_attachment_references_page(
    std::string_view conversation_id,
    const std::optional<std::string>& after_id,
    int limit) {
    const std::string sql =
        "SELECT id, images, attachmentMeta FROM messages WHERE conversationId = ? AND (? IS NULL "
        "OR id > ?) AND ((images != '' AND images != '[]') OR (attachmentMeta IS NOT NULL AND "
        "attachmentMeta != '')) ORDER BY id LIMIT ?";
    const SqlValue after = optional_text(after_id);
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::text_value(std::string(conversation_id)), after, after,
                         SqlValue::integer_value(limit)});
    return attachment_rows(rows);
}

std::vector<MessageToolMediaReference>
ChatMediaReferencesDao::get_message_tool_media_references_page(
    const std::optional<std::string>& after_id,
    int limit) {
    const std::string sql =
        "SELECT id, toolCallJson FROM messages WHERE (? IS NULL OR id > ?) AND toolCallJson IS "
        "NOT NULL AND toolCallJson != '' ORDER BY id LIMIT ?";
    const SqlValue after = optional_text(after_id);
    const std::vector<SqlRow> rows =
        sql_.query(sql, {after, after, SqlValue::integer_value(limit)});
    std::vector<MessageToolMediaReference> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 2, "MessageToolMediaReference: row shape mismatch");
        MessageToolMediaReference reference;
        reference.id = row[0].as_text();
        reference.tool_call_json = row[1].as_text();
        out.push_back(std::move(reference));
    }
    return out;
}

std::vector<ConversationDraftAttachmentReference>
ChatMediaReferencesDao::get_conversation_draft_attachment_references_page(
    const std::optional<std::string>& after_id,
    int limit,
    const std::optional<std::string>& path_token) {
    const std::string sql =
        "SELECT id, draftAttachments FROM conversations WHERE (? IS NULL OR id > ?) AND (? IS "
        "NULL OR instr(draftAttachments, ?) > 0) AND draftAttachments IS NOT NULL AND "
        "draftAttachments != '' ORDER BY id LIMIT ?";
    const SqlValue after = optional_text(after_id);
    const SqlValue path = optional_text(path_token);
    const std::vector<SqlRow> rows =
        sql_.query(sql, {after, after, path, path, SqlValue::integer_value(limit)});
    std::vector<ConversationDraftAttachmentReference> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 2, "ConversationDraftAttachmentReference: row shape mismatch");
        ConversationDraftAttachmentReference reference;
        reference.id = row[0].as_text();
        reference.draft_attachments = row[1].as_text();
        out.push_back(std::move(reference));
    }
    return out;
}

std::optional<NewChatDraftAttachmentReference>
ChatMediaReferencesDao::get_new_chat_draft_attachment_reference(
    const std::optional<std::string>& path_token) {
    const std::string sql =
        "SELECT draftAttachments FROM new_chat_persist WHERE id = 0 AND (? IS NULL OR "
        "instr(draftAttachments, ?) > 0) AND draftAttachments IS NOT NULL AND draftAttachments "
        "!= ''";
    const SqlValue path = optional_text(path_token);
    const std::vector<SqlRow> rows = sql_.query(sql, {path, path});
    if (rows.empty()) return std::nullopt;
    kotlin_check(rows.front().size() == 1, "NewChatDraftAttachmentReference: row shape mismatch");
    NewChatDraftAttachmentReference reference;
    reference.draft_attachments = rows.front().front().as_text();
    return reference;
}

}  // namespace agora::core::media_references
