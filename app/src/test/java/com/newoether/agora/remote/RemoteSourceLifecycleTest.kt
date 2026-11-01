package com.newoether.agora.remote

import com.newoether.agora.viewmodel.ScrollRequestCoordinator
import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.mockk
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

@OptIn(ExperimentalCoroutinesApi::class)
class RemoteSourceLifecycleTest {
    private val caps = RemoteCapabilities.legacy
    private val desktop = RemoteAdapter("codex-desktop", "codex", "desktop", "connected",
        nativeScopeId = "scope-a", capabilities = caps)
    private val cli = desktop.copy(id = "codex-cli", origin = "cli")
    private fun state(source: RemoteAdapter = desktop, scope: String = source.nativeScopeId,
        binding: String = "binding-a", ref: String = "canonical", draft: Boolean = false): RemoteState {
        val session = RemoteSession("session", "Native", "/workspace", 1,
            access = if (draft) null else RemoteSessionAccess(scope, ref, binding, caps))
        val initial = RemoteState(devices = listOf(RemoteDevice("device", "Computer", "address",
            RemoteDeviceStatus.CONNECTED, adapters = listOf(desktop, cli), multiAdapter = true)),
            deviceId = "device", adapterSelections = mapOf("device" to source.id), session = session,
            draftSessionId = session.id.takeIf { draft }, restoring = false,
            runtime = RemoteRuntime("idle"), models = listOf(RemoteModel("model", "Model", true)))
        return initial.copy(drafts = mapOf(requireNotNull(initial.owner) to "hello"))
    }

    @Test fun verifiedCanonicalIdentitySharesOnlyDraftAndUnreadWhileControlKeysStayQualified() {
        val first = state()
        val second = state(cli, binding = "cli-binding")
        assertEquals(first.owner, second.owner)
        assertNotEquals(first.accessOwner, second.accessOwner)
        assertNotEquals(first.settingsKey, second.settingsKey)
        assertNotEquals(first.confirmationPath, second.confirmationPath)
        assertNotEquals(first.owner, state(cli, scope = "another-store").owner)
        assertNotEquals(first.owner, state(cli, ref = "another-session").owner)
        val unverified = second.copy(session = second.session!!.copy(access = null))
        assertNotEquals(first.owner, unverified.owner)
        assertFalse(unverified.canSend)
        assertFalse(unverified.canStop)
        assertFalse(unverified.canEditSettings)
    }

    @Test fun legacyNamespacesRemainExactAndNeverAliasAnAdvertisedScope() {
        val legacy = state().copy(devices = listOf(RemoteDevice("device", "Computer", "address")),
            adapterSelections = emptyMap(), session = RemoteSession("session", "", "", 0))
        assertEquals("device/session", legacy.owner)
        assertEquals("device/session", legacy.accessOwner)
        assertNull(legacy.accessPath)
        assertNotEquals(legacy.owner, state().owner)
    }

    @Test fun nativeReceiptOnTheAdmittedSourceConfirmsAfterTurnBindingRotates() = runTest {
        val client = mockk<FiloClient>()
        coEvery { client.send(any(), any(), any(), any()) } returns "native-turn"
        val mutable = MutableStateFlow(state())
        val scroll = ScrollRequestCoordinator()
        val submission = RemoteSubmission(backgroundScope, mutable, { client }, { _, _ -> true }, { 1 },
            { true }, {}, null, scroll, { _, _, _ -> null }, {}, { false })
        val owner = mutable.value.owner!!
        submission.send()
        runCurrent()
        val accepted = mutable.value.attempts[owner]!!
        assertEquals(RemoteDelivery.ACCEPTED, accepted.delivery)
        mutable.value = mutable.value.copy(session = mutable.value.session!!.copy(
            access = mutable.value.session!!.access!!.copy(binding = "active-turn-binding")))
        submission.confirm(owner, accepted, listOf(RemoteInputReceipt(accepted.clientId, "wrong-turn", "user")))
        assertEquals(accepted, mutable.value.attempts[owner])
        val admitted = mutable.value
        mutable.value = admitted.copy(adapterSelections = mapOf("device" to cli.id))
        submission.confirm(owner, accepted, listOf(RemoteInputReceipt(accepted.clientId, "native-turn", "user")))
        assertEquals(accepted, mutable.value.attempts[owner])
        assertEquals("hello", mutable.value.drafts[owner])
        mutable.value = admitted
        submission.confirm(owner, accepted, listOf(RemoteInputReceipt(accepted.clientId, "native-turn", "user")))
        assertEquals(RemoteDelivery.DELIVERED, mutable.value.attempts[owner]?.delivery)
        assertNull(mutable.value.drafts[owner])
        coVerify(exactly = 1) { client.send("session", "hello", accepted.clientId, emptyList()) }
    }

    @Test fun unknownOutcomeNeverTransfersToAnotherSourceAndExplicitOriginalRetryGetsFreshIdentity() = runTest {
        val client = mockk<FiloClient>()
        coEvery { client.send(any(), any(), any(), any()) } throws IOException("lost")
        val mutable = MutableStateFlow(state())
        val submission = RemoteSubmission(backgroundScope, mutable, { client }, { _, _ -> true }, { 1 },
            { true }, {}, null, ScrollRequestCoordinator(), { _, _, _ -> RemoteFailure.NETWORK }, {}, { false })
        val owner = mutable.value.owner!!
        submission.send()
        runCurrent()
        val unknown = mutable.value.attempts[owner]!!
        assertEquals(RemoteDelivery.UNKNOWN, unknown.delivery)
        mutable.value = mutable.value.copy(adapterSelections = mapOf("device" to cli.id))
        submission.send()
        runCurrent()
        assertEquals(unknown, mutable.value.attempts[owner])
        coVerify(exactly = 1) { client.send(any(), any(), any(), any()) }
        mutable.value = mutable.value.copy(adapterSelections = mapOf("device" to desktop.id))
        submission.send()
        runCurrent()
        assertNotEquals(unknown.clientId, mutable.value.attempts[owner]?.clientId)
        coVerify(exactly = 2) { client.send(any(), any(), any(), any()) }
    }

    @Test fun lateCreationAfterConnectionOrAccountReplacementCannotPublishAliasesOrNavigate() = runTest {
        for (replaceConnection in listOf(false, true)) {
            val client = mockk<FiloClient>()
            val created = CompletableDeferred<RemoteSession>()
            coEvery { client.create(any(), any(), any(), any()) } coAnswers { created.await() }
            val mutable = MutableStateFlow(state(draft = true))
            var currentConnection = true
            val submission = RemoteSubmission(backgroundScope, mutable, { client }, { admitted, _ ->
                currentConnection && admitted.selectedAdapter.nativeScopeId == mutable.value.selectedAdapter.nativeScopeId
            }, { 1 }, { true }, { fail("Old creation must not refresh") }, null,
                ScrollRequestCoordinator(), { _, _, _ -> null }, {}, { false })
            submission.send()
            runCurrent()
            coVerify(exactly = 1) { client.create(any(), any(), any(), any()) }
            if (replaceConnection) currentConnection = false else mutable.value = mutable.value.copy(
                devices = listOf(mutable.value.devices.single().copy(adapters = listOf(desktop.copy(nativeScopeId = "scope-b")))))
            val replaced = mutable.value
            created.complete(RemoteSession("old-created", "Old", "", 2, acceptedTurnId = "turn",
                access = RemoteSessionAccess("scope-a", "old-canonical", "binding", caps)))
            runCurrent()
            assertEquals(replaced, mutable.value)
            assertTrue(mutable.value.sessionOwners.isEmpty())
            assertTrue(mutable.value.isDraft)
        }
    }

    @Test fun readonlyCapabilitiesNeverBorrowCodexWritesOrModelOptions() = runTest {
        val source = RemoteAdapter("claude-code", "claude", "cli", "connected", nativeScopeId = "claude",
            capabilities = RemoteCapabilities(history = true, historyMode = "bounded"))
        val readonly = state(source, scope = "claude").let {
            it.copy(devices = listOf(it.devices.single().copy(adapters = listOf(source))),
                session = requireNotNull(it.session).let { session -> session.copy(
                    access = requireNotNull(session.access).copy(capabilities = source.capabilities)) })
        }
        val mutable = MutableStateFlow(readonly)
        val client = mockk<FiloClient>()
        val submission = RemoteSubmission(backgroundScope, mutable, { client }, { _, _ -> true }, { 1 },
            { true }, {}, null, ScrollRequestCoordinator(), { _, _, _ -> null }, {}, { false })
        submission.send()
        runCurrent()
        assertEquals(readonly, mutable.value)
        assertTrue(readonly.historyBounded)
        assertFalse(readonly.canCreate)
        assertFalse(readonly.canChooseModel)
        assertFalse(readonly.canEditSettings)
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
    }
}
