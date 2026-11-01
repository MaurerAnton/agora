package com.newoether.agora.data

const val DEFAULT_SUBAGENT_MAX_TOTAL = 16
const val DEFAULT_SUBAGENT_MAX_CONCURRENT = 4
val SUBAGENT_MAX_TOTAL_RANGE = 1..16
val SUBAGENT_MAX_CONCURRENT_RANGE = 1..16

/** Missing values use defaults; every explicit integer clamps to a positive supported limit. */
fun normalizeSubagentMaxTotal(value: Int?): Int =
    (value ?: DEFAULT_SUBAGENT_MAX_TOTAL).coerceIn(SUBAGENT_MAX_TOTAL_RANGE)

/** Concurrency is always bounded by both its own range and the normalized total limit. */
fun normalizeSubagentMaxConcurrent(value: Int?, maxTotal: Int): Int =
    (value ?: DEFAULT_SUBAGENT_MAX_CONCURRENT)
        .coerceIn(SUBAGENT_MAX_CONCURRENT_RANGE)
        .coerceAtMost(normalizeSubagentMaxTotal(maxTotal))
