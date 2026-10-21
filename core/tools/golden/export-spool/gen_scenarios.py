#!/usr/bin/env python3
"""Deterministic scenario generator for ExportSpool.

Each scenario carries either a `writes` op sequence (replayed through the real
ExportSpoolWriter, producing bytes + index) or raw `content`, plus a `reads` list that probes the
real ExportSpoolReader through the same file. Slices may be symbolic ("conv:0", "task:0")
resolved against the finished index, or explicit [start, end] byte ranges.
"""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation(conversation_id: str, data_changed_at: int, payload: str) -> dict:
    return {"op": "conversation", "id": conversation_id,
            "dataChangedAt": data_changed_at, "json": payload}


def simple(op: str, payload: str) -> dict:
    return {"op": op, "json": payload}


def records(slice_spec) -> dict:
    return {"kind": "records", "slice": slice_spec}


def iterator(slices_, record_type=None, probe_next=None) -> dict:
    value = {"kind": "iterator", "slices": slices_}
    if record_type is not None:
        value["type"] = record_type
    if probe_next is not None:
        value["probeNext"] = probe_next
    return value


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # ------------------------------------------------------------------ writer scenarios
    write("01-write-basic", {
        "writes": [
            conversation("c1", 1700000000000, '{"id":"c1"}'),
            simple("run", '{"run":"r1"}'),
            simple("message", '{"msg":"m1"}'),
            simple("message", '{"msg":"m2"}'),
            simple("loop", '{"loop":"l1"}'),
        ],
        "reads": [
            records("conv:0"),
            records([0, 1000]),
            iterator(["conv:0"]),
            iterator(["conv:0"], "M"),
            iterator(["conv:0"], "R"),
            iterator(["conv:0"], "M", probe_next=True),
        ],
    })
    write("02-write-task-between-conversations", {
        "writes": [
            conversation("c1", 100, '{"id":"c1"}'),
            simple("message", '{"msg":"m1"}'),
            simple("task", '{"task":"t1"}'),
            simple("task", '{"task":"t2"}'),
            conversation("c2", 200, '{"id":"c2"}'),
            simple("message", '{"msg":"m2"}'),
            simple("task", '{"task":"t3"}'),
        ],
        "reads": [
            records("conv:0"),
            records("conv:1"),
            records("task:0"),
            records("task:1"),
            iterator(["conv:0", "conv:1"]),
            iterator(["task:0", "task:1"], "T"),
            iterator(["conv:0", "task:1", "conv:1"]),
        ],
    })
    write("03-write-utf8-offsets", {
        "writes": [
            conversation("беседа-🙂", 1700000000001, '{"title":"юникод 🙂 текст"}'),
            simple("message", '{"text":"日本語のメッセージ"}'),
        ],
        "reads": [
            records("conv:0"),
            iterator(["conv:0"]),
        ],
    })
    write("04-write-run-without-conversation", {
        "writes": [
            simple("run", '{"run":"r1"}'),
            conversation("c1", 5, '{"id":"c1"}'),
            simple("message", '{"msg":"m1"}'),
        ],
        "reads": [records("conv:0")],
    })
    write("05-write-task-first", {
        "writes": [
            simple("task", '{"task":"t0"}'),
            conversation("c1", 7, '{"id":"c1"}'),
            simple("message", '{"msg":"m1"}'),
        ],
        "reads": [records("task:0"), records("conv:0")],
    })
    write("06-write-empty-finish", {"writes": [], "reads": [records([0, 0])]})
    write("07-write-empty-payloads", {
        "writes": [
            conversation("c1", 1, ""),
            simple("message", ""),
        ],
        "reads": [records("conv:0"), iterator(["conv:0"])],
    })
    write("08-write-payload-with-newline", {
        "writes": [
            conversation("c1", 1, '{"id":"c1"}'),
            simple("message", '{"text":"line1\nline2"}'),
        ],
        "reads": [
            records("conv:0"),
            iterator(["conv:0"]),
        ],
    })
    write("09-write-large-record-crosses-chunks", {
        "writes": [
            conversation("big", 42, '{"blob":"' + ("x" * 200000) + '"}'),
            simple("message", '{"msg":"tail"}'),
        ],
        "reads": [
            records("conv:0"),
            iterator(["conv:0"]),
            iterator(["conv:0"], "M", probe_next=True),
        ],
    })
    write("10-write-readonly-slice-clamps", {
        "writes": [
            conversation("c1", 11, '{"id":"c1"}'),
            simple("message", '{"msg":"m1"}'),
        ],
        "reads": [
            records([0, 999999]),
            records([5, 3]),
            records([999, 1000]),
            iterator(["conv:0", [5, 3], [999, 1000]]),
        ],
    })

    # ------------------------------------------------------------- raw content scenarios
    write("20-read-content-unterminated-last-line", {
        "content": "C\t{\"id\":\"c1\"}\nM\t{\"msg\":\"m1\"}\nM\tunterminated",
        "reads": [records([0, 1000]), iterator([[0, 1000]])],
    })
    write("21-read-content-empty-line", {
        "content": "C\t{\"id\":\"c1\"}\n\nM\t{\"msg\":\"m1\"}\n",
        "reads": [records([0, 1000]), iterator([[0, 1000]])],
    })
    write("22-read-content-tab-first", {
        "content": "\t{\"id\":\"c1\"}\nM\t{\"msg\":\"m1\"}\n",
        "reads": [records([0, 1000]), iterator([[0, 1000]])],
    })
    write("23-read-content-no-tab", {
        "content": "C\t{\"id\":\"c1\"}\nplainline\nM\t{\"msg\":\"m1\"}\n",
        "reads": [records([0, 1000]), iterator([[0, 1000]], probe_next=True)],
    })
    write("24-read-iterator-type-filter", {
        "content": "C\tc\nR\tr\nM\tm1\nM\tm2\nT\tt\nL\tl\n",
        "reads": [
            iterator([[0, 1000]], "M"),
            iterator([[0, 1000]], "T"),
            iterator([[0, 1000]], "X"),
            iterator([[0, 1000]], "M", probe_next=True),
            iterator([[0, 1000]]),
        ],
    })
    write("25-read-iterator-splits-across-slices", {
        "content": "C\t{\"id\":\"c1\"}\nM\t{\"msg\":\"m1\"}\nM\t{\"msg\":\"m2\"}\nT\t{\"task\":\"t1\"}\n",
        "reads": [
            iterator([[0, 12], [12, 40]]),
            iterator([[0, 13], [13, 26]]),
            iterator([[5, 18], [18, 100]], "M"),
            records([5, 18]),
        ],
    })
    write("26-read-empty-slices", {
        "content": "C\tc\n",
        "reads": [
            iterator([]),
            iterator([[0, 0], [0, 0]], probe_next=True),
            records([0, 0]),
        ],
    })
    write("27-read-content-utf8", {
        "content": "C\t{\"title\":\"юникод 🙂\"}\nM\t{\"text\":\"日本語\"}\n",
        "reads": [
            records([0, 1000]),
            iterator([[0, 1000]]),
            iterator([[0, 10], [10, 100]]),
            records([1, 12]),
        ],
    })
    write("28-read-content-invalid-after-valid", {
        "content": "C\t{\"id\":\"c1\"}\nM\tok\nBADLINE\nM\t{\"msg\":\"m2\"}\n",
        "reads": [
            records([0, 1000]),
            iterator([[0, 1000]], probe_next=True),
        ],
    })
    write("29-read-empty-content", {
        "content": "",
        "reads": [
            records([0, 0]),
            iterator([[0, 0]], probe_next=True),
        ],
    })
    write("30-read-large-crosses-chunks", {
        "content": "M\t" + ("y" * 150000) + "\nM\ttail\n",
        "reads": [
            records([0, 1000000]),
            iterator([[0, 1000000]]),
            iterator([[0, 65536], [65536, 1000000]], "M"),
        ],
    })

    # ------------------------------------------------------------------------ fuzz
    ops_pool = ["run", "message", "loop", "task"]
    for index in range(10):
        rng = random.Random(5000 + index)
        writes = []
        conversation_counter = 0
        for _ in range(rng.randint(0, 8)):
            choice = rng.random()
            if choice < 0.45:
                conversation_counter += 1
                payload = rng.choice([
                    '{"id":"c%d"}' % conversation_counter,
                    '{"id":"юникод%d"}' % conversation_counter,
                    "",
                    '{"a":%d,"b":"text"}' % conversation_counter,
                ])
                writes.append(conversation(
                    "c%d" % conversation_counter,
                    rng.randint(1, 10 ** 12),
                    payload,
                ))
            else:
                writes.append(simple(
                    rng.choice(ops_pool),
                    rng.choice(['{"x":1}', '{"y":"日本"}', "", '{"z":"line"}']),
                ))
        reads = []
        for _ in range(rng.randint(1, 4)):
            kind = rng.choice(["records", "iterator"])
            count = rng.randint(1, 3)
            slices = []
            for _ in range(count):
                choice = rng.random()
                if choice < 0.4 and conversation_counter > 0:
                    slices.append("conv:%d" % rng.randint(0, conversation_counter - 1))
                elif choice < 0.6:
                    start = rng.randint(0, 60)
                    slices.append([start, start + rng.randint(0, 80)])
                elif choice < 0.8:
                    slices.append([0, 10 ** 6])
                else:
                    slices.append([rng.randint(0, 30), rng.randint(0, 30)])
            if kind == "records":
                reads.append(records(slices[0]))
            else:
                reads.append(iterator(
                    slices,
                    rng.choice([None, None, "C", "M", "T", "R"]),
                    probe_next=rng.choice([False, True, None]),
                ))
        scenario = {"writes": writes, "reads": reads}
        if rng.random() < 0.3:
            scenario.pop("writes")
            scenario["content"] = rng.choice([
                "C\tc\nM\tm\n",
                "T\tt",
                "\n",
                "bad\n",
                "M\tok\n",
            ])
            # content mode has no index: every read becomes an explicit byte range
            explicit = []
            for read in scenario["reads"]:
                if read["kind"] == "records":
                    start = rng.randint(0, 10)
                    explicit.append(records([start, start + rng.randint(0, 40)]))
                else:
                    ranges = []
                    for _ in range(rng.randint(0, 3)):
                        start = rng.randint(0, 10)
                        ranges.append([start, start + rng.randint(0, 40)])
                    explicit.append(iterator(ranges, read.get("type"),
                                             probe_next=read.get("probeNext")))
            scenario["reads"] = explicit
        write("40-fuzz-%02d" % index, scenario)

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
