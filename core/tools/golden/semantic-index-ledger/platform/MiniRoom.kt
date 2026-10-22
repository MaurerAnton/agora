// The platform substitute for Room's annotation processor.
//
// The DAO interface in data/local/SemanticIndexLedger.kt is compiled verbatim: its @Query SQL,
// @Insert/@Upsert declarations and @Transaction default-method bodies are the app's real code.
// Room's generated implementation is replaced here by a reflective executor that
//
//   * reads @Query SQL from the RUNTIME-retained stand annotation and binds the Kotlin parameter
//     names, expanding collection parameters exactly like Room's `appendPlaceholders`,
//   * implements @Insert / @Upsert with Room's documented algorithm (insert, and on a constraint
//     conflict update by primary key),
//   * runs @Transaction methods inside the stand database's transaction nesting, and
//   * maps cursor rows back onto the entity data classes by column name.
//
// It deliberately contains no ledger logic: all SQL text and every conditional sequence come from
// the compiled application source.
package stand.room

import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.RoomDatabase
import androidx.room.Transaction
import androidx.room.Upsert
import java.lang.reflect.InvocationHandler
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Method
import java.lang.reflect.Proxy
import java.sql.Connection
import java.sql.PreparedStatement
import java.sql.ResultSet
import java.sql.SQLException
import kotlin.reflect.KClass
import kotlin.reflect.KType
import kotlin.reflect.full.findAnnotation
import kotlin.reflect.full.memberProperties
import kotlin.reflect.full.primaryConstructor
import kotlin.reflect.jvm.kotlinFunction

class MiniRoom(
    private val database: RoomDatabase,
    private val connection: Connection,
) {
    fun <T : Any> create(dao: Class<T>): T {
        @Suppress("UNCHECKED_CAST")
        return Proxy.newProxyInstance(dao.classLoader, arrayOf(dao)) { proxy, method, args ->
            invoke(proxy, method, args ?: emptyArray())
        } as T
    }

    private fun invoke(proxy: Any, method: Method, args: Array<Any?>): Any? {
        if (method.declaringClass == Any::class.java) {
            return when (method.name) {
                "equals" -> proxy === args[0]
                "hashCode" -> System.identityHashCode(proxy)
                "toString" -> "MiniRoomProxy(${method.declaringClass.simpleName})"
                else -> null
            }
        }

        if (method.getAnnotation(Transaction::class.java) != null) {
            database.beginTransaction()
            try {
                val result = dispatch(proxy, method, args)
                database.setTransactionSuccessful()
                return result
            } finally {
                database.endTransaction()
            }
        }
        return dispatch(proxy, method, args)
    }

    private fun dispatch(proxy: Any, method: Method, args: Array<Any?>): Any? {
        if (method.isDefault) {
            return invokeDefault(proxy, method, args)
        }
        method.getAnnotation(Query::class.java)?.let { query ->
            return executeQuery(method, query.value, args)
        }
        method.getAnnotation(Insert::class.java)?.let { insert ->
            return insertEntity(args.first()!!, insert.onConflict)
        }
        if (method.getAnnotation(Upsert::class.java) != null) {
            upsertEntity(args.first()!!)
            return Unit
        }
        error("MiniRoom: no Room annotation on ${method.declaringClass.name}.${method.name}")
    }

    private fun invokeDefault(proxy: Any, method: Method, args: Array<Any?>): Any? {
        val invokeDefault = InvocationHandler::class.java.getMethod(
            "invokeDefault",
            Any::class.java,
            Method::class.java,
            Array<Any>::class.java,
        )
        try {
            return invokeDefault.invoke(null, proxy, method, args)
        } catch (failure: InvocationTargetException) {
            throw failure.targetException
        }
    }

    // ---- queries -------------------------------------------------------------------------

    private fun executeQuery(method: Method, sqlText: String, args: Array<Any?>): Any? {
        val function = method.kotlinFunction
            ?: error("MiniRoom: not a Kotlin function: $method")
        val (sql, parameters) = bindParameters(sqlText, namedArguments(method, args))
        connection.prepareStatement(sql).use { statement ->
            bindValues(statement, parameters)
            if (sqlText.trimStart().startsWith("SELECT", ignoreCase = true)) {
                statement.executeQuery().use { rows -> return readRows(function.returnType, rows) }
            }
            return statement.executeUpdate()
        }
    }

    private fun namedArguments(method: Method, args: Array<Any?>): Map<String, Any?> {
        val parameters = method.parameters.toList()
        val values = args.toList()
        val result = LinkedHashMap<String, Any?>()
        val count = if (method.kotlinFunction?.isSuspend == true) parameters.size - 1 else parameters.size
        for (index in 0 until count) {
            result[parameters[index].name] = values.getOrNull(index)
        }
        return result
    }

    private fun bindParameters(sql: String, values: Map<String, Any?>): Pair<String, List<Any?>> {
        val out = StringBuilder()
        val parameters = mutableListOf<Any?>()
        var cursor = 0
        while (cursor < sql.length) {
            val match = PLACEHOLDER.find(sql, cursor)
            if (match == null) {
                out.append(sql, cursor, sql.length)
                break
            }
            out.append(sql, cursor, match.range.first)
            val name = match.groupValues[1]
            if (!values.containsKey(name)) error("MiniRoom: no argument named $name")
            when (val value = values[name]) {
                is Collection<*> -> {
                    if (value.isEmpty()) {
                        // Room's appendPlaceholders(0) leaves the parentheses empty; SQLite
                        // accepts `IN ()` as a never-matching list.
                        out.append("NULL")
                    } else {
                        value.forEachIndexed { index, element ->
                            if (index > 0) out.append(',')
                            out.append('?')
                            parameters.add(element)
                        }
                    }
                }
                else -> {
                    out.append('?')
                    parameters.add(value)
                }
            }
            cursor = match.range.last + 1
        }
        return out.toString() to parameters
    }

    private fun bindValues(statement: PreparedStatement, values: List<Any?>) {
        values.forEachIndexed { index, value ->
            val position = index + 1
            when (value) {
                null -> statement.setObject(position, null)
                is String -> statement.setString(position, value)
                is Long -> statement.setLong(position, value)
                is Int -> statement.setInt(position, value)
                is Boolean -> statement.setInt(position, if (value) 1 else 0)
                is ByteArray -> statement.setBytes(position, value)
                is Double -> statement.setDouble(position, value)
                else -> statement.setObject(position, value)
            }
        }
    }

    // ---- cursor mapping ------------------------------------------------------------------

    private fun readRows(type: KType, rows: ResultSet): Any? {
        val classifier = type.classifier as? KClass<*> ?: error("MiniRoom: unsupported type $type")
        if (classifier == List::class) {
            val element = type.arguments.first().type ?: error("MiniRoom: unsupported list type $type")
            val result = mutableListOf<Any?>()
            while (rows.next()) result.add(mapRow(element, rows))
            return result
        }
        return if (rows.next()) mapRow(type, rows) else null
    }

    private fun mapRow(type: KType, row: ResultSet): Any? {
        val classifier = type.classifier as? KClass<*> ?: error("MiniRoom: unsupported type $type")
        if (isScalar(classifier)) return convert(row.getObject(1), type)
        val constructor = classifier.primaryConstructor
            ?: error("MiniRoom: no primary constructor for ${classifier.qualifiedName}")
        val properties = classifier.memberProperties.associateBy { it.name }
        val arguments = constructor.parameters.map { parameter ->
            val name = parameter.name ?: error("MiniRoom: unnamed parameter")
            val property = properties[name] ?: error("MiniRoom: no property $name")
            convert(row.getObject(property.name), parameter.type)
        }
        return constructor.call(*arguments.toTypedArray())
    }

    private fun isScalar(type: KClass<*>): Boolean = type in SCALARS

    private fun convert(raw: Any?, type: KType): Any? {
        if (raw == null) return null
        return when (type.classifier as? KClass<*>) {
            String::class -> raw as String
            Long::class -> (raw as Number).toLong()
            Int::class -> (raw as Number).toInt()
            Boolean::class -> (raw as Number).toInt() != 0
            ByteArray::class -> raw as ByteArray
            Double::class -> (raw as Number).toDouble()
            else -> raw
        }
    }

    // ---- insert / upsert -----------------------------------------------------------------

    private fun insertEntity(entity: Any, onConflict: Int): Long {
        val table = tableOf(entity)
        val columns = columnsOf(entity)
        val included = columns.filter(::includeOnInsert)
        val sql = buildString {
            append("INSERT ")
            if (onConflict == OnConflictStrategy.IGNORE) append("OR IGNORE ")
            append("INTO ").append(table).append(" (")
            append(included.joinToString(", ") { it.name })
            append(") VALUES (")
            append(included.joinToString(", ") { "?" })
            append(')')
        }
        val changed = connection.prepareStatement(sql).use { statement ->
            bindValues(statement, included.map { it.value })
            statement.executeUpdate()
        }
        if (changed == 0) return -1L
        return lastInsertRowId()
    }

    /**
     * Room's @Upsert: insert, and when a constraint rejects the row, update the existing row by
     * primary key. An auto-generated primary key is omitted when it is zero; the update then
     * targets id = 0 and changes nothing, exactly as the generated adapter does.
     */
    private fun upsertEntity(entity: Any) {
        val table = tableOf(entity)
        val columns = columnsOf(entity)
        val included = columns.filter(::includeOnInsert)
        val sql = buildString {
            append("INSERT INTO ").append(table).append(" (")
            append(included.joinToString(", ") { it.name })
            append(") VALUES (")
            append(included.joinToString(", ") { "?" })
            append(')')
        }
        try {
            connection.prepareStatement(sql).use { statement ->
                bindValues(statement, included.map { it.value })
                statement.executeUpdate()
            }
        } catch (failure: SQLException) {
            if (!isConstraintFailure(failure)) throw failure
            val primaryKeys = columns.filter { it.primaryKey }
            if (primaryKeys.isEmpty()) throw failure
            val updates = columns.filter { !it.primaryKey }
            val updateSql = buildString {
                append("UPDATE ").append(table).append(" SET ")
                append(updates.joinToString(", ") { "${it.name} = ?" })
                append(" WHERE ")
                append(primaryKeys.joinToString(" AND ") { "${it.name} = ?" })
            }
            connection.prepareStatement(updateSql).use { statement ->
                bindValues(statement, updates.map { it.value } + primaryKeys.map { it.value })
                statement.executeUpdate()
            }
        }
    }

    private fun tableOf(entity: Any): String {
        val annotation = entity::class.findAnnotation<Entity>()
            ?: error("MiniRoom: ${entity::class.qualifiedName} is not a Room entity")
        return annotation.tableName
    }

    private fun columnsOf(entity: Any): List<Column> {
        val constructor = entity::class.primaryConstructor
            ?: error("MiniRoom: no primary constructor for ${entity::class.qualifiedName}")
        val properties = entity::class.memberProperties.associateBy { it.name }
        // Composite primary keys are declared on the entity (`primaryKeys = [...]`), not on the
        // properties; both shapes mark the primary-key columns for the upsert fallback.
        val compositeKeys = entity::class.findAnnotation<Entity>()?.primaryKeys.orEmpty().toSet()
        return constructor.parameters.map { parameter ->
            val name = parameter.name ?: error("MiniRoom: unnamed parameter")
            val property = properties[name] ?: error("MiniRoom: no property $name")
            val primary = parameter.annotations.filterIsInstance<PrimaryKey>().firstOrNull()
                ?: property.annotations.filterIsInstance<PrimaryKey>().firstOrNull()
            Column(
                name = name,
                value = property.getter.call(entity),
                primaryKey = primary != null || name in compositeKeys,
                autoGenerate = primary?.autoGenerate == true,
            )
        }
    }

    private fun includeOnInsert(column: Column): Boolean {
        if (!column.primaryKey || !column.autoGenerate) return true
        return when (val value = column.value) {
            is Long -> value != 0L
            is Int -> value != 0
            else -> true
        }
    }

    private fun lastInsertRowId(): Long =
        connection.createStatement().use { statement ->
            statement.executeQuery("SELECT last_insert_rowid()").use { rows ->
                rows.next()
                rows.getLong(1)
            }
        }

    private fun isConstraintFailure(failure: SQLException): Boolean {
        var current: Throwable? = failure
        while (current != null) {
            if (current is SQLException) {
                val message = current.message.orEmpty().lowercase()
                if (current.errorCode == 19 || message.contains("constraint")) return true
            }
            current = current.cause
        }
        return false
    }

    private class Column(
        val name: String,
        val value: Any?,
        val primaryKey: Boolean,
        val autoGenerate: Boolean,
    )

    private companion object {
        val PLACEHOLDER = Regex(":([A-Za-z_][A-Za-z0-9_]*)")
        val SCALARS = setOf(
            String::class,
            Long::class,
            Int::class,
            Boolean::class,
            ByteArray::class,
            Double::class,
        )
    }
}
