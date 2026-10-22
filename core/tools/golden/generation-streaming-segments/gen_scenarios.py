#!/usr/bin/env python3
"""Deterministic scenario generator for the generation streaming segments."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def segment(kind: str, content: str = "", **extra) -> dict:
    value = {"type": kind, "content": content}
    value.update(extra)
    return value


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-segments-merge", {
        "clock": [0, 100, 150, 200, 300, 400, 500],
        "timingSteps": [
            {"action": "ensureStarted"},
            {"action": "live"},
            {"action": "finish"},
            {"action": "live"},
            {"action": "reset"},
            {"action": "live"},
            {"action": "adopt", "duration": 1234},
        ],
        "merge": [
            segment("answer", "first"),
            segment("answer", " second"),
            segment("thought", "thinking", signature="sig", durationMs=100),
            segment("thought", " more", signature=None, durationMs=50),
            segment("tool", "tool output"),
            segment("answer", "third"),
        ],
        "live": {
            "flushed": [segment("citation", "citation body")],
            "answer": "live answer",
            "thought": "live thought",
            "signature": "sig",
            "signatureProvider": "openai",
            "thoughtDurationMs": 700,
            "answerDeltas": [{"sequence": 1, "codePointCount": 5}],
        },
    })

    write("02-segments-live-variants", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "live": {
            "flushed": [],
            "answer": "",
            "thought": "",
        },
    })

    write("03-segments-live-error", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "live": {
            "flushed": [segment("answer", "kept")],
            "answer": "partial",
            "thought": "",
            "errorMessage": "provider failed",
            "errorCode": "http_500",
        },
    })

    write("04-segments-live-blank-error", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "live": {
            "flushed": [],
            "answer": "a",
            "thought": "",
            "errorMessage": "   ",
            "errorCode": "x",
        },
    })

    write("05-segments-citation-rebase", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "citation": {
            "sourceId": "s1",
            "provider": "openai",
            "kind": "url",
            "title": "Doc",
            "url": "https://example.com",
            "anchors": [{"startIndex": 0, "endIndex": 5, "citedText": "hello"}],
        },
        "providerAnswerStart": 10,
        "finalAnswer": "hello world, this is the final answer",
    })

    write("06-segments-citation-rebase-overflow", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "citation": {
            "sourceId": "s1",
            "provider": "openai",
            "kind": "url",
            "title": "Doc",
            "url": "https://example.com",
            "anchors": [{"startIndex": 2147483640, "endIndex": 2147483647,
                          "citedText": "tail"}],
        },
        "providerAnswerStart": 100,
        "finalAnswer": "short",
    })

    write("07-segments-citation-without-anchors", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "citation": {
            "sourceId": "s1",
            "provider": "openai",
            "kind": "url",
            "title": "Doc",
            "url": "https://example.com",
            "anchors": [],
        },
        "providerAnswerStart": 3,
        "finalAnswer": "answer",
    })

    write("08-segments-bounded-transform", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "message": {
            "id": "m1",
            "text": "0123456789",
            "status": "SUCCESS",
            "participant": "MODEL",
            "timestamp": 1,
            "segments": [
                segment("answer", "01234"),
                segment("answer", "56789"),
            ],
        },
        "transform": {"replacement": "abcde"},
    })

    write("09-segments-bounded-transform-single", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "message": {
            "id": "m1",
            "text": "hello",
            "status": "SUCCESS",
            "participant": "MODEL",
            "timestamp": 1,
            "segments": [segment("answer", "hello")],
        },
        "transform": {"replacement": "hi"},
    })

    write("10-segments-terminal-error", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "terminalError": {"status": "ERROR", "currentError": "boom", "fallbackError": "fallback"},
    })

    write("11-segments-terminal-error-blank", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "terminalError": {"status": "ERROR", "currentError": "   ", "fallbackError": "fallback"},
    })

    write("12-segments-terminal-no-error", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "terminalError": {"status": "SUCCESS", "currentError": "boom", "fallbackError": "fallback"},
    })

    write("13-segments-snapshot", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "snapshot": {
            "messageId": "m1",
            "parentId": "m0",
            "text": "the answer",
            "images": ["a.png"],
            "thoughts": "  ",
            "thoughtTitle": "title",
            "tokenCount": 42,
            "timestamp": 1700000000000,
            "thoughtTimeMs": 1200,
            "modelName": "OpenAI:gpt-5.4",
            "flushedSegments": [segment("citation", "cite")],
            "answerBuffer": "the answer",
            "thoughtBuffer": "",
            "thoughtSignature": "sig",
            "thoughtSignatureProvider": "openai",
            "thoughtDurationMs": 900,
            "errorMessage": None,
            "errorCode": None,
            "runId": "run-1",
            "runSequence": 7,
            "answerDeltas": [{"sequence": 3, "codePointCount": 4}],
        },
    })

    write("14-segments-snapshot-error", {
        "clock": [],
        "timingSteps": [],
        "merge": [],
        "snapshot": {
            "messageId": "m2",
            "parentId": None,
            "text": "partial",
            "images": [],
            "thoughts": "thinking",
            "thoughtTitle": None,
            "tokenCount": 0,
            "timestamp": 1700000001000,
            "thoughtTimeMs": None,
            "modelName": "",
            "flushedSegments": [],
            "answerBuffer": "",
            "thoughtBuffer": "thinking",
            "thoughtSignature": None,
            "thoughtSignatureProvider": None,
            "thoughtDurationMs": 100,
            "errorMessage": "failed",
            "errorCode": "e1",
            "runId": "run-2",
            "runSequence": 1,
            "answerDeltas": [],
        },
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
