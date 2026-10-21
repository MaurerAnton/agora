#!/usr/bin/env python3
"""Deterministic scenario generator for ImportedCitationProjection.

Three ops: "gpt" (projectChatGptCitations), "claude" (projectClaudeCitation) and
"encode" (encodeImportedCitations). Private markers use the real U+E200/U+E201/U+E202
envelope characters the Kotlin regexes match.
"""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

E200 = "\ue200"
E201 = "\ue201"
E202 = "\ue202"

TURN = "turn3f12"


def cite(marker_id: str, kind: str = "cite") -> str:
    return f"{E200}{kind}{E202}{marker_id}{E201}"


def write(name: str, payload: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def gpt(name: str, raw_text: str, references: list | None = None) -> None:
    write(name, {"op": "gpt", "rawText": raw_text, "references": references or []})


def claude(name: str, citation: dict, start_index: int = 0, answer_text: str = "") -> None:
    write(name, {"op": "claude", "citation": citation, "startIndex": start_index,
                 "answerText": answer_text})


def encode(name: str, citations: list, answer_text: str = "") -> None:
    write(name, {"op": "encode", "citations": citations, "answerText": answer_text})


def record(source_id: str, *, provider: str = "openai", kind: str = "url",
           title: str = "T", url: str | None = "https://example.com/a",
           file_name: str | None = None, location: str | None = None,
           provider_source_id: str | None = None, excerpt: str | None = None,
           anchors: list | None = None, version: int | None = None) -> dict:
    value = {"sourceId": source_id, "provider": provider, "kind": kind, "title": title}
    if version is not None:
        value["version"] = version
    if url is not None:
        value["url"] = url
    if file_name is not None:
        value["fileName"] = file_name
    if location is not None:
        value["location"] = location
    if provider_source_id is not None:
        value["providerSourceId"] = provider_source_id
    if excerpt is not None:
        value["excerpt"] = excerpt
    if anchors is not None:
        value["anchors"] = anchors
    return value


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # ------------------------------------------------------------------ gpt markers
    gpt("01-gpt-plain-text", "Just a plain answer with no markers at all.")
    gpt("02-gpt-single-cite",
        f"First sentence. {cite('turn3f12')} Second sentence.",
        [{"source_id": "turn3f12", "url": "https://example.com/doc", "title": "Doc",
          "matched_text": "Second", "start_idx": 16, "end_idx": 22}])
    gpt("03-gpt-filecite",
        f"See {cite('turn3f12', 'filecite')} for details.",
        [{"source_id": "turn3f12", "file_name": "notes.md", "file_type": "text/markdown",
          "extracted_content": "# notes"}])
    gpt("04-gpt-multi-id-marker",
        f"A{cite('turn3f12')}B{E200}cite{E202}turn3f13{E202} turn3f14 {E201}C",
        [{"source_id": "turn3f12", "url": "https://one.example", "title": "One",
          "matched_text": "A"},
         {"source_id": "turn3f13", "url": "https://two.example", "title": "Two",
          "matched_text": "B"},
         {"source_id": "turn3f14", "url": "https://three.example", "title": "Three",
          "matched_text": "C"}])
    gpt("05-gpt-unknown-id-dropped",
        f"Keep {cite('turn9z99')} but no source matches.",
        [{"source_id": "turn7x77", "url": "https://example.com/a", "title": "Other",
          "matched_text": "Keep "}])
    gpt("06-gpt-cjk-marker-structured-ref",
        f"Answer text {f'【{TURN}】'} end.",
        [{"turn_index": 3, "ref_type": "f", "ref_index": 12,
          "url": "https://structured.example", "title": "Structured"}])
    gpt("07-gpt-marker-without-references", f"Only {cite('turn3f12')} marker here.", [])
    gpt("08-gpt-title-only-source",
        f"Text {cite('turn3f12')} tail.",
        [{"source_id": "turn3f12", "title": "Title only", "matched_text": "tail."}])
    gpt("09-gpt-invalid-url-source",
        f"Text {cite('turn3f12')} tail.",
        [{"source_id": "turn3f12", "url": "not a url", "matched_text": "tail."}])
    gpt("10-gpt-safe-urls-fallback",
        f"Text {cite('turn3f12')} tail.",
        [{"source_id": "turn3f12", "safe_urls": ["https://safe.example/x"],
          "matched_text": "tail."},
         {"source_id": "turn3f13", "safe_urls": [42, None], "matched_text": "tail."}])
    gpt("11-gpt-exact-range-anchor",
        f"prefix {cite('turn3f12')} suffix",
        [{"source_id": "turn3f12", "url": "https://example.com/range", "title": "R",
          "matched_text": "suffix", "start_idx": 8, "end_idx": 14}])
    gpt("12-gpt-matched-fallback-blank-gap",
        f"alpha beta {cite('turn3f12')} gamma   ",
        [{"source_id": "turn3f12", "url": "https://example.com/fb", "title": "F",
          "matched_text": "alpha beta"}])
    gpt("13-gpt-matched-range-mismatch",
        f"alpha beta {cite('turn3f12')} tail",
        [{"source_id": "turn3f12", "url": "https://example.com/mm", "title": "M",
          "matched_text": "ALPHA", "start_idx": 0, "end_idx": 6}])
    gpt("14-gpt-range-out-of-bounds",
        f"short {cite('turn3f12')} tail",
        [{"source_id": "turn3f12", "url": "https://example.com/ob", "title": "O",
          "matched_text": "tail", "start_idx": 100, "end_idx": 104}])
    gpt("15-gpt-duplicate-markers-dedup",
        f"{cite('turn3f12')} and {cite('turn3f12')} again.",
        [{"source_id": "turn3f12", "url": "https://example.com/dup", "title": "Dup",
          "matched_text": "again."}])
    gpt("16-gpt-nested-references",
        f"nested {cite('turn3f12')} end",
        [[{"source_id": "turn3f12", "url": "https://nested.example", "title": "Nested",
           "matched_text": "end"}],
         {"whatever": {"deep": [{"source_id": "turn3f13", "title": "Deep",
                                 "matched_text": "nested "}]}}])
    gpt("17-gpt-non-string-primitives-ignored",
        f"x {cite('turn3f12')} y",
        [{"title": 42, "url": True, "matched_text": "y", "turn_index": "3",
          "ref_type": "f", "ref_index": 12}])
    gpt("18-gpt-uppercase-marker",
        f"up {E200}CITE{E202}TURN3F12{E201} case",
        [{"source_id": "turn3f12", "url": "https://example.com/upper", "title": "U",
          "matched_text": "case"}])
    gpt("19-gpt-blank-id-parts-filtered",
        f"{E200}cite{E202}turn3f12{E202}{E202} {E201}ok",
        [{"source_id": "turn3f12", "url": "https://example.com/blank", "title": "B",
          "matched_text": "ok"}])
    gpt("20-gpt-no-identifying-field",
        f"drop {cite('turn3f12')} this citation",
        [{"source_id": "turn3f12", "matched_text": "this"}])
    gpt("21-gpt-source-ids-in-string-content",
        f"content {cite(TURN)} end",
        [{"text": "see turn3f12 in text", "url": "https://example.com/content",
          "matched_text": "end"}])
    gpt("22-gpt-marker-end-beyond-stripped",
        f"{E200}cite{E202}turn3f12{E201}{E200}cite{E202}turn3f13{E201}",
        [{"source_id": "turn3f12", "url": "https://example.com/beyond", "title": "B",
          "matched_text": "gone"}])
    gpt("23-gpt-cjk-marker-without-source", "plain 【turn5a1】 marker", [])
    gpt("24-gpt-multiple-answers-order",
        f"one {cite('turn3f12')} two {cite('turn3f13')} three",
        [{"source_id": "turn3f12", "url": "https://a.example", "matched_text": "one "},
         {"source_id": "turn3f13", "url": "https://b.example", "matched_text": "two "}])
    gpt("25-gpt-emoji-and-unicode-text",
        f"emoji 🙂 {cite('turn3f12')} юникод текст",
        [{"source_id": "turn3f12", "url": "https://unicode.example", "title": "Юникод",
          "matched_text": "юникод"}])
    gpt("26-gpt-empty-text", "", [])
    gpt("27-gpt-int-accessor-trimmed-string",
        "tail ZZZ more " + cite("turn1f1"),
        [{"source_id": "turn1f1", "url": "https://example.com/trim",
          "matched_text": "ZZZ", "start_idx": " 5 ", "end_idx": " 8 "}])
    gpt("28-gpt-int-accessor-fraction-string",
        "tail ZZZ more " + cite("turn1f1"),
        [{"source_id": "turn1f1", "url": "https://example.com/fraction",
          "matched_text": "ZZZ", "start_idx": 5, "end_idx": "8.0"}])
    gpt("29-gpt-structured-ref-exponent-index",
        "exp " + cite("turn300f12"),
        [{"turn_index": "3e2", "ref_type": "f", "ref_index": 12,
          "url": "https://example.com/exp", "title": "Exp"}])

    # --------------------------------------------------------------- claude citations
    claude("30-claude-url-source",
           {"url": "https://claude.example/doc", "title": "Claude Doc",
            "uuid": "cite-1", "matched_text": "answer"},
           0, "the answer text")
    claude("31-claude-file-source",
           {"file_name": "report.pdf", "file_type": "application/pdf",
            "title": "Report"}, 0, "answer")
    claude("32-claude-structured-anchor-range",
           {"url": "https://range.example", "title": "R", "matched_text": "ZZZ"},
           4, "abcdZZZtail")
    claude("33-claude-empty-answer",
           {"url": "https://empty.example", "title": "E", "matched_text": "m"}, 0, "")
    claude("34-claude-no-source", {"foo": "bar"}, 0, "answer")
    claude("35-claude-title-only", {"title": "Only title"}, 0, "answer")
    claude("36-claude-start-index-beyond",
           {"url": "https://b.example", "matched_text": "x"}, 99, "short")

    # --------------------------------------------------------------------- encode
    encode("40-encode-empty", [], "answer")
    encode("41-encode-single",
           [record("s1", excerpt="An excerpt", anchors=[
               {"startIndex": 0, "endIndex": 6, "citedText": "answer"}])],
           "answer text")
    encode("42-encode-dedup-two-sources",
           [record("s1", title="First"), record("s2", title="Second")], "answer")
    encode("43-encode-version-and-fields",
           [record("s3", version=1, provider="anthropic", kind="file",
                   url=None, file_name="a.txt", location="line 1",
                   provider_source_id="turn3f12", excerpt="ex")], "answer")
    # Over the 600000-byte persistence budget: five records with the maximum anchor payload.
    big_anchor = {"startIndex": 0, "endIndex": 4096, "citedText": "a" * 4096}
    big_records = [
        record(f"big{i}", title="big", url=f"https://big.example/{i}",
               excerpt="e" * 512, anchors=[dict(big_anchor) for _ in range(32)])
        for i in range(6)
    ]
    encode("44-encode-over-budget", big_records, "answer")

    # ------------------------------------------------------------------------- fuzz
    fragment_pool = [
        "plain text ", "({}) ", "【turn{}{}{}】 ", "tail.",
        "🙂 unicode ", "  ", "end", cite("turn{}{}{}", "filecite"),
    ]
    ref_pool = [
        {"url": "https://fuzz.example/{}", "title": "F{}", "matched_text": "end"},
        {"file_name": "f{}.txt", "file_type": "text/plain"},
        {"turn_index": {}, "ref_type": "f", "ref_index": {},
         "url": "https://struct.example"},
        {"safe_urls": ["https://safe.example/{}"]},
        {"title": "T{}", "matched_text": "tail."},
        {"uuid": "id-{}", "matched_text": "plain text "},
    ]
    for index in range(10):
        rng = random.Random(3000 + index)
        pieces = []
        for _ in range(rng.randint(3, 7)):
            kind = rng.choice(["plain", "cite", "cjk", "filecite", "blank"])
            if kind == "plain":
                pieces.append(rng.choice(["alpha ", "beta ", "gamma ", "🙂 "]))
            elif kind == "cite":
                pieces.append(cite(f"turn{rng.randint(1, 4)}f{rng.randint(1, 20):02d}"))
            elif kind == "filecite":
                pieces.append(cite(f"turn{rng.randint(1, 4)}f{rng.randint(1, 20):02d}",
                                  "filecite"))
            elif kind == "cjk":
                pieces.append(f"【turn{rng.randint(1, 4)}f{rng.randint(1, 20):02d}】")
            else:
                pieces.append(rng.choice([" ", "\n", "\t"]))
        marker_ids = set()
        for part in pieces:
            if part.startswith(E200) and E202 in part:
                marker_ids.add(part.split(E202)[1].split(E201)[0])
            elif part.startswith("【") and part.endswith("】"):
                marker_ids.add(part[1:-1])
        marker_ids = sorted(marker_ids)
        references = []
        for marker_id in marker_ids:
            kind = rng.choice([
                {"source_id": marker_id, "url": "https://fuzz.example",
                 "title": "F", "matched_text": "tail."},
                {"source_id": marker_id, "file_name": "f.txt",
                 "file_type": "text/plain"},
                {"source_id": marker_id, "title": "T"},
                {"source_id": marker_id, "url": "not a url"},
            ])
            references.append(kind)
        for reference_index in range(rng.randint(0, 2)):
            template = json.dumps(rng.choice(ref_pool))
            references.append(json.loads(template.replace("{}", str(reference_index))))
        gpt(f"50-fuzz-{index:02d}", "".join(pieces), references)

    print(f"Wrote {len(os.listdir(OUT))} scenarios to {OUT}")


if __name__ == "__main__":
    main()
