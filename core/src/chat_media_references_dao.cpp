// Port of data/local/ChatMediaReferencesDao.kt.
//
// Four read-only projections over messages/conversations; the SQL text below is the annotation
// text unchanged. The combined page method is the Kotlin interface default: a null cursor seeks
// the first page, a present cursor seeks the next one.
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

std::optional<ConversationDraftAttachmentReference>
ChatMediaReferencesDao::get_conversation_draft_attachment_reference(std::string_view id) {
    const std::string sql = "SELECT id, draftAttachments FROM conversations WHERE id = ?";
    const std::vector<SqlRow> rows = sql_.query(sql, {SqlValue::text_value(std::string(id))});
    if (rows.empty()) return std::nullopt;
    kotlin_check(rows.front().size() == 2, "ConversationDraftAttachmentReference: row shape mismatch");
    ConversationDraftAttachmentReference reference;
    reference.id = rows.front()[0].as_text();
    reference.draft_attachments = rows.front()[1].text_or_null();
    return reference;
}

std::vector<MessageAttachmentReference> ChatMediaReferencesDao::get_conversation_attachment_first_page(
    std::string_view conversation_id,
    int limit) {
    const std::string sql =
        "SELECT id, images, attachmentMeta FROM messages WHERE conversationId = ? ORDER BY id "
        "LIMIT ?";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::text_value(std::string(conversation_id)),
                         SqlValue::integer_value(limit)});
    return attachment_rows(rows);
}

std::vector<MessageAttachmentReference> ChatMediaReferencesDao::get_conversation_attachment_next_page(
    std::string_view conversation_id,
    const std::string& after_id,
    int limit) {
    const std::string sql =
        "SELECT id, images, attachmentMeta FROM messages WHERE conversationId = ? AND id > ? "
        "ORDER BY id LIMIT ?";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::text_value(std::string(conversation_id)),
                         SqlValue::text_value(after_id), SqlValue::integer_value(limit)});
    return attachment_rows(rows);
}

std::vector<MessageAttachmentReference>
ChatMediaReferencesDao::get_conversation_message_attachment_references_page(
    std::string_view conversation_id,
    const std::optional<std::string>& after_id,
    int limit) {
    // Kotlin interface default body: if (afterId == null) first page else next page.
    if (after_id.has_value()) {
        return get_conversation_attachment_next_page(conversation_id, *after_id, limit);
    }
    return get_conversation_attachment_first_page(conversation_id, limit);
}

}  // namespace agora::core::media_references
