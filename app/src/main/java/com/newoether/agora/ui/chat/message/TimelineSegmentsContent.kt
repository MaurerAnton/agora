package com.newoether.agora.ui.chat.message

import androidx.compose.animation.core.*
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.snapshots.SnapshotStateMap
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.CitationRecord
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.ui.components.*
import com.newoether.agora.util.noOpBringIntoView

@Composable
internal fun TimelineSegmentsContent(
    segments: List<MessageSegment>,
    detailSegments: List<MessageSegment>,
    message: ChatMessage,
    isStreaming: Boolean,
    generationActive: Boolean,
    groupAdjacentBlocks: Boolean,
    autoExpandActiveGroup: Boolean,
    autoExpansionController: GroupedSegmentAutoExpansionController,
    expandedStates: SnapshotStateMap<String, Boolean>,
    renderContext: ChatMarkdownRenderContext,
    searchHighlight: SearchHighlightSpec?,
    citations: List<CitationRecord>,
    onCitationActivate: (List<CitationRecord>) -> Unit,
    segmentAppearanceRegistry: SegmentAppearanceRegistry,
    onLayoutMutationStarted: (String) -> Unit,
    onLayoutMutationSettled: (String) -> Unit,
    onMediaClick: (List<String>, Int) -> Unit,
    opensDetailSheet: Boolean = false,
    preserveInitialCompactIdentity: Boolean = false,
    onGroupHeaderClick: ((List<Int>) -> Unit)? = null,
    onSegmentClick: (List<Int>) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        var detailIndex = 0
        var answerOffset = 0
        var index = 0
        var previousVisibleWasAnswer = false
        val lastVisibleSegmentIndex = segments.indexOfLast { segment ->
            segment.isVisibleAnswerSegment() || segment.isInfoSegment()
        }
        while (index < segments.size) {
            val seg = segments[index]
            when (seg.type) {
                "answer" -> {
                    if (seg.content.isNotBlank()) {
                        val answerIsStreaming =
                            isStreaming && index == lastVisibleSegmentIndex
                        val citationProjection = citationMarkdownProjection(
                            answerText = seg.content,
                            citations = citationRecordsForAnswerSlice(
                                citations = citations,
                                sliceStart = answerOffset,
                                sliceText = seg.content,
                            ),
                            isStreaming = answerIsStreaming,
                        )
                        val answerSearchHighlight = searchHighlight?.forSourceSlice(
                            sliceStart = answerOffset,
                            sliceLength = seg.content.length,
                        )
                        val answerAppearanceKey =
                            "${segmentAppearanceKey(message.id, index, seg)}:timeline"
                        val answerFadeTracker =
                            segmentAppearanceRegistry.streamingFadeTracker("$answerAppearanceKey:fade")
                        AnimatedTimelineBlockAppearance(
                            animationKey = answerAppearanceKey,
                            appearanceRegistry = segmentAppearanceRegistry,
                            isStreaming = isStreaming,
                        ) {
                            Box(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(top = if (index == 0) 0.dp else 6.dp)
                            ) {
                                CitationTerminalProjectionHost(
                                    animationKey = answerAppearanceKey,
                                    projection = citationProjection,
                                    isStreaming = answerIsStreaming,
                                    onLayoutMutationStarted = onLayoutMutationStarted,
                                    onLayoutMutationSettled = onLayoutMutationSettled,
                                    modifier = Modifier.fillMaxWidth(),
                                ) { presentedProjection, presentedIsStreaming ->
                                    val presentedContent =
                                        presentedProjection?.markdown ?: seg.content
                                    CitationInlineContentHost(
                                        projection = presentedProjection,
                                        onActivate = onCitationActivate,
                                    ) {
                                        CompositionLocalProvider(
                                            LocalSearchHighlightSpec provides answerSearchHighlight,
                                        ) {
                                            StreamingMarkdownMessage(
                                                content = presentedContent,
                                                isStreaming = presentedIsStreaming,
                                                renderContext = renderContext,
                                                modifier = Modifier
                                                    .fillMaxWidth()
                                                    .noOpBringIntoView(),
                                                selectionEnabled = !presentedIsStreaming,
                                                textDeltas = seg.streamingTextDeltas,
                                                fadeTracker = answerFadeTracker,
                                            )
                                        }
                                    }
                                }
                            }
                        }
                        previousVisibleWasAnswer = true
                    }
                    answerOffset += seg.content.length
                    index++
                }
                "thought", "tool", "transcription" -> {
                    if (groupAdjacentBlocks) {
                        val blockSegments = mutableListOf<MessageSegment>()
                        val blockDetailIndices = mutableListOf<Int>()
                        val blockEnd = groupedInfoBlockEndExclusive(segments, index)
                        var blockCursor = index
                        while (blockCursor < blockEnd) {
                            val blockSeg = segments[blockCursor]
                            if (blockSeg.isInfoSegment()) {
                                blockSegments.add(blockSeg)
                                blockDetailIndices.add(detailIndex)
                                detailIndex++
                            }
                            blockCursor++
                        }
                        val imageBoundary =
                            blockSegments.lastOrNull()?.takeIf { it.isImageGenerationSegment() }
                        val imageDetailIndex =
                            blockDetailIndices.lastOrNull().takeIf { imageBoundary != null }
                        val firstDetailIndex = blockDetailIndices.firstOrNull() ?: index
                        val useInitialCompactIdentity =
                            preserveInitialCompactIdentity &&
                                blockDetailIndices.firstOrNull() == 0
                        val expansionKey = if (useInitialCompactIdentity) {
                            message.id
                        } else {
                            groupedSegmentBlockAppearanceKey(message.id, firstDetailIndex)
                        }
                        val cardAppearanceKey = if (useInitialCompactIdentity) {
                            "${compactSegmentBlockAppearanceKey(message.id)}:card"
                        } else {
                            "$expansionKey:card"
                        }
                        val blockTopPaddingExtra =
                            timelineInfoTopPaddingExtra(previousVisibleWasAnswer)
                        CompactSegmentBlock(
                            segs = blockSegments,
                            segmentIndices = blockDetailIndices,
                            message = message,
                            isStreaming = isStreaming,
                            useLiveStatus =
                                isStreaming &&
                                    blockEnd > lastVisibleSegmentIndex,
                            generationActive = generationActive,
                            isCurrentCard = blockEnd > lastVisibleSegmentIndex,
                            expandedStates = expandedStates,
                            expansionKey = expansionKey,
                            cardAppearanceKey = cardAppearanceKey,
                            segmentAppearanceRegistry = segmentAppearanceRegistry,
                            autoExpansionController = autoExpansionController,
                            autoExpansionEnabled = autoExpandActiveGroup,
                            autoExpansionActive = isStreaming && blockEnd == segments.size,
                            collapseForImageBoundary = imageBoundary != null,
                            topPaddingExtra = blockTopPaddingExtra,
                            bottomPaddingExtra = 0.dp,
                            onExpansionStarted = onLayoutMutationStarted,
                            onExpansionSettled = onLayoutMutationSettled,
                            onSegmentClick = { selectedDetailIndex ->
                                onSegmentClick(listOf(selectedDetailIndex))
                            },
                            onHeaderClick = if (opensDetailSheet) {
                                {
                                    (onGroupHeaderClick ?: onSegmentClick)(blockDetailIndices)
                                }
                            } else {
                                null
                            },
                            opensDetailSheet = opensDetailSheet,
                        )
                        if (imageBoundary != null && imageDetailIndex != null) {
                            GeneratedImageThumbnail(
                                segment = imageBoundary,
                                messageId = message.id,
                                detailIndex = imageDetailIndex,
                                isStreaming = isStreaming,
                                segmentAppearanceRegistry = segmentAppearanceRegistry,
                                onMediaClick = onMediaClick,
                            )
                        }
                        previousVisibleWasAnswer = false
                        index = blockEnd
                    } else {
                        val currentDetailIndex = detailIndex
                        detailIndex++
                        val cardTopPaddingExtra =
                            timelineInfoTopPaddingExtra(previousVisibleWasAnswer)
                        val timelineKey = detailSegmentAppearanceKey(
                            message.id,
                            currentDetailIndex,
                            seg,
                        )
                        TimelineInfoSegmentCard(
                            seg = seg,
                            detailSegments = detailSegments,
                            detailIndex = currentDetailIndex,
                            isStreamingContent =
                                isStreaming && index == lastVisibleSegmentIndex,
                            animateAppearance = isStreaming,
                            topPaddingExtra = cardTopPaddingExtra,
                            groupPosition = timelineSegmentGroupPosition(segments, index),
                            endsAtGeneratedImageBoundary = seg.isImageGenerationSegment(),
                            extendIntoMessageInsets = true,
                            cardAnimationKey = "$timelineKey:card",
                            segmentAppearanceRegistry = segmentAppearanceRegistry,
                            onClick = { onSegmentClick(listOf(currentDetailIndex)) },
                        )
                        if (seg.isImageGenerationSegment()) {
                            GeneratedImageThumbnail(
                                segment = seg,
                                messageId = message.id,
                                detailIndex = currentDetailIndex,
                                isStreaming = isStreaming,
                                segmentAppearanceRegistry = segmentAppearanceRegistry,
                                onMediaClick = onMediaClick,
                            )
                        }
                        previousVisibleWasAnswer = false
                        index++
                    }
                }
                else -> {
                    index++
                }
            }
        }
    }
}
