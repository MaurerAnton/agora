// Golden trace driver for data/local/ChatDao.kt.
//
// ChatDao.kt, its six inherited interfaces, ChatDaoRuntimeRecovery.kt and every entity/model file
// they reference compile VERBATIM from the app tree. Room's generated DAO is replaced by the
// stand-local MiniRoom: @Query runs on real SQLite over platform/schema.sql (the Room-generated
// schema 36 DDL), @Insert/@Upsert/@Update implement Room's algorithms, @Transaction default
// methods execute with Room's nesting, and observed queries return cold single-emission Flows.
//
// Entity seeding uses the DAO's own upserts, so every seeded row has passed the same entity
// constructors and converters the app uses. Clock-reading transactions are visible only through
// dataChangedAt; a scenario that invokes one sets "maskClock": true and both returned
// conversation rows and the table dump render that column as 0 instead of a wall-clock timestamp.
package chat.dao.golden

import androidx.room.RoomDatabase
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.ConversationSettingsImportTransferEntity
import com.newoether.agora.data.local.ConversationSettingsTransferEntity
import com.newoether.agora.data.local.DrawerConversationRow
import com.newoether.agora.data.local.EmbeddingEntity
import com.newoether.agora.data.local.ExecutionMessageSummaryRow
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.MessageStreamCheckpoint
import com.newoether.agora.data.local.NewChatPersistEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.local.RunGraphCommit
import com.newoether.agora.data.local.TaskEntity
import com.newoether.agora.data.local.ToolRoundCommit
import com.newoether.agora.model.ChatConversation
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import java.io.File
import java.sql.Connection
import java.sql.DriverManager
import java.util.Base64
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put
import stand.room.MiniRoom

internal val json = Json

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

internal fun render(scenarioFile: File): String = runBlocking {
    val root = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val maskClock = root.optionalBoolean("maskClock") ?: false
    val connection = DriverManager.getConnection("jdbc:sqlite::memory:")
    try {
        val pragma = connection.createStatement()
        pragma.execute("PRAGMA foreign_keys = ON")
        pragma.close()
        val schema = File("platform/schema.sql").readText()
        val statements = schema.lines()
            .map { line -> line.substringBefore("--") }
            .joinToString("\n")
            .split(";")
        for (statement in statements) {
            if (statement.isBlank()) continue
            val stmt = connection.createStatement()
            stmt.execute(statement)
            stmt.close()
        }

        val database = object : RoomDatabase() {
            override fun onBegin() {
                connection.createStatement().use { it.execute("BEGIN") }
            }

            override fun onCommit() {
                connection.createStatement().use { it.execute("COMMIT") }
            }

            override fun onRollback() {
                connection.createStatement().use { it.execute("ROLLBACK") }
            }
        }
        val dao = MiniRoom(database, connection).create(ChatDao::class.java)
        seed(dao, root["seed"] as? JsonObject)

        val steps = mutableListOf<JsonElement>()
        val stepList = root["steps"] as? JsonArray ?: JsonArray(emptyList())
        for (element in stepList) {
            val step = element.jsonObject
            val op = step.string("op")
            val result = try {
                executeOperation(op, step, dao, maskClock)
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", failure::class.simpleName ?: failure::class.java.name)
                    put("message", failure.message ?: "")
                }
            }
            steps.add(buildJsonObject { put("op", op); put("result", result) })
        }

        val trace = buildJsonObject {
            put("steps", JsonArray(steps))
            put("tables", dumpTables(connection, maskClock))
        }
        json.encodeToString(JsonObject.serializer(), trace)
    } finally {
        connection.close()
    }
}

// ---- JSON helpers --------------------------------------------------------------------------

internal fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

internal fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

internal fun JsonObject.long(name: String): Long = string(name).toLong()

internal fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

internal fun JsonObject.int(name: String): Int = string(name).toInt()

internal fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

internal fun JsonObject.boolean(name: String): Boolean =
    optionalString(name)?.toBooleanStrictOrNull() ?: error("missing boolean $name")

internal fun JsonObject.optionalBoolean(name: String): Boolean? =
    optionalString(name)?.toBooleanStrictOrNull()

internal fun JsonObject.stringList(name: String): List<String> =
    (this[name] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList()

internal fun JsonObject.fieldObject(name: String): JsonObject =
    this[name] as? JsonObject ?: error("missing object $name")

internal fun JsonObject.fieldObjectOrNull(name: String): JsonObject? = this[name] as? JsonObject

internal fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it.jsonObject } ?: emptyList()

internal fun JsonObject.stringMap(name: String): Map<String, String> =
    (this[name] as? JsonObject)?.entries?.associate { (key, value) ->
        key to (value as JsonPrimitive).content
    } ?: emptyMap()

internal fun JsonObject.selectionMap(name: String): Map<String?, String> =
    stringMap(name).mapKeys { (key, _) -> if (key == "null") null else key }
