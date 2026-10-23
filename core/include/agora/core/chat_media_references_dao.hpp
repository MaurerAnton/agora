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

/** MessageToolMediaReference (data/local/ChatEntities.kt): the tool payload only. */
struct MessageToolMediaReference {
    std::string id;
    std::string tool_call_json;
};

/** ConversationDraftAttachmentReference: a conversation draft column projection. */
struct ConversationDraftAttachmentReference {
    std::string id;
    std::string draft_attachments;
};

/** NewChatDraftAttachmentReference: the singleton new-chat draft projection. */
struct NewChatDraftAttachmentReference {
    std::string draft_attachments;
};

/**
 * Port of data/local/ChatMediaReferencesDao.kt: the bounded media-reference reads inherited by
 * the sole ChatDao. All five members are read-only @Query projections; the SQL text is
 * transcribed verbatim and Room is replaced by the SqlExecutor boundary.
 *
 * `pathToken` is a raw, case-sensitive `instr` substring over the stored JSON text (never a LIKE
 * pattern), and the eligibility predicate looks at the raw column values: an image list counts
 * only when it is neither '' nor '[]', while an attachment meta counts only when it is non-null
 * and non-empty.
 */
class ChatMediaReferencesDao {
public:
    explicit ChatMediaReferencesDao(SqlExecutor& sql) : sql_(sql) {}

    std::vector<MessageAttachmentReference> get_message_attachment_references_page(
        const std::optional<std::string>& after_id,
        int limit,
        const std::optional<std::string>& path_token = std::nullopt);

    std::vector<MessageAttachmentReference> get_conversation_message_attachment_references_page(
        std::string_view conversation_id,
        const std::optional<std::string>& after_id,
        int limit);

    std::vector<MessageToolMediaReference> get_message_tool_media_references_page(
        const std::optional<std::string>& after_id,
        int limit);

    std::vector<ConversationDraftAttachmentReference>
    get_conversation_draft_attachment_references_page(
        const std::optional<std::string>& after_id,
        int limit,
        const std::optional<std::string>& path_token = std::nullopt);

    std::optional<NewChatDraftAttachmentReference> get_new_chat_draft_attachment_reference(
        const std::optional<std::string>& path_token = std::nullopt);

private:
    SqlExecutor& sql_;
};

}  // namespace agora::core::media_references
