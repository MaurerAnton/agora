// Golden trace driver for model/ConversationRuntimeReducer.kt, which compiles VERBATIM from the
// app tree together with RunState.kt, ConversationCommand.kt, RunEffect.kt, RuntimeTransition.kt
// and RuntimeIdentity.kt. The whole cluster has no imports at all, so this stand needs no stubs.
//
// Each scenario is a conversation id plus a sequence of commands. The driver starts from
// RunState.Idle and replays them, printing the command, the resulting state, the emitted effects
// and the rejection for every step.
//
// State and effects are printed with Kotlin's data-class `toString()`. That is deliberate: it pins
// every declared field, in declaration order, including nested identities and phase objects, so a
// C++ port that drops, renames or reorders a field fails the trace instead of silently drifting.
package conversation.runtime.golden

import com.newoether.agora.model.CommandRejection
import com.newoether.agora.model.ConversationCommand
import com.newoether.agora.model.ConversationRuntimeReducer
import com.newoether.agora.model.ProviderPassResult
import com.newoether.agora.model.RunEffectIdentity
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunRecoverySnapshot
import com.newoether.agora.model.RunState
import com.newoether.agora.model.RunStatus
import com.newoether.agora.model.RuntimeRunIdentity
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.longOrNull
import java.io.File

/** Field-safe escaping: no raw separators or control characters survive. */
private fun esc(value: String): String {
    val out = StringBuilder(value.length)
    for (c in value) {
        when (c) {
            '\\' -> out.append("\\\\")
            '|' -> out.append("\\p")
            ';' -> out.append("\\s")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

private fun JsonObject.text(name: String): String? =
    (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonObject.number(name: String): Long? =
    (this[name] as? JsonPrimitive)?.longOrNull

private fun JsonObject.flag(name: String): Boolean? =
    (this[name] as? JsonPrimitive)?.booleanOrNull

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

private class Builder(private val conversationId: String) {

    fun effectIdentity(
        value: JsonObject?,
        defaultRunId: String = "run",
        defaultEffectId: String = "e",
    ): RunEffectIdentity = RunEffectIdentity(
        conversationId = value?.text("conversationId") ?: conversationId,
        ownerToken = value?.number("ownerToken") ?: 1L,
        runId = value?.text("runId") ?: defaultRunId,
        pass = value?.number("pass")?.toInt() ?: 0,
        effectId = value?.text("effectId") ?: defaultEffectId,
    )

    fun runtimeIdentity(value: JsonObject): RuntimeRunIdentity = RuntimeRunIdentity(
        conversationId = value.text("conversationId") ?: conversationId,
        ownerToken = value.number("ownerToken") ?: 1L,
        runId = value.text("runId"),
        pass = value.number("pass")?.toInt() ?: 0,
    )

    fun command(value: JsonObject): ConversationCommand {
        val kind = value.requiredText("kind")
        fun identity(): RunEffectIdentity = effectIdentity(value["identity"] as? JsonObject)
        return when (kind) {
            "Recover" -> {
                val snapshot = value["snapshot"] as JsonObject
                ConversationCommand.Recover(
                    RunRecoverySnapshot(
                        conversationId = snapshot.text("conversationId") ?: conversationId,
                        runId = snapshot.requiredText("runId"),
                        pass = snapshot.number("pass")?.toInt() ?: 0,
                        status = RunStatus.valueOf(snapshot.requiredText("status")),
                    ),
                )
            }
            "RecoveryCompleted" -> ConversationCommand.RecoveryCompleted(
                identity = identity(),
                success = value.flag("success") ?: false,
            )
            "AcquireSlot" -> ConversationCommand.AcquireSlot(
                runtimeIdentity(value["identity"] as JsonObject),
            )
            "SendRequested" -> ConversationCommand.SendRequested(
                identity = identity(),
                directOnly = value.flag("directOnly") ?: false,
                hasPendingGuidance = value.flag("hasPendingGuidance") ?: false,
            )
            "InputPersisted" -> ConversationCommand.InputPersisted(identity())
            "InputPersistenceFailed" -> ConversationCommand.InputPersistenceFailed(identity())
            "SendLaunchAbandoned" -> ConversationCommand.SendLaunchAbandoned(identity())
            "BindRun" -> ConversationCommand.BindRun(
                runtimeIdentity(value["identity"] as JsonObject),
            )
            "ProviderPassRequested" -> ConversationCommand.ProviderPassRequested(identity())
            "ProviderPassCompleted" -> ConversationCommand.ProviderPassCompleted(
                identity = identity(),
                result = ProviderPassResult.valueOf(value.requiredText("result")),
            )
            "ToolBatchRequested" -> ConversationCommand.ToolBatchRequested(identity())
            "ToolBatchCompleted" -> ConversationCommand.ToolBatchCompleted(identity())
            "ToolRoundCommitted" -> ConversationCommand.ToolRoundCommitted(
                identity = identity(),
                success = value.flag("success") ?: false,
            )
            "FinalizationRequested" -> ConversationCommand.FinalizationRequested(
                identity = identity(),
                status = RunStatus.valueOf(value.requiredText("status")),
                reason = RunEndReason.valueOf(value.requiredText("reason")),
                markConversationUnread = value.flag("markConversationUnread") ?: false,
            )
            "FinalizationCompleted" -> ConversationCommand.FinalizationCompleted(
                identity = identity(),
                success = value.flag("success") ?: false,
            )
            "StopRequested" -> ConversationCommand.StopRequested(
                identity = runtimeIdentity(value["identity"] as JsonObject),
                coroutineAlreadySettled = value.flag("coroutineAlreadySettled") ?: false,
                requiresPersistence = value.flag("requiresPersistence") ?: false,
                effectId = value.text("effectId"),
            )
            "CoroutineSettled" -> ConversationCommand.CoroutineSettled(
                runtimeIdentity(value["identity"] as JsonObject),
            )
            "PersistenceSettled" -> ConversationCommand.PersistenceSettled(
                identity = identity(),
                success = value.flag("success") ?: false,
            )
            else -> error("unknown command kind: $kind")
        }
    }
}

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val conversationId = root.text("conversationId") ?: "c"
    val builder = Builder(conversationId)

    var state: RunState = RunState.Idle(conversationId)
    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("initial|").append(esc(state.toString())).append('\n')

    (root["commands"] as JsonArray).forEachIndexed { index, element ->
        val command = builder.command(element as JsonObject)
        val transition = ConversationRuntimeReducer.reduce(state, command)
        out.append("cmd|").append(index).append('|').append(esc(command.toString())).append('\n')
        out.append("state|").append(index).append('|').append(esc(transition.newState.toString()))
            .append('\n')
        out.append("effects|").append(index).append('|').append(esc(transition.effects.toString()))
            .append('\n')
        out.append("reject|").append(index).append('|')
            .append(transition.rejection?.name ?: "-").append('\n')
        state = transition.newState
    }
    print(out)
}
