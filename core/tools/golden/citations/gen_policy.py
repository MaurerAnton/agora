# Generates the CitationPolicy scenarios (create / normalize / merge / deduplicate / strip /
# copy / encode / decode / safeUrl / matchTitle).
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
base = os.path.join(HERE, "scenarios")
os.makedirs(base, exist_ok=True)


def w(name, body):
    with open(os.path.join(base, name + ".json"), "w") as f:
        json.dump({"name": name, **body}, f, indent=2, ensure_ascii=True)
        f.write("\n")


def anchor(start, end, text):
    return {"startIndex": start, "endIndex": end, "citedText": text}


def rec(**kw):
    body = {
        "version": 1,
        "sourceId": "citation_000000000000000000000000",
        "provider": "web",
        "kind": "url",
        "title": "T",
    }
    body.update(kw)
    return body


def create(name, **kw):
    w(name, {"mode": "create", **kw})


# --- create: normalization, fallbacks, rejection ------------------------------------------------

create("policy_create_minimal", provider="web", kind="url", title="T")
create("policy_create_blank_provider", provider="   ", kind="url", title="T")
create("policy_create_blank_kind", provider="web", kind=" \t ", title="T")
create("policy_create_empty_provider", provider="", kind="url", title="T")
create("policy_create_folds_case", provider="Anthropic", kind="URL", title="T")
create("policy_create_trims_metadata", provider="  web  ", kind="  url  ", title="  T  ")
create("policy_create_title_512_boundary", provider="web", kind="url", title="a" * 512)
create("policy_create_title_513_truncated", provider="web", kind="url", title="a" * 513)
create(
    "policy_create_title_surrogate_boundary",
    provider="web",
    kind="url",
    title="a" * 511 + "\U0001f600",
)
create("policy_create_url_only", provider="web", kind="url", url="https://example.com/x")
create("policy_create_url_host_www", provider="web", kind="url", url="https://www.example.com/x")
create("policy_create_url_bad_rejected", provider="web", kind="url", url="not a url")
create("policy_create_url_bad_title_kept", provider="web", kind="url", url="not a url", title="Kept")
create("policy_create_url_bad_filename_kept", provider="web", kind="url", url="not a url", fileName="f.pdf")
create("policy_create_title_from_filename", provider="web", kind="url", fileName="f.pdf")
create("policy_create_title_from_provider", provider="web", kind="url")
create("policy_create_excerpt_boundary", provider="web", kind="url", title="T", excerpt="e" * 8192)
create("policy_create_excerpt_truncated", provider="web", kind="url", title="T", excerpt="e" * 8193)
create("policy_create_location_trimmed", provider="web", kind="url", title="T", location="  Pages 3-4  ")
create("policy_create_location_blank", provider="web", kind="url", title="T", location="   ")
create("policy_create_psid_trimmed", provider="web", kind="url", title="T", providerSourceId="  doc:7  ")
create("policy_create_identity_join", provider="anthropic", kind="document", title="Doc", fileName="d.pdf", location="Pages 1-2", providerSourceId="document:0")
create("policy_create_anchors_dedupe", provider="web", kind="url", title="T", answerText="hello world", anchors=[anchor(0, 5, "hello"), anchor(0, 5, "hello")])
create(
    "policy_create_anchors_take_32",
    provider="web",
    kind="url",
    title="T",
    answerText="abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP",
    anchors=[anchor(i, i + 1, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP"[i]) for i in range(40)],
)
create("policy_create_anchor_len_mismatch", provider="web", kind="url", title="T", anchors=[anchor(0, 5, "hi")])
create("policy_create_anchor_exact_slice", provider="web", kind="url", title="T", answerText="hello world", anchors=[anchor(6, 11, "world")])
create("policy_create_anchor_recovered", provider="web", kind="url", title="T", answerText="hello world", anchors=[anchor(0, 5, "world")])
create("policy_create_anchor_ambiguous", provider="web", kind="url", title="T", answerText="ab ab", anchors=[anchor(0, 2, "ab")])
create("policy_create_anchor_missing", provider="web", kind="url", title="T", answerText="hello world", anchors=[anchor(0, 5, "nope")])
create("policy_create_anchor_beyond_answer", provider="web", kind="url", title="T", answerText="hi", anchors=[anchor(0, 5, "hi")])
create("policy_create_anchor_negative_start", provider="web", kind="url", title="T", anchors=[anchor(-1, 2, "hi")])
create("policy_create_anchor_empty_cited", provider="web", kind="url", title="T", anchors=[anchor(0, 0, "")])
create("policy_create_anchor_4096_boundary", provider="web", kind="url", title="T", answerText="x" * 4096, anchors=[anchor(0, 4096, "x" * 4096)])
create("policy_create_anchor_4097_truncated", provider="web", kind="url", title="T", answerText="x" * 4097, anchors=[anchor(0, 4097, "x" * 4097)])
create("policy_create_title_beats_url", provider="web", kind="url", title="Given", url="https://example.com/x")
create("policy_create_url_ipv6", provider="web", kind="url", url="https://[::1]:8080/x")
create("policy_create_url_uppercase", provider="web", kind="url", url="HTTPS://EXAMPLE.COM/Path")
create("policy_create_url_default_port_stripped", provider="web", kind="url", url="http://example.com:80/x")
create("policy_create_url_http_443_kept", provider="web", kind="url", url="http://example.com:443/x")
create("policy_create_url_port_kept", provider="web", kind="url", url="https://example.com:8080/x")
create("policy_create_url_dot_segments", provider="web", kind="url", url="http://h/a/./b/../c")
create("policy_create_url_empty_segments", provider="web", kind="url", url="http://h/a//b")
create("policy_create_url_userinfo_rejected", provider="web", kind="url", url="https://user:pw@example.com/x")
create("policy_create_url_userinfo_title_kept", provider="web", kind="url", url="https://user:pw@example.com/x", title="Kept")
create("policy_create_url_whitespace_title_kept", provider="web", kind="url", url="http://example.com/a b", title="Kept")
create("policy_create_url_trimmed", provider="web", kind="url", url="  https://example.com/x  ")
create("policy_create_url_ftp_rejected", provider="web", kind="url", url="ftp://example.com/x")
create("policy_create_url_query_fragment", provider="web", kind="url", url="http://h/p?q=1#frag")
create("policy_create_url_percent_kept", provider="web", kind="url", url="http://h/%7euser")
create("policy_create_url_unicode_path", provider="web", kind="url", url="http://h/\u00e9")
create("policy_create_url_unicode_host_rejected", provider="web", kind="url", url="http://\u00e9.example.com/", title="Kept")
create("policy_create_url_trailing_dot_host", provider="web", kind="url", url="https://example.com./x")
# --- normalize ------------------------------------------------------------------------------------

w("policy_normalize_roundtrip", {"mode": "normalize", "provider": "web", "kind": "url", "title": "T", "sourceId": "citation_old"})
w("policy_normalize_version_mismatch", {"mode": "normalize", "version": 2, "provider": "web", "kind": "url", "title": "T", "sourceId": "citation_old"})
w(
    "policy_normalize_drops_bad_anchors",
    {
        "mode": "normalize",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_old",
        "answerText": "hello world",
        "anchors": [anchor(0, 5, "world"), anchor(0, 5, "nope")],
    },
)

# --- merge ----------------------------------------------------------------------------------------

A = "a" * 8
w(
    "policy_merge_anchor_replaced",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_same",
        "anchors": [anchor(0, 3, "abc")],
        "incoming": rec(sourceId="citation_same", anchors=[anchor(0, 5, "abcde")]),
    },
)
w(
    "policy_merge_anchor_shorter_ignored",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_same",
        "anchors": [anchor(0, 5, "abcde")],
        "incoming": rec(sourceId="citation_same", anchors=[anchor(0, 3, "abc")]),
    },
)
w(
    "policy_merge_anchor_appended",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_same",
        "anchors": [anchor(0, 3, "abc")],
        "incoming": rec(sourceId="citation_same", anchors=[anchor(9, 12, "xyz")]),
    },
)
w(
    "policy_merge_anchor_duplicate_skipped",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_same",
        "anchors": [anchor(0, 3, "abc")],
        "incoming": rec(sourceId="citation_same", anchors=[anchor(9, 12, "xyz"), anchor(9, 12, "xyz")]),
    },
)
w(
    "policy_merge_anchor_cap",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_same",
        "answerText": "x" * 40,
        "anchors": [anchor(i, i + 1, "x" * 40) if False else anchor(i, i + 1, "x") for i in range(32)],
        "incoming": rec(sourceId="citation_same", anchors=[anchor(35, 36, "x")]),
    },
)
w(
    "policy_merge_title_fallback",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "   ",
        "sourceId": "citation_same",
        "incoming": rec(sourceId="citation_same", title="FromIncoming", excerpt="e", location="loc"),
    },
)
w(
    "policy_merge_url_fallback",
    {
        "mode": "merge",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "sourceId": "citation_same",
        "incoming": rec(sourceId="citation_same", url="https://example.com/x", fileName="f", providerSourceId="ps"),
    },
)
w("policy_merge_requires_same_source", {"mode": "merge", "provider": "web", "kind": "url", "title": "T", "sourceId": "citation_one", "incoming": rec(sourceId="citation_two")})

# --- deduplicate ----------------------------------------------------------------------------------

w("policy_dedup_merges_same_identity", {"mode": "deduplicate", "records": [rec(title="A"), rec(title="A", excerpt="e")]})
w("policy_dedup_distinct", {"mode": "deduplicate", "records": [rec(title="A", providerSourceId="p1"), rec(title="A", providerSourceId="p2")]})
w("policy_dedup_skips_invalid", {"mode": "deduplicate", "records": [rec(title="A"), rec(version=2, title="B")]})
w(
    "policy_dedup_cap_99",
    {"mode": "deduplicate", "records": [rec(title="T{0:03d}".format(i)) for i in range(100)]},
)

# --- strip ----------------------------------------------------------------------------------------

BE = "\ue200"
SEP = "\ue202"
EE = "\ue201"


def strip(name, text):
    w(name, {"mode": "strip", "text": text})


strip("policy_strip_private_envelope", "before " + BE + "cite" + SEP + "doc12" + EE + " after")
strip("policy_strip_filecite_envelope", BE + "filecite" + SEP + "f1" + EE + "text")
strip("policy_strip_envelope_case", BE + "CiTe" + SEP + "x" + EE + BE + "FILECITE" + SEP + "y" + EE)
strip("policy_strip_bare_envelope", BE + "turn1a1" + SEP + "turn2b2" + EE)
strip("policy_strip_bare_envelope_single", "a" + BE + "turn3c4" + EE + "b")
strip("policy_strip_cjk_marker", "x\u3010turn3c4\u3011y")
strip("policy_strip_cjk_marker_case", "x\u3010TURN3C4\u3011y")
strip("policy_strip_plain_artifact", "before cite turn1a1turn2b2 after")
strip("policy_strip_plain_artifact_case", "CiTeTuRn1A1x")
strip("policy_strip_trailing_artifact", "answer text cite turn9z9")
strip("policy_strip_trailing_newline", "answer text cite\n")
strip("policy_strip_trailing_crlf", "answer text cite turn9z9\r\n")
strip("policy_strip_trailing_partial_turn", "answer cite turn")
strip("policy_strip_trailing_just_cite", "answer cite")
strip("policy_strip_leftover_marks", "a" + BE + "b" + EE + "c" + SEP + "d")
strip("policy_strip_unclosed_envelope", BE + "cite" + SEP + "doc without close")
strip("policy_strip_envelope_inner_mark", BE + "cite" + SEP + " a " + BE + " b" + EE)
strip("policy_strip_double_envelope", BE + "cite" + SEP + "a" + EE + BE + "cite" + SEP + "b" + EE)
strip("policy_strip_nested_turns", "n" + BE + "turn1a1" + SEP + "turn2b2" + SEP + "turn3c3" + EE + "n")
strip("policy_strip_artifact_digits_only", "go cite turn12")
strip("policy_strip_artifact_no_digits", "go cite turna")
strip("policy_strip_empty", "")

# --- copy -----------------------------------------------------------------------------------------

w("policy_copy_no_citations", {"mode": "copy", "text": "plain answer  \n\n", "records": []})
w("policy_copy_blank_answer", {"mode": "copy", "text": "", "records": [rec(title="A")]})
w(
    "policy_copy_with_sources",
    {
        "mode": "copy",
        "text": "answer",
        "records": [rec(title="A", url="https://a.example/x", location="Pages 1-2"), rec(title="B", fileName="b.pdf")],
    },
)
w(
    "policy_copy_url_paren_replaced",
    {"mode": "copy", "text": "answer", "records": [rec(title="A", url="https://a.example/x_(y)")]},
)
w(
    "policy_copy_escapes_labels",
    {"mode": "copy", "text": "answer", "records": [rec(title="a[\\]b", providerSourceId="p1")]},
)
w(
    "policy_copy_trim_end",
    {"mode": "copy", "text": "answer \t\n\n", "records": [rec(title="A")]},
)
w(
    "policy_copy_dedup_applied",
    {"mode": "copy", "text": "answer", "records": [rec(title="A"), rec(title="A", excerpt="e")]},
)
w(
    "policy_copy_invalid_dropped",
    {"mode": "copy", "text": "answer", "records": [rec(version=2, title="Gone"), rec(title="A")]},
)

# --- encode ---------------------------------------------------------------------------------------

w("policy_encode_defaults_omitted", {"mode": "encode", "provider": "web", "kind": "url", "title": "T", "sourceId": "citation_x"})
w(
    "policy_encode_all_fields",
    {
        "mode": "encode",
        "version": 2,
        "sourceId": "citation_x",
        "provider": "web",
        "kind": "url",
        "title": "T",
        "url": "https://e/x",
        "fileName": "f.pdf",
        "location": "loc",
        "providerSourceId": "ps",
        "excerpt": "ex",
        "anchors": [anchor(0, 3, "abc")],
    },
)
w(
    "policy_encode_escaping",
    {"mode": "encode", "provider": "web", "kind": "url", "title": "q\"\\\n\b\f\t\r\u0001\u0002\ud83d\ude00\u00e9", "sourceId": "s"},
)
w("policy_encode_empty_title", {"mode": "encode", "provider": "web", "kind": "url", "title": "", "sourceId": "s"})

# --- decode ---------------------------------------------------------------------------------------

w("policy_decode_roundtrip", {"mode": "decode", "raw": json.dumps(rec(title="A", anchors=[anchor(0, 2, "ab")]))})
w("policy_decode_unknown_keys", {"mode": "decode", "raw": json.dumps(rec(title="A", extra="ignored"))})
w("policy_decode_missing_required", {"mode": "decode", "raw": json.dumps({"sourceId": "s", "provider": "web"})})
w("policy_decode_version_string", {"mode": "decode", "raw": json.dumps({"version": "1", "sourceId": "s", "provider": "web", "kind": "url", "title": "T"})})
w("policy_decode_version_float", {"mode": "decode", "raw": json.dumps({"version": 1.5, "sourceId": "s", "provider": "web", "kind": "url", "title": "T"})})
w("policy_decode_url_number", {"mode": "decode", "raw": json.dumps({"sourceId": "s", "provider": "web", "kind": "url", "title": "T", "url": 5})})
w("policy_decode_nullable_null", {"mode": "decode", "raw": json.dumps({"sourceId": "s", "provider": "web", "kind": "url", "title": "T", "url": None})})
w("policy_decode_anchor_wrong_type", {"mode": "decode", "raw": json.dumps({"sourceId": "s", "provider": "web", "kind": "url", "title": "T", "anchors": [{"startIndex": "0", "endIndex": 2, "citedText": "ab"}]})})
w("policy_decode_trailing_garbage", {"mode": "decode", "raw": json.dumps(rec(title="A")) + " trailing"})
w("policy_decode_empty_object", {"mode": "decode", "raw": "{}"})
w("policy_decode_anchor_extra_keys", {"mode": "decode", "raw": json.dumps(rec(title="A", anchors=[{"startIndex": 0, "endIndex": 2, "citedText": "ab", "extra": 1}]))})

# --- safeUrl --------------------------------------------------------------------------------------

W = w


def safe(name, url):
    w(name, {"mode": "safeUrl", "url": url})


safe("policy_safe_url_plain", "https://example.com/x")
safe("policy_safe_url_none", None)
safe("policy_safe_url_empty", "")
safe("policy_safe_url_blank", "   ")
safe("policy_safe_url_trim", "  https://example.com/x  ")
safe("policy_safe_url_inner_space", "https://example.com/a b")
safe("policy_safe_url_newline", "https://example.com/a\nb")
safe("policy_safe_url_upper", "HTTPS://EXAMPLE.COM/Path")
safe("policy_safe_url_userinfo", "https://u@example.com/x")
safe("policy_safe_url_ftp", "ftp://example.com/x")
safe("policy_safe_url_no_scheme", "example.com/x")
safe("policy_safe_url_ipv6", "https://[::1]:8080/x")
safe("policy_safe_url_ipv6_zone", "https://[fe80::1%25eth0]/x")
safe("policy_safe_url_port_80", "http://example.com:80/x")
safe("policy_safe_url_port_443_http", "http://example.com:443/x")
safe("policy_safe_url_port_443_https", "https://example.com:443/x")
safe("policy_safe_url_port_0", "https://example.com:0/x")
safe("policy_safe_url_port_big", "https://example.com:65536/x")
safe("policy_safe_url_percent_bad", "https://example.com/%zz")
safe("policy_safe_url_percent_ok", "https://example.com/%7euser")
safe("policy_safe_url_dot_segments", "https://example.com/a/./b/../c")
safe("policy_safe_url_empty_segments", "https://example.com/a//b")
safe("policy_safe_url_unicode_path", "https://example.com/\u00e9")
safe("policy_safe_url_unicode_host", "https://\u00e9.example.com/")
safe("policy_safe_url_regname_underscore", "https://exa_mple.com/")
safe("policy_safe_url_trailing_dot", "https://example.com./x")
safe("policy_safe_url_dash_end", "https://example-.com/")
safe("policy_safe_url_query_space", "https://example.com/x?a=b c")
safe("policy_safe_url_fragment_space", "https://example.com/x#f g")
safe("policy_safe_url_authority_only", "https://example.com")
safe("policy_safe_url_empty_path", "https://example.com?q=1")
safe("policy_safe_url_opaque", "mailto:a@b.com")
safe("policy_safe_url_relative", "a/b")
safe("policy_safe_url_scheme_only", "https:")
safe("policy_safe_url_long_ascii_ok", "https://example.com/" + "a" * 4076)
safe("policy_safe_url_long_unicode_rejected", "https://example.com/" + "\u00e9" * 4076)
safe("policy_safe_url_over_4096_truncated", "https://example.com/" + "a" * 5000)

# --- matchTitle -----------------------------------------------------------------------------------

w("policy_match_blank_query", {"mode": "matchTitle", "query": "   ", "records": [rec(title="Alpha")]})
w("policy_match_case_insensitive", {"mode": "matchTitle", "query": "alpha", "records": [rec(title="Alpha Beta")]})
w("policy_match_miss", {"mode": "matchTitle", "query": "gamma", "records": [rec(title="Alpha")]})
w("policy_match_second_record", {"mode": "matchTitle", "query": "bee", "records": [rec(title="Alpha"), rec(title="Bee")]})
