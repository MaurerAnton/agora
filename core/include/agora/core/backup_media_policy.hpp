#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::backup {

/** Port of model/AttachmentMeta.kt's AttachmentStorage (serialized by @SerialName). */
enum class AttachmentStorage { app_private, local_sandbox_pending, local_sandbox_runtime };

/** AttachmentStorage.isLocalSandbox. */
inline bool is_local_sandbox(AttachmentStorage storage) {
    return storage != AttachmentStorage::app_private;
}

/** Port of model/AttachmentMeta.kt's AttachmentImportState. */
enum class AttachmentImportState { processing, ready, failed };

/** Port of model/AttachmentMeta.kt's AttachmentItem. Field order matches the Kotlin declaration. */
struct AttachmentItem {
    std::optional<std::string> original_uri;
    std::string type;
    std::optional<std::string> file_name;
    std::optional<std::string> mime_type;
    std::optional<int> image_index;
    std::optional<int> page_count;
    std::optional<int> pixel_width;
    std::optional<int> pixel_height;
    std::optional<std::string> warning;
    std::optional<std::string> text_content;
    std::optional<std::string> transcription;
    AttachmentStorage storage = AttachmentStorage::app_private;
    std::optional<std::string> sandbox_path;
    std::optional<std::int64_t> file_size;
    bool unavailable = false;
};

/** Port of model/AttachmentMeta.kt's AttachmentMeta. */
struct AttachmentMeta {
    std::vector<AttachmentItem> items;
};

/** Port of model/ChatMessage.kt's SelectedAttachment. */
struct SelectedAttachment {
    std::string local_id;
    std::string uri;
    std::string type;
    std::optional<int> frame_count;
    std::optional<std::int64_t> slice_interval_ms;
    std::optional<int> page_count;
    std::optional<std::int64_t> video_duration_ms;
    std::optional<std::string> file_name;
    std::optional<std::string> mime_type;
    std::optional<std::int64_t> file_size;
    std::optional<int> pixel_width;
    std::optional<int> pixel_height;
    std::optional<std::vector<std::string>> processed_frames;
    std::optional<std::vector<int>> selected_pages;  // a LinkedHashSet in Kotlin
    std::optional<std::vector<std::string>> pre_rendered_paths;
    std::optional<std::string> local_path;
    AttachmentStorage storage = AttachmentStorage::app_private;
    std::optional<std::string> sandbox_path;
    AttachmentImportState import_state = AttachmentImportState::ready;
    std::optional<std::string> prepared_text;
    bool unavailable = false;
};

/** Port of model/ChatMessage.kt's ToolImageAttachment. */
struct ToolImageAttachment {
    std::string path;
    std::string mime_type;
    std::int64_t size_bytes = 0;
    std::optional<int> width;
    std::optional<int> height;
    std::string sha256;
};

/**
 * Port of model/ChatMessage.kt's MessageSegment.
 *
 * Only the serialized fields are modelled. toolImageRequestKey and streamingTextDeltas are
 * @Transient in Kotlin and are therefore absent here rather than carried and dropped.
 * responseOutputItems holds each element re-serialized from the parsed JSON: kotlinx decodes
 * them as JsonObject (a LinkedHashMap, so source order) and re-encodes in that same order, which
 * makes the verbatim text the faithful representation.
 */
struct MessageSegment {
    std::string type;
    std::string content;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_args;
    std::optional<std::string> tool_result;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> signature;
    std::optional<std::string> signature_provider;
    std::optional<std::int64_t> duration_ms;
    std::optional<std::string> tool_state;
    std::optional<std::string> tool_progress;
    std::optional<std::string> tool_target;
    std::optional<std::string> tool_display_name;
    std::optional<std::string> tool_result_text;
    std::optional<std::string> tool_structured_result;
    std::vector<ToolImageAttachment> tool_images;
    std::optional<std::string> tool_transcription;
    std::vector<std::string> response_output_items;
    std::optional<std::string> response_output_item_provider;
    std::optional<std::string> error_code;
};

// ---------------------------------------------------------------------------------------------
// Archive callbacks
// ---------------------------------------------------------------------------------------------

/** Maps a live source path to its archive entry; nullopt means "not in this backup". */
using SourceLookup = std::function<std::optional<std::string>(std::string_view source)>;
/** Reports a resource the archive could not carry, so the UI can tell the user. */
using MissingResource = std::function<void()>;
/** Restores an archive entry to its (localPath, uri) pair; nullopt means "gone". */
using PrimaryLookup =
    std::function<std::optional<std::pair<std::string, std::string>>(std::string_view entry)>;
/** Restores an archive entry to a single device path; nullopt means "gone". */
using PathLookup = std::function<std::optional<std::string>(std::string_view entry)>;

// ---------------------------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------------------------

/** Serializes exactly like kotlinx with encodeDefaults = false: declaration order, no defaults. */
std::string encode_attachment_meta(const AttachmentMeta& meta);
std::string encode_attachment_item(const AttachmentItem& item);
std::string encode_selected_attachment(const SelectedAttachment& attachment);
std::string encode_tool_image(const ToolImageAttachment& image);
std::string encode_message_segment(const MessageSegment& segment);
/** Canonical kotlinx list encoding, used by the persistence guard and the backup codec. */
std::string encode_message_segments(const std::vector<MessageSegment>& segments);

/** Decodes with kotlinx's ignoreUnknownKeys = true; nullopt stands in for a thrown exception. */
std::optional<AttachmentMeta> decode_attachment_meta(std::string_view raw);
std::optional<std::vector<MessageSegment>> decode_message_segments(std::string_view raw);

/**
 * Native port of data/NativeBackupMediaPolicy.kt.
 *
 * The policy decides which attachment bytes a native backup may carry and how a restored draft
 * points at them again. Two invariants make a port easy to get subtly wrong and are pinned by the
 * golden traces:
 *
 *  - Candidate sources are probed in a fixed order and the first hit wins, so both the result and
 *    the sequence of archive lookups matter. A port that resolves the same value while asking
 *    for different paths is wrong about what the archive layer is being told.
 *  - Availability is decided per attachment type: a video needs an archived payload, a file needs
 *    its text, an image or pdf needs at least one surviving media index, and any other type is
 *    always available.
 */
class NativeBackupMediaPolicy {
   public:
    /** Sandbox payloads belong to the live runtime and are never copied into native backups. */
    static std::vector<SelectedAttachment> exportable_draft_attachments(
        const std::vector<SelectedAttachment>& attachments);

    static std::vector<SelectedAttachment> rewrite_draft_attachments_for_export(
        const std::vector<SelectedAttachment>& attachments, const SourceLookup& archive_entry,
        const MissingResource& on_missing);

    static std::vector<SelectedAttachment> restore_draft_attachments(
        const std::vector<SelectedAttachment>& attachments, const PrimaryLookup& restored_primary,
        const PathLookup& restored_path);

    static std::vector<std::string> tool_image_paths(const std::optional<std::string>& raw);

    static std::optional<std::string> rewrite_attachment_meta_for_export(
        const std::optional<std::string>& raw, const std::vector<std::string>& original_image_sources,
        const std::map<int, int>& old_to_new_image_index, const SourceLookup& archive_entry,
        const MissingResource& on_missing);

    static std::optional<std::string> rewrite_tool_image_paths_for_export(
        const std::optional<std::string>& raw, const SourceLookup& archive_entry);

    static std::optional<std::string> restore_attachment_meta(
        const std::optional<std::string>& raw, int archive_version,
        const std::map<int, int>& old_to_new_image_index,
        const std::map<int, std::string>& legacy_video_uris, const PathLookup& restored_uri);

    static std::optional<std::string> restore_tool_image_paths(
        const std::optional<std::string>& raw, int archive_version, const PathLookup& restored_path);
};

}  // namespace agora::core::backup