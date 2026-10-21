#!/usr/bin/env python3
"""Deterministic scenario generator for the ZIP reader stand.

`entries` scenarios are built by the Kotlin driver (java.util.zip.ZipOutputStream) and frozen as
`fixtures/<stem>.zip` next to the traces; `raw` scenarios carry the archive bytes as hex so the
corruption paths (bad signature, unsupported method, wrong CRC/size, truncated payload, damaged
deflate stream) are pinned too.
"""
from __future__ import annotations

import binascii
import json
import os
import random
import shutil
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def entry(name: str, content: str, method: str = "deflate", extra: str | None = None) -> dict:
    value = {"name": name, "content": content, "method": method}
    if extra is not None:
        value["extra"] = extra
    return value


def local_header(name: bytes, data: bytes, *, method: int = 0, flags: int = 0,
                 crc: int | None = None, size: int | None = None,
                 compressed_size: int | None = None, extra: bytes = b"",
                 descriptor_crc: int | None = None,
                 descriptor_size: int | None = None) -> bytes:
    crc_value = binascii.crc32(data) & 0xFFFFFFFF if crc is None else crc
    size_value = len(data) if size is None else size
    compressed_value = len(data) if compressed_size is None else compressed_size
    header = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, 0, 0,
                         crc_value, compressed_value, size_value,
                         len(name), len(extra)) + name + extra
    if descriptor_crc is None and descriptor_size is None and not (flags & 0x08):
        return header + data
    descriptor = struct.pack("<III", descriptor_crc if descriptor_crc is not None else crc_value,
                             len(data) if descriptor_size is None else descriptor_size,
                             size_value)
    return header + data + descriptor


def raw_deflate(payload: bytes) -> bytes:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(payload) + compressor.flush()


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # ------------------------------------------------------------- built archives
    write("01-single-deflate", {
        "entries": [entry("conversations.json", '{"conversations":[]}')],
    })
    write("02-multiple-entries", {
        "entries": [
            entry("a.json", '{"a":1}'),
            entry("b/c.json", '{"b":2}', method="stored"),
            entry("d.txt", "plain text", method="stored"),
        ],
    })
    write("03-empty-entry", {
        "entries": [entry("empty.json", "")],
    })
    write("04-utf8-name-and-content", {
        "entries": [entry("диалоги/日本語.json", '{"title":"юникод 🙂"}')],
    })
    write("05-binary-content", {
        "entries": [entry("bin.dat", "\u0000\u0001\u0002\u00ff")],
    })
    write("06-extra-field", {
        "entries": [entry("with-extra.json", '{"x":1}', extra="EXTRA-INFO-BYTES")],
    })
    write("07-many-entries", {
        "entries": [entry(f"part-{index}.json", '{"i":%d}' % index) for index in range(20)],
    })
    write("08-large-repetitive", {
        "entries": [entry("big.txt", "abcdefgh" * 20000)],
    })
    write("09-large-incompressible", {
        "entries": [entry("noise.bin", "".join(
            chr(0x4E00 + (index * 7919) % 0x1000) for index in range(20000)))],
    })
    write("10-mixed-methods", {
        "entries": [
            entry("one.json", '{"one":1}', method="stored"),
            entry("two.json", '{"two":2}'),
            entry("three.json", '{"three":3}', method="stored"),
        ],
    })
    write("11-single-byte-content", {
        "entries": [entry("tiny.txt", "x")],
    })
    write("12-tab-and-newline-content", {
        "entries": [entry("lines.txt", "a\tb\nc\rd\n")],
    })

    # --------------------------------------------------------------- raw archives
    payload = b'{"raw":true}'
    write("20-raw-valid-stored", {"raw": local_header(b"a.json", payload).hex()})
    write("21-raw-bad-signature", {
        "raw": (b"PK\x03\x05" + local_header(b"a.json", payload)[4:]).hex(),
    })
    write("22-raw-bad-method", {
        "raw": local_header(b"a.json", payload, method=12).hex(),
    })
    write("23-raw-truncated", {
        "raw": local_header(b"a.json", payload)[:-4].hex(),
    })
    write("24-raw-crc-mismatch", {
        "raw": local_header(b"a.json", payload, crc=0xDEADBEEF).hex(),
    })
    write("25-raw-size-mismatch", {
        "raw": local_header(b"a.json", payload, size=len(payload) + 5).hex(),
    })
    write("26-raw-garbage", {"raw": b"not a zip at all, just bytes".hex()})
    write("27-raw-empty", {"raw": ""})
    write("28-raw-central-directory-only", {
        "raw": struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0).hex(),
    })
    write("29-raw-damaged-deflate", {
        "raw": local_header(b"a.json", raw_deflate(payload * 40), method=8).hex(),
    })
    write("30-raw-damaged-deflate-byte", {
        "raw": _damaged_deflate(payload * 40),
    })
    write("31-raw-descriptor-wrong-crc", {
        "raw": local_header(b"a.json", payload, flags=0x08, descriptor_crc=0x12345678).hex(),
    })
    write("32-raw-descriptor-wrong-size", {
        "raw": local_header(b"a.json", payload, flags=0x08, descriptor_size=999).hex(),
    })
    write("33-raw-directory-entry", {
        "raw": local_header(b"dir/", b"").hex(),
    })
    write("34-raw-invalid-utf8-content", {
        "raw": local_header(b"a.bin", b"\xff\xfe\x00abc").hex(),
    })
    write("35-raw-zero-length-name", {
        "raw": local_header(b"", payload).hex(),
    })
    write("36-raw-deflate-empty", {
        "raw": local_header(b"a.json", raw_deflate(b""), method=8).hex(),
    })
    write("37-raw-stored-empty", {
        "raw": local_header(b"a.json", b"").hex(),
    })
    write("38-raw-two-entries-second-broken", {
        "raw": (local_header(b"a.json", payload) +
                local_header(b"b.json", payload, crc=1)).hex(),
    })

    # ----------------------------------------------------------------------- fuzz
    rng = random.Random(9000)
    for index in range(8):
        chunks = []
        for _ in range(rng.randint(1, 3)):
            name = ("f%d.bin" % rng.randint(0, 9)).encode()
            size = rng.randint(0, 64)
            data = bytes(rng.randrange(256) for _ in range(size))
            method = rng.choice([0, 8])
            if method == 8:
                data = raw_deflate(data)
                header = local_header(name, data, method=8, flags=0)
            else:
                header = local_header(name, data, method=0)
            if rng.random() < 0.3:
                header = header[:4] + bytes([rng.randrange(256)]) + header[5:]
            chunks.append(header)
        write("90-fuzz-%02d" % index, {"raw": b"".join(chunks).hex()})

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


def _damaged_deflate(payload: bytes) -> str:
    data = bytearray(raw_deflate(payload * 40))
    if len(data) > 6:
        data[6] ^= 0xFF
    return local_header(b"a.json", bytes(data), method=8).hex()


if __name__ == "__main__":
    main()
