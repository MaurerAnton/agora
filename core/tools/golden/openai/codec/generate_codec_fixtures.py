#!/usr/bin/env python3
"""Regenerates the binary codec fixtures shared by the native and Kotlin tests.

The fixtures are the single source of truth for the wire format documented in
core/include/agora/core/openai_stream_codec.hpp: the C++ test byte-compares its
encoder against them, the Kotlin unit test decodes them back into events. A
change to either side that is not mirrored here fails one of the two tests.

Output: app/src/test/resources/openai_stream_codec/{line_outcome,termination}.bin
"""
import pathlib
import struct

HERE = pathlib.Path(__file__).resolve().parent
OUTPUT = HERE.parents[4] / "app" / "src" / "test" / "resources" / "openai_stream_codec"

NULL_STRING = 0xFFFFFFFF


def u8(value: int) -> bytes:
    return struct.pack(">B", value)


def u16(value: int) -> bytes:
    return struct.pack(">H", value)


def i32(value: int) -> bytes:
    return struct.pack(">i", value)


def string(value: str) -> bytes:
    data = value.encode("utf-8")
    return struct.pack(">I", len(data)) + data


def nstring(value) -> bytes:
    return struct.pack(">I", NULL_STRING) if value is None else string(value)


def optional_int(value) -> bytes:
    return u8(0) if value is None else u8(1) + i32(value)


def tool_call(call_id, name, arguments, signature, stream_key) -> bytes:
    return (
        string(call_id)
        + string(name)
        + string(arguments)
        + nstring(signature)
        + string(stream_key)
    )


def usage(total, input_, cached, cache_write, uncached, output, reasoning) -> bytes:
    return (
        i32(total)
        + optional_int(input_)
        + optional_int(cached)
        + optional_int(cache_write)
        + optional_int(uncached)
        + optional_int(output)
        + optional_int(reasoning)
    )


def error(kind: int, fields: bytes = b"") -> bytes:
    return u8(kind) + fields


def event(kind: int, fields: bytes) -> bytes:
    return u8(kind) + fields


def line_outcome() -> bytes:
    logs = ["[agora] malformed stream payload exception=JsonDecodingException"]
    events = [
        event(1, string("Привет 😀\n\t")),  # text
        event(2, string("думаю") + nstring(None) + nstring(None)),  # thought
        event(  # toolCallUpdate without an id yet
            3,
            string("call_stream_00000000-0000-4000-8000-000000000001")
            + nstring(None)
            + string("")
            + string("{}"),
        ),
        event(  # toolCallUpdate with an id
            3,
            string("call_stream_00000000-0000-4000-8000-000000000002")
            + nstring("call_ab")
            + string("search")
            + string('{"q":"x"}'),
        ),
        event(4, tool_call("call_ab", "search", '{"q":"x"}', None, "call_ab")),
        event(
            5,
            u16(2)
            + tool_call("c1", "n1", "{}", None, "c1")
            + tool_call("c2", "n2", "[]", "sig2", "c2"),
        ),
        event(6, usage(100, 10, None, 5, None, 20, None)),
        event(7, error(3, string("raw|x") + string("cause;text\nmore"))),  # sseParse
    ]
    payload = u8(1) + u16(len(logs))
    for log in logs:
        payload += string(log)
    payload += u16(len(events))
    for item in events:
        payload += item
    return payload


def termination() -> bytes:
    # IncompleteStream(provider="agora", stop="length", inFlight, produced), timed out.
    stream_error = error(
        5,
        string("agora") + nstring("length") + u8(1) + u8(1),
    )
    return (
        u8(0)  # sawTerminalMarker
        + nstring(None)  # stopReason
        + u8(1)  # producedContent
        + u8(1)  # toolCallInFlight
        + u8(0)  # alreadyReportedError
        + u8(1)  # timedOut
        + stream_error
    )


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    (OUTPUT / "line_outcome.bin").write_bytes(line_outcome())
    (OUTPUT / "termination.bin").write_bytes(termination())
    for name in ("line_outcome.bin", "termination.bin"):
        size = (OUTPUT / name).stat().st_size
        print(f"wrote {OUTPUT / name} ({size} bytes)")


if __name__ == "__main__":
    main()
