#!/usr/bin/env python3
"""Generates the backup-media-policy scenarios. Deterministic; rerun after editing.

Every branch of NativeBackupMediaPolicy.kt gets at least one case, including the ones that are
easy to get wrong: firstNotNullOfOrNull's short-circuit (visible only through the call log),
sourceFileName's separator handling, and the different `unavailable` rules per attachment type.

The call log is what pins probe ORDER. A port that tries candidates in a different order returns
the same value while asking the archive layer for different things, which is a real bug in a
backup format -- so the scenario tables and the resulting call sequence both matter.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "scenarios")

SCENARIOS_LIST = []


def scenario(name, fn, **kwargs):
    entry = {"name": name, "fn": fn}
    entry.update(kwargs)
    entry["name"] = name
    SCENARIOS_LIST.append(entry)


def attachment(local_id, uri, type_="image", **kwargs):
    entry = {"localId": local_id, "uri": uri, "type": type_}
    entry.update(kwargs)
    return entry


def item(type_, **kwargs):
    entry = {"type": type_}
    entry.update(kwargs)
    return entry


# ---------------------------------------------------------------------------------------------
# exportableDraftAttachments
# ---------------------------------------------------------------------------------------------

scenario(
    "exportable_keeps_app_private_and_drops_both_sandbox_kinds",
    "exportableDraftAttachments",
    attachments=[
        attachment("a1", "file:///one.png"),
        attachment("a2", "content://sandbox/two.png", storage="local_sandbox_pending"),
        attachment("a3", "content://sandbox/three.png", storage="local_sandbox_runtime"),
        attachment("a4", "file:///four.pdf", type_="pdf"),
    ],
)

scenario("exportable_empty_list", "exportableDraftAttachments", attachments=[])

scenario(
    "exportable_drops_everything_when_all_are_sandbox",
    "exportableDraftAttachments",
    attachments=[
        attachment("a1", "content://sandbox/one", storage="local_sandbox_pending"),
        attachment("a2", "content://sandbox/two", storage="local_sandbox_runtime"),
    ],
)

# ---------------------------------------------------------------------------------------------
# rewriteDraftAttachmentsForExport
# ---------------------------------------------------------------------------------------------

scenario(
    "draft_export_keeps_explicit_file_name",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "file:///tmp/abc.bin", fileName="chosen.txt", localPath="/tmp/live.bin")],
    table={"archiveEntryForSource": {"/tmp/live.bin": "media/live.bin"}},
)

scenario(
    "draft_export_derives_file_name_from_local_path",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "file:///tmp/abc.bin", localPath="/tmp/live.bin")],
    table={"archiveEntryForSource": {"/tmp/live.bin": "media/live.bin"}},
)

scenario(
    "draft_export_derives_file_name_from_uri_when_no_local_path",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://media/external/images/report%20final.pdf")],
    table={"archiveEntryForSource": {"content://media/external/images/report%20final.pdf": "media/report.pdf"}},
)

scenario(
    "draft_export_blank_file_name_is_kept_verbatim",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "file:///tmp/abc.bin", fileName="", localPath="/tmp/live.bin")],
    table={"archiveEntryForSource": {"/tmp/live.bin": "media/live.bin"}},
)

scenario(
    "draft_export_file_name_falls_through_when_local_path_names_nothing",
    "rewriteDraftAttachmentsForExport",
    # "/" trims to "" so sourceFileName returns null and the uri becomes the candidate.
    attachments=[attachment("a1", "file:///tmp/report.pdf", type_="pdf", localPath="/")],
    table={"archiveEntryForSource": {"file:///tmp/report.pdf": "media/report.pdf"}},
)

scenario(
    "draft_export_file_name_stays_null_when_no_candidate_names_anything",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://")],
    table={},
)

scenario(
    "draft_export_source_file_name_strips_query_and_fragment",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://media/one", localPath="/cache/pic.png?v=2#frag")],
    table={"archiveEntryForSource": {"/cache/pic.png?v=2#frag": "media/pic.png"}},
)

scenario(
    "draft_export_source_file_name_takes_the_last_separator_of_either_kind",
    "rewriteDraftAttachmentsForExport",
    # substringAfterLast('/') then substringAfterLast('\\') -- the last separator wins regardless
    # of which kind it is, so this names "c", not "b\\c".
    attachments=[attachment("a1", "content://media/one", localPath="a/b\\c")],
    table={"archiveEntryForSource": {"a/b\\c": "media/c.bin"}},
)

scenario(
    "draft_export_source_file_name_rejects_a_non_breaking_space",
    "rewriteDraftAttachmentsForExport",
    # U+00A0 is whitespace to Kotlin's isNotBlank, so a path ending in one names nothing.
    attachments=[attachment("a1", "content://media/one", localPath="/tmp/ ")],
    table={"archiveEntryForSource": {"/tmp/ ": "media/nbsp.bin"}},
)

scenario(
    "draft_export_source_file_name_with_only_backslashes",
    "rewriteDraftAttachmentsForExport",
    # substringAfterLast returns the WHOLE string when the delimiter is absent, so a Windows path
    # with no '/' still names its last segment. A port that returns "" here diverges.
    attachments=[attachment("a1", "content://media/one", localPath="C:\\Users\\test\\report.pdf")],
    table={"archiveEntryForSource": {"C:\\Users\\test\\report.pdf": "media/report.pdf"}},
)

scenario(
    "draft_export_source_file_name_without_any_separator",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://media/one", localPath="report.pdf")],
    table={"archiveEntryForSource": {"report.pdf": "media/report.pdf"}},
)

scenario(
    "draft_export_source_file_name_trims_trailing_separators",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://media/one", localPath="/cache/dir///")],
    table={"archiveEntryForSource": {"/cache/dir///": "media/dir.png"}},
)

scenario(
    "draft_export_short_circuits_on_local_path",
    "rewriteDraftAttachmentsForExport",
    # The uri entry is in the table too; it must never be asked for.
    attachments=[attachment("a1", "file:///other/thing.png", localPath="/tmp/live.png")],
    table={
        "archiveEntryForSource": {
            "/tmp/live.png": "media/live.png",
            "file:///other/thing.png": "media/other.png",
        }
    },
)

scenario(
    "draft_export_probes_blank_local_path_before_the_uri",
    "rewriteDraftAttachmentsForExport",
    # A blank localPath is still a list element, so archiveEntryForSource("") really is called.
    attachments=[attachment("a1", "file:///tmp/thing.png", localPath=" ")],
    table={"archiveEntryForSource": {"file:///tmp/thing.png": "media/thing.png"}},
)

scenario(
    "draft_export_probes_the_uri_when_the_local_path_resolves_to_null",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "file:///tmp/thing.png", localPath="/gone.png")],
    table={"archiveEntryForSource": {"/gone.png": None, "file:///tmp/thing.png": "media/thing.png"}},
)

scenario(
    "draft_export_unavailable_attachment_becomes_a_placeholder",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment("a1", "file:///tmp/thing.png", localPath="/tmp/live.png", fileName="live.png", unavailable=True)
    ],
    table={"archiveEntryForSource": {"/tmp/live.png": "media/live.png"}},
)

scenario(
    "draft_export_missing_archive_entry_becomes_a_placeholder",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "file:///tmp/thing.png", localPath="/gone.png")],
    table={"archiveEntryForSource": {"/gone.png": None}},
)

scenario(
    "draft_export_missing_entry_keeps_the_derived_file_name",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://media", localPath="/gone/named.png")],
    table={"archiveEntryForSource": {"/gone/named.png": None}},
)

scenario(
    "draft_export_clears_sandbox_storage_and_path",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment(
            "a1",
            "file:///tmp/thing.png",
            localPath="/tmp/live.png",
            storage="local_sandbox_pending",
            sandboxPath="/sandbox/runtime/thing.png",
        )
    ],
    table={"archiveEntryForSource": {"/tmp/live.png": "media/live.png"}},
)

scenario(
    "draft_export_maps_frames_and_drops_unresolved_ones",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment(
            "a1",
            "file:///tmp/thing.png",
            localPath="/tmp/live.png",
            processedFrames=["/frames/f0.jpg", "/frames/f1.jpg", "/frames/f2.jpg"],
            preRenderedPaths=["/pre/p0.jpg", "/pre/gone.jpg"],
        )
    ],
    table={
        "archiveEntryForSource": {
            "/tmp/live.png": "media/live.png",
            "/frames/f0.jpg": "media/f0.jpg",
            "/frames/f2.jpg": "media/f2.jpg",
            "/pre/p0.jpg": "media/p0.jpg",
        }
    },
)

scenario(
    "draft_export_nulls_the_frame_list_when_nothing_resolves",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment(
            "a1",
            "file:///tmp/thing.png",
            localPath="/tmp/live.png",
            processedFrames=["/frames/f0.jpg"],
            preRenderedPaths=[],
        )
    ],
    table={"archiveEntryForSource": {"/tmp/live.png": "media/live.png"}},
)

scenario(
    "draft_export_never_probes_a_sandbox_attachment",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment("a1", "content://sandbox/one", localPath="/sandbox/one", storage="local_sandbox_pending"),
        attachment("a2", "file:///tmp/thing.png", localPath="/tmp/live.png"),
    ],
    table={"archiveEntryForSource": {"/tmp/live.png": "media/live.png"}},
)

scenario(
    "draft_export_reports_one_missing_resource_per_unavailable_attachment",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment("a1", "file:///a", localPath="/gone-a.png"),
        attachment("a2", "file:///b", localPath="/gone-b.png", unavailable=True),
        attachment("a3", "file:///c", localPath="/live-c.png"),
    ],
    table={"archiveEntryForSource": {"/live-c.png": "media/c.png"}},
)

scenario(
    "draft_export_preserves_unrelated_attachment_fields",
    "rewriteDraftAttachmentsForExport",
    attachments=[
        attachment(
            "a1",
            "file:///tmp/thing.png",
            type_="video",
            localPath="/tmp/live.mp4",
            frameCount=24,
            sliceIntervalMs=500,
            videoDurationMs=12345,
            mimeType="video/mp4",
            fileSize=98765,
            pixelWidth=1920,
            pixelHeight=1080,
            selectedPages=[3, 1, 2],
            importState="processing",
            preparedText="transcribed",
        )
    ],
    table={"archiveEntryForSource": {"/tmp/live.mp4": "media/live.mp4"}},
)

# ---------------------------------------------------------------------------------------------
# restoreDraftAttachments
# ---------------------------------------------------------------------------------------------

scenario(
    "draft_restore_unavailable_short_circuits_without_probing",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "file:///tmp/x.png", localPath="/archive/x.png", fileName="x.png", unavailable=True)],
    table={"restoredPrimaryForArchiveEntry": {"/archive/x.png": ["/data/x.png", "file:///data/x.png"]}},
)

scenario(
    "draft_restore_prefers_the_local_path",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "file:///other.png", localPath="/archive/x.png")],
    table={
        "restoredPrimaryForArchiveEntry": {
            "/archive/x.png": ["/data/x.png", "file:///data/x.png"],
            "file:///other.png": ["/data/other.png", "file:///data/other.png"],
        }
    },
)

scenario(
    "draft_restore_falls_back_to_the_uri",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "file:///other.png")],
    table={"restoredPrimaryForArchiveEntry": {"file:///other.png": ["/data/other.png", "file:///data/other.png"]}},
)

scenario(
    "draft_restore_ignores_a_blank_local_path",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "file:///other.png", localPath="  ")],
    table={"restoredPrimaryForArchiveEntry": {"file:///other.png": ["/data/other.png", "file:///data/other.png"]}},
)

scenario(
    "draft_restore_missing_primary_becomes_a_placeholder",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "file:///other.png", localPath="/archive/missing.png")],
    table={"restoredPrimaryForArchiveEntry": {"/archive/missing.png": None}},
)

scenario(
    "draft_restore_placeholder_keeps_an_explicit_file_name",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "file:///other.png", fileName="kept.png")],
    table={},
)

scenario(
    "draft_restore_placeholder_derives_a_file_name_from_the_entry",
    "restoreDraftAttachments",
    attachments=[attachment("a1", "content://media", localPath="archive/named.png")],
    table={"restoredPrimaryForArchiveEntry": {"archive/named.png": None}},
)

scenario(
    "draft_restore_maps_frames_and_drops_unresolved_ones",
    "restoreDraftAttachments",
    attachments=[
        attachment(
            "a1",
            "file:///data/x.png",
            localPath="/archive/x.png",
            processedFrames=["/archive/f0.jpg", "/archive/f1.jpg"],
            preRenderedPaths=["/archive/p0.jpg", "/archive/gone.jpg"],
        )
    ],
    table={
        "restoredPrimaryForArchiveEntry": {"/archive/x.png": ["/data/x.png", "file:///data/x.png"]},
        "restoredPathForArchiveEntry": {"/archive/f0.jpg": "/data/f0.jpg", "/archive/p0.jpg": "/data/p0.jpg"},
    },
)

scenario(
    "draft_restore_forces_app_private_storage",
    "restoreDraftAttachments",
    attachments=[
        attachment(
            "a1",
            "file:///data/x.png",
            localPath="/archive/x.png",
            storage="local_sandbox_runtime",
            sandboxPath="/sandbox/x.png",
            importState="failed",
        )
    ],
    table={"restoredPrimaryForArchiveEntry": {"/archive/x.png": ["/data/x.png", "file:///data/x.png"]}},
)

# ---------------------------------------------------------------------------------------------
# toolImagePaths
# ---------------------------------------------------------------------------------------------

scenario("tool_image_paths_null_raw", "toolImagePaths", raw_json=None)
scenario("tool_image_paths_blank_raw", "toolImagePaths", raw="   ")
scenario("tool_image_paths_empty_array", "toolImagePaths", raw_json=[])
scenario("tool_image_paths_malformed_raw", "toolImagePaths", raw='[{"type":"answer",')
scenario("tool_image_paths_not_an_array", "toolImagePaths", raw_json={"type": "answer"})
scenario(
    "tool_image_paths_requires_the_type_field",
    "toolImagePaths",
    raw_json=[{"content": "no type here"}],
)

scenario(
    "tool_image_paths_flattens_every_segment",
    "toolImagePaths",
    raw_json=[
        {"type": "answer", "toolImages": [
            {"path": "/data/a.png", "mimeType": "image/png", "sizeBytes": 10, "sha256": "aa"},
            {"path": "/data/b.png", "mimeType": "image/png", "sizeBytes": 20, "sha256": "bb"},
        ]},
        {"type": "thought"},
        {"type": "tool", "toolImages": [
            {"path": "/data/c.png", "mimeType": "image/png", "sizeBytes": 30, "sha256": "cc"},
        ]},
    ],
)

scenario(
    "tool_image_paths_ignores_unknown_keys",
    "toolImagePaths",
    raw_json=[
        {
            "type": "answer",
            "somethingRemovedInAnUpgrade": [1, 2, 3],
            "toolImages": [
                {"path": "/data/a.png", "mimeType": "image/png", "sizeBytes": 1, "sha256": "aa", "extra": True},
            ],
        }
    ],
)

scenario(
    "tool_image_paths_malformed_image_entry_yields_nothing",
    "toolImagePaths",
    raw_json=[
        {"type": "answer", "toolImages": [
            {"path": "/data/a.png", "mimeType": "image/png", "sizeBytes": 1},
        ]}
    ],
)

# ---------------------------------------------------------------------------------------------
# rewriteAttachmentMetaForExport
# ---------------------------------------------------------------------------------------------

scenario(
    "meta_export_null_raw_falls_back_to_the_legacy_sources",
    "rewriteAttachmentMetaForExport",
    raw_json=None,
    originalImageSources=["/orig/one.png", "/orig/two.png"],
    oldToNewImageIndex={"0": 0, "1": 1},
)

scenario(
    "meta_export_malformed_raw_falls_back_to_the_legacy_sources",
    "rewriteAttachmentMetaForExport",
    raw='{"items":[{"type":"image"',
    originalImageSources=["/orig/one.png"],
    oldToNewImageIndex={"0": 3},
)

scenario(
    "meta_export_wrong_shape_raw_falls_back_to_the_legacy_sources",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": "not a list"},
    originalImageSources=["/orig/one.png"],
    oldToNewImageIndex={"0": 0},
)

scenario(
    "meta_export_empty_items_still_applies_the_legacy_sources",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": []},
    originalImageSources=["/orig/one.png"],
    oldToNewImageIndex={"0": 0},
)

scenario("meta_export_nothing_to_export_returns_null", "rewriteAttachmentMetaForExport", raw_json=None)

scenario(
    "meta_export_sandbox_items_alone_return_null",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [item("image", storage="local_sandbox_runtime")]},
    oldToNewImageIndex={"0": 0},
)

scenario(
    "meta_export_claimed_indices_suppress_the_legacy_items",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=1, page_count=2, originalUri="/orig/two.png"),
    ]},
    originalImageSources=["/orig/one.png", "/orig/two.png", "/orig/three.png"],
    oldToNewImageIndex={"0": 0, "1": 1, "2": 2, "3": 3},
)

scenario(
    "meta_export_items_without_an_index_claim_nothing",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", originalUri="/orig/two.png"),
    ]},
    originalImageSources=["/orig/one.png", "/orig/two.png"],
    oldToNewImageIndex={"0": 0, "1": 1},
)

scenario(
    "meta_export_page_count_below_one_counts_as_one",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, page_count=0, originalUri="/orig/one.png"),
        item("image", image_index=2, page_count=-4, originalUri="/orig/three.png"),
    ]},
    originalImageSources=["/orig/one.png", "/orig/two.png", "/orig/three.png"],
    oldToNewImageIndex={"0": 0, "1": 1, "2": 2},
)

scenario(
    "meta_export_legacy_items_take_type_file_name_and_index",
    "rewriteAttachmentMetaForExport",
    raw_json=None,
    originalImageSources=["/orig/one.png", "content://media/two", "/orig/nothing"],
    oldToNewImageIndex={"0": 5, "1": 6},
)

scenario(
    "meta_export_drops_sandbox_items",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, storage="local_sandbox_pending"),
        item("image", image_index=1, originalUri="/orig/two.png"),
    ]},
    oldToNewImageIndex={"0": 0, "1": 1},
)

scenario(
    "meta_export_video_without_an_archive_entry_is_unavailable",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("video", originalUri="file:///device/movies/clip.mp4"),
    ]},
    oldToNewImageIndex={},
    table={"archiveEntryForSource": {"file:///device/movies/clip.mp4": None}},
)

scenario(
    "meta_export_video_keeps_the_archived_original_uri",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("video", originalUri="file:///device/movies/clip.mp4", fileName="clip.mp4"),
    ]},
    oldToNewImageIndex={},
    table={"archiveEntryForSource": {"file:///device/movies/clip.mp4": "media/clip.mp4"}},
)

scenario(
    "meta_export_unavailable_video_is_never_probed",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("video", originalUri="file:///device/movies/clip.mp4", unavailable=True),
    ]},
    oldToNewImageIndex={},
    table={"archiveEntryForSource": {"file:///device/movies/clip.mp4": "media/clip.mp4"}},
)

scenario(
    "meta_export_file_without_text_content_is_unavailable",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("file", originalUri="/orig/doc.txt"),
        item("file", originalUri="/orig/doc2.txt", text_content="hello"),
    ]},
    oldToNewImageIndex={},
)

scenario(
    "meta_export_image_without_surviving_indices_is_unavailable",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, page_count=3),
    ]},
    oldToNewImageIndex={"1": 7},
)

scenario(
    "meta_export_pdf_follows_the_image_rule",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("pdf", image_index=0),
        item("pdf", image_index=1),
    ]},
    oldToNewImageIndex={"1": 1},
)

scenario(
    "meta_export_an_unknown_type_stays_available",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("audio", originalUri="/orig/track.mp3"),
    ]},
    oldToNewImageIndex={},
)

scenario(
    "meta_export_an_image_drops_its_original_uri_but_keeps_the_file_name",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, originalUri="/device/DCIM/photo.jpg"),
    ]},
    oldToNewImageIndex={"0": 4},
)

scenario(
    "meta_export_an_image_without_a_file_name_derives_one_from_the_original_uri",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, originalUri="/device/DCIM/photo.jpg"),
    ]},
    oldToNewImageIndex={"0": 0},
)

scenario(
    "meta_export_page_count_shrinks_to_the_surviving_indices",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, page_count=4),
    ]},
    oldToNewImageIndex={"0": 10, "2": 12},
)

scenario(
    "meta_export_an_image_index_without_a_page_count_keeps_no_page_count",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0),
    ]},
    oldToNewImageIndex={"0": 3},
)

scenario(
    "meta_export_a_file_with_text_content_stays_available",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("file", originalUri="/orig/doc.txt", text_content="body"),
    ]},
    oldToNewImageIndex={},
    table={"archiveEntryForSource": {}},
)

scenario(
    "meta_export_an_unavailable_item_loses_its_text_content",
    "rewriteAttachmentMetaForExport",
    # textContent survives only because unavailable is false; flip the flag and it is cleared.
    raw_json={"items": [
        item("file", originalUri="/orig/doc.txt", text_content="body", unavailable=True),
    ]},
    oldToNewImageIndex={},
    table={"archiveEntryForSource": {}},
)

scenario(
    "meta_export_counts_one_missing_resource_per_unavailable_item",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item("image", image_index=0, page_count=2),
        item("video", originalUri="/device/movies/clip.mp4"),
        item("file", text_content="body"),
    ]},
    oldToNewImageIndex={"0": 0},
    table={"archiveEntryForSource": {"/device/movies/clip.mp4": None}},
)

scenario(
    "meta_export_preserves_the_untouched_fields_of_an_available_item",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [
        item(
            "image",
            image_index=0,
            pixel_width=1920,
            pixel_height=1080,
            mime_type="image/jpeg",
            file_size=4242,
            transcription="a cat",
            warning="downscaled",
        ),
    ]},
    oldToNewImageIndex={"0": 0},
)

# ---------------------------------------------------------------------------------------------
# rewriteToolImagePathsForExport
# ---------------------------------------------------------------------------------------------

scenario("tool_images_export_null_raw", "rewriteToolImagePathsForExport", raw_json=None)
scenario("tool_images_export_blank_raw_is_returned_as_is", "rewriteToolImagePathsForExport", raw="  \t ")
scenario("tool_images_export_malformed_raw_is_returned_as_is", "rewriteToolImagePathsForExport", raw="[{\"type\":")
scenario(
    "tool_images_export_drops_unresolved_images",
    "rewriteToolImagePathsForExport",
    raw_json=[
        {"type": "answer", "toolImages": [
            {"path": "/data/a.png", "mimeType": "image/png", "sizeBytes": 1, "sha256": "aa"},
            {"path": "/data/gone.png", "mimeType": "image/png", "sizeBytes": 2, "sha256": "bb"},
        ]},
    ],
    table={"archiveEntryForSource": {"/data/a.png": "media/a.png"}},
)

scenario(
    "tool_images_export_round_trips_a_fully_populated_segment",
    "rewriteToolImagePathsForExport",
    raw_json=[
        {
            "type": "tool",
            "content": "looked at a file",
            "toolName": "view_image",
            "toolArgs": "{\"path\":\"/data/a.png\"}",
            "toolResult": "ok",
            "toolCallId": "call-1",
            "signature": "sig",
            "signatureProvider": "openai",
            "durationMs": 1234,
            "toolState": "succeeded",
            "toolProgress": "50%",
            "toolTarget": "/data/a.png",
            "toolDisplayName": "View image",
            "toolResultText": "a picture",
            "toolStructuredResult": "{\"n\":1}",
            "toolImages": [
                {"path": "/data/a.png", "mimeType": "image/png", "sizeBytes": 1,
                 "width": 10, "height": 20, "sha256": "aa"},
            ],
            "toolTranscription": "a cat on a mat",
            "responseOutputItems": [{"type": "message", "id": "m1"}],
            "responseOutputItemProvider": "anthropic",
            "errorCode": "rate_limited",
        },
    ],
    table={"archiveEntryForSource": {"/data/a.png": "media/a.png"}},
)

scenario(
    "tool_images_export_keeps_segments_without_images",
    "rewriteToolImagePathsForExport",
    raw_json=[{"type": "answer", "content": "plain"}, {"type": "tool", "toolImages": []}],
)

scenario(
    "tool_images_export_escapes_control_characters_and_keeps_unicode",
    "rewriteToolImagePathsForExport",
    # Pins kotlinx's encoder exactly: the short escapes for the five specials, \uXXXX for the rest
    # of the C0 range, quotes and backslashes doubled, and non-ASCII written through as UTF-8.
    raw_json=[
        {
            "type": "answer",
            "content": "tab\there\nnewline\rreturn\bback\fformfeed bell escape-done \"quoted\" \\slash\\ "
                       "€100 — emoji \U0001f600 and a lone \u00a0nbsp",
            "toolImages": [
                {"path": "/data/a.png", "mimeType": "image/png", "sizeBytes": 1, "sha256": "aa"},
            ],
        },
    ],
    table={"archiveEntryForSource": {"/data/a.png": "media/a.png"}},
)

scenario(
    "draft_export_name_with_separators_stops_at_the_last_backslash",
    "rewriteDraftAttachmentsForExport",
    # The trace escapes | and \ itself, so this exercises both layers -- and pins that
    # sourceFileName takes the last separator of EITHER kind, so the name is "d.png", not
    # "a|b;c\d.png".
    attachments=[attachment("a1", "content://media/a|b;c\\d.png")],
    table={"archiveEntryForSource": {"content://media/a|b;c\\d.png": "media/a-b-c-d.png"}},
)

scenario(
    "draft_export_file_name_containing_trace_separators",
    "rewriteDraftAttachmentsForExport",
    attachments=[attachment("a1", "content://media/a|b;c.png")],
    table={"archiveEntryForSource": {"content://media/a|b;c.png": "media/archived.png"}},
)

scenario(
    "meta_export_a_video_with_an_unmapped_index_loses_the_index",
    "rewriteAttachmentMetaForExport",
    # Availability for a video does not depend on the media index, so survivingIndices can be
    # empty here; firstOrNull must then yield no index rather than blowing up.
    raw_json={"items": [item("video", image_index=5, originalUri="/device/movies/clip.mp4")]},
    oldToNewImageIndex={},
    table={"archiveEntryForSource": {"/device/movies/clip.mp4": "media/clip.mp4"}},
)

scenario(
    "meta_export_null_items_fails_the_decode",
    "rewriteAttachmentMetaForExport",
    # items is a non-nullable List: an explicit null throws, so the legacy sources take over.
    raw_json={"items": None},
    originalImageSources=["/orig/one.png"],
    oldToNewImageIndex={"0": 2},
)

scenario(
    "meta_export_a_null_storage_fails_the_decode",
    "rewriteAttachmentMetaForExport",
    raw_json={"items": [item("image", storage=None, image_index=0)]},
    originalImageSources=["/orig/one.png"],
    oldToNewImageIndex={"0": 0, "1": 1},
)

scenario(
    "draft_export_an_empty_selected_page_set_is_encoded_as_an_array",
    "rewriteDraftAttachmentsForExport",
    # The declared default is null, so an explicitly empty set is still written out.
    attachments=[attachment("a1", "file:///tmp/thing.png", localPath="/tmp/live.png", selectedPages=[])],
    table={"archiveEntryForSource": {"/tmp/live.png": "media/live.png"}},
)

scenario(
    "tool_image_paths_null_content_fails_the_decode",
    "toolImagePaths",
    raw_json=[{"type": "answer", "content": None}],
)

scenario(
    "tool_image_paths_null_tool_images_fails_the_decode",
    "toolImagePaths",
    raw_json=[{"type": "answer", "toolImages": None}],
)

scenario(
    "tool_image_paths_null_response_output_items_fails_the_decode",
    "toolImagePaths",
    raw_json=[{"type": "answer", "responseOutputItems": None}],
)

scenario(
    "tool_image_paths_an_empty_content_string_round_trips",
    "rewriteToolImagePathsForExport",
    # "" equals the declared default, so kotlinx omits the key entirely.
    raw_json=[{"type": "answer", "content": ""}],
)

scenario(
    "tool_image_paths_an_empty_response_output_list_is_omitted",
    "rewriteToolImagePathsForExport",
    raw_json=[{"type": "answer", "responseOutputItems": []}],
)

# ---------------------------------------------------------------------------------------------
# restoreAttachmentMeta
# ---------------------------------------------------------------------------------------------

scenario("meta_restore_null_raw", "restoreAttachmentMeta", raw_json=None, legacyVideoUris={})
scenario(
    "meta_restore_blank_raw_is_returned_as_is",
    "restoreAttachmentMeta",
    raw="",
    legacyVideoUris={},
)
scenario(
    "meta_restore_malformed_raw_returns_null",
    "restoreAttachmentMeta",
    raw="not json at all",
    legacyVideoUris={},
)
scenario(
    "meta_restore_empty_items_stays_empty",
    "restoreAttachmentMeta",
    raw_json={"items": []},
    legacyVideoUris={},
)

scenario(
    "meta_restore_v4_resolves_the_original_uri",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("video", originalUri="media/clip.mp4", image_index=0)]},
    oldToNewImageIndex={"0": 0},
    legacyVideoUris={"0": "/device/legacy.mp4"},
    table={"restoredUriForArchiveEntry": {"media/clip.mp4": "file:///data/clip.mp4"}},
)

scenario(
    "meta_restore_below_v4_uses_the_legacy_video_uri",
    "restoreAttachmentMeta",
    archiveVersion=3,
    raw_json={"items": [item("video", originalUri="media/clip.mp4", image_index=2)]},
    oldToNewImageIndex={"2": 1},
    legacyVideoUris={"2": "/device/legacy-clip.mp4"},
)

scenario(
    "meta_restore_below_v4_without_an_index_falls_back_to_zero",
    "restoreAttachmentMeta",
    archiveVersion=1,
    raw_json={"items": [item("video", originalUri="media/clip.mp4")]},
    legacyVideoUris={"0": "/device/first.mp4", "1": "/device/second.mp4"},
)

scenario(
    "meta_restore_below_v4_drops_the_original_uri_of_a_non_video",
    "restoreAttachmentMeta",
    archiveVersion=3,
    raw_json={"items": [item("image", originalUri="media/photo.jpg", image_index=0)]},
    oldToNewImageIndex={"0": 0},
    legacyVideoUris={},
    table={"restoredUriForArchiveEntry": {"media/photo.jpg": "file:///data/photo.jpg"}},
)

scenario(
    "meta_restore_v4_ignores_the_legacy_video_uris",
    "restoreAttachmentMeta",
    archiveVersion=5,
    raw_json={"items": [item("video", originalUri="media/clip.mp4", image_index=1)]},
    oldToNewImageIndex={"1": 1},
    legacyVideoUris={"1": "/device/legacy.mp4"},
    table={"restoredUriForArchiveEntry": {"media/clip.mp4": None}},
)

scenario(
    "meta_restore_unavailable_item_is_never_probed",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("video", originalUri="media/clip.mp4", unavailable=True)]},
    legacyVideoUris={"0": "/device/legacy.mp4"},
    table={"restoredUriForArchiveEntry": {"media/clip.mp4": "file:///data/clip.mp4"}},
)

scenario(
    "meta_restore_forces_app_private_storage_and_clears_the_sandbox_path",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [
        item("image", image_index=0, storage="local_sandbox_runtime", sandbox_path="/sandbox/a.png"),
    ]},
    oldToNewImageIndex={"0": 0},
    legacyVideoUris={},
)

scenario(
    "meta_restore_file_without_text_content_is_unavailable",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("file", originalUri="media/doc.txt")]},
    legacyVideoUris={},
)

scenario(
    "meta_restore_image_without_surviving_indices_is_unavailable",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("image", image_index=0, page_count=2, originalUri="media/photo.jpg")]},
    oldToNewImageIndex={"1": 1},
    legacyVideoUris={},
)

scenario(
    "meta_restore_an_unknown_type_stays_available_and_keeps_its_file_name",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("audio", originalUri="media/track.mp3")]},
    legacyVideoUris={},
    table={"restoredUriForArchiveEntry": {}},
)

scenario(
    "meta_restore_an_image_never_regains_its_file_name_from_the_original_uri",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("image", image_index=0, originalUri="media/photo.jpg")]},
    oldToNewImageIndex={"0": 0},
    legacyVideoUris={},
    table={"restoredUriForArchiveEntry": {"media/photo.jpg": "file:///data/photo.jpg"}},
)

scenario(
    "meta_restore_page_count_shrinks_to_the_surviving_indices",
    "restoreAttachmentMeta",
    archiveVersion=4,
    raw_json={"items": [item("image", image_index=0, page_count=3)]},
    oldToNewImageIndex={"0": 0, "2": 2},
    legacyVideoUris={},
)

# ---------------------------------------------------------------------------------------------
# restoreToolImagePaths
# ---------------------------------------------------------------------------------------------

scenario("tool_images_restore_null_raw", "restoreToolImagePaths", raw_json=None, archiveVersion=4)
scenario(
    "tool_images_restore_blank_raw_is_returned_as_is",
    "restoreToolImagePaths",
    raw=" \t",
    archiveVersion=4,
)
scenario(
    "tool_images_restore_malformed_raw_is_returned_as_is",
    "restoreToolImagePaths",
    raw="[{\"type\": \"answer\"",
    archiveVersion=4,
)
scenario(
    "tool_images_restore_below_v4_drops_every_image_without_probing",
    "restoreToolImagePaths",
    archiveVersion=3,
    raw_json=[{"type": "answer", "toolImages": [
        {"path": "media/a.png", "mimeType": "image/png", "sizeBytes": 1, "sha256": "aa"},
    ]}],
    table={"restoredPathForArchiveEntry": {"media/a.png": "/data/a.png"}},
)
scenario(
    "tool_images_restore_v4_resolves_the_paths",
    "restoreToolImagePaths",
    archiveVersion=4,
    raw_json=[{"type": "answer", "toolImages": [
        {"path": "media/a.png", "mimeType": "image/png", "sizeBytes": 1, "sha256": "aa"},
        {"path": "media/gone.png", "mimeType": "image/png", "sizeBytes": 2, "sha256": "bb"},
    ]}],
    table={"restoredPathForArchiveEntry": {"media/a.png": "/data/a.png"}},
)
scenario(
    "tool_images_restore_round_trips_a_fully_populated_segment",
    "restoreToolImagePaths",
    archiveVersion=4,
    raw_json=[
        {
            "type": "tool",
            "content": "looked at a file",
            "toolName": "view_image",
            "toolArgs": "{\"path\":\"/data/a.png\"}",
            "toolResult": "ok",
            "toolCallId": "call-1",
            "signature": "sig",
            "signatureProvider": "openai",
            "durationMs": 1234,
            "toolState": "succeeded",
            "toolProgress": "50%",
            "toolTarget": "/data/a.png",
            "toolDisplayName": "View image",
            "toolResultText": "a picture",
            "toolStructuredResult": "{\"n\":1}",
            "toolImages": [
                {"path": "media/a.png", "mimeType": "image/png", "sizeBytes": 1,
                 "width": 10, "height": 20, "sha256": "aa"},
            ],
            "toolTranscription": "a cat on a mat",
            "responseOutputItems": [{"type": "message", "id": "m1"}],
            "responseOutputItemProvider": "anthropic",
            "errorCode": "rate_limited",
        },
    ],
    table={"restoredPathForArchiveEntry": {"media/a.png": "/data/a.png"}},
)
scenario(
    "tool_images_restore_keeps_segments_without_images",
    "restoreToolImagePaths",
    archiveVersion=4,
    raw_json=[{"type": "answer", "content": "plain"}, {"type": "tool", "toolImages": []}],
)


def main():
    os.makedirs(SCENARIOS, exist_ok=True)
    for stale in sorted(os.listdir(SCENARIOS)):
        if stale.endswith(".json"):
            os.remove(os.path.join(SCENARIOS, stale))

    seen = set()
    for entry in SCENARIOS_LIST:
        name = entry["name"]
        if name in seen:
            raise SystemExit(f"duplicate scenario name: {name}")
        seen.add(name)
        path = os.path.join(SCENARIOS, f"{name}.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(entry, handle, indent=2, ensure_ascii=False, sort_keys=True)
            handle.write("\n")
    print(f"wrote {len(SCENARIOS_LIST)} scenarios to {SCENARIOS}")


if __name__ == "__main__":
    main()