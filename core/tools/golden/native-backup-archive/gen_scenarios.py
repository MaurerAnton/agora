#!/usr/bin/env python3
"""Generates the archives (fixtures/) and scenarios for the NativeBackupArchive golden stand.

Deterministic: rerunning rewrites fixtures/ and scenarios/ identically. The archives are
committed, so the Kotlin driver and the C++ test read the exact same bytes. Freeze the golden
traces afterwards with run_golden.sh --freeze.

Fixture builders:
  * zipfile with fixed timestamps – the normal archives the functional scenarios open
  * RawZip – byte-exact local/central headers, for declared sizes and CRCs that disagree with
    the payload, directory entries with data, unsafe names, and sizes near Long.MAX/2
"""
import json
import os
import shutil
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")
SCENARIOS = os.path.join(HERE, "scenarios")

B = 1 << 62  # ~Long.MAX/2: two of these overflow Math.addExact in checkedAdd

ALL_SCENARIOS = []


def scenario(name, archive, ops, **kw):
    item = {"name": name, "archive": archive, "ops": ops}
    item.update(kw)
    ALL_SCENARIOS.append(item)


# --------------------------------------------------------------------------------- fixtures
def zip_builder(path, entries):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data, method in entries:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = method
            info.external_attr = 0o600 << 16
            archive.writestr(
                info, data,
                compresslevel=(6 if method == zipfile.ZIP_DEFLATED else None),
            )


VALID_ENTRIES = [
    ("manifest.json", b'{"agora_export_version":6,"categories":["conversations"]}',
     zipfile.ZIP_DEFLATED),
    ("settings.json", b'{"selected_model":"m1"}', zipfile.ZIP_DEFLATED),
    ("system_prompts.json", b'[{"id":"p1","title":"T"}]', zipfile.ZIP_DEFLATED),
    ("api_keys.json", b'{"apiKeys":[]}', zipfile.ZIP_STORED),
    ("conversations.json", b'[{"id":"c1"}]', zipfile.ZIP_DEFLATED),
    ("conv/tasks.json", b'{"records":[]}', zipfile.ZIP_DEFLATED),
    ("conv/items/aaaaaaaaaaaaaaaa.json", b'{"conversation":{}}', zipfile.ZIP_DEFLATED),
    ("media/images/11111111-2222-3333-4444-555555555555.png", b"PNGDATA-ONE",
     zipfile.ZIP_DEFLATED),
    ("media/drafts/99999999-8888-7777-6666-555555555555.jpg", b"DRAFT-IMAGE",
     zipfile.ZIP_STORED),
    ("custom_font/font", b"FONTDATA", zipfile.ZIP_DEFLATED),
    ("images/legacy.png", b"LEGACY-IMAGE", zipfile.ZIP_DEFLATED),
    ("videos/legacy.mp4", b"LEGACY-VIDEO", zipfile.ZIP_STORED),
    ("custom_font/legacy.ttf", b"LEGACY-FONT", zipfile.ZIP_STORED),
    ("memories/memory_db/note.md", b"# note", zipfile.ZIP_DEFLATED),
    ("theme.bin", b"\x00\x01\x02\x7fMETA", zipfile.ZIP_STORED),
    ("dir/", b"", zipfile.ZIP_STORED),
]


class RawZip:
    """Assembles a STORED ZIP byte-for-byte: local headers + data + central directory + EOCD.

    A size_override above 0xFFFFFFFF is written as a central-directory ZIP64 extra (only the
    central value matters for the validation scenarios: those entries are never inflated).
    """

    def __init__(self):
        self.entries = []  # (name, data, size_override, crc_override, is_dir)

    def add(self, name, data=b"", size_override=None, crc_override=None, is_dir=False):
        raw_name = name.encode("utf-8") if isinstance(name, str) else name
        self.entries.append((raw_name, data, size_override, crc_override, is_dir))

    @staticmethod
    def _extra_for(size):
        if size is not None and size > 0xFFFFFFFF:
            return struct.pack("<HHQ", 0x0001, 8, size)
        return b""

    @staticmethod
    def _size_field(size):
        if size is None:
            return None
        return 0xFFFFFFFF if size > 0xFFFFFFFF else size

    def build(self):
        out = bytearray()
        central = bytearray()
        for raw_name, data, size_override, crc_override, is_dir in self.entries:
            offset = len(out)
            crc = zlib.crc32(data) & 0xFFFFFFFF if crc_override is None else crc_override
            size = len(data) if size_override is None else size_override
            size_field = self._size_field(size_override) if size_override is not None else len(data)
            out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0,
                               crc, len(data), len(data), len(raw_name), 0)
            out += raw_name
            out += data
            extra = self._extra_for(size)
            central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0,
                                   crc, len(data), size_field, len(raw_name),
                                   len(extra), 0, 0, 0,
                                   (0o40755 if is_dir else 0o600) << 16, offset)
            central += raw_name
            central += extra
        cd_offset = len(out)
        out += central
        out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(self.entries), len(self.entries),
                           len(central), cd_offset, 0)
        return bytes(out)


def write_raw(name, builder):
    with open(os.path.join(FIXTURES, name), "wb") as handle:
        handle.write(builder.build())


def raw_deflate_archive(entry_name, manifest_payload, broken_block):
    """A single DEFLATED entry; when broken_block the stored block is truncated mid-payload."""
    raw_name = entry_name.encode("utf-8")
    crc = zlib.crc32(manifest_payload) & 0xFFFFFFFF
    if broken_block:
        comp = b"\x01" + struct.pack("<HH", len(manifest_payload), 0xFFFF) + manifest_payload[:2]
    else:
        compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
        comp = compressor.compress(manifest_payload) + compressor.flush()
    out = bytearray()
    out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 8, 0, 0, crc, len(comp),
                       len(manifest_payload), len(raw_name), 0)
    out += raw_name + comp
    cd = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 8, 0, 0, crc, len(comp),
                     len(manifest_payload), len(raw_name), 0, 0, 0, 0, 0o600 << 16, 0)
    cd += raw_name
    cd_offset = len(out)
    out += cd
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1, len(cd), cd_offset, 0)
    return bytes(out)


def make_fixtures():
    shutil.rmtree(FIXTURES, ignore_errors=True)
    os.makedirs(FIXTURES)

    zip_builder(os.path.join(FIXTURES, "valid-v6.zip"), VALID_ENTRIES)

    bad_crc = RawZip()
    bad_crc.add("conv/tasks.json", b'{"records":[]}')
    bad_crc.add("conv/items/bbbbbbbbbbbbbbbb.json", b'{"conversation":{}}',
                crc_override=0xDEADBEEF)
    write_raw("crc-mismatch-stream.zip", bad_crc)

    bad_meta = RawZip()
    bad_meta.add("manifest.json", b'{"agora_export_version":6}', crc_override=0x0BADF00D)
    write_raw("crc-mismatch-metadata.zip", bad_meta)

    dup = RawZip()
    dup.add("manifest.json", b"one")
    dup.add("manifest.json", b"two")
    write_raw("duplicate.zip", dup)

    parent_file = RawZip()
    parent_file.add("a", b"a-file")
    parent_file.add("a/b.txt", b"child")
    write_raw("parent-as-file.zip", parent_file)

    backslash = RawZip()
    backslash.add("media\\images\\x.png", b"img")
    write_raw("backslash.zip", backslash)

    drive = RawZip()
    drive.add("C:/x.png", b"img")
    write_raw("drive-letter.zip", drive)

    absolute = RawZip()
    absolute.add("/abs.txt", b"abs")
    write_raw("absolute.zip", absolute)

    empty_segment = RawZip()
    empty_segment.add("a//b.txt", b"x")
    write_raw("empty-segment.zip", empty_segment)

    dot_segment = RawZip()
    dot_segment.add("a/./b.txt", b"x")
    write_raw("dot-segment.zip", dot_segment)

    parent_segment = RawZip()
    parent_segment.add("a/../b.txt", b"x")
    write_raw("parent-segment.zip", parent_segment)

    empty_name = RawZip()
    empty_name.add("", b"x")
    write_raw("empty-name.zip", empty_name)

    dir_data = RawZip()
    dir_data.add("d/", b"data", is_dir=True)
    write_raw("dir-with-data.zip", dir_data)

    huge = RawZip()
    huge.add("manifest.json", b"m", size_override=B)
    huge.add("settings.json", b"s", size_override=B)
    write_raw("huge-metadata.zip", huge)

    over_budget = RawZip()
    over_budget.add("manifest.json", b"m", size_override=0x7FFFFFFF)
    write_raw("over-budget.zip", over_budget)

    unknown_size = RawZip()
    unknown_size.add("manifest.json", b"{}", size_override=0xFFFFFFFF)
    write_raw("unknown-size.zip", unknown_size)

    with open(os.path.join(FIXTURES, "bad-deflate.zip"), "wb") as handle:
        handle.write(raw_deflate_archive("manifest.json", b'{"agora_export_version":6}',
                                         broken_block=True))

    with open(os.path.join(FIXTURES, "bad-deflate-stream.zip"), "wb") as handle:
        handle.write(raw_deflate_archive("conv/tasks.json", b'{"records":[]}',
                                         broken_block=True))

    size_mismatch = RawZip()
    size_mismatch.add("conv/tasks.json", b'{"records":[]}', size_override=99)
    write_raw("size-mismatch.zip", size_mismatch)

    # A metadata entry whose declared size passes the budget but whose payload is larger.
    big_payload = RawZip()
    big_payload.add("manifest.json", b"x" * 50, size_override=10)
    write_raw("big-payload.zip", big_payload)

    # Streamed payloads (never consumed at open) whose sizes overflow the preflight spool sum.
    huge_streamed = RawZip()
    huge_streamed.add("conv/tasks.json", b"{}", size_override=B)
    huge_streamed.add("conv/items/aaaaaaaaaaaaaaaa.json", b"{}", size_override=B)
    write_raw("huge-streamed.zip", huge_streamed)

    # Conversation resources whose sizes overflow the selected-resource fold.
    huge_resources = RawZip()
    huge_resources.add("media/images/one.png", b"x", size_override=B)
    huge_resources.add("media/images/two.png", b"y", size_override=B)
    write_raw("huge-resources.zip", huge_resources)

    with open(os.path.join(FIXTURES, "garbage.bin"), "wb") as handle:
        handle.write(b"this is not a zip archive at all, definitely not" * 3)

    valid = open(os.path.join(FIXTURES, "valid-v6.zip"), "rb").read()
    with open(os.path.join(FIXTURES, "truncated.zip"), "wb") as handle:
        handle.write(valid[:len(valid) // 3])


# --------------------------------------------------------------------------------- scenarios
def open_op(mode, **kw):
    op = {"op": "open", "mode": mode}
    op.update(kw)
    return op


HUGE = 1 << 50

V6 = "valid-v6.zip"

# 1. functional probe matrix over the normal archive (file mode owns the copy: close deletes it)
scenario("01-open-file-probes", V6, [
    open_op("file"),
    {"op": "names"},
    {"op": "size", "name": "manifest.json"},
    {"op": "size", "name": "nope.json"},
    {"op": "has", "name": "conv/tasks.json"},
    {"op": "has", "name": "nope.json"},
    {"op": "bytes", "name": "manifest.json"},
    {"op": "bytes", "name": "media/images/11111111-2222-3333-4444-555555555555.png"},
    {"op": "bytes", "name": "conversations.json"},
    {"op": "bytes", "name": "custom_font/font"},
    {"op": "bytes", "name": "theme.bin"},
    {"op": "bytes", "name": "missing.json"},
    {"op": "prefix", "name": "settings.json", "count": 5},
    {"op": "prefix", "name": "settings.json", "count": 500},
    {"op": "prefix", "name": "conv/tasks.json", "count": 6},
    {"op": "prefix", "name": "theme.bin", "count": 5},
    {"op": "prefix", "name": "media/images/11111111-2222-3333-4444-555555555555.png",
     "count": 3},
    {"op": "prefix", "name": "missing.json", "count": 3},
    {"op": "prefix", "name": "settings.json", "count": -1},
    {"op": "temp_exists"},
    {"op": "close"},
    {"op": "temp_exists"},
])

# 2. uri-backed open (Android path): descriptor stream, source closeable, no owned file.
scenario("02-open-uri-probes", V6, [
    open_op("uri"),
    {"op": "names"},
    {"op": "bytes", "name": "custom_font/font"},
    {"op": "stream_open", "key": "res", "name": "media/images/11111111-2222-3333-4444-555555555555.png"},
    {"op": "stream_open", "key": "s", "name": "conv/tasks.json"},
    {"op": "stream_read", "key": "s", "count": 7},
    {"op": "stream_close", "key": "s"},
    {"op": "close"},
    {"op": "temp_exists"},
])

# 3. seekable channel open
scenario("03-open-channel-probes", V6, [
    open_op("channel"),
    {"op": "has", "name": "manifest.json"},
    {"op": "size", "name": "manifest.json"},
    {"op": "close"},
    {"op": "temp_exists"},
])

# 4. non-seekable channel
scenario("04-open-nonseekable", V6, [
    open_op("nonseek"),
    {"op": "names"},
    {"op": "close"},
])

# 5. uri with no descriptor for it
scenario("05-open-uri-null", V6, [
    open_op("uri_null"),
    {"op": "names"},
    {"op": "temp_exists"},
])

# 6. missing backing file: FileNotFoundException rethrown as-is (path masked in the trace)
scenario("06-open-missing-file", "missing.zip", [
    open_op("file"),
    {"op": "names"},
    {"op": "temp_exists"},
])

# 7/8. metadata limit wiring
scenario("07-open-metadata-limit", V6, [
    open_op("file", metadataLimit=5),
    {"op": "names"},
    {"op": "temp_exists"},
])
scenario("08-open-negative-metadata-limit", V6, [
    open_op("file", metadataLimit=-1),
    {"op": "names"},
    {"op": "temp_exists"},
])

# 9..24 open validation failures (Kotlin's own messages; commons messages are observed too)
for name, archive, _ in [
    ("09-open-crc-mismatch-metadata", "crc-mismatch-metadata.zip", None),
    ("10-open-huge-metadata-overflow", "huge-metadata.zip", None),
    ("11-open-over-metadata-budget", "over-budget.zip", None),
    ("12-open-unknown-size", "unknown-size.zip", None),
    ("13-open-bad-deflate", "bad-deflate.zip", None),
    ("14-open-duplicate", "duplicate.zip", None),
    ("15-open-parent-as-file", "parent-as-file.zip", None),
    ("16-open-backslash", "backslash.zip", None),
    ("17-open-drive-letter", "drive-letter.zip", None),
    ("18-open-absolute", "absolute.zip", None),
    ("19-open-empty-segment", "empty-segment.zip", None),
    ("20-open-dot-segment", "dot-segment.zip", None),
    ("21-open-parent-segment", "parent-segment.zip", None),
    ("22-open-empty-name", "empty-name.zip", None),
    ("23-open-dir-with-data", "dir-with-data.zip", None),
    ("24-open-truncated", "truncated.zip", None),
]:
    scenario(name, archive, [open_op("file"), {"op": "names"}, {"op": "temp_exists"}])

# 25. streamed CRC mismatch: verified only when the stream is read to its end
scenario("25-stream-crc-mismatch", "crc-mismatch-stream.zip", [
    open_op("file"),
    {"op": "stream_open", "key": "good", "name": "conv/tasks.json"},
    {"op": "stream_read", "key": "good", "count": 6},
    {"op": "stream_read_to_end", "key": "good"},
    {"op": "stream_close", "key": "good"},
    {"op": "stream_open", "key": "bad", "name": "conv/items/bbbbbbbbbbbbbbbb.json"},
    {"op": "stream_read", "key": "bad", "count": 5},
    {"op": "stream_read_to_end", "key": "bad"},
    {"op": "stream_open", "key": "early", "name": "conv/items/bbbbbbbbbbbbbbbb.json"},
    {"op": "stream_read", "key": "early", "count": 2},
    {"op": "stream_close", "key": "early"},
    {"op": "stream_open", "key": "skip", "name": "conv/tasks.json"},
    {"op": "stream_skip", "key": "skip", "count": 5},
    {"op": "stream_read", "key": "skip", "count": 100},
    {"op": "stream_close", "key": "skip"},
])

# 26. declared size vs payload
scenario("26-stream-size-mismatch", "size-mismatch.zip", [
    open_op("file"),
    {"op": "stream_open", "key": "s", "name": "conv/tasks.json"},
    {"op": "stream_read", "key": "s", "count": 4},
    {"op": "stream_read_to_end", "key": "s"},
])

# 27. copyTo matrix
scenario("27-copy-matrix", V6, [
    open_op("file"),
    {"op": "copy_to", "name": "custom_font/font", "target": "font.bin",
     "maxBytes": HUGE, "available": HUGE},
    {"op": "copy_to", "name": "media/images/11111111-2222-3333-4444-555555555555.png",
     "target": "img.png", "maxBytes": HUGE, "available": HUGE},
    {"op": "copy_to", "name": "conv/tasks.json", "target": "tasks.json",
     "maxBytes": HUGE, "available": HUGE},
    {"op": "copy_to", "name": "missing.bin", "target": "nope.bin",
     "maxBytes": HUGE, "available": HUGE},
    {"op": "copy_to", "name": "settings.json", "target": "small.json", "maxBytes": 4,
     "available": HUGE},
    {"op": "copy_to", "name": "settings.json", "target": "poor.json", "maxBytes": HUGE,
     "available": 3},
    {"op": "temp_exists"},
    {"op": "close"},
])

# 28. copyTo destination that cannot be created, and copyStreamToFile's buffer require
scenario("28-copy-destination-and-buffer", V6, [
    open_op("file"),
    {"op": "copy_to", "name": "settings.json", "target": "blocker/out.json",
     "maxBytes": HUGE, "available": HUGE},
    {"op": "copy_stream_raw", "target": "buffer.json", "input": "abc",
     "bufferBytes": 0, "sourceName": "buffer.json"},
    {"op": "copy_stream_raw", "target": "direct.bin", "input": "payload",
     "declaredSize": 99, "expectedCrc": 0, "sourceName": "direct.bin"},
    {"op": "copy_stream_raw", "target": "crc.bin", "input": "payload",
     "expectedCrc": 12345, "sourceName": "crc.bin"},
    {"op": "copy_stream_raw", "target": "more.bin", "input": "payload",
     "declaredSize": 2, "sourceName": "more.bin"},
    {"op": "close"},
], setupFiles=["blocker"])

# 29/30. preflight arithmetic
scenario("29-preflight-matrix", V6, [
    open_op("file"),
    {"op": "preflight", "conversations": True, "settings": True, "version": 6,
     "dest": "import_dest", "fontLimit": 1000, "available": HUGE},
    {"op": "preflight", "conversations": False, "settings": False, "version": 6,
     "dest": "import_dest2", "fontLimit": 1000, "available": HUGE},
    {"op": "preflight", "conversations": True, "settings": False, "version": 3,
     "dest": "import_dest3", "fontLimit": 1000, "available": HUGE},
    {"op": "preflight", "conversations": True, "settings": True, "version": 6,
     "dest": "import_dest4", "fontLimit": 0, "available": 40},
    {"op": "close"},
])
scenario("30-preflight-overflow", "huge-streamed.zip", [
    open_op("file"),
    {"op": "preflight", "conversations": True, "settings": False, "version": 6,
     "dest": "dest", "fontLimit": 1000, "available": HUGE},
])
scenario("31-preflight-resource-overflow", "huge-resources.zip", [
    open_op("file"),
    {"op": "preflight", "conversations": True, "settings": False, "version": 6,
     "dest": "dest", "fontLimit": 1000, "available": HUGE},
])
scenario("32-preflight-insufficient", V6, [
    open_op("file"),
    {"op": "preflight", "conversations": True, "settings": True, "version": 6,
     "dest": "tight", "fontLimit": 1000, "available": 10},
])

# 33. non-zip garbage through a seekable channel (commons' open message, observed)
scenario("33-open-channel-garbage", "garbage.bin", [
    open_op("channel", channelFixture="garbage.bin"),
    {"op": "names"},
])

# 34. byte rendering (text vs hex), stream EOF verification, resource stream rejection and the
# driver's stream state machine
scenario("34-stream-rendering", V6, [
    open_op("file"),
    {"op": "stream_open", "key": "theme", "name": "theme.bin"},
    {"op": "stream_read", "key": "theme", "count": 3},
    {"op": "stream_read", "key": "theme", "count": 100},
    {"op": "stream_read", "key": "theme", "count": 1},
    {"op": "stream_close", "key": "theme"},
    {"op": "stream_open", "key": "res", "name": "media/images/11111111-2222-3333-4444-555555555555.png"},
    {"op": "stream_read", "key": "res", "count": 4},
    {"op": "stream_open", "key": "font", "name": "custom_font/font"},
    {"op": "stream_read", "key": "missing", "count": 4},
    {"op": "stream_close", "key": "missing"},
    {"op": "close"},
])

# 35. a damaged DEFLATED streamed payload: open succeeds (streamed payloads are not consumed
# until read), the read itself surfaces commons' channel-read failure
scenario("35-stream-bad-deflate", "bad-deflate-stream.zip", [
    open_op("file"),
    {"op": "stream_open", "key": "s", "name": "conv/tasks.json"},
    {"op": "stream_read", "key": "s", "count": 8},
    {"op": "stream_close", "key": "s"},
    {"op": "temp_exists"},
])

# 36. metadata whose declared size fits the budget but whose payload is larger
scenario("36-open-big-metadata-payload", "big-payload.zip", [
    open_op("file", metadataLimit=40),
    {"op": "names"},
    {"op": "temp_exists"},
])


def main():
    make_fixtures()
    shutil.rmtree(SCENARIOS, ignore_errors=True)
    os.makedirs(SCENARIOS)
    for item in ALL_SCENARIOS:
        path = os.path.join(SCENARIOS, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL_SCENARIOS)} scenarios and {len(os.listdir(FIXTURES))} fixtures")


if __name__ == "__main__":
    main()
