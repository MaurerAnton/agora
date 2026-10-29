// Golden trace driver for the DuckDuckGo scraper, used to pin the C++ port.
//
// Runs the REAL Kotlin parsing pieces compiled from the app sources (extracted verbatim by
// `agora_extract duckduckgo`: WebResult, the parsing regexes, parseResults, extractUrl, stripHtml and
// the loop constants) over canned page HTML and prints a canonical trace. The search loop
// itself is a line-for-line transcription of DuckDuckGoScraper.search() with fetchPage replaced
// by scenario-supplied pages (the fetch — query/offset/vqd form values and OkHttp — is the
// transport boundary). The C++ core must reproduce the same trace byte for byte.
//
// Scenario format: {"name", "mode": "parse"|"search", "html", "maxResults", "pages": [{"html"}
// or {"error"}]}. Trace format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package duckduckgo.golden

import com.newoether.agora.api.CAPTCHA_REGEX
import com.newoether.agora.api.DEFAULT_MAX_RESULTS
import com.newoether.agora.api.MAX_PAGES
import com.newoether.agora.api.OFFSET_REGEX
import com.newoether.agora.api.PAGE_SIZE
import com.newoether.agora.api.VQD_REGEX
import com.newoether.agora.api.WebResult
import com.newoether.agora.api.parseResults
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

/** Field-safe escaping for the trace format: no raw separators or control characters survive. */
private fun esc(value: String): String = buildString {
    for (c in value) {
        when (c) {
            '\\' -> append("\\\\")
            '|' -> append("\\p")
            ';' -> append("\\s")
            '\n' -> append("\\n")
            '\r' -> append("\\r")
            '\t' -> append("\\t")
            else -> append(c)
        }
    }
}

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "search",
    val html: String = "",
    val maxResults: Int = DEFAULT_MAX_RESULTS,
    val pages: List<ScenarioPage> = emptyList(),
)

@Serializable
private data class ScenarioPage(
    val html: String? = null,
    val error: String? = null,
)

/** Outcome of a single page fetch (the transport boundary, supplied by the scenario). */
private sealed class PageFetch {
    data class Fetched(val html: String) : PageFetch()
    data class Failed(val message: String) : PageFetch()
}

private sealed class SearchOutcome {
    data class Success(val results: List<WebResult>) : SearchOutcome()
    data class Failure(val type: String, val message: String) : SearchOutcome()
}

private fun fetchFrom(pages: List<ScenarioPage>, index: Int): PageFetch {
    val page = pages.getOrNull(index)
        ?: return PageFetch.Failed("Empty response body from DuckDuckGo.")
    page.error?.let { return PageFetch.Failed(it) }
    val html = page.html ?: return PageFetch.Failed("Empty response body from DuckDuckGo.")
    return PageFetch.Fetched(html)
}

/**
 * Transcription of DuckDuckGoScraper.search(): the page loop, the captcha/vqd/offset handling,
 * the dedup counting and the partial-result fallbacks — verbatim over the real regexes and
 * parseResults.
 */
private fun runSearch(maxResults: Int, pages: List<ScenarioPage>): SearchOutcome {
    val allResults = mutableListOf<WebResult>()
    val seenUrls = mutableSetOf<String>()
    var offset = 0
    var vqd = ""

    for (page in 0 until MAX_PAGES) {
        if (allResults.size >= maxResults) break

        val pageResult = fetchFrom(pages, page)
        when (pageResult) {
            is PageFetch.Fetched -> {
                val html = pageResult.html

                if (CAPTCHA_REGEX.containsMatchIn(html)) {
                    return if (allResults.isEmpty()) {
                        SearchOutcome.Failure(
                            "CAPTCHA",
                            "DuckDuckGo bot detection triggered. Try again later or use a different search provider.",
                        )
                    } else {
                        // Partial results are better than nothing — return what we have.
                        break
                    }
                }

                // Update vqd for subsequent pages (first page provides the token).
                if (page == 0) {
                    vqd = VQD_REGEX.find(html)?.groupValues?.get(1).orEmpty()
                }

                val pageResults = parseResults(html, maxResults = PAGE_SIZE)
                var newInThisPage = 0
                for (r in pageResults) {
                    if (allResults.size >= maxResults) break
                    if (seenUrls.add(r.url)) {
                        allResults.add(r)
                        newInThisPage++
                    }
                }

                if (newInThisPage == 0) break      // No new unique results.
                if (vqd.isEmpty()) break            // No Next Page button.

                val nextOffset = OFFSET_REGEX.find(html)?.groupValues?.get(1)?.toIntOrNull()
                if (nextOffset == null) break       // Malformed pagination form.
                offset = nextOffset
            }

            is PageFetch.Failed -> {
                return if (allResults.isEmpty()) {
                    SearchOutcome.Failure("NETWORK_ERROR", pageResult.message)
                } else {
                    // Partial results — return what we have rather than failing.
                    break
                }
            }
        }
    }

    return if (allResults.isEmpty()) {
        SearchOutcome.Failure(
            "NO_RESULTS",
            "DuckDuckGo returned no results for this query.",
        )
    } else {
        SearchOutcome.Success(allResults)
    }
}

private fun resultLine(out: StringBuilder, result: WebResult) {
    out.append("result|")
        .append(esc(result.title)).append('|')
        .append(esc(result.url)).append('|')
        .append(esc(result.snippet)).append('\n')
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: DuckDuckGoTraceDump <scenario.json> <unusedRoot>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    when (scenario.mode) {
        "parse" -> {
            for (result in parseResults(scenario.html, maxResults = scenario.maxResults)) {
                resultLine(out, result)
            }
        }
        "search" -> {
            when (val outcome = runSearch(scenario.maxResults, scenario.pages)) {
                is SearchOutcome.Success -> {
                    for (result in outcome.results) {
                        resultLine(out, result)
                    }
                    line("response", "success")
                }
                is SearchOutcome.Failure -> {
                    line("response", "error", outcome.type, esc(outcome.message))
                }
            }
        }
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}