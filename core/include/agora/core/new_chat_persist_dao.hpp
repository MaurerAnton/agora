#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "agora/core/sql_boundary.hpp"

namespace agora::core::new_chat_persist {

/** NewChatPersistEntity (data/local/ChatEntities.kt): the id=0 singleton draft row. */
struct NewChatPersistEntity {
    int id = 0;
    std::optional<std::string> model_id;
    std::optional<std::string> system_prompt_id;
    std::optional<std::string> conversation_settings_json;
    std::string draft_text;
    std::optional<std::string> draft_attachments;

    /** NewChatPersistEntity.init: require(id == SINGLETON_ID). */
    void validate() const;
};

/**
 * Port of data/local/NewChatPersistDao.kt: the singleton New Chat draft persistence inherited by
 * ChatDao.
 *
 * `observeNewChatPersist` returns a Room Flow. The C++ application has no coroutine stream, so
 * the port exposes the query's current row — the Flow's initial emission. Re-emission after a
 * write is store invalidation, which the caller's store owns.
 *
 * @Upsert follows Room's generated algorithm: INSERT, and on a primary-key conflict UPDATE every
 * non-key column of the row with the same key.
 */
class NewChatPersistDao {
public:
    explicit NewChatPersistDao(SqlExecutor& sql) : sql_(sql) {}

    std::optional<NewChatPersistEntity> observe_new_chat_persist();

    std::optional<NewChatPersistEntity> get_new_chat_persist();

    void upsert_new_chat_persist(const NewChatPersistEntity& entity);

    int delete_new_chat_persist();

    int delete_new_chat_persist_if_matches(
        const std::optional<std::string>& model_id,
        const std::optional<std::string>& system_prompt_id,
        const std::optional<std::string>& conversation_settings_json,
        std::string_view draft_text,
        const std::optional<std::string>& draft_attachments);

private:
    SqlExecutor& sql_;
};

}  // namespace agora::core::new_chat_persist
