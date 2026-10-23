// Port of data/local/NewChatPersistDao.kt.
//
// One singleton row (`id = 0`) read by both the Flow and the suspend member, written by Room's
// @Upsert algorithm, and deleted either unconditionally or through the null-safe `IS` comparison
// of every persisted field.
#include "agora/core/new_chat_persist_dao.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::new_chat_persist {

namespace {

constexpr const char* kSelectSql =
    "SELECT id, modelId, systemPromptId, conversationSettingsJson, draftText, draftAttachments "
    "FROM new_chat_persist WHERE id = 0";

SqlValue optional_text(const std::optional<std::string>& value) {
    return value.has_value() ? SqlValue::text_value(*value) : SqlValue::null();
}

std::optional<NewChatPersistEntity> read_row(SqlExecutor& sql) {
    const std::vector<SqlRow> rows = sql.query(kSelectSql, {});
    if (rows.empty()) return std::nullopt;
    kotlin_check(rows.front().size() == 6, "NewChatPersistEntity: row shape mismatch");
    const SqlRow& row = rows.front();
    NewChatPersistEntity entity;
    entity.id = static_cast<int>(row[0].as_integer());
    entity.model_id = row[1].text_or_null();
    entity.system_prompt_id = row[2].text_or_null();
    entity.conversation_settings_json = row[3].text_or_null();
    entity.draft_text = row[4].as_text();
    entity.draft_attachments = row[5].text_or_null();
    return entity;
}

}  // namespace

void NewChatPersistEntity::validate() const { kotlin_require(id == 0); }

std::optional<NewChatPersistEntity> NewChatPersistDao::observe_new_chat_persist() {
    return read_row(sql_);
}

std::optional<NewChatPersistEntity> NewChatPersistDao::get_new_chat_persist() {
    return read_row(sql_);
}

void NewChatPersistDao::upsert_new_chat_persist(const NewChatPersistEntity& entity) {
    entity.validate();
    try {
        sql_.execute(
            "INSERT INTO new_chat_persist (id, modelId, systemPromptId, "
            "conversationSettingsJson, draftText, draftAttachments) VALUES (?, ?, ?, ?, ?, ?)",
            {SqlValue::integer_value(entity.id), optional_text(entity.model_id),
             optional_text(entity.system_prompt_id),
             optional_text(entity.conversation_settings_json),
             SqlValue::text_value(entity.draft_text),
             optional_text(entity.draft_attachments)});
    } catch (const SqlConstraintError&) {
        // Room's @Upsert: on a primary-key conflict, update the row with the same key.
        sql_.execute(
            "UPDATE new_chat_persist SET modelId = ?, systemPromptId = ?, "
            "conversationSettingsJson = ?, draftText = ?, draftAttachments = ? WHERE id = ?",
            {optional_text(entity.model_id), optional_text(entity.system_prompt_id),
             optional_text(entity.conversation_settings_json),
             SqlValue::text_value(entity.draft_text), optional_text(entity.draft_attachments),
             SqlValue::integer_value(entity.id)});
    }
}

int NewChatPersistDao::delete_new_chat_persist() {
    return static_cast<int>(sql_.execute("DELETE FROM new_chat_persist WHERE id = 0", {}));
}

int NewChatPersistDao::delete_new_chat_persist_if_matches(
    const std::optional<std::string>& model_id,
    const std::optional<std::string>& system_prompt_id,
    const std::optional<std::string>& conversation_settings_json,
    std::string_view draft_text,
    const std::optional<std::string>& draft_attachments) {
    return static_cast<int>(sql_.execute(
        "DELETE FROM new_chat_persist WHERE id = 0 AND modelId IS ? AND systemPromptId IS ? AND "
        "conversationSettingsJson IS ? AND draftText = ? AND draftAttachments IS ?",
        {optional_text(model_id), optional_text(system_prompt_id),
         optional_text(conversation_settings_json),
         SqlValue::text_value(std::string(draft_text)), optional_text(draft_attachments)}));
}

}  // namespace agora::core::new_chat_persist
