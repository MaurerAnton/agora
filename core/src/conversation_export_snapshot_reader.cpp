#include "agora/core/conversation_export_snapshot_reader.hpp"

#include <exception>

namespace agora::core::conversation_export_snapshot {
namespace {

void check_active(const EnsureActive& ensure_active) {
    if (ensure_active) ensure_active();
}

}  // namespace

ChatDaoSnapshotStore::ChatDaoSnapshotStore(::agora::core::chat_dao::ChatDao& dao,
                                           SqlExecutor& sql)
    : dao_(dao), transactions_(sql) {}

void ChatDaoSnapshotStore::begin_transaction() { transactions_.begin(); }

void ChatDaoSnapshotStore::set_transaction_successful() { transactions_.mark_successful(); }

void ChatDaoSnapshotStore::end_transaction() { transactions_.end(); }

std::vector<ChatEntity> ChatDaoSnapshotStore::get_all_conversations_list() {
    return dao_.get_all_conversations_list();
}

std::vector<RunEntity> ChatDaoSnapshotStore::get_runs_for_conversation_snapshot(
    std::string_view conversation_id) {
    return dao_.get_runs_for_conversation_snapshot(conversation_id);
}

std::vector<MessageEntity> ChatDaoSnapshotStore::get_conversation_messages_page(
    std::string_view conversation_id,
    const std::optional<std::string>& after_id,
    int limit) {
    return dao_.get_conversation_messages_page(conversation_id, after_id, limit);
}

std::vector<LoopEntity> ChatDaoSnapshotStore::get_loops_for_conversation_snapshot(
    std::string_view conversation_id) {
    return dao_.get_loops_for_conversation_snapshot(conversation_id);
}

std::vector<TaskEntity> ChatDaoSnapshotStore::get_all_tasks_list() {
    return dao_.get_all_tasks_list();
}

void read_snapshot(SnapshotStore& store,
                   const IncludeBody& include_body,
                   const RecordSink& on_record,
                   const EnsureActive& ensure_active) {
    store.begin_transaction();
    try {
        for (const ChatEntity& conversation : store.get_all_conversations_list()) {
            check_active(ensure_active);
            on_record(SnapshotRecord{conversation});

            const bool include = include_body ? include_body(conversation) : true;
            if (!include) continue;

            for (const RunEntity& run :
                 store.get_runs_for_conversation_snapshot(conversation.id)) {
                check_active(ensure_active);
                on_record(SnapshotRecord{run});
            }

            std::optional<std::string> after_message_id;
            while (true) {
                check_active(ensure_active);
                const std::vector<MessageEntity> page = store.get_conversation_messages_page(
                    conversation.id, after_message_id, kMessagePageSize);
                if (page.empty()) break;

                for (const MessageEntity& message : page) {
                    check_active(ensure_active);
                    on_record(SnapshotRecord{message});
                }

                after_message_id = page.back().id;
                if (static_cast<int>(page.size()) < kMessagePageSize) break;
            }

            for (const LoopEntity& loop :
                 store.get_loops_for_conversation_snapshot(conversation.id)) {
                check_active(ensure_active);
                on_record(SnapshotRecord{loop});
            }
        }

        for (const TaskEntity& task : store.get_all_tasks_list()) {
            check_active(ensure_active);
            on_record(SnapshotRecord{task});
        }

        store.set_transaction_successful();
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        try {
            store.end_transaction();
        } catch (...) {
            // Preserve the traversal failure; Kotlin's withTransaction does the same while the
            // reader's finally closes the platform connection.
        }
        std::rethrow_exception(failure);
    }
    store.end_transaction();
}

}  // namespace agora::core::conversation_export_snapshot
