package com.newoether.agora.ui.chat.message

import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import com.newoether.agora.ui.motion.MotionAwareCircularProgressIndicator as CircularProgressIndicator
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BrokenImage
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.res.stringResource
import com.newoether.agora.R
import com.newoether.agora.model.CitationPolicy
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.ToolImageAttachment
import com.newoether.agora.ui.chat.MEDIA_LOADING_INDICATOR_STROKE_WIDTH
import com.newoether.agora.ui.chat.MEDIA_STATE_CROSSFADE_MILLIS
import com.newoether.agora.ui.chat.MediaLoadPresentation
import com.newoether.agora.ui.theme.ChatType
import com.newoether.agora.ui.theme.MonoFamily
import com.newoether.agora.util.NoAutoScrollSelectionContainer
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull

internal val LocalToolImageLoader =
    staticCompositionLocalOf<(suspend (String, String) -> ToolImageAttachment)?> { null }

@Composable
internal fun ToolDetailContent(
    segment: MessageSegment,
    onMediaClick: (List<String>, Int) -> Unit,
) {
    val presentation = ToolPresentationResolver.resolve(segment)
    val contentAlignmentModifier = if (presentation.kind == ToolKind.WEB_SEARCH) {
        Modifier.padding(horizontal = 8.dp)
    } else {
        Modifier
    }
    val args = presentation.rawArguments
    if (!args.isNullOrBlank() && args != "{}") {
        Column(modifier = contentAlignmentModifier.fillMaxWidth()) {
            ToolSectionLabel(stringResource(R.string.arguments_label))
            Spacer(Modifier.height(5.dp))
            JsonOrPlainView(args)
            Spacer(Modifier.height(18.dp))
        }
    }

    if (presentation.kind == ToolKind.MCP) {
        Column(modifier = contentAlignmentModifier.fillMaxWidth()) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MetaPill(text = "MCP", emphasized = true)
                presentation.device
                    ?.takeIf(String::isNotBlank)
                    ?.let { MetaPill(it) }
            }
            Spacer(Modifier.height(18.dp))
        }
    }

    Column(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = contentAlignmentModifier.fillMaxWidth()) {
            ToolSectionLabel(stringResource(R.string.result_label))
            Spacer(Modifier.height(6.dp))
        }
        if (segment.toolImages.isNotEmpty() || segment.toolImageRequestKey != null) {
            Column(modifier = contentAlignmentModifier.fillMaxWidth()) {
                ToolImageResults(
                    images = segment.toolImages,
                    squareCrop = segment.isImageGenerationSegment(),
                    onMediaClick = onMediaClick,
                    toolCallId = segment.toolCallId,
                    requestKey = segment.toolImageRequestKey,
                )
            }
            Spacer(Modifier.height(12.dp))
        }
    }
    if (presentation.kind == ToolKind.SHELL_EXECUTE ||
        presentation.kind == ToolKind.SHELL_JOB_GET ||
        presentation.kind == ToolKind.SHELL_JOB_WAIT
    ) {
        Column(modifier = contentAlignmentModifier.fillMaxWidth()) {
            ShellResult(presentation)
        }
        return
    }
    if (
        presentation.kind == ToolKind.WEB_SEARCH &&
        (
            presentation.state == ToolPresentationState.EMPTY ||
                presentation.state == ToolPresentationState.COMPLETED
            )
    ) {
        ToolCompletedContent(presentation)
        return
    }
    Column(modifier = contentAlignmentModifier.fillMaxWidth()) {
        when (presentation.state) {
            ToolPresentationState.CALLING -> ToolActiveContent(
                text = toolSummary(presentation),
                output = presentation.liveOutput,
            )
            ToolPresentationState.RUNNING,
            ToolPresentationState.BACKGROUND_RUNNING -> ToolActiveContent(
                text = toolSummary(presentation),
                output = presentation.liveOutput ?: resultOutput(presentation.result),
            )
            ToolPresentationState.FAILED -> {
                ToolErrorContent(
                    presentation.errorMessage ?: stringResource(R.string.tool_call_failed),
                )
                if (!presentation.liveOutput.isNullOrBlank()) {
                    Spacer(Modifier.height(8.dp))
                    TerminalOutput(presentation.liveOutput)
                }
            }
            ToolPresentationState.STOPPED -> GenerationTerminalText(
                text = stringResource(R.string.tool_execution_stopped),
                fillWidth = true,
            )
            ToolPresentationState.EMPTY,
            ToolPresentationState.COMPLETED -> ToolCompletedContent(presentation)
        }
    }
}

internal fun toolDetailHorizontalPadding(segment: MessageSegment): Dp =
    when (ToolPresentationResolver.resolve(segment).kind) {
        ToolKind.WEB_SEARCH -> 16.dp
        else -> 24.dp
    }

@Composable
private fun ToolImageResults(
    images: List<ToolImageAttachment>,
    squareCrop: Boolean,
    onMediaClick: (List<String>, Int) -> Unit,
    toolCallId: String?,
    requestKey: String?,
) {
    val displayImages = remember(images) {
        images.filter { it.path.isNotBlank() }
    }
    val paths = remember(displayImages) {
        displayImages.map(ToolImageAttachment::path)
    }
    val loader = LocalToolImageLoader.current
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (requestKey != null && toolCallId != null && displayImages.isEmpty()) {
            key(toolCallId, requestKey) {
                ToolImagePreview(image = null, squareCrop = false,
                    loadImage = { checkNotNull(loader).invoke(toolCallId, requestKey) },
                    onClick = { onMediaClick(listOf(it.path), 0) })
            }
        }
        displayImages.forEachIndexed { index, image ->
            key(image.path, image.sha256) {
                ToolImagePreview(
                    image = image,
                    squareCrop = squareCrop,
                    onClick = { onMediaClick(paths, index) },
                )
            }
        }
    }
}

@Composable
private fun ToolImagePreview(
    image: ToolImageAttachment?,
    squareCrop: Boolean,
    loadImage: (suspend () -> ToolImageAttachment)? = null,
    onClick: (ToolImageAttachment) -> Unit,
) {
    var attachment by remember(image) { mutableStateOf(image) }
    val aspectRatio = remember(image?.width, image?.height) {
        val width = image?.width?.takeIf { it > 0 }
        val height = image?.height?.takeIf { it > 0 }
        if (width == null || height == null) {
            1f
        } else {
            (width.toFloat() / height.toFloat()).coerceIn(0.55f, 2.2f)
        }
    }
    var loadState by remember(image) {
        mutableStateOf(MediaLoadPresentation.LOADING)
    }
    var presentedState by remember(image) {
        mutableStateOf(MediaLoadPresentation.LOADING)
    }
    LaunchedEffect(loadState) {
        presentedState = loadState
    }
    LaunchedEffect(image) {
        if (loadImage != null) {
            try { attachment = loadImage() }
            catch (cancelled: kotlinx.coroutines.CancellationException) { throw cancelled }
            catch (_: Exception) { loadState = MediaLoadPresentation.FAILED }
        }
    }

    BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
        val previewHeight = if (squareCrop) {
            maxWidth
        } else if (loadImage != null) {
            maxWidth
        } else {
            (maxWidth / aspectRatio).coerceIn(140.dp, 420.dp)
        }
        val previewModifier = Modifier.fillMaxWidth().height(previewHeight)
        Box(
            modifier = previewModifier
                .clip(RoundedCornerShape(12.dp))
                .background(
                    MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.45f),
                )
                .clickable(
                    enabled = presentedState == MediaLoadPresentation.LOADED,
                    onClick = { attachment?.let(onClick) },
                ),
            contentAlignment = Alignment.Center,
        ) {
            val displayedImage = attachment
            if (displayedImage != null) coil.compose.AsyncImage(
                model = displayedImage.path,
                contentDescription = stringResource(R.string.tool_view_image),
                contentScale = if (squareCrop) ContentScale.Crop else ContentScale.Fit,
                alignment = Alignment.Center,
                onLoading = { loadState = MediaLoadPresentation.LOADING },
                onSuccess = { loadState = MediaLoadPresentation.LOADED },
                onError = { loadState = MediaLoadPresentation.FAILED },
                modifier = previewModifier,
            )
            Crossfade(
                targetState = presentedState,
                animationSpec = tween(MEDIA_STATE_CROSSFADE_MILLIS),
                label = "toolImagePreview",
                modifier = Modifier.fillMaxSize(),
            ) { state ->
                when (state) {
                    MediaLoadPresentation.LOADING -> Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(
                                MaterialTheme.colorScheme.surfaceContainerHighest,
                            ),
                        contentAlignment = Alignment.Center,
                    ) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(28.dp),
                            strokeWidth = MEDIA_LOADING_INDICATOR_STROKE_WIDTH,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    }
                    MediaLoadPresentation.LOADED -> Spacer(Modifier.fillMaxSize())
                    MediaLoadPresentation.FAILED -> Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(
                                MaterialTheme.colorScheme.surfaceContainerHighest,
                            ),
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            imageVector = Icons.Default.BrokenImage,
                            contentDescription = stringResource(R.string.attachment_copy_failed_image),
                            tint = MaterialTheme.colorScheme.error,
                            modifier = Modifier.size(32.dp),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ToolSectionLabel(text: String) {
    Text(
        text = text,
        style = ChatType.meta,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        fontWeight = FontWeight.SemiBold,
    )
}

@Composable
private fun ToolActiveContent(text: String, output: String?) {
    Text(
        text = text,
        style = ChatType.metaNormal,
        color = MaterialTheme.colorScheme.primary,
    )
    if (!output.isNullOrBlank()) {
        Spacer(Modifier.height(8.dp))
        TerminalOutput(output)
    }
}

@Composable
private fun ToolErrorContent(message: String) {
    GenerationTerminalText(
        text = message,
        selectable = true,
        fillWidth = true,
    )
}

@Composable
private fun ToolMutedContent(message: String) {
    Text(
        text = message,
        style = ChatType.metaNormal,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun ToolCompletedContent(
    presentation: ToolPresentation,
) {
    when (presentation.kind) {
        ToolKind.MCP -> McpResultContent(presentation)
        ToolKind.FILE_GLOB -> FileGlobResult(presentation)
        ToolKind.FILE_GREP -> FileGrepResult(presentation)
        ToolKind.FILE_READ -> FileReadResult(presentation)
        ToolKind.WEB_SEARCH -> WebSearchResult(presentation)
        else -> {
            val result = presentation.rawResult
            if (result.isNullOrEmpty()) {
                ToolMutedContent(toolSummary(presentation))
            } else {
                JsonOrPlainView(result)
            }
        }
    }
}

@Composable
private fun McpResultContent(
    presentation: ToolPresentation,
) {
    val text = presentation.rawTextResult?.takeIf(String::isNotBlank)
    val structured = presentation.rawStructuredResult?.takeIf(String::isNotBlank)

    if (text != null) {
        JsonOrPlainView(text)
    }
    if (structured != null) {
        if (text != null) Spacer(Modifier.height(12.dp))
        JsonOrPlainView(structured)
    }
    if (text == null && structured == null) {
        val legacyResult = presentation.rawResult
        if (legacyResult.isNullOrEmpty()) {
            ToolMutedContent(toolSummary(presentation))
        } else {
            JsonOrPlainView(legacyResult)
        }
    }
}

@Composable
private fun FileGlobResult(presentation: ToolPresentation) {
    val files = (presentation.result as? JsonObject)
        ?.get("files") as? JsonArray
    if (files.isNullOrEmpty()) {
        ToolMutedContent(stringResource(R.string.tool_found_no_files))
        return
    }
    Column(verticalArrangement = Arrangement.spacedBy(7.dp)) {
        files.forEachIndexed { index, value ->
            val path = (value as? JsonPrimitive)?.contentOrNull ?: value.toString()
            IndexedCodeLine(index + 1, path)
        }
    }
}

private data class GrepUiMatch(
    val path: String,
    val line: Int?,
    val content: String,
)

@Composable
private fun FileGrepResult(presentation: ToolPresentation) {
    val matches = ((presentation.result as? JsonObject)?.get("matches") as? JsonArray)
        ?.mapNotNull { value ->
            val item = value as? JsonObject ?: return@mapNotNull null
            GrepUiMatch(
                path = item.string("path").orEmpty(),
                line = item.int("line"),
                content = item.string("content").orEmpty(),
            )
        }
        .orEmpty()
    if (matches.isEmpty()) {
        ToolMutedContent(stringResource(R.string.tool_found_no_matches))
        return
    }
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        matches.groupBy { it.path }.forEach { (path, pathMatches) ->
            Text(
                text = path.ifBlank { stringResource(R.string.file_path_unknown) },
                style = ChatType.thoughtCodeLarge,
                color = MaterialTheme.colorScheme.primary,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                pathMatches.forEach { match ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.Top,
                    ) {
                        Surface(
                            shape = RoundedCornerShape(5.dp),
                            color = MaterialTheme.colorScheme.surfaceContainerHighest,
                        ) {
                            Text(
                                text = match.line?.toString() ?: "\u2014",
                                style = ChatType.meta,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                            )
                        }
                        Spacer(Modifier.width(8.dp))
                        NoAutoScrollSelectionContainer(modifier = Modifier.weight(1f)) {
                            Text(
                                text = match.content,
                                style = ChatType.thoughtCodeLarge,
                                fontFamily = MonoFamily,
                                color = MaterialTheme.colorScheme.onSurface,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ShellResult(
    presentation: ToolPresentation,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        MetaPill(
            text = shellStatusLabel(presentation),
            emphasized = true,
        )
        MetaPill(
            presentation.device
                ?.takeIf { it.isNotBlank() }
                ?: stringResource(R.string.tool_unknown_device),
        )
    }
    if (presentation.state == ToolPresentationState.FAILED &&
        !presentation.errorMessage.isNullOrBlank()
    ) {
        Spacer(Modifier.height(8.dp))
        ToolErrorContent(presentation.errorMessage)
    }
    Spacer(Modifier.height(8.dp))
    TerminalOutput(
        shellOutputText(presentation)
            ?: stringResource(R.string.tool_no_output),
    )
}

@Composable
private fun shellStatusLabel(presentation: ToolPresentation): String {
    return shellExecutionSummary(presentation)
}

internal fun shellOutputText(presentation: ToolPresentation): String? {
    val result = presentation.result as? JsonObject
    val completedOutput = result.string("output")
        ?.takeIf(String::isNotBlank)
        ?: listOfNotNull(
            result.string("stdout")?.takeIf(String::isNotBlank),
            result.string("stderr")?.takeIf(String::isNotBlank),
        ).takeIf(List<String>::isNotEmpty)?.joinToString("\n")
    if (completedOutput != null) return completedOutput

    return presentation.liveOutput
        ?.takeIf { it.isNotBlank() }
        ?.takeUnless { output ->
            output.startsWith("Connecting to ") ||
                output == "Starting durable background job"
        }
}

@Composable
private fun FileReadResult(presentation: ToolPresentation) {
    val result = presentation.result as? JsonObject
    val path = result.string("path") ?: presentation.subject
    val lines = result.int("lines")
    if (path != null || lines != null) {
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            path?.let { MetaPill(it, modifier = Modifier.weight(1f, fill = false)) }
            lines?.let { MetaPill(stringResource(R.string.tool_line_count, it)) }
        }
        Spacer(Modifier.height(8.dp))
    }
    val content = result.string("content").orEmpty()
    if (content.isEmpty()) {
        ToolMutedContent(
            if (path == null) {
                stringResource(R.string.tool_read_file_empty_default)
            } else {
                stringResource(R.string.tool_read_file_empty, path)
            },
        )
    } else {
        TerminalOutput(content)
    }
    if (result.boolean("truncated") == true) {
        val nextOffset = (result.long("offset") ?: 0L) +
            (result.long("returned_bytes") ?: content.toByteArray(Charsets.UTF_8).size.toLong())
        Spacer(Modifier.height(8.dp))
        ToolMutedContent(stringResource(R.string.tool_read_file_truncated, nextOffset))
    }
}

@Composable
private fun WebSearchResult(
    presentation: ToolPresentation,
) {
    val uriHandler = LocalUriHandler.current
    val resultShape = RoundedCornerShape(12.dp)
    val results = ((presentation.result as? JsonObject)?.get("results") as? JsonArray)
        .orEmpty()
    if (results.isEmpty()) {
        Column(modifier = Modifier.padding(horizontal = 8.dp)) {
            ToolMutedContent(toolSummary(presentation))
        }
        return
    }
    Column {
        results.forEachIndexed { index, value ->
            val item = value as? JsonObject
            val title = item.string("title") ?: stringResource(R.string.tool_web_result, index + 1)
            val url = item.string("url") ?: item.string("href")
            val safeUrl = remember(url) { CitationPolicy.safeHttpUrl(url) }
            val snippet = item.string("snippet")
                ?: item.string("description")
                ?: item.string("content")
                ?: item.string("body")
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(resultShape)
                    .clickable(
                        enabled = safeUrl != null,
                        onClick = {
                            safeUrl?.let { destination ->
                                runCatching { uriHandler.openUri(destination) }
                            }
                        },
                    )
                    .padding(horizontal = 8.dp, vertical = 12.dp),
            ) {
                Text(
                    text = title,
                    style = ChatType.body,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                if (!snippet.isNullOrBlank()) {
                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = snippet,
                        style = ChatType.thoughtBody,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (!url.isNullOrBlank()) {
                    Spacer(Modifier.height(6.dp))
                    Text(
                        text = url,
                        style = ChatType.micro,
                        color = MaterialTheme.colorScheme.primary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            if (index < results.lastIndex) {
                HorizontalDivider(
                    color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f),
                )
            }
        }
    }
}

@Composable
private fun IndexedCodeLine(index: Int, text: String) {
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        Text(
            text = index.toString(),
            style = ChatType.meta,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.55f),
            modifier = Modifier.width(28.dp),
        )
        NoAutoScrollSelectionContainer(modifier = Modifier.weight(1f)) {
            Text(
                text = text,
                style = ChatType.thoughtCodeLarge,
                fontFamily = MonoFamily,
                color = MaterialTheme.colorScheme.onSurface,
            )
        }
    }
}

@Composable
private fun TerminalOutput(output: String) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.5f),
        shape = RoundedCornerShape(10.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        NoAutoScrollSelectionContainer {
            Text(
                text = output,
                style = ChatType.thoughtCodeLarge,
                fontFamily = MonoFamily,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.padding(10.dp),
            )
        }
    }
}

@Composable
private fun MetaPill(
    text: String,
    emphasized: Boolean = false,
    modifier: Modifier = Modifier,
) {
    val containerColor = if (emphasized) {
        MaterialTheme.colorScheme.primaryContainer
    } else {
        MaterialTheme.colorScheme.surfaceContainerHighest
    }
    val contentColor = if (emphasized) {
        MaterialTheme.colorScheme.onPrimaryContainer
    } else {
        MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        shape = CircleShape,
        color = containerColor,
        modifier = modifier,
    ) {
        Text(
            text = text,
            style = ChatType.meta,
            color = contentColor,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 3.dp),
        )
    }
}

private fun resultOutput(result: JsonElement?): String? =
    (result as? JsonObject).string("output")

private fun JsonObject?.string(key: String): String? =
    (this?.get(key) as? JsonPrimitive)?.contentOrNull

private fun JsonObject?.int(key: String): Int? =
    (this?.get(key) as? JsonPrimitive)?.intOrNull
private fun JsonObject?.long(key: String): Long? =
    string(key)?.toLongOrNull()
private fun JsonObject?.boolean(key: String): Boolean? =
    string(key)?.toBooleanStrictOrNull()
