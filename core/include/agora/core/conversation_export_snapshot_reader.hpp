#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agora/core/chat_dao.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::conversation_export_snapshot {

using ChatEntity = ::agora::core::ChatEntity;
using MessageEntity = ::agora::core::MessageEntity;
using RunEntity = ::agora::core::context_compact::RunEntity;
using LoopEntity = ::agora::core::automation::LoopEntity;
using TaskEntity = ::agora::core::automation::TaskEntity;

/** ConversationExportSnapshotReader.MESSAGE_PAGE_SIZE. */
inline constexpr int kMessagePageSize = 64;

enum class RecordType {
    Conversation = 0,
    Run = 1,
    Message = 2,
    Loop = 3,
    Task = 4,
};

/** One ordered snapshot record. The variant index matches RecordType. */
struct SnapshotRecord {
    using Payload = std::variant<ChatEntity, RunEntity, MessageEntity, LoopEntity, TaskEntity>;

    Payload payload;

    [[nodiscard]] RecordType type() const {
        return static_cast<RecordType>(payload.index());
    }
};

/**
 * The reader's DAO boundary.
 *
 * Kotlin opens a separate Room reader connection and wraps the traversal in one deferred
 * transaction, then closes the connection/executor in `finally`. The C++ caller owns that
 * connection lifecycle; ChatDaoSnapshotStore below supplies the same transaction contract and
 * delegates the reads to ChatDao over the shared SQL stack.
 */
class SnapshotStore {
public:
    virtual ~SnapshotStore() = default;

    virtual void begin_transaction() = 0;
    virtual void set_transaction_successful() = 0;
    virtual void end_transaction() = 0;

    virtual std::vector<ChatEntity> get_all_conversations_list() = 0;
    virtual std::vector<RunEntity> get_runs_for_conversation_snapshot(
        std::string_view conversation_id) = 0;
    virtual std::vector<MessageEntity> get_conversation_messages_page(
        std::string_view conversation_id,
        const std::optional<std::string>& after_id,
        int limit) = 0;
    virtual std::vector<LoopEntity> get_loops_for_conversation_snapshot(
        std::string_view conversation_id) = 0;
    virtual std::vector<TaskEntity> get_all_tasks_list() = 0;
};

/**
 * ChatDao-backed store used by the C++ composition. The owner supplies the same SQL executor
 * ChatDao reads through; the store's transaction stack supplies the reader transaction.
 */
class ChatDaoSnapshotStore final : public SnapshotStore {
public:
    ChatDaoSnapshotStore(::agora::core::chat_dao::ChatDao& dao, SqlExecutor& sql);

    void begin_transaction() override;
    void set_transaction_successful() override;
    void end_transaction() override;

    std::vector<ChatEntity> get_all_conversations_list() override;
    std::vector<RunEntity> get_runs_for_conversation_snapshot(
        std::string_view conversation_id) override;
    std::vector<MessageEntity> get_conversation_messages_page(
        std::string_view conversation_id,
        const std::optional<std::string>& after_id,
        int limit) override;
    std::vector<LoopEntity> get_loops_for_conversation_snapshot(
        std::string_view conversation_id) override;
    std::vector<TaskEntity> get_all_tasks_list() override;

private:
    ::agora::core::chat_dao::ChatDao& dao_;
    ::agora::core::TransactionStack transactions_;
};

using IncludeBody = std::function<bool(const ChatEntity&)>;
using RecordSink = std::function<void(const SnapshotRecord&)>;

/**
 * Coroutine cancellation is a runtime concern in Kotlin. The C++ port exposes the exact
 * `ensureActive` call sites as this callback so a caller can abort at the same points; an empty
 * callback is a no-op.
 */
using EnsureActive = std::function<void()>;

/**
 * Port of data/ConversationExportSnapshotReader.kt.
 *
 * Emits every conversation header, then (when includeBody accepts it) its runs, id-ordered
 * message pages, and loop, followed by all tasks. The whole traversal is one transaction and a
 * thrown includeBody/onRecord/ensureActive rolls it back.
 */
void read_snapshot(SnapshotStore& store,
                   const IncludeBody& include_body,
                   const RecordSink& on_record,
                   const EnsureActive& ensure_active = {});

}  // namespace agora::core::conversation_export_snapshot
