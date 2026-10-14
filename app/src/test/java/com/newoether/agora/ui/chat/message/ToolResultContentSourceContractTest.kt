package com.newoether.agora.ui.chat.message

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ToolResultContentSourceContractTest {
    @Test
    fun `Web Search results keep semantic tiers and use rounded full-row link ripples`() {
        val source = source(locateMainSourceRoot(), "ToolResultContent.kt")
        val segmentDetailSheet = source(locateMainSourceRoot(), "SegmentDetailSheet.kt")
        val webSearch = source
            .substringAfter("private fun WebSearchResult(")
            .substringBefore("private fun IndexedCodeLine(")

        assertTrue(webSearch.contains("style = ChatType.body,"))
        assertTrue(webSearch.contains("fontWeight = FontWeight.SemiBold"))
        assertTrue(webSearch.contains("style = ChatType.thoughtBody,"))
        assertTrue(webSearch.contains("style = ChatType.micro,"))
        assertTrue(webSearch.contains("HorizontalDivider("))
        assertFalse(webSearch.contains(".background("))
        assertTrue(webSearch.contains("val uriHandler = LocalUriHandler.current"))
        assertTrue(webSearch.contains("val resultShape = RoundedCornerShape(12.dp)"))
        assertTrue(webSearch.contains("val safeUrl = remember(url) { CitationPolicy.safeHttpUrl(url) }"))
        assertTrue(webSearch.contains("enabled = safeUrl != null"))
        assertTrue(webSearch.contains("runCatching { uriHandler.openUri(destination) }"))
        assertTrue(
            source.contains("internal fun toolDetailHorizontalPadding(segment: MessageSegment): Dp"),
        )
        assertTrue(source.contains("ToolKind.WEB_SEARCH -> 16.dp"))
        assertTrue(source.contains("else -> 24.dp"))
        assertFalse(source.contains("segment.isImageGenerationSegment() -> 0.dp"))
        assertTrue(
            segmentDetailSheet.contains(
                ".padding(horizontal = toolDetailHorizontalPadding(detailSeg))",
            ),
        )
        assertTrue(
            segmentDetailSheet.contains(
                ".padding(horizontal = toolDetailHorizontalPadding(seg))",
            ),
        )
        val toolDetail = source
            .substringAfter("internal fun ToolDetailContent(")
            .substringBefore("internal fun toolDetailHorizontalPadding(")
        assertTrue(
            toolDetail.contains(
                "val contentAlignmentModifier = if (presentation.kind == ToolKind.WEB_SEARCH)",
            ),
        )
        assertFalse(
            toolDetail.contains(
                "segment.isImageGenerationSegment() -> Modifier.padding(horizontal = 24.dp)",
            ),
        )

        val clipPosition = webSearch.indexOf(".clip(resultShape)")
        val clickablePosition = webSearch.indexOf(".clickable(")
        val paddingPosition = webSearch.indexOf(
            ".padding(horizontal = 8.dp, vertical = 12.dp)",
        )
        assertTrue(clipPosition >= 0)
        assertTrue(clickablePosition > clipPosition)
        assertTrue(paddingPosition > clickablePosition)

        val titlePosition = webSearch.indexOf("text = title")
        val snippetPosition = webSearch.indexOf("text = snippet")
        val urlPosition = webSearch.indexOf("text = url")
        assertTrue(titlePosition >= 0)
        assertTrue(snippetPosition > titlePosition)
        assertTrue(urlPosition > snippetPosition)
    }

    @Test
    fun `Generated image thumbnail keeps ordered fixed lifecycle presentation`() {
        val root = locateMainSourceRoot()
        val source = source(root, "ToolResultContent.kt")
        val timeline = source(root, "MessageItemTimeline.kt") +
            source(root, "TimelineSegmentsContent.kt")
        val assistant = source(root, "AssistantMessageContent.kt")
        val detailSheet = source(root, "SegmentDetailSheet.kt")
        val generatedSource = source(root, "GeneratedImageThumbnail.kt")
        val thumbnail = generatedSource
            .substringAfter("internal fun GeneratedImageThumbnail(")
            .substringBefore("private fun GeneratedImagePendingDots(")
        val pending = generatedSource
            .substringAfter("private fun GeneratedImagePendingDots(")
            .substringBefore("internal fun toolDetailHorizontalPadding(")

        assertTrue(thumbnail.contains("generatedImageAppearanceKey(messageId, detailIndex)"))
        assertTrue(thumbnail.contains("rememberSegmentAppearance("))
        assertTrue(thumbnail.contains("generationLifecycleAppearanceModifier("))
        assertTrue(thumbnail.contains("initialScale = SEGMENT_ENTER_INITIAL_SCALE"))
        assertTrue(thumbnail.contains("contentAlignment = Alignment.TopStart"))
        assertTrue(thumbnail.contains("val thumbnailSize = 300.dp"))
        assertTrue(thumbnail.contains("thumbnailSize.roundToPx().coerceAtLeast(1)"))
        assertTrue(thumbnail.contains("image?.path?.let { path ->"))
        assertTrue(thumbnail.contains("ImageRequest.Builder(context)"))
        assertTrue(thumbnail.contains(".data(path)"))
        assertTrue(thumbnail.contains(".size(thumbnailSizePx, thumbnailSizePx)"))
        assertTrue(thumbnail.contains("rememberAsyncImagePainter(model = imageRequest)"))
        assertFalse(thumbnail.contains("rememberAsyncImagePainter(model = image?.path)"))
        assertTrue(thumbnail.contains(".size(thumbnailSize)"))
        assertTrue(thumbnail.contains("Crossfade("))
        assertTrue(thumbnail.contains("imagePainter.state.toMediaLoadPresentation()"))
        assertTrue(thumbnail.contains("MEDIA_STATE_CROSSFADE_MILLIS"))
        assertTrue(thumbnail.contains("modifier = Modifier.fillMaxSize()"))
        assertTrue(thumbnail.contains("Icons.Default.BrokenImage"))
        assertTrue(thumbnail.contains("contentScale = ContentScale.Crop"))
        assertTrue(thumbnail.contains("onMediaClick(paths, 0)"))

        assertTrue(pending.contains("LocalAgoraMotionPolicy.current.allowContinuousMotion"))
        assertTrue(pending.contains("Offset(0.5f, 0.5f)"))
        assertTrue(pending.contains("val dotFieldInsetPx = with(density) { 16.dp.toPx() }"))
        assertTrue(pending.contains("val anchorInsetPx = with(density) { 32.dp.toPx() }"))
        assertTrue(pending.contains("if (!allowContinuousMotion)"))
        assertTrue(pending.contains("anchorStart = Offset(0.5f, 0.5f)"))
        assertTrue(pending.contains("anchorTarget = anchorStart"))
        assertTrue(pending.contains("while (true)"))
        assertTrue(pending.contains("x = random.nextFloat()"))
        assertTrue(pending.contains("y = random.nextFloat()"))
        assertTrue(pending.contains("progress.animateTo("))
        assertTrue(pending.contains("durationMillis = 1_300"))
        assertFalse(pending.contains("durationMillis = 2_600"))
        assertTrue(pending.contains("val maxRadiusPx = with(density) { 3.9.dp.toPx() }"))
        assertFalse(pending.contains("val maxRadiusPx = with(density) { 2.6.dp.toPx() }"))
        assertTrue(pending.contains("Canvas(modifier = modifier)"))
        assertTrue(pending.contains("val dotLeft = dotFieldInsetPx"))
        assertTrue(pending.contains("val dotRight = (size.width - dotFieldInsetPx)"))
        assertTrue(pending.contains("val dotCenterLeft = (dotLeft + maxRadiusPx)"))
        assertTrue(pending.contains("val dotCenterRight = (dotRight - maxRadiusPx)"))
        assertTrue(pending.contains("val columnCount = ((dotFieldWidth / spacingPx).toInt() + 1)"))
        assertTrue(pending.contains("val rowCount = ((dotFieldHeight / spacingPx).toInt() + 1)"))
        assertTrue(pending.contains("val columnStep = if (columnCount > 1)"))
        assertTrue(pending.contains("val rowStep = if (rowCount > 1)"))
        assertTrue(pending.contains("repeat(rowCount) { row ->"))
        assertTrue(pending.contains("repeat(columnCount) { column ->"))
        assertTrue(pending.contains("val anchorLeft = anchorInsetPx"))
        assertTrue(pending.contains("val anchorRight = (size.width - anchorInsetPx)"))
        assertTrue(pending.contains("val influenceDistancePx = with(density) { 150.dp.toPx() }"))
        assertTrue(
            pending.contains(
                "val distanceScale = (1f - distance / influenceDistancePx).coerceIn(0f, 1f)",
            ),
        )
        assertTrue(pending.contains("val influence = distanceScale * distanceScale"))
        assertFalse(pending.contains("maxDistance"))
        assertTrue(pending.contains("radius = minRadiusPx +"))
        assertFalse(pending.contains("center = anchorPx"))

        val imageResults = source
            .substringAfter("private fun ToolImageResults(")
            .substringBefore("private fun ToolSectionLabel(")
        assertTrue(source.contains("squareCrop = segment.isImageGenerationSegment()"))
        assertTrue(imageResults.contains("val previewHeight = if (squareCrop)"))
        assertTrue(imageResults.contains("maxWidth"))
        assertTrue(
            imageResults.contains(
                "val previewModifier = Modifier.fillMaxWidth().height(previewHeight)",
            ),
        )
        assertTrue(imageResults.contains("alignment = Alignment.Center"))
        assertTrue(
            imageResults.contains(
                "contentScale = if (squareCrop) ContentScale.Crop else ContentScale.Fit",
            ),
        )
        assertFalse(imageResults.contains("minOf(maxWidth, maxPreviewHeight)"))
        assertFalse(imageResults.contains("maxPreviewHeight"))
        assertFalse(detailSheet.contains("BoxWithConstraints(modifier = Modifier.fillMaxSize())"))
        assertFalse(detailSheet.contains("imagePreviewMaxHeight"))

        assertTrue(timeline.contains("val blockEnd = groupedInfoBlockEndExclusive(segments, index)"))
        assertTrue(timeline.contains("preserveInitialCompactIdentity"))
        assertTrue(timeline.contains("expansionKey = if (useInitialCompactIdentity)"))
        assertTrue(timeline.contains("compactSegmentBlockAppearanceKey(message.id)"))
        assertTrue(timeline.contains("collapseForImageBoundary = imageBoundary != null"))
        assertTrue(timeline.contains("GENERATED_IMAGE_BOUNDARY_GAP_DP = 8"))
        assertTrue(timeline.contains("if (collapseForImageBoundary) {"))
        assertTrue(timeline.contains("GENERATED_IMAGE_BOUNDARY_GAP_DP.dp"))
        assertTrue(
            timeline.contains("endsAtGeneratedImageBoundary = seg.isImageGenerationSegment()"),
        )
        assertTrue(timeline.contains("if (endsAtGeneratedImageBoundary)"))
        assertTrue(timeline.contains("else segmentGroupBottomPadding(groupPosition)"))
        assertTrue(timeline.contains("!collapseImageBoundaryOnAppearance &&"))
        assertTrue(timeline.contains("collapseImageBoundaryOnAppearance || !allowSpatialTransitions"))
        assertTrue(timeline.contains("allowSpatialTransitions && !collapseImageBoundaryOnAppearance"))
        assertTrue(timeline.contains("collapseImageBoundaryOnAppearance -> EnterTransition.None"))
        assertTrue(timeline.contains("collapseImageBoundaryOnAppearance -> ExitTransition.None"))
        assertTrue(timeline.contains("onGroupHeaderClick: ((List<Int>) -> Unit)? = null"))
        assertTrue(timeline.contains("(onGroupHeaderClick ?: onSegmentClick)(blockDetailIndices)"))
        assertTrue(timeline.contains("GeneratedImageThumbnail("))
        assertTrue(timeline.contains("onMediaClick = onMediaClick"))
        assertTrue(assistant.contains("val hasImageGenerationBoundary ="))
        assertTrue(assistant.contains("hasImageGenerationBoundary &&\n                        mergedSegments.none"))
        assertTrue(assistant.contains("useTimelineSegments =\n                    hasImageGenerationBoundary ||"))
        assertTrue(assistant.contains("message.images.isNotEmpty()"))
    }

    @Test
    fun `Completed wait for job uses the shell exit summary`() {
        val source = source(locateMainSourceRoot(), "MessageItemToolLabels.kt")
        val completedSummary = source
            .substringAfter("private fun completedSummary(")

        assertTrue(
            completedSummary.contains(
                "ToolKind.SHELL_JOB_WAIT -> shellToolSummary(presentation)",
            ),
        )
        assertFalse(completedSummary.contains("tool_waited_shell_job"))
    }

    private fun source(root: File, name: String): String =
        File(root, "com/newoether/agora/ui/chat/message/$name")
            .readText()
            .replace("\r\n", "\n")

    private fun locateMainSourceRoot(): File {
        var directory = File(requireNotNull(System.getProperty("user.dir"))).absoluteFile
        repeat(8) {
            listOf(
                File(directory, "app/src/main/java"),
                File(directory, "src/main/java"),
            ).firstOrNull(File::isDirectory)?.let { return it }
            directory = directory.parentFile ?: error("Reached filesystem root")
        }
        error("Unable to locate the main Java source directory")
    }
}
