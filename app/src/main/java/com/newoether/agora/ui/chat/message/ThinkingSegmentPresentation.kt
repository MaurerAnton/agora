package com.newoether.agora.ui.chat.message

import android.os.SystemClock
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.res.stringResource
import com.newoether.agora.R
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive

internal const val THINKING_COLLAPSED_WIDTH_ALLOWANCE_DP = 6
internal const val AUXILIARY_CARD_START_EXTENSION_DP = 4

@Composable
internal fun segmentDetailTitle(
    seg: MessageSegment,
    detailSegments: List<MessageSegment>,
    detailIndex: Int,
): String = when (seg.type) {
    "tool" -> toolDisplayName(seg)
    "transcription" -> transcriptionLabel(detailSegments, detailIndex)
    else -> stringResource(R.string.tool_thinking)
}

@Composable
private fun thinkingDurationBreakdownTitle(
    seconds: Int,
    live: Boolean,
    toolCount: Int? = null,
): String {
    val hours = seconds / 3_600
    val minutes = (seconds % 3_600) / 60
    val remainingSeconds = seconds % 60
    return when {
        live && hours > 0 -> stringResource(
            R.string.thinking_for_hours_ellipsis,
            hours,
            minutes,
            remainingSeconds,
        )
        live && seconds >= 60 -> stringResource(
            R.string.thinking_for_minutes_ellipsis,
            minutes,
            remainingSeconds,
        )
        live -> stringResource(R.string.thinking_for_seconds_ellipsis, seconds)
        toolCount != null && hours > 0 -> stringResource(
            R.string.thought_for_hours_called_tools,
            hours,
            minutes,
            remainingSeconds,
            toolCount,
        )
        toolCount != null && seconds >= 60 -> stringResource(
            R.string.thought_for_minutes_called_tools,
            minutes,
            remainingSeconds,
            toolCount,
        )
        toolCount != null -> stringResource(
            R.string.thought_for_seconds_called_tools,
            seconds,
            toolCount,
        )
        hours > 0 -> stringResource(
            R.string.thought_for_hours,
            hours,
            minutes,
            remainingSeconds,
        )
        seconds >= 60 -> stringResource(
            R.string.thought_for_minutes,
            minutes,
            remainingSeconds,
        )
        else -> stringResource(R.string.thought_for_seconds, seconds)
    }
}

@Composable
internal fun thoughtDurationTitle(thoughtMs: Long?, toolCount: Int): String {
    if (thoughtMs == null) return if (toolCount > 0) {
        stringResource(R.string.thought_for_a_while_called_tools, toolCount)
    } else stringResource(R.string.thought_for_a_while)
    return thinkingDurationBreakdownTitle(
        seconds = (thoughtMs / 1_000L).toInt(),
        live = false,
        toolCount = toolCount.takeIf { it > 0 },
    )
}

@Composable
internal fun compactSegmentTitle(
    segs: List<MessageSegment>,
    message: ChatMessage,
    useLiveStatus: Boolean,
): String {
    val lastSeg = segs.lastOrNull() ?: return ""
    val isLastTool = lastSeg.type == "tool"
    val isToolInProgress = useLiveStatus && isLastTool && ToolPresentationResolver.resolve(lastSeg).isActive
    val isThinking = useLiveStatus && message.status == MessageStatus.THINKING
    val isToolCalling = useLiveStatus && message.status == MessageStatus.TOOL_CALLING
    val isTranscribing = useLiveStatus && message.status == MessageStatus.TRANSCRIBING
    val toolCount = segs.count { it.type == "tool" }
    val thoughtMs = thoughtDurationMs(segs, fallbackMs = message.thoughtTimeMs)
    val hasThought = segs.any { it.type == "thought" }
    return when {
        isThinking -> message.thoughtTitle ?: stringResource(R.string.thinking_ellipsis)
        isTranscribing -> message.thoughtTitle ?: stringResource(R.string.transcription_ellipsis)
        isToolCalling || isToolInProgress ->
            if (segs.any { it.type == "transcription" }) {
                // Tool-result image transcription streams while the message is TOOL_CALLING;
                // the group title must stay the transcription label, not the tool name
                // (the transcription segment would otherwise fall back to the generic "Tool").
                transcriptionLabel(
                    segs,
                    segs.indexOfLast { it.type == "transcription" },
                )
            } else {
                toolDisplayName(lastSeg)
            }
        hasThought -> thoughtDurationTitle(thoughtMs, toolCount)
        toolCount > 0 -> stringResource(R.string.called_n_tools, toolCount)
        segs.any { it.type == "transcription" } -> transcriptionLabel(
            segs,
            segs.indexOfLast { it.type == "transcription" },
        )
        else -> ""
    }
}

@Composable
internal fun compactSegmentDisplayTitle(
    segs: List<MessageSegment>,
    message: ChatMessage,
    useLiveStatus: Boolean,
): String {
    val isThinking = useLiveStatus &&
        message.status == MessageStatus.THINKING &&
        segs.any { it.type == "thought" }
    val thoughtMs = thoughtDurationMs(segs, fallbackMs = message.thoughtTimeMs)
    // Unknown native duration is not zero and does not authorize a local elapsed-time claim.
    if (thoughtMs == null) return compactSegmentTitle(segs, message, useLiveStatus)
    val thinkingPlaceholder = stringResource(R.string.thinking_ellipsis)
    val usesDefaultThinkingTitle =
        message.thoughtTitle.isNullOrBlank() || message.thoughtTitle == thinkingPlaceholder
    val liveThoughtMs by produceState(
        initialValue = thoughtMs,
        isThinking,
        thoughtMs,
    ) {
        val baselineMs = thoughtMs
        value = baselineMs
        if (isThinking) {
            val baselineRealtimeMs = SystemClock.elapsedRealtime()
            while (isActive) {
                value = baselineMs + (SystemClock.elapsedRealtime() - baselineRealtimeMs)
                delay(1_000L)
            }
        }
    }
    return if (isThinking && usesDefaultThinkingTitle) {
        thinkingDurationBreakdownTitle(
            seconds = (liveThoughtMs / 1_000L).toInt(),
            live = true,
        )
    } else {
        compactSegmentTitle(segs, message, useLiveStatus)
    }
}
