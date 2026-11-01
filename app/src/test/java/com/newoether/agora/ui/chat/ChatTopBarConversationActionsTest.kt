package com.newoether.agora.ui.chat

import android.app.Application
import android.view.KeyEvent
import android.view.View
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.text.TextLayoutResult
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.R
import com.newoether.agora.ui.components.AgoraDropdownMenuItem
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.repository.SettingsRepository
import com.newoether.agora.model.ChatConversation
import com.newoether.agora.viewmodel.ChatViewModel
import com.newoether.agora.viewmodel.ConversationComposerSubmissionController
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.util.ReflectionHelpers

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = Application::class)
class ChatTopBarConversationActionsTest {
    @get:Rule val compose = createComposeRule()

    private val resources get() = ApplicationProvider.getApplicationContext<Application>().resources
    private fun label(id: Int) = resources.getString(id)
    private fun openMenu() {
        compose.onNodeWithContentDescription(label(R.string.options)).performClick()
    }
    private fun content(block: @Composable () -> Unit) {
        compose.setContent { MaterialTheme { block() } }
    }
    private fun textColor(id: Int): Color {
        val layouts = mutableListOf<TextLayoutResult>()
        compose.onNodeWithText(label(id), useUnmergedTree = true).performSemanticsAction(
            SemanticsActions.GetTextLayoutResult,
        ) { it(layouts) }
        return layouts.single().layoutInput.style.color
    }

    @Test
    fun metadataActionsStayAvailableWhenRunningDisablesSearchForkAndShare() {
        val callbacks = mutableListOf<String>()
        var errorColor = Color.Unspecified
        content {
            errorColor = MaterialTheme.colorScheme.error
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = "child",
                currentConversationTitle = "Child", totalTokens = 0, contextTokenBudget = 1024,
                conversationActionsEnabled = false, conversationMetadataActionsEnabled = true,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                onSearchClick = { callbacks += "search" },
                onForkConversation = { callbacks += "fork" },
                onShareConversation = { callbacks += "share" },
                onRenameConversation = { callbacks += "rename:$it" },
                onDeleteConversation = { callbacks += "delete:$it" },
            )
        }
        openMenu()
        for (id in listOf(R.string.conversation_search, R.string.conversation_fork_menu, R.string.conversation_share)) {
            compose.onNodeWithText(label(id)).assertIsNotEnabled()
        }
        val share = compose.onNodeWithText(label(R.string.conversation_share)).fetchSemanticsNode().boundsInRoot
        val rename = compose.onNodeWithText(label(R.string.rename)).assertIsEnabled().fetchSemanticsNode().boundsInRoot
        val delete = compose.onNodeWithText(label(R.string.delete)).assertIsEnabled().fetchSemanticsNode().boundsInRoot
        assertTrue(share.top < rename.top && rename.top < delete.top)
        assertEquals(errorColor, textColor(R.string.delete))
        compose.onNodeWithText(label(R.string.rename)).performScrollTo().performClick()
        compose.onNodeWithText(label(R.string.rename)).assertDoesNotExist()
        assertEquals(listOf("rename:child"), callbacks)
        openMenu()
        compose.onNodeWithText(label(R.string.delete)).performScrollTo().performClick()
        compose.onNodeWithText(label(R.string.delete)).assertDoesNotExist()
        assertEquals(listOf("rename:child", "delete:child"), callbacks)
    }

    @Test
    fun newChatOrMissingOwnerCannotEnableMetadataActions() {
        var newChat by mutableStateOf(true)
        var owner by mutableStateOf<String?>("old-owner")
        val callbacks = mutableListOf<String>()
        content {
            ChatTopBar(
                isNewChatMode = newChat, conversations = emptyList(), currentConversationId = owner,
                totalTokens = 0, contextTokenBudget = 1024,
                conversationActionsEnabled = true, conversationMetadataActionsEnabled = true,
                conversationDeleteEnabled = true,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                onRenameConversation = { callbacks += "rename:$it" },
                onDeleteConversation = { callbacks += "delete:$it" },
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertIsNotEnabled()
        compose.onNodeWithText(label(R.string.delete)).assertIsNotEnabled()
        compose.runOnIdle { newChat = false; owner = null }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertIsNotEnabled()
        compose.onNodeWithText(label(R.string.delete)).assertIsNotEnabled()
        assertTrue(callbacks.isEmpty())
    }

    @Test
    fun metadataGuardDisablesBothActionsWithoutChangingSearchForkOrShare() {
        var ready by mutableStateOf(true)
        val callbacks = mutableListOf<String>()
        content {
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = "c",
                totalTokens = 0, contextTokenBudget = 1024,
                conversationActionsEnabled = true, conversationMetadataActionsEnabled = ready,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                onRenameConversation = { callbacks += "rename:$it" },
                onDeleteConversation = { callbacks += "delete:$it" },
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertIsEnabled()
        compose.runOnIdle { ready = false }
        compose.onNodeWithText(label(R.string.rename)).assertIsNotEnabled()
        compose.onNodeWithText(label(R.string.delete)).assertIsNotEnabled()
        for (id in listOf(R.string.conversation_search, R.string.conversation_fork_menu, R.string.conversation_share)) {
            compose.onNodeWithText(label(id)).assertIsEnabled()
        }
        assertTrue(callbacks.isEmpty())
    }

    @Test
    fun frozenSubmissionDisablesOnlyDeleteAndUsesExistingDisabledErrorColor() {
        var deleteAllowed by mutableStateOf(true)
        var errorColor = Color.Unspecified
        content {
            errorColor = MaterialTheme.colorScheme.error
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = "c",
                totalTokens = 0, contextTokenBudget = 1024,
                conversationMetadataActionsEnabled = true, conversationDeleteEnabled = deleteAllowed,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.delete)).assertIsEnabled()
        assertEquals(errorColor, textColor(R.string.delete))
        compose.runOnIdle { deleteAllowed = false }
        compose.onNodeWithText(label(R.string.rename)).assertIsEnabled()
        compose.onNodeWithText(label(R.string.delete)).assertIsNotEnabled()
        assertEquals(errorColor.copy(alpha = 0.5f), textColor(R.string.delete))
    }

    @Test
    fun canceledMenuDoesNotRequestRenameOrDelete() {
        val callbacks = mutableListOf<String>()
        content {
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = "c",
                totalTokens = 0, contextTokenBudget = 1024, conversationMetadataActionsEnabled = true,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                onRenameConversation = { callbacks += "rename:$it" },
                onDeleteConversation = { callbacks += "delete:$it" },
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertIsDisplayed()
        val oldRename = compose.onNodeWithText(label(R.string.rename)).fetchSemanticsNode()
            .config[SemanticsActions.OnClick].action!!
        val oldDelete = compose.onNodeWithText(label(R.string.delete)).fetchSemanticsNode()
            .config[SemanticsActions.OnClick].action!!
        compose.runOnIdle {
            val windowManager = ReflectionHelpers.callStaticMethod<Any>(
                Class.forName("android.view.WindowManagerGlobal"), "getInstance",
            )
            val roots = ReflectionHelpers.getField<List<View>>(windowManager, "mViews")
            val popup = roots.single { it.javaClass.name.endsWith(".PopupLayout") }
            popup.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK))
            popup.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK))
        }
        compose.waitForIdle()
        compose.onNodeWithText(label(R.string.rename)).assertDoesNotExist()
        compose.onNodeWithText(label(R.string.delete)).assertDoesNotExist()
        compose.runOnIdle { oldRename(); oldDelete() }
        assertTrue(callbacks.isEmpty())
        openMenu()
        compose.runOnIdle { oldRename(); oldDelete() }
        assertTrue(callbacks.isEmpty())
        compose.onNodeWithText(label(R.string.rename)).performScrollTo().performClick()
        assertEquals(listOf("rename:c"), callbacks)
    }

    @Test
    fun ownerChangeClosesMenuAndRejectsCapturedCallbacksEvenAfterNewOwnerOpensMenu() {
        var owner by mutableStateOf("source")
        val callbacks = mutableListOf<String>()
        content {
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = owner,
                totalTokens = 0, contextTokenBudget = 1024, conversationMetadataActionsEnabled = true,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                onRenameConversation = { callbacks += "rename:$it" },
                onDeleteConversation = { callbacks += "delete:$it" },
            )
        }
        openMenu()
        val oldRename = compose.onNodeWithText(label(R.string.rename)).fetchSemanticsNode()
            .config[SemanticsActions.OnClick].action!!
        val oldDelete = compose.onNodeWithText(label(R.string.delete)).fetchSemanticsNode()
            .config[SemanticsActions.OnClick].action!!
        compose.runOnIdle { owner = "new-owner" }
        compose.onNodeWithText(label(R.string.rename)).assertDoesNotExist()
        compose.runOnIdle { owner = "source" }
        compose.onNodeWithText(label(R.string.rename)).assertDoesNotExist()
        compose.runOnIdle { oldRename(); oldDelete() }
        assertTrue(callbacks.isEmpty())
        compose.runOnIdle { owner = "new-owner" }
        openMenu()
        compose.runOnIdle { oldRename(); oldDelete() }
        assertTrue(callbacks.isEmpty())
        compose.onNodeWithText(label(R.string.rename)).performScrollTo().performClick()
        assertEquals(listOf("rename:new-owner"), callbacks)
    }

    @Test
    fun sharedDialogsKeepExactChildOwnerAndRawTitleAfterNavigationWithoutDrawerRows() {
        var owner by mutableStateOf("child")
        val rawTitle = "  custom-provider-id / child  "
        val dialogs = ChatAppDialogState(mutableStateOf(false))
        content {
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = owner,
                currentConversationTitle = "Display Child", totalTokens = 0, contextTokenBudget = 1024,
                conversationMetadataActionsEnabled = true,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                onRenameConversation = { dialogs.requestRename(it, rawTitle) },
                onDeleteConversation = dialogs::requestDelete,
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).performScrollTo().performClick()
        compose.runOnIdle { owner = "parent" }
        assertEquals("child", dialogs.renameConversationId)
        assertEquals(rawTitle, dialogs.renameInitialName)
        compose.runOnIdle { dialogs.dismissRename(); owner = "child" }
        openMenu()
        compose.onNodeWithText(label(R.string.delete)).performScrollTo().performClick()
        compose.runOnIdle { owner = "parent" }
        assertEquals("child", dialogs.deleteConversationId)
        assertNull(dialogs.renameConversationId)
    }

    @Test
    fun productionBindingUsesRawChildTitleAndSeparatesLoadingRunningAndSubmission() {
        val owner = MutableStateFlow<String?>("child")
        val rawTitle = "  custom-provider-id / child  "
        val child = ChatConversation(id = "child", title = rawTitle)
        val current = MutableStateFlow<ChatConversation?>(child)
        val loadedOwner = MutableStateFlow<String?>(null)
        val switching = MutableStateFlow(true)
        val running = MutableStateFlow(true)
        val submitting = MutableStateFlow<Set<String>>(emptySet())
        val viewModel = mockk<ChatViewModel>()
        val settings = mockk<SettingsRepository>()
        val submission = mockk<ConversationComposerSubmissionController>()
        every { viewModel.currentConversationId } returns owner
        every { viewModel.currentConversation } returns current
        every { viewModel.loadedMessagesConversationId } returns loadedOwner
        every { viewModel.isNewChatMode } returns MutableStateFlow(false)
        every { viewModel.isSwitching } returns switching
        every { viewModel.isTransitioningToNewChat } returns MutableStateFlow(false)
        every { viewModel.isLoading } returns running
        every { viewModel.settings } returns settings
        every { settings.customProviders } returns MutableStateFlow(emptyList<CustomProviderConfig>())
        every { viewModel.conversationComposerSubmission } returns submission
        every { submission.activeOwnerIds } returns submitting
        every { viewModel.isConversationDeleteLocked(any()) } answers { firstArg<String>() in submitting.value }
        val dialogs = ChatAppDialogState(mutableStateOf(false))
        val interaction = ConversationInteractionProjection(ConversationInteractionState(), emptySet(), emptyList())
        content {
            ChatTopBar(
                viewModel = viewModel, dialogState = dialogs, conversationInteraction = interaction,
                conversations = emptyList(), totalTokens = 0, contextTokenBudget = 1024,
                systemPromptEnabled = true, onNavigateBack = null,
                onOpenDrawer = {}, onForkConversation = {}, onNewChat = {},
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertIsNotEnabled()
        compose.onNodeWithText(label(R.string.delete)).assertIsNotEnabled()
        compose.runOnIdle { switching.value = false }
        compose.onNodeWithText(label(R.string.rename)).assertIsNotEnabled()
        compose.runOnIdle { loadedOwner.value = "child" }
        compose.onNodeWithText(label(R.string.conversation_search)).assertIsNotEnabled()
        compose.onNodeWithText(label(R.string.rename)).assertIsEnabled().performScrollTo().performClick()
        assertEquals("child", dialogs.renameConversationId)
        assertEquals(rawTitle, dialogs.renameInitialName)
        compose.runOnIdle {
            owner.value = "parent"
            current.value = ChatConversation(id = "parent", title = "Parent")
            loadedOwner.value = "parent"
        }
        assertEquals("child", dialogs.renameConversationId)
        assertEquals(rawTitle, dialogs.renameInitialName)
        compose.runOnIdle {
            dialogs.dismissRename()
            owner.value = "child"
            current.value = child
            loadedOwner.value = "child"
            submitting.value = setOf("child")
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertIsEnabled()
        compose.onNodeWithText(label(R.string.delete)).assertIsNotEnabled()
        compose.runOnIdle { submitting.value = emptySet() }
        compose.onNodeWithText(label(R.string.delete)).assertIsEnabled().performScrollTo().performClick()
        compose.runOnIdle { owner.value = "parent" }
        assertEquals("child", dialogs.deleteConversationId)
    }

    @Test
    fun remoteCustomMenuDoesNotGainAgoraConversationActions() {
        var remoteClicks = 0
        content {
            ChatTopBar(
                isNewChatMode = false, conversations = emptyList(), currentConversationId = "remote",
                totalTokens = 0, contextTokenBudget = 1024, conversationMetadataActionsEnabled = true,
                onOpenDrawer = {}, onSystemPromptClick = {}, onNewChat = {},
                moreMenuContent = { dismiss ->
                    AgoraDropdownMenuItem(text = { Text("Remote Action") }, onClick = {
                        remoteClicks++
                        dismiss()
                    })
                },
            )
        }
        openMenu()
        compose.onNodeWithText(label(R.string.rename)).assertDoesNotExist()
        compose.onNodeWithText(label(R.string.delete)).assertDoesNotExist()
        compose.onNodeWithText("Remote Action").performClick()
        assertEquals(1, remoteClicks)
        compose.onNodeWithText("Remote Action").assertDoesNotExist()
    }
}
