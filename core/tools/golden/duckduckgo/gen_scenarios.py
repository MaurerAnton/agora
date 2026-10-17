#!/usr/bin/env python3
"""Generates the scenario corpus for the DuckDuckGo golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def parse_case(name, html, max_results=5):
    ALL.append({"name": name, "mode": "parse", "html": html, "maxResults": max_results})


def search_case(name, pages, max_results=5):
    normalized = [{"html": page_html} if isinstance(page_html, str) else page_html
                  for page_html in pages]
    ALL.append({"name": name, "mode": "search", "maxResults": max_results,
                "pages": normalized})


def row(url, title, snippet=None, quote='"'):
    """One DDG result: the result-link anchor followed by its snippet cell."""
    anchor = (f"<a {quote}class={quote}result-link{quote} "
              f"href={quote}{url}{quote}>{title}</a>")
    if snippet is None:
        return anchor
    return anchor + f"<td {quote}class={quote}result-snippet{quote}>{snippet}</td>"


def page(*rows, vqd=None, offset=None, captcha=False, extra=""):
    html = "<html><table>" + "".join(rows) + "</table>"
    if vqd is not None:
        html += f'<form><input name="vqd" value="{vqd}"></form>'
    if offset is not None:
        html += f'<form><input name="s" value="{offset}"></form>'
    if captcha:
        html += '<div class="anomaly-modal">challenge-form</div>'
    return html + extra


def redirect(href):
    return f"//duckduckgo.com/l/?{href}"


# ---- parseResults --------------------------------------------------------------------------------
parse_case("parse_single_result",
           page(row("https://example.com/a", "Alpha", "First snippet.")))
parse_case("parse_entities_sequential",
           page(row("http://x/", "Fish &amp; Chips &lt;hot&gt; &quot;q&quot; &#x27;s&#39;&nbsp;x",
                    "A &amp;lt; B &amp;amp; C &gt; D")))
parse_case("parse_double_unescape",
           page(row("http://x/", "A &amp;lt; B")))
parse_case("parse_tags_stripped",
           page(row("http://x/", "<b>Bold</b> plain <i>it</i>", "<span>snip</span> tail")))
parse_case("parse_nested_anchor_text",
           page(row("http://x/", "A<span>B</span>")))
parse_case("parse_mismatched_class_quotes",
           '<a class=\'result-link" href="http://odd.example/">Odd</a>')
parse_case("parse_missing_href",
           '<a class="result-link">No href here</a>')
parse_case("parse_blank_title_filtered",
           '<a class="result-link" href="http://x/">&nbsp;   </a>')
parse_case("parse_max_results_cap",
           page(row("http://x/1", "One"), row("http://x/2", "Two"), row("http://x/3", "Three")),
           max_results=1)
parse_case("parse_uddg_percent_encoded",
           page(row(redirect("uddg=https%3A%2F%2Fexample.com%2Fa%20b&rut=xyz"), "Wrapped")))
parse_case("parse_uddg_plus_decoded",
           page(row(redirect("uddg=https%3A%2F%2Fx.example%2Fa+b"), "Plus")))
parse_case("parse_protocol_relative",
           page(row("//example.org/path", "Proto")))
parse_case("parse_absolute_unchanged",
           page(row("https://keep.example/path?q=1", "Kept")))
parse_case("parse_snippet_missing_pair",
           row("http://x/1", "One", "Snip one") + row("http://x/2", "Two"))
parse_case("parse_single_quotes_everywhere",
           "<a class='result-link' href='http://sq.example/'>Single</a>"
           "<td class='result-snippet'>Snip sq</td>")

# ---- search() loop ------------------------------------------------------------------------------
search_case("search_no_results", [page("<p>nothing here</p>")])
search_case("search_first_page_no_vqd", [page(row("http://x/1", "One"))])
search_case("search_success_single_page",
            [page(row("http://x/1", "One", "s1"), row("http://x/2", "Two", "s2"),
                  vqd="vqd-token", offset="10")])
search_case("search_captcha_first_page",
            [page(row("http://x/1", "One"), captcha=True)])
search_case("search_captcha_partial",
            [page(row("http://x/1", "One", "s1"), row("http://x/2", "Two", "s2"),
                  vqd="tok", offset="10"),
             page(row("http://x/9", "Nine"), captcha=True)])
search_case("search_network_first_missing_page", [])
search_case("search_network_explicit",
            [{"error": "DuckDuckGo returned HTTP 503."}])
search_case("search_network_partial",
            [page(row("http://x/1", "One", "s1"), vqd="tok", offset="10"),
             {"error": "Network error: timeout"}])
search_case("search_pagination_two_pages",
            [page(row("http://x/1", "One"), row("http://x/2", "Two"), vqd="tok", offset="10"),
             page(row("http://x/3", "Three"), row("http://x/4", "Four"),
                  vqd="tok", offset="20")],
            max_results=20)
search_case("search_dedup_overlap_and_missing_offset",
            [page(row("http://x/1", "One"), row("http://x/2", "Two"), vqd="tok", offset="10"),
             page(row("http://x/2", "Two again"), row("http://x/3", "Three"))],
            max_results=20)
search_case("search_no_new_results_breaks",
            [page(row("http://x/1", "One"), row("http://x/2", "Two"), vqd="tok", offset="10"),
             page(row("http://x/1", "One"), row("http://x/2", "Two"))],
            max_results=20)
search_case("search_offset_malformed_breaks",
            [page(row("http://x/1", "One"), vqd="tok", offset="not-a-number")])
search_case("search_max_results_top_break",
            [page(row("http://x/1", "One"), row("http://x/2", "Two"),
                  row("http://x/3", "Three"), vqd="tok", offset="10"),
             page(row("http://x/4", "Four"))],
            max_results=1)
search_case("search_vqd_only_from_first_page",
            [page(row("http://x/1", "One"), vqd="first-token", offset="10"),
             page(row("http://x/2", "Two"), offset="20"),
             page(row("http://x/3", "Three"), offset="30")],
            max_results=20)


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
