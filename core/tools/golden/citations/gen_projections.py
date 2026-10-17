# Generates the citation projection scenarios (Anthropic toAnthropicCitationRecord and
# Gemini toGeminiCitations).
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
base = os.path.join(HERE, "scenarios")
os.makedirs(base, exist_ok=True)


def w(name, body):
    with open(os.path.join(base, name + ".json"), "w") as f:
        json.dump({"name": name, **body}, f, indent=2, ensure_ascii=True)
        f.write("\n")


def anth(name, citation, text="hello world", start=10):
    w(name, {"mode": "anthropicCitation", "citation": json.dumps(citation), "text": text, "answerStartIndex": start})


def gem(name, metadata, text="hello world"):
    w(name, {"mode": "geminiCitations", "metadata": json.dumps(metadata), "text": text})


# --- anthropic ------------------------------------------------------------------------------------

anth(
    "proj_anthropic_web_full",
    {
        "type": "web_search_result_location",
        "url": "https://example.com/x",
        "title": "Example",
        "encrypted_index": "enc123",
        "cited_text": "quote",
    },
)
anth("proj_anthropic_web_type_case", {"type": "WEB_SEARCH_RESULT_LOCATION", "url": "https://example.com/x", "title": "T"})
anth("proj_anthropic_no_type", {"url": "https://example.com/x", "title": "T"})
anth("proj_anthropic_empty_type", {"type": "", "url": "https://example.com/x", "title": "T"})
anth("proj_anthropic_web_no_title", {"type": "web_search_result_location", "url": "https://example.com/x"})
anth("proj_anthropic_web_bad_url_no_title", {"type": "web_search_result_location", "url": "not a url"})
anth("proj_anthropic_web_bad_url_with_title", {"type": "web_search_result_location", "url": "not a url", "title": "T"})
anth("proj_anthropic_web_empty_answer", {"type": "web_search_result_location", "url": "https://example.com/x", "title": "T"}, text="")
anth("proj_anthropic_web_utf16_anchor", {"type": "web_search_result_location", "url": "https://e/x", "cited_text": "h\u00e9llo"}, text="h\u00e9llo", start=5)
anth(
    "proj_anthropic_search_url_source",
    {
        "type": "search_result_location",
        "source": "https://s.example/x",
        "title": "S",
        "search_result_index": 2,
        "start_block_index": 1,
        "end_block_index": 3,
        "cited_text": "q",
    },
)
anth(
    "proj_anthropic_search_document_source",
    {"type": "search_result_location", "source": "doc:1", "title": "D", "start_block_index": 1, "end_block_index": 3},
)
anth("proj_anthropic_search_index_only", {"type": "search_result_location", "search_result_index": 4})
anth("proj_anthropic_search_no_psid", {"type": "search_result_location", "title": ""})
anth("proj_anthropic_search_blank_source", {"type": "search_result_location", "source": "  ", "search_result_index": 1})
anth(
    "proj_anthropic_char_location",
    {"type": "char_location", "document_index": 0, "document_title": "Doc", "start_char_index": 3, "end_char_index": 9, "cited_text": "DQ"},
)
anth("proj_anthropic_char_start_only", {"type": "char_location", "start_char_index": 3, "document_title": "Doc"})
anth("proj_anthropic_char_end_only", {"type": "char_location", "end_char_index": 3, "document_title": "Doc"})
anth("proj_anthropic_char_no_range", {"type": "char_location", "document_title": "Doc"})
anth(
    "proj_anthropic_page_location",
    {"type": "page_location", "document_index": 1, "document_title": "Doc", "start_page_number": 2, "end_page_number": 4},
)
anth("proj_anthropic_content_block_location", {"type": "content_block_location", "document_index": 2, "start_block_index": 5, "end_block_index": 6})
anth("proj_anthropic_document_no_index", {"type": "page_location", "document_title": "Doc", "start_page_number": 1})
anth("proj_anthropic_wrong_type_int", {"type": 5, "url": "https://e/x"})
anth("proj_anthropic_wrong_cited_text", {"type": "char_location", "cited_text": 123})
anth("proj_anthropic_index_string", {"type": "char_location", "document_index": "3", "document_title": "Doc"})
anth("proj_anthropic_index_float", {"type": "char_location", "document_index": 3.5, "document_title": "Doc"})
anth("proj_anthropic_unknown_fields", {"type": "char_location", "document_title": "Doc", "extra": {"deep": [1, 2]}, "start_char_index": 0, "end_char_index": 2})
anth("proj_anthropic_element_string", "abc")
anth("proj_anthropic_element_number", 5)
anth("proj_anthropic_element_array", [1, 2])
anth("proj_anthropic_element_null", None)
anth("proj_anthropic_title_number", {"type": "web_search_result_location", "url": "https://e/x", "title": 123})
anth("proj_anthropic_provider_source_id_trim", {"type": "web_search_result_location", "url": "https://e/x", "title": "T", "encrypted_index": "  e1  "})

# --- gemini ---------------------------------------------------------------------------------------

gem(
    "proj_gemini_basic",
    {
        "groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}],
        "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}],
    },
)
gem(
    "proj_gemini_snake_keys",
    {
        "grounding_chunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}],
        "grounding_supports": [{"segment": {"start_index": 0, "end_index": 5}, "grounding_chunk_indices": [0]}],
    },
)
gem("proj_gemini_no_chunks", {"groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]})
gem("proj_gemini_no_supports", {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}]})
gem("proj_gemini_segment_missing", {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"groundingChunkIndices": [0]}]})
gem("proj_gemini_index_out_of_range", {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [1]}]})
gem("proj_gemini_index_negative", {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [-1]}]})
gem("proj_gemini_index_duplicate", {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0, 0]}]})
gem("proj_gemini_chunk_no_web", {"groundingChunks": [{"other": 1}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]})
gem(
    "proj_gemini_support_not_object",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [5, {"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_utf8_multibyte",
    {
        "groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}],
        "groundingSupports": [{"segment": {"startIndex": 1, "endIndex": 3}, "groundingChunkIndices": [0]}],
    },
    text="h\u00e9llo",
)
gem(
    "proj_gemini_utf8_emoji",
    {
        "groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}],
        "groundingSupports": [{"segment": {"startIndex": 1, "endIndex": 5}, "groundingChunkIndices": [0]}],
    },
    text="a\U0001f600b",
)
gem(
    "proj_gemini_byte_beyond",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 99}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_byte_reversed",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 5, "endIndex": 3}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_byte_negative",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": -1, "endIndex": 3}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_start_default",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"endIndex": 5}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_text_mismatch",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5, "text": "wrong"}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_text_match",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5, "text": "hello"}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_uri_null_title",
    {"groundingChunks": [{"web": {"uri": None, "title": "T"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_uri_null_no_title",
    {"groundingChunks": [{"web": {"uri": None}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_title_number",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": 123}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_title_blank",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "  "}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]},
)
gem(
    "proj_gemini_index_quoted",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": ["0"]}]},
)
gem(
    "proj_gemini_index_fractional",
    {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0.5]}]},
)
gem(
    "proj_gemini_two_supports_same_chunk",
    {
        "groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}],
        "groundingSupports": [
            {"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]},
            {"segment": {"startIndex": 6, "endIndex": 11}, "groundingChunkIndices": [0]},
        ],
    },
)
gem(
    "proj_gemini_camel_wins_over_snake",
    {
        "groundingChunks": [{"web": {"uri": "https://camel.example/x", "title": "C"}}],
        "grounding_chunks": [{"web": {"uri": "https://snake.example/x", "title": "S"}}],
        "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}],
    },
)
gem("proj_gemini_empty_answer", {"groundingChunks": [{"web": {"uri": "https://g.example/x", "title": "G"}}], "groundingSupports": [{"segment": {"startIndex": 0, "endIndex": 5}, "groundingChunkIndices": [0]}]}, text="")
gem("proj_gemini_metadata_not_object", [1, 2])
w("proj_gemini_metadata_malformed", {"mode": "geminiCitations", "metadata": "{broken", "text": "hello"})
