#include "agora/core/memory_tool_provider.hpp"

#include <algorithm>
#include <utility>

namespace agora::core::memory {

namespace {

namespace json = agora::core::json;

constexpr const char* kListMemoryFiles = "list_memory_files";
constexpr const char* kReadMemoryFile = "read_memory_file";
constexpr const char* kCreateMemoryFile = "create_memory_file";
constexpr const char* kEditMemoryFile = "edit_memory_file";
constexpr const char* kDeleteMemoryFile = "delete_memory_file";
constexpr const char* kReadActiveMemory = "read_active_memory";
constexpr const char* kUpdateActiveMemory = "update_active_memory";

// ---- JSON builders / Kotlin accessors ------------------------------------------------------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

std::string render(const json::Value& value) { return json::to_kotlin_string(value); }

const json::Value* map_get(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** `arg(key)`: `(args[key] as? JsonPrimitive)?.content ?: ""` — JsonNull's content is "null". */
std::string arg(const json::Value& args, const std::string& key) {
    const json::Value* value = map_get(args, key);
    if (value == nullptr) return "";
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return "";
}

shell_provider::ToolExecutionResult ok(std::string text) {
    shell_provider::ToolExecutionResult result;
    result.text = std::move(text);
    return result;
}

shell_provider::ToolExecutionResult fail(std::string message) {
    shell_provider::ToolExecutionResult result;
    result.text = std::move(message);
    result.is_error = true;
    return result;
}

bool is_tool_name(const std::string& name) {
    static constexpr const char* kNames[] = {
        "list_memory_files", "read_memory_file", "create_memory_file", "edit_memory_file",
        "delete_memory_file", "read_active_memory", "update_active_memory"};
    for (const char* candidate : kNames) {
        if (name == candidate) return true;
    }
    return false;
}

}  // namespace

// ---- Provider -------------------------------------------------------------------------------------

MemoryToolProvider::MemoryToolProvider(MemorySeam& memory) : memory_(memory) {}

bool MemoryToolProvider::handles(const std::string& name) const { return is_tool_name(name); }

std::vector<openai::ToolDefinition> MemoryToolProvider::definitions(
    const shell_provider::GenerationContext& ctx) const {
    std::vector<openai::ToolDefinition> definitions;
    if (ctx.access_saved_memories) {
        {
            openai::ToolFunction function;
            function.name = kListMemoryFiles;
            function.description =
                "List all files in the memory database with their names and descriptions.";
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
        {
            openai::ToolFunction function;
            function.name = kReadMemoryFile;
            function.description =
                "Read the content of one or more files from the memory database.";
            openai::ToolProperty name;
            name.type = "string";
            name.description = "The file name to read.";
            openai::ToolProperty name_item;
            name_item.type = "string";
            name_item.description = "A file name.";
            openai::ToolProperty names;
            names.type = "array";
            names.description = "Multiple file names to read in one call.";
            names.items = std::make_shared<openai::ToolProperty>(std::move(name_item));
            function.parameters.properties = {
                {"name", std::move(name)},
                {"names", std::move(names)},
            };
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
        {
            openai::ToolFunction function;
            function.name = kCreateMemoryFile;
            function.description =
                "Create a new file in the memory database with the given content and optional "
                "description.";
            openai::ToolProperty name;
            name.type = "string";
            name.description = "The file name to create (e.g., 'notes.md').";
            openai::ToolProperty content;
            content.type = "string";
            content.description = "The markdown content for the file.";
            openai::ToolProperty description;
            description.type = "string";
            description.description =
                "A short description of what this file contains (optional).";
            function.parameters.properties = {
                {"name", std::move(name)},
                {"content", std::move(content)},
                {"description", std::move(description)},
            };
            function.parameters.required = {"name", "content"};
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
        {
            openai::ToolFunction function;
            function.name = kEditMemoryFile;
            function.description =
                "Edit one aspect of a memory file. operation must be replace, patch, rename, or "
                "describe.";
            openai::ToolProperty name;
            name.type = "string";
            name.description = "The current file name.";
            openai::ToolProperty operation;
            operation.type = "string";
            operation.description = "One of: replace, patch, rename, describe.";
            openai::ToolProperty content;
            content.type = "string";
            content.description = "Complete replacement content for replace. May be empty.";
            openai::ToolProperty old_string;
            old_string.type = "string";
            old_string.description = "Exact non-empty unique text to replace for patch.";
            openai::ToolProperty new_string;
            new_string.type = "string";
            new_string.description =
                "Replacement text for patch. May be empty to delete the match.";
            openai::ToolProperty new_name;
            new_name.type = "string";
            new_name.description = "The target file name for rename.";
            openai::ToolProperty description;
            description.type = "string";
            description.description =
                "The new description for describe. May be empty to remove it.";
            function.parameters.properties = {
                {"name", std::move(name)},
                {"operation", std::move(operation)},
                {"content", std::move(content)},
                {"old_string", std::move(old_string)},
                {"new_string", std::move(new_string)},
                {"new_name", std::move(new_name)},
                {"description", std::move(description)},
            };
            function.parameters.required = {"name", "operation"};
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
        {
            openai::ToolFunction function;
            function.name = kDeleteMemoryFile;
            function.description = "Delete a file from the memory database.";
            openai::ToolProperty name;
            name.type = "string";
            name.description = "The file name to delete.";
            function.parameters.properties = {{"name", std::move(name)}};
            function.parameters.required = {"name"};
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
    }
    if (ctx.access_active_memory) {
        {
            openai::ToolFunction function;
            function.name = kReadActiveMemory;
            function.description =
                "Read the current stored active memory. The copy in the system prompt is captured "
                "once per turn, so edits made during this turn appear here but not in the prompt "
                "until the next turn.";
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
        {
            openai::ToolFunction function;
            function.name = kUpdateActiveMemory;
            function.description =
                "Update the active memory context. Modes: 'replace' (overwrite with 'content'), "
                "'append' (add 'content' to end), 'prepend' (add 'content' to beginning), "
                "'patch' (find 'old_string' exactly once and replace with 'new_string'). Default "
                "is replace.";
            openai::ToolProperty content;
            content.type = "string";
            content.description =
                "The content to write (for replace/append/prepend modes).";
            openai::ToolProperty mode;
            mode.type = "string";
            mode.description = "One of: replace, append, prepend, patch. Default is replace.";
            openai::ToolProperty old_string;
            old_string.type = "string";
            old_string.description =
                "Exact string to find and replace in the active memory. Required for patch mode. "
                "Must match exactly once.";
            openai::ToolProperty new_string;
            new_string.type = "string";
            new_string.description =
                "Replacement string for old_string in patch mode. Pass empty string to delete the "
                "matched text.";
            function.parameters.properties = {
                {"content", std::move(content)},
                {"mode", std::move(mode)},
                {"old_string", std::move(old_string)},
                {"new_string", std::move(new_string)},
            };
            function.parameters.required = {"content"};
            definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
        }
    }
    return definitions;
}

std::string MemoryToolProvider::execute(const std::string& name, const std::string& arguments,
                                        const shell_provider::GenerationContext& ctx) {
    return execute_one_shot_result(name, arguments, ctx).text;
}

shell_provider::ToolExecutionResult MemoryToolProvider::execute_one_shot_result(
    const std::string& name, const std::string& arguments,
    const shell_provider::GenerationContext& /*ctx*/) {
    // Kotlin decodes the arguments BEFORE the name dispatch: malformed JSON propagates as a
    // tool failure (there is no catch here), unlike ask_user's runCatching envelope.
    const std::string args_text = json::is_blank(arguments) ? "{}" : arguments;
    const json::ParseResult parsed = json::parse(args_text);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw std::runtime_error("Expected JSON object");
    }
    const json::Value& args = *parsed.value;

    if (name == kListMemoryFiles) {
        const std::vector<MemoryFile> files = memory_.list_files();
        std::vector<json::Value> entries;
        entries.reserve(files.size());
        for (const MemoryFile& file : files) {
            entries.push_back(json_object({{"name", json_string(file.name)},
                                           {"description", json_string(file.description)}}));
        }
        return ok(render(json_object(
            {{ "type", json_string(kListMemoryFiles) },
             { "files", json_array(std::move(entries)) }})));
    }

    if (name == kReadMemoryFile) {
        const std::string single_name = arg(args, "name");
        const json::Value* names_element = map_get(args, "names");
        const bool has_names = names_element != nullptr && names_element->kind == json::Kind::Array &&
                               !names_element->elements.empty();
        if (has_names) {
            std::vector<std::string> names;
            for (const json::Value& element : names_element->elements) {
                // (it as? JsonPrimitive)?.content ?: "" then filter { it.isNotEmpty() }.
                // isNotEmpty keeps whitespace-only entries; JsonNull content is "null".
                std::string value;
                if (element.kind == json::Kind::String || element.kind == json::Kind::Literal) {
                    value = element.text;
                }
                if (!value.empty()) names.push_back(std::move(value));
            }
            // joinToString("\n\n") over an empty (fully filtered) list yields "".
            std::string joined;
            for (std::size_t index = 0; index < names.size(); ++index) {
                if (index > 0) joined += "\n\n";
                joined += "--- " + names[index] + " ---\n";
                joined += memory_.read_file(names[index]);
            }
            return ok(std::move(joined));
        }
        if (!single_name.empty()) return ok(memory_.read_file(single_name));
        return fail(
            "Error: No file name provided. Use 'name' for a single file or 'names' for "
            "multiple files.");
    }

    if (name == kCreateMemoryFile) {
        return ok(memory_.create_file(arg(args, "name"), arg(args, "content"),
                                      arg(args, "description")));
    }

    if (name == kEditMemoryFile) {
        const std::string file_name = arg(args, "name");
        // operation = arg("operation").trim().lowercase()
        std::string operation = json::trim(arg(args, "operation"));
        for (char& c : operation) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (operation == "replace") {
            EditFileRequest request;
            request.name = file_name;
            request.content = arg(args, "content");
            return ok(memory_.edit_file(request));
        }
        if (operation == "patch") {
            const std::string old_string = arg(args, "old_string");
            if (old_string.empty()) {
                return fail("Error: patch requires a non-empty old_string.");
            }
            EditFileRequest request;
            request.name = file_name;
            request.old_string = old_string;
            request.new_string = arg(args, "new_string");
            return ok(memory_.edit_file(request));
        }
        if (operation == "rename") {
            // takeIf(String::isNotBlank): whitespace-only rejects, padding is preserved.
            const std::string new_name = arg(args, "new_name");
            if (json::is_blank(new_name)) {
                return fail("Error: rename requires a non-blank new_name.");
            }
            EditFileRequest request;
            request.name = file_name;
            request.new_name = new_name;
            return ok(memory_.edit_file(request));
        }
        if (operation == "describe") {
            EditFileRequest request;
            request.name = file_name;
            request.description = arg(args, "description");
            return ok(memory_.edit_file(request));
        }
        return fail("Error: operation must be replace, patch, rename, or describe.");
    }

    if (name == kDeleteMemoryFile) {
        return ok(memory_.delete_file(arg(args, "name")));
    }

    if (name == kReadActiveMemory) {
        return ok(memory_.get_active_memory());
    }

    if (name == kUpdateActiveMemory) {
        // mode = arg("mode").ifBlank { "replace" } — blank (Unicode) falls back, case kept.
        std::string mode = arg(args, "mode");
        if (json::is_blank(mode)) mode = "replace";
        // old_string/new_string = arg(...).ifBlank { null } — note JsonNull's content "null"
        // is not blank and therefore travels as the literal string.
        const std::string old_text = arg(args, "old_string");
        const std::string new_text = arg(args, "new_string");
        std::optional<std::string> old_string;
        std::optional<std::string> new_string;
        if (!json::is_blank(old_text)) old_string = old_text;
        if (!json::is_blank(new_text)) new_string = new_text;
        if (mode == "patch" && !old_string.has_value()) {
            return fail("Error: 'old_string' is required for patch mode.");
        }
        return ok(memory_.update_active_memory(arg(args, "content"), mode, old_string, new_string));
    }

    return fail("Unknown tool: " + name);
}

}  // namespace agora::core::memory