#include "agora/core/skill_tool_provider.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "agora/core/json.hpp"

namespace agora::core::skill {

namespace {

namespace json = agora::core::json;

constexpr const char* kListSkillFiles = "list_skill_files";
constexpr const char* kReadSkillFile = "read_skill_file";
constexpr const char* kCreateSkillFile = "create_skill_file";
constexpr const char* kEditSkillFile = "edit_skill_file";
constexpr const char* kDeleteSkillFile = "delete_skill_file";

bool is_read_tool(const std::string& name) {
    return name == kListSkillFiles || name == kReadSkillFile;
}

bool is_modify_tool(const std::string& name) {
    return name == kCreateSkillFile || name == kEditSkillFile || name == kDeleteSkillFile;
}

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
        if (member.key == key) found = &member.value;  // last duplicate wins, like a Map
    }
    return found;
}

/** `(element as? JsonPrimitive)?.content`. */
std::optional<std::string> primitive_content(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return std::nullopt;
}

/** Kotlin arg(key) = (args[key] as? JsonPrimitive)?.content.orEmpty(). */
std::string arg(const json::Value& args, std::string_view key) {
    return primitive_content(map_get(args, key)).value_or("");
}

/** Kotlin String.lowercase() restricted to ASCII (the operation tokens are ASCII). */
std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

shell_provider::ToolExecutionResult text_result(std::string text) {
    shell_provider::ToolExecutionResult result;
    result.text = std::move(text);
    return result;
}

shell_provider::ToolExecutionResult error_result(std::string text) {
    shell_provider::ToolExecutionResult result;
    result.text = std::move(text);
    result.is_error = true;
    return result;
}

openai::ToolProperty property(std::string type, std::string description) {
    openai::ToolProperty result;
    result.type = std::move(type);
    result.description = std::move(description);
    return result;
}

/** Kotlin `tool()` helper: no required list and no properties by default. */
openai::ToolDefinition tool(
    std::string name, std::string description,
    std::vector<std::pair<std::string, openai::ToolProperty>> properties = {},
    std::vector<std::string> required = {}) {
    openai::ToolFunction function;
    function.name = std::move(name);
    function.description = std::move(description);
    function.parameters.properties = std::move(properties);
    function.parameters.required = std::move(required);
    return openai::ToolDefinition{"function", std::move(function)};
}

}  // namespace

SkillToolProvider::SkillToolProvider(SkillManager& skills) : skills_(skills) {}

std::vector<openai::ToolDefinition> SkillToolProvider::definitions(
    const shell_provider::GenerationContext& ctx) const {
    if (!ctx.skill_read_access && !ctx.skill_modify_access) return {};
    std::vector<openai::ToolDefinition> definitions;
    if (ctx.skill_read_access) {
        definitions.push_back(tool(kListSkillFiles,
                                   "List saved skill files with their names and compact "
                                   "descriptions."));
        openai::ToolProperty item = property("string", "A skill file name.");
        openai::ToolProperty names = property("array", "Multiple skill file names.");
        names.items = std::make_shared<openai::ToolProperty>(std::move(item));
        definitions.push_back(tool(
            kReadSkillFile,
            "Read one or more saved skill files. Read a relevant skill before applying it.",
            {{"name", property("string", "One skill file name.")}, {"names", std::move(names)}}));
    }
    if (ctx.skill_modify_access) {
        definitions.push_back(tool(
            kCreateSkillFile, "Create a saved Markdown skill file.",
            {{"name", property("string", "The skill file name.")},
             {"content", property("string", "The complete Markdown instructions.")},
             {"description", property("string", "A compact catalog description.")}},
            {"name", "content"}));
        definitions.push_back(tool(
            kEditSkillFile,
            "Edit one aspect of a saved skill file. operation must be replace, patch, rename, "
            "or describe.",
            {{"name", property("string", "The current file name.")},
             {"operation", property("string", "One of: replace, patch, rename, describe.")},
             {"content", property("string", "Complete replacement content for replace. May be "
                                            "empty.")},
             {"old_string", property("string", "Exact non-empty unique text to replace for "
                                               "patch.")},
             {"new_string", property("string", "Replacement text for patch. May be empty to "
                                               "delete the match.")},
             {"new_name", property("string", "The target file name for rename.")},
             {"description", property("string", "The new compact description for describe. May "
                                                "be empty to remove it.")}},
            {"name", "operation"}));
        definitions.push_back(tool(kDeleteSkillFile, "Delete a saved skill file.",
                                   {{"name", property("string", "The skill file name to "
                                                               "delete.")}},
                                   {"name"}));
    }
    return definitions;
}

bool SkillToolProvider::handles(const std::string& name) const {
    return is_read_tool(name) || is_modify_tool(name);
}

std::string SkillToolProvider::execute(const std::string& name, const std::string& arguments,
                                       const shell_provider::GenerationContext& ctx) {
    return execute_one_shot_result(name, arguments, ctx).text;
}

shell_provider::ToolExecutionResult SkillToolProvider::execute_one_shot_result(
    const std::string& name, const std::string& arguments,
    const shell_provider::GenerationContext& ctx) {
    // The access gates run before argument decoding, so they answer even malformed args.
    if (!ctx.skill_read_access && is_read_tool(name)) {
        return error_result("Error: Skill read access is disabled.");
    }
    if (!ctx.skill_modify_access && is_modify_tool(name)) {
        return error_result("Error: Skill modify access is disabled.");
    }

    // Json.decodeFromString<Map<String, JsonElement>> — the Kotlin body has no try/catch.
    const std::string text = json::is_blank(arguments) ? "{}" : arguments;
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw std::runtime_error("Expected a JSON object");
    }
    const json::Value& args = *parsed.value;

    if (name == kListSkillFiles) {
        std::vector<json::Value> files;
        for (const SkillManager::SkillFileInfo& file : skills_.list_files()) {
            files.push_back(json_object({{"name", json_string(file.name)},
                                         {"description", json_string(file.description)}}));
        }
        return text_result(render(json_object({{"type", json_string(kListSkillFiles)},
                                               {"files", json_array(std::move(files))}})));
    }
    if (name == kReadSkillFile) {
        // names: primitive contents with blanks dropped (JsonNull reads as "null").
        std::vector<std::string> names;
        const json::Value* raw_names = map_get(args, "names");
        if (raw_names != nullptr && raw_names->kind == json::Kind::Array) {
            for (const json::Value& element : raw_names->elements) {
                const std::optional<std::string> content = primitive_content(&element);
                if (!content.has_value() || json::is_blank(*content)) continue;
                names.push_back(*content);
            }
        }
        if (!names.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i > 0) joined += "\n\n";
                joined += "--- " + names[i] + " ---\n";
                joined += skills_.read_file(names[i]);
            }
            return text_result(std::move(joined));
        }
        if (!json::is_blank(arg(args, "name"))) {
            return text_result(skills_.read_file(arg(args, "name")));
        }
        return error_result("Error: Provide name or names.");
    }
    if (name == kCreateSkillFile) {
        return text_result(skills_.create_file(arg(args, "name"), arg(args, "content"),
                                               arg(args, "description")));
    }
    if (name == kEditSkillFile) {
        const std::string file_name = arg(args, "name");
        const std::string operation = lower_ascii(json::trim(arg(args, "operation")));
        if (operation == "replace") {
            return text_result(skills_.edit_file(file_name, arg(args, "content")));
        }
        if (operation == "patch") {
            const std::string old_string = arg(args, "old_string");
            if (old_string.empty()) {
                return error_result("Error: patch requires a non-empty old_string.");
            }
            return text_result(skills_.edit_file(file_name, std::nullopt, std::nullopt,
                                                 std::nullopt, old_string,
                                                 arg(args, "new_string")));
        }
        if (operation == "rename") {
            const std::string new_name = arg(args, "new_name");
            if (json::is_blank(new_name)) {
                return error_result("Error: rename requires a non-blank new_name.");
            }
            return text_result(skills_.edit_file(file_name, std::nullopt, new_name));
        }
        if (operation == "describe") {
            return text_result(skills_.edit_file(file_name, std::nullopt, std::nullopt,
                                                 arg(args, "description")));
        }
        return error_result("Error: operation must be replace, patch, rename, or describe.");
    }
    if (name == kDeleteSkillFile) {
        return text_result(skills_.delete_file(arg(args, "name")));
    }
    return error_result("Unknown tool: " + name);
}

}  // namespace agora::core::skill
