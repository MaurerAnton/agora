package com.newoether.agora.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Chat
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Description
import androidx.compose.material.icons.filled.DriveFileMove
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.*
import com.newoether.agora.ui.motion.MotionAwareCircularProgressIndicator as CircularProgressIndicator
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.newoether.agora.R
import com.newoether.agora.ui.components.clearFocusOnTap
import com.newoether.agora.util.DebugLog
import com.newoether.agora.viewmodel.ChatViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.newoether.agora.ui.components.AgoraDropdownMenu
import com.newoether.agora.ui.components.AgoraDropdownMenuItem

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsMemoryPage(viewModel: ChatViewModel, onBack: () -> Unit) {
    val accessSavedMemories by viewModel.settings.accessSavedMemories.collectAsState()
    val accessActiveMemory by viewModel.settings.accessActiveMemory.collectAsState()
    val catalogRevision by viewModel.memoryManager.catalogRevision.collectAsState()
    val activeMemoryRevision by viewModel.memoryManager.activeMemoryRevision.collectAsState()
    val scope = rememberCoroutineScope()
    val unknownError = stringResource(R.string.unknown_error)
    var activeMemoryContent by remember { mutableStateOf("") }
    var memoryFiles by remember { mutableStateOf<List<com.newoether.agora.data.MemoryManager.MemoryFileInfo>>(emptyList()) }
    var memoryLoaded by remember { mutableStateOf(false) }
    var memoryOperationInFlight by remember { mutableStateOf(false) }
    var showFileEditor by remember { mutableStateOf<String?>(null) }
    var fileEditorContent by remember { mutableStateOf("") }
    var fileEditorDesc by remember { mutableStateOf("") }
    var showNewFileDialog by remember { mutableStateOf(false) }
    var newFileName by remember { mutableStateOf("") }
    var newFileContent by remember { mutableStateOf("") }
    var newFileDesc by remember { mutableStateOf("") }
    var showDeleteFileConfirm by remember { mutableStateOf<String?>(null) }
    var memorySearchQuery by remember { mutableStateOf("") }
    var currentFolder by remember { mutableStateOf("") }
    var showMoveDialog by remember { mutableStateOf<String?>(null) }
    var showNewFolderDialog by remember { mutableStateOf(false) }
    val clipboardManager = LocalClipboardManager.current
    val copiedText = stringResource(R.string.memory_copied)

    fun reportMemoryFailure(action: String, error: Throwable) {
        DebugLog.e("SettingsMemory", action, error)
        viewModel.emitSnackbar(error.localizedMessage ?: unknownError)
    }

    fun copyMemoryText(text: String) {
        clipboardManager.setText(AnnotatedString(text))
        viewModel.emitSnackbar(copiedText)
    }

    fun copyMemoryFile(name: String) {
        if (memoryOperationInFlight) return
        memoryOperationInFlight = true
        scope.launch {
            val loaded = withContext(Dispatchers.IO) {
                runCatching { viewModel.memoryManager.readFile(name) }
            }
            loaded.onSuccess { content ->
                copyMemoryText(content)
            }.onFailure { error ->
                reportMemoryFailure("Unable to copy memory file", error)
            }
            memoryOperationInFlight = false
        }
    }

    fun copyAllMemories() {
        if (memoryOperationInFlight) return
        memoryOperationInFlight = true
        scope.launch {
            val loaded = withContext(Dispatchers.IO) {
                runCatching {
                    buildString {
                        var firstSection = true
                        fun sectionHeader(title: String) {
                            if (!firstSection) append("\n\n")
                            firstSection = false
                            append("# $title\n\n")
                        }
                        val active = viewModel.memoryManager.getActiveMemory()
                        if (active.isNotBlank()) {
                            sectionHeader("Active memory")
                            append(active)
                        }
                        for (file in viewModel.memoryManager.listFiles()) {
                            sectionHeader(file.name.removeSuffix(".md"))
                            append(viewModel.memoryManager.readFile(file.name))
                        }
                    }
                }
            }
            loaded.onSuccess { text ->
                copyMemoryText(text)
            }.onFailure { error ->
                reportMemoryFailure("Unable to copy memories", error)
            }
            memoryOperationInFlight = false
        }
    }

    val visibleMemoryFiles = remember(memoryFiles, memorySearchQuery) {
        val query = memorySearchQuery.trim()
        if (query.isEmpty()) memoryFiles
        else memoryFiles.filter {
            it.name.contains(query, ignoreCase = true) ||
                it.description.contains(query, ignoreCase = true)
        }
    }

    fun parentFolderOf(path: String): String =
        if ('/' in path) path.substringBeforeLast('/') else ""

    val allFolders = remember(memoryFiles) {
        memoryFiles.flatMap { file ->
            val parts = file.name.split('/').dropLast(1)
            (1..parts.size).map { parts.take(it).joinToString("/") }
        }.distinct().sorted()
    }
    val childFolders = remember(allFolders, currentFolder) {
        allFolders.filter { parentFolderOf(it) == currentFolder }
    }
    val childFiles = remember(visibleMemoryFiles, currentFolder, memorySearchQuery) {
        if (memorySearchQuery.isBlank()) {
            visibleMemoryFiles.filter { parentFolderOf(it.name) == currentFolder }
        } else visibleMemoryFiles
    }

    fun moveMemoryFile(name: String, targetFolder: String) {
        if (memoryOperationInFlight) return
        val base = name.substringAfterLast('/')
        val target = if (targetFolder.isBlank()) base else "$targetFolder/$base"
        if (target == name) return
        memoryOperationInFlight = true
        scope.launch {
            val moved = withContext(Dispatchers.IO) {
                runCatching {
                    viewModel.memoryManager.editFile(name = name, newName = target)
                    viewModel.memoryManager.listFiles()
                }
            }
            moved.onSuccess { files ->
                memoryFiles = files
            }.onFailure { error ->
                reportMemoryFailure("Unable to move memory file", error)
            }
            memoryOperationInFlight = false
        }
    }

    LaunchedEffect(catalogRevision, activeMemoryRevision) {
        val loaded = withContext(Dispatchers.IO) {
            runCatching {
                viewModel.memoryManager.getActiveMemory() to viewModel.memoryManager.listFiles()
            }
        }
        loaded.onSuccess { (active, files) ->
            activeMemoryContent = active
            memoryFiles = files
        }.onFailure { error ->
            reportMemoryFailure("Unable to load memories", error)
        }
        memoryLoaded = true
    }
    val showDocFab by viewModel.settings.showDocumentationFab.collectAsState()

    CollapsingSettingsScaffold(
        title = stringResource(R.string.memory_title),
        onBack = onBack,
        floatingActionButton = { if (showDocFab) DocumentationFab("memory.md") }
    ) {
            SettingsGroupColumn {
                SettingsGroup(
                    title = stringResource(R.string.memory_access_title),
                    items = listOf(
                        {
                            SettingsItem(
                                headlineContent = { Text(stringResource(R.string.memory_access_saved)) },
                                supportingContent = { Text(stringResource(R.string.memory_access_saved_desc)) },
                                leadingContent = { Icon(Icons.Default.Description, null, tint = MaterialTheme.colorScheme.primary) },
                                trailingContent = {
                                    Switch(checked = accessSavedMemories, onCheckedChange = { viewModel.settings.setAccessSavedMemories(it) })
                                },
                                modifier = Modifier.clickable { viewModel.settings.setAccessSavedMemories(!accessSavedMemories) }
                            )
                        },
                        {
                            SettingsItem(
                                headlineContent = { Text(stringResource(R.string.memory_access_active)) },
                                supportingContent = { Text(stringResource(R.string.memory_access_active_desc)) },
                                leadingContent = { Icon(Icons.Default.Memory, null, tint = MaterialTheme.colorScheme.primary) },
                                trailingContent = {
                                    Switch(checked = accessActiveMemory, onCheckedChange = { viewModel.settings.setAccessActiveMemory(it) })
                                },
                                modifier = Modifier.clickable { viewModel.settings.setAccessActiveMemory(!accessActiveMemory) }
                            )
                        }
                    )
                )

                SettingsGroup(
                    title = stringResource(R.string.memory_active_title),
                    items = listOf(
                        {
                            SettingsItem(
                                headlineContent = { Text(stringResource(R.string.memory_active_context)) },
                                supportingContent = {
                                    if (memoryLoaded) {
                                        Text(
                                            if (activeMemoryContent.isBlank()) stringResource(R.string.memory_active_empty)
                                            else activeMemoryContent.take(100) + if (activeMemoryContent.length > 100) "..." else ""
                                        )
                                    } else {
                                        CircularProgressIndicator(
                                            modifier = Modifier.size(18.dp),
                                            strokeWidth = 2.dp,
                                        )
                                    }
                                },
                                leadingContent = { Icon(Icons.Default.Memory, null, tint = MaterialTheme.colorScheme.primary) },
                                modifier = Modifier.clickable(
                                    enabled = memoryLoaded && !memoryOperationInFlight,
                                ) {
                                    showFileEditor = "ACTIVE_MEMORY"
                                    fileEditorContent = activeMemoryContent
                                }
                            )
                        }
                    )
                )

                SettingsGroup(
                    title = stringResource(R.string.memory_saved_title),
                items = buildList {
                    if (!memoryLoaded) {
                        add {
                            Box(
                                modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp),
                                contentAlignment = Alignment.Center,
                            ) {
                                CircularProgressIndicator(
                                    modifier = Modifier.size(20.dp),
                                    strokeWidth = 2.dp,
                                )
                            }
                        }
                    } else {
                        add {
                            OutlinedTextField(
                                value = memorySearchQuery,
                                onValueChange = { memorySearchQuery = it },
                                placeholder = { Text(stringResource(R.string.memory_search_hint)) },
                                leadingIcon = { Icon(Icons.Default.Search, null) },
                                trailingIcon = if (memorySearchQuery.isEmpty()) null else ({
                                    IconButton(onClick = { memorySearchQuery = "" }) {
                                        Icon(Icons.Default.Clear, stringResource(R.string.provider_cancel))
                                    }
                                }),
                                singleLine = true,
                                shape = RoundedCornerShape(16.dp),
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 16.dp, vertical = 4.dp),
                            )
                        }
                        if (memoryFiles.isNotEmpty() && memorySearchQuery.isBlank()) {
                            add {
                                SettingsItem(
                                    headlineContent = {
                                        Text(stringResource(R.string.memory_new_folder_title))
                                    },
                                    leadingContent = { Icon(Icons.Default.Folder, null, tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f)) },
                                    modifier = Modifier.clickable { showNewFolderDialog = true },
                                )
                            }
                            if (currentFolder.isNotEmpty()) {
                                add {
                                    Row(
                                        modifier = Modifier
                                            .fillMaxWidth()
                                            .padding(horizontal = 16.dp, vertical = 2.dp),
                                        verticalAlignment = Alignment.CenterVertically,
                                    ) {
                                        IconButton(onClick = { currentFolder = "" }) {
                                            Icon(Icons.Default.Home, stringResource(R.string.memory_saved_title))
                                        }
                                        var path = ""
                                        currentFolder.split('/').forEach { segment ->
                                            Text(
                                                "/",
                                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            )
                                            path = if (path.isEmpty()) segment else "$path/$segment"
                                            val target = path
                                            TextButton(onClick = { currentFolder = target }) {
                                                Text(segment)
                                            }
                                        }
                                    }
                                }
                            }
                            childFolders.forEach { folder ->
                                add {
                                    val contained = memoryFiles.count {
                                        it.name == folder || it.name.startsWith("$folder/")
                                    }
                                    SettingsItem(
                                        headlineContent = {
                                            Text(
                                                folder.substringAfterLast('/') + " ($contained)",
                                                fontWeight = FontWeight.Medium,
                                            )
                                        },
                                        leadingContent = { Icon(Icons.Default.Folder, null, tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f)) },
                                        modifier = Modifier.clickable { currentFolder = folder },
                                    )
                                }
                            }
                        }
                        if (memoryFiles.isNotEmpty()) {
                            add {
                                SettingsItem(
                                    headlineContent = {
                                        Text(
                                            stringResource(R.string.memory_copy_all) +
                                                " (${memoryFiles.size})"
                                        )
                                    },
                                    leadingContent = { Icon(Icons.Default.ContentCopy, null, tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f)) },
                                    trailingContent = if (memoryOperationInFlight) {
                                        {
                                            CircularProgressIndicator(
                                                modifier = Modifier.size(18.dp),
                                                strokeWidth = 2.dp,
                                            )
                                        }
                                    } else null,
                                    modifier = Modifier.clickable(enabled = !memoryOperationInFlight) {
                                        copyAllMemories()
                                    },
                                )
                            }
                        }
                    }
                    if (memoryLoaded && childFiles.isEmpty() && childFolders.isEmpty() && memorySearchQuery.isBlank()) {
                        add {
                            SettingsItem(
                                headlineContent = { Text(stringResource(R.string.memory_no_files), color = MaterialTheme.colorScheme.onSurfaceVariant) },
                                supportingContent = { Text(stringResource(R.string.memory_create_hint), color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)) },
                                leadingContent = { Icon(Icons.Default.Chat, null, tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)) },
                                modifier = Modifier.heightIn(min = 64.dp)
                            )
                        }
                    } else if (memoryLoaded) {
                        if (visibleMemoryFiles.isEmpty() && memorySearchQuery.isNotBlank()) {
                            add {
                                SettingsItem(
                                    headlineContent = { Text(stringResource(R.string.tool_conversation_search_no_result, memorySearchQuery.trim()), color = MaterialTheme.colorScheme.onSurfaceVariant) },
                                    leadingContent = { Icon(Icons.Default.Search, null, tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)) },
                                    modifier = Modifier.heightIn(min = 64.dp)
                                )
                            }
                        }
                        val listedFiles =
                            if (memorySearchQuery.isBlank()) childFiles else visibleMemoryFiles
                        listedFiles.forEach { file ->
                            add {
                                var showFileMenu by remember { mutableStateOf(false) }
                                val displayName = if (memorySearchQuery.isBlank()) {
                                    file.name.substringAfterLast('/').removeSuffix(".md")
                                } else file.name.removeSuffix(".md")
                                SettingsItem(
                                    headlineContent = { Text(displayName, fontWeight = FontWeight.Medium) },
                                    supportingContent = if (file.description.isNotBlank()) {{ Text(file.description) }} else null,
                                    leadingContent = { Icon(Icons.Default.Description, null, tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f)) },
                                    trailingContent = {
                                        Box {
                                            IconButton(onClick = { showFileMenu = true }) {
                                                Icon(
                                                    Icons.Default.MoreVert,
                                                    stringResource(R.string.menu),
                                                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                                )
                                            }
                                            AgoraDropdownMenu(
                                                containerColor = MaterialTheme.colorScheme.surfaceContainer,
                                                tonalElevation = 16.dp,
                                                expanded = showFileMenu,
                                                onDismissRequest = { showFileMenu = false }
                                            ) {
                                                AgoraDropdownMenuItem(
                                                    text = { Text(stringResource(R.string.copy)) },
                                                    leadingIcon = { Icon(Icons.Default.ContentCopy, null) },
                                                    onClick = {
                                                        showFileMenu = false
                                                        copyMemoryFile(file.name)
                                                    },
                                                    enabled = !memoryOperationInFlight,
                                                )
                                                AgoraDropdownMenuItem(
                                                    text = { Text(stringResource(R.string.memory_move)) },
                                                    leadingIcon = { Icon(Icons.Default.DriveFileMove, null) },
                                                    onClick = {
                                                        showFileMenu = false
                                                        showMoveDialog = file.name
                                                    },
                                                    enabled = !memoryOperationInFlight,
                                                )
                                                AgoraDropdownMenuItem(
                                                    text = { Text(stringResource(R.string.provider_edit)) },
                                                    leadingIcon = { Icon(Icons.Default.Edit, null) },
                                                    onClick = {
                                                        showFileMenu = false
                                                        if (!memoryOperationInFlight) {
                                                            memoryOperationInFlight = true
                                                            scope.launch {
                                                                val loaded = withContext(Dispatchers.IO) {
                                                                    runCatching {
                                                                        viewModel.memoryManager.readFile(file.name) to
                                                                            viewModel.memoryManager.getDescription(file.name)
                                                                    }
                                                                }
                                                                loaded.onSuccess { (content, description) ->
                                                                    fileEditorContent = content
                                                                    fileEditorDesc = description
                                                                    showFileEditor = file.name
                                                                }.onFailure { error ->
                                                                    reportMemoryFailure(
                                                                        "Unable to open memory file",
                                                                        error,
                                                                    )
                                                                }
                                                                memoryOperationInFlight = false
                                                            }
                                                        }
                                                    },
                                                    enabled = !memoryOperationInFlight,
                                                )
                                                AgoraDropdownMenuItem(
                                                    text = { Text(stringResource(R.string.provider_delete), color = MaterialTheme.colorScheme.error) },
                                                    leadingIcon = { Icon(Icons.Default.Delete, null, tint = MaterialTheme.colorScheme.error) },
                                                    onClick = {
                                                        showFileMenu = false
                                                        showDeleteFileConfirm = file.name
                                                    }
                                                )
                                            }
                                        }
                                    }
                                )
                            }
                        }
                    }
                    add {
                        SettingsAddItem(
                            label = stringResource(R.string.memory_add),
                            enabled = memoryLoaded && !memoryOperationInFlight,
                            onClick = { showNewFileDialog = true },
                        )
                    }
                }
            )
            }
            if (showDocFab) { Spacer(modifier = Modifier.height(80.dp)) }
    }

    // Move file dialog
    showMoveDialog?.let { fileName ->
        SettingsMemoryMoveDialog(
            fileName = fileName,
            folders = allFolders,
            onDismiss = { showMoveDialog = null },
            onConfirm = { target ->
                showMoveDialog = null
                moveMemoryFile(fileName, target)
            },
        )
    }

    // New folder dialog: no disk writes — the folder materializes with its first file.
    if (showNewFolderDialog) {
        var folderName by remember { mutableStateOf("") }
        val normalized = folderName.trim().trim('/').split('/').filter { it.isNotEmpty() }
        val valid = normalized.isNotEmpty() && normalized.none { it == ".." }
        AlertDialog(
            modifier = Modifier.clearFocusOnTap(),
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            onDismissRequest = { showNewFolderDialog = false },
            title = { Text(stringResource(R.string.memory_new_folder_title), fontWeight = FontWeight.Bold) },
            text = {
                OutlinedTextField(
                    value = folderName,
                    onValueChange = { folderName = it },
                    label = { Text(stringResource(R.string.memory_new_folder)) },
                    singleLine = true,
                    shape = RoundedCornerShape(16.dp),
                    modifier = Modifier.fillMaxWidth(),
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        currentFolder = normalized.joinToString("/")
                        showNewFolderDialog = false
                    },
                    enabled = valid,
                ) { Text(stringResource(R.string.memory_create)) }
            },
            dismissButton = {
                TextButton(onClick = { showNewFolderDialog = false }) {
                    Text(stringResource(R.string.provider_cancel))
                }
            },
        )
    }

    // Delete file confirmation
    showDeleteFileConfirm?.let { fileName ->
        AlertDialog(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            onDismissRequest = { showDeleteFileConfirm = null },
            title = { Text(stringResource(R.string.memory_delete_title), fontWeight = FontWeight.Bold) },
            text = { Text(stringResource(R.string.memory_delete_text, fileName)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (!memoryOperationInFlight) {
                            memoryOperationInFlight = true
                            scope.launch {
                                val deleted = withContext(Dispatchers.IO) {
                                    runCatching {
                                        viewModel.memoryManager.deleteFile(fileName)
                                        viewModel.memoryManager.listFiles()
                                    }
                                }
                                deleted.onSuccess { files ->
                                    memoryFiles = files
                                    showDeleteFileConfirm = null
                                }.onFailure { error ->
                                    reportMemoryFailure("Unable to delete memory file", error)
                                }
                                memoryOperationInFlight = false
                            }
                        }
                    },
                    enabled = !memoryOperationInFlight,
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)
                ) { Text(stringResource(R.string.provider_delete)) }
            },
            dismissButton = { TextButton(onClick = { showDeleteFileConfirm = null }) { Text(stringResource(R.string.provider_cancel)) } }
        )
    }

    // File Editor Dialog
    showFileEditor?.let { fileName ->
        val isActiveMemory = fileName == "ACTIVE_MEMORY"
        var editFileName by remember { mutableStateOf(if (isActiveMemory) "" else fileName.removeSuffix(".md")) }
        var editContent by remember { mutableStateOf(fileEditorContent) }
        var editDesc by remember { mutableStateOf(fileEditorDesc) }

        AlertDialog(
            modifier = Modifier.clearFocusOnTap(),
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            onDismissRequest = {
                showFileEditor = null
                fileEditorContent = ""
                fileEditorDesc = ""
            },
            title = { Text(if (isActiveMemory) stringResource(R.string.memory_edit_active) else stringResource(R.string.memory_edit), fontWeight = FontWeight.Bold) },
            text = {
                Column(Modifier.fillMaxWidth()) {
                    if (isActiveMemory) {
                        Text(
                            stringResource(R.string.memory_active_desc),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                        )
                        Spacer(modifier = Modifier.height(12.dp))
                    } else {
                        OutlinedTextField(
                            value = editFileName,
                            onValueChange = { editFileName = it },
                            label = { Text(stringResource(R.string.memory_title_hint)) },
                            singleLine = true,
                            shape = RoundedCornerShape(16.dp),
                            modifier = Modifier.fillMaxWidth()
                        )
                        Spacer(modifier = Modifier.height(12.dp))
                    }
                    if (!isActiveMemory) {
                        OutlinedTextField(
                            value = editDesc,
                            onValueChange = { editDesc = it },
                            label = { Text(stringResource(R.string.memory_desc_hint)) },
                            singleLine = true,
                            shape = RoundedCornerShape(16.dp),
                            modifier = Modifier.fillMaxWidth()
                        )
                        Spacer(modifier = Modifier.height(12.dp))
                    }
                    OutlinedTextField(
                        value = editContent,
                        onValueChange = { editContent = it },
                        label = { Text(stringResource(R.string.memory_content_hint)) },
                        shape = RoundedCornerShape(16.dp),
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 200.dp, max = 400.dp),
                        textStyle = MaterialTheme.typography.bodySmall.copy(
                            fontFamily = FontFamily.Monospace
                        )
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (!memoryOperationInFlight) {
                            val contentSnapshot = editContent
                            val descriptionSnapshot = editDesc
                            val editedName = editFileName.trim()
                            val newName = editedName.takeIf {
                                !isActiveMemory && it != fileName.removeSuffix(".md")
                            }
                            memoryOperationInFlight = true
                            scope.launch {
                                val saved = withContext(Dispatchers.IO) {
                                    runCatching {
                                        if (isActiveMemory) {
                                            viewModel.memoryManager.updateActiveMemory(contentSnapshot)
                                            viewModel.memoryManager.getActiveMemory() to null
                                        } else {
                                            viewModel.memoryManager.editFile(
                                                name = fileName,
                                                content = contentSnapshot,
                                                newName = newName,
                                                description = descriptionSnapshot,
                                            )
                                            null to viewModel.memoryManager.listFiles()
                                        }
                                    }
                                }
                                saved.onSuccess { (active, files) ->
                                    if (active != null) activeMemoryContent = active
                                    if (files != null) memoryFiles = files
                                    showFileEditor = null
                                    fileEditorContent = ""
                                    fileEditorDesc = ""
                                }.onFailure { error ->
                                    reportMemoryFailure("Unable to save memory", error)
                                }
                                memoryOperationInFlight = false
                            }
                        }
                    },
                    enabled = !memoryOperationInFlight &&
                        (isActiveMemory || editFileName.isNotBlank()),
                ) { Text(stringResource(R.string.provider_save)) }
            },
            dismissButton = {
                Row {
                    TextButton(onClick = { copyMemoryText(editContent) }) {
                        Text(stringResource(R.string.copy))
                    }
                    TextButton(onClick = {
                        showFileEditor = null
                        fileEditorContent = ""
                        fileEditorDesc = ""
                    }) { Text(stringResource(R.string.provider_cancel)) }
                }
            }
        )
    }

    // New File Dialog
    if (showNewFileDialog) {
        AlertDialog(
            modifier = Modifier.clearFocusOnTap(),
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            onDismissRequest = { showNewFileDialog = false },
            title = { Text(stringResource(R.string.memory_add_title), fontWeight = FontWeight.Bold) },
            text = {
                Column(Modifier.fillMaxWidth()) {
                    OutlinedTextField(
                        value = newFileName,
                        onValueChange = { newFileName = it },
                        label = { Text(stringResource(R.string.memory_title_hint)) },
                        singleLine = true,
                        shape = RoundedCornerShape(16.dp),
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(modifier = Modifier.height(12.dp))
                    OutlinedTextField(
                        value = newFileDesc,
                        onValueChange = { newFileDesc = it },
                        label = { Text(stringResource(R.string.memory_desc_hint)) },
                        singleLine = true,
                        shape = RoundedCornerShape(16.dp),
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(modifier = Modifier.height(12.dp))
                    OutlinedTextField(
                        value = newFileContent,
                        onValueChange = { newFileContent = it },
                        label = { Text(stringResource(R.string.memory_content_hint)) },
                        shape = RoundedCornerShape(16.dp),
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 150.dp),
                        textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace)
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (newFileName.isNotBlank() && !memoryOperationInFlight) {
                            val nameSnapshot = newFileName
                            val contentSnapshot = newFileContent
                            val descriptionSnapshot = newFileDesc
                            memoryOperationInFlight = true
                            scope.launch {
                                val created = withContext(Dispatchers.IO) {
                                    runCatching {
                                        viewModel.memoryManager.createFile(
                                            if (currentFolder.isBlank()) nameSnapshot
                                            else "$currentFolder/$nameSnapshot",
                                            contentSnapshot,
                                            descriptionSnapshot,
                                        )
                                        viewModel.memoryManager.listFiles()
                                    }
                                }
                                created.onSuccess { files ->
                                    memoryFiles = files
                                    showNewFileDialog = false
                                    newFileName = ""
                                    newFileContent = ""
                                    newFileDesc = ""
                                }.onFailure { error ->
                                    reportMemoryFailure("Unable to create memory file", error)
                                }
                                memoryOperationInFlight = false
                            }
                        }
                    },
                    enabled = newFileName.isNotBlank() && !memoryOperationInFlight,
                ) { Text(stringResource(R.string.memory_create)) }
            },
            dismissButton = {
                TextButton(onClick = {
                    showNewFileDialog = false
                    newFileName = ""
                    newFileContent = ""
                    newFileDesc = ""
                }) { Text(stringResource(R.string.provider_cancel)) }
            }
        )
    }
}
