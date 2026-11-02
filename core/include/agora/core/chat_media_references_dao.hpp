#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/sql_boundary.hpp"

namespace agora::core::media_references {

/** MessageAttachmentReference (data/local/ChatEntities.kt): an image/meta projection. */
struct MessageAttachmentReference {
    std::string id;
    std::vector<std::string> images;
    std::optional<std::string> attachment_meta;
};

/** ConversationDraftAttachmentReference: a conversation draft column projection (nullable). */
struct ConversationDraftAttachmentReference {
    std::string id;
    std::optional<std::string> draft_attachments;
};

/**
 * Port of data/local/ChatMediaReferencesDao.kt: the bounded media-reference reads inherited by
 * the sole ChatDao. The four members are read-only projections; the SQL text is transcribed
 * verbatim and Room is replaced by the SqlExecutor boundary. The combined page method mirrors the
 * Kotlin interface default: a null cursor seeks the first page, a present cursor the next one.
 */
class ChatMediaReferencesDao {
public:
    explicit ChatMediaReferencesDao(SqlExecutor& sql) : sql_(sql) {}

    std::optional<ConversationDraftAttachmentReference> get_conversation_draft_attachment_reference(
        std::string_view id);

    std::vector<MessageAttachmentReference> get_conversation_attachment_first_page(
        std::string_view conversation_id,
        int limit);

    std::vector<MessageAttachmentReference> get_conversation_attachment_next_page(
        std::string_view conversation_id,
        const std::string& after_id,
        int limit);

    std::vector<MessageAttachmentReference> get_conversation_message_attachment_references_page(
        std::string_view conversation_id,
        const std::optional<std::string>& after_id,
        int limit);

private:
    SqlExecutor& sql_;
};

}  // namespace agora::core::media_references
