package com.newoether.agora.webui

import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.verify
import kotlinx.coroutines.ExperimentalCoroutinesApi
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class WebUiSyncRecoveryTest {
    @Test fun explicitOpenUsesSharedFamilyRecoveryForEveryIndexedChild() {
        val fixture = WebUiSyncTest()
        coEvery { fixture.todoDao.getSubagentIds("a") } returns listOf("child")
        coEvery { fixture.todoDao.getSubagentFamilyRootId("child") } returns "a"

        fixture.sync { send, received ->
            send("""{"type":"open","conversationId":"a","seq":1}""")

            coVerify(exactly = 1) { fixture.conversations.recoverConversationRuntime("a", any()) }
            coVerify(exactly = 1) { fixture.conversations.recoverConversationRuntime("child", any()) }
            verify(exactly = 0) { fixture.registry.getOrCreate("child") }
            assertTrue(received().any { it.type == "path" })
        }
    }
}
