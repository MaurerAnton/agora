// Unit tests for the MemoryToolProvider port: the two context gates, the seven definitions,
// the executeOneShotResult envelopes (isError), the read_memory_file join format and the
// arg()/trim quirks the Kotlin dispatch relies on.

#include "agora/core/memory_tool_provider.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"

namespace memory = agora::core::memory;
namespace shell_provider = agora::core::shell_provider;

namespace {

struct ScriptedMemory final : memory::MemorySeam {
    std::vector<memory::MemoryFile> files;
    std::map<std::string, std::string> contents;

    std::vector<std::string> reads;
    std::string created_name;
    std::string created_content;
    std::string created_description;
    int create_calls = 0;
    std::vector<memory::EditFileRequest> edits;
    std::vector<std::string> deletes;
    std::string active_memory = "stored active memory";
    int update_calls = 0;
    std::string update_content;
    std::string update_mode;
    std::optional<std::string> update_old_string;
    std::optional<std::string> update_new_string;
    std::string next_result = "seam-result";

    std::vector<memory::MemoryFile> list_files() override { return files; }

    std::string read_file(const std::string& name) override {
        reads.push_back(name);
        const auto found = contents.find(name);
        return found == contents.end() ? "" : found->second;
    }

    std::string create_file(const std::string& name, const std::string& content,
                            const std::string& description) override {
        ++create_calls;
        created_name = name;
        created_content = content;
        created_description = description;
        return next_result;
    }

    std::string edit_file(const memory::EditFileRequest& request) override {
        edits.push_back(request);
        return next_result;
    }

    std::string delete_file(const std::string& name) override {
        deletes.push_back(name);
        return next_result;
    }

    std::string get_active_memory() override { return active_memory; }

    std::string update_active_memory(const std::string& content, const std::string& mode,
                                     const std::optional<std::string>& old_string,
                                     const std::optional<std::string>& new_string) override {
        ++update_calls;
        update_content = content;
        update_mode = mode;
        update_old_string = old_string;
        update_new_string = new_string;
        return next_result;
    }
};

struct Fixture {
    ScriptedMemory seam;
    memory::MemoryToolProvider provider;

    Fixture() : provider(seam) {}
};

shell_provider::GenerationContext make_ctx(bool saved = true, bool active = true) {
    shell_provider::GenerationContext ctx;
    ctx.access_saved_memories = saved;
    ctx.access_active_memory = active;
    return ctx;
}

}  // namespace

// ---- Definitions -------------------------------------------------------------------------------

TEST(memory_definitions_honour_both_gates) {
    Fixture fixture;
    CHECK(fixture.provider.definitions(make_ctx(false, false)).empty());
    CHECK_EQ(std::size_t(5), fixture.provider.definitions(make_ctx(true, false)).size());
    CHECK_EQ(std::size_t(2), fixture.provider.definitions(make_ctx(false, true)).size());
    CHECK_EQ(std::size_t(7), fixture.provider.definitions(make_ctx(true, true)).size());

    // handles() carries the seven names regardless of the gates.
    CHECK(fixture.provider.handles("list_memory_files"));
    CHECK(fixture.provider.handles("read_memory_file"));
    CHECK(fixture.provider.handles("create_memory_file"));
    CHECK(fixture.provider.handles("edit_memory_file"));
    CHECK(fixture.provider.handles("delete_memory_file"));
    CHECK(fixture.provider.handles("read_active_memory"));
    CHECK(fixture.provider.handles("update_active_memory"));
    CHECK(!fixture.provider.handles("web_search"));
}

TEST(memory_definitions_saved_tools_exact) {
    Fixture fixture;
    const auto definitions = fixture.provider.definitions(make_ctx(true, false));
    CHECK_EQ(std::size_t(5), definitions.size());

    CHECK_EQ(std::string("list_memory_files"), definitions[0].function.name);
    CHECK_EQ(std::string("List all files in the memory database with their names and "
                         "descriptions."),
             definitions[0].function.description);
    CHECK(definitions[0].function.parameters.properties.empty());

    CHECK_EQ(std::string("read_memory_file"), definitions[1].function.name);
    CHECK_EQ(std::string("Read the content of one or more files from the memory database."),
             definitions[1].function.description);
    const auto& read_props = definitions[1].function.parameters.properties;
    CHECK_EQ(std::size_t(2), read_props.size());
    CHECK_EQ(std::string("name"), read_props[0].first);
    CHECK_EQ(std::string("The file name to read."), read_props[0].second.description);
    CHECK_EQ(std::string("names"), read_props[1].first);
    CHECK_EQ(std::string("array"), read_props[1].second.type);
    CHECK_EQ(std::string("Multiple file names to read in one call."),
             read_props[1].second.description);
    CHECK(read_props[1].second.items);
    CHECK_EQ(std::string("string"), read_props[1].second.items->type);
    CHECK_EQ(std::string("A file name."), read_props[1].second.items->description);
    CHECK(definitions[1].function.parameters.required.empty());

    CHECK_EQ(std::string("create_memory_file"), definitions[2].function.name);
    CHECK_EQ(std::string("Create a new file in the memory database with the given content and "
                         "optional description."),
             definitions[2].function.description);
    const auto& create_props = definitions[2].function.parameters.properties;
    CHECK_EQ(std::size_t(3), create_props.size());
    CHECK_EQ(std::string("The file name to create (e.g., 'notes.md')."),
             create_props[0].second.description);
    CHECK_EQ(std::string("The markdown content for the file."), create_props[1].second.description);
    CHECK_EQ(std::string("A short description of what this file contains (optional)."),
             create_props[2].second.description);
    CHECK_EQ(std::size_t(2), definitions[2].function.parameters.required.size());
    CHECK_EQ(std::string("name"), definitions[2].function.parameters.required[0]);
    CHECK_EQ(std::string("content"), definitions[2].function.parameters.required[1]);

    CHECK_EQ(std::string("edit_memory_file"), definitions[3].function.name);
    CHECK_EQ(std::string("Edit one aspect of a memory file. operation must be replace, patch, "
                         "rename, or describe."),
             definitions[3].function.description);
    const auto& edit_props = definitions[3].function.parameters.properties;
    CHECK_EQ(std::size_t(7), edit_props.size());
    CHECK_EQ(std::string("name"), edit_props[0].first);
    CHECK_EQ(std::string("The current file name."), edit_props[0].second.description);
    CHECK_EQ(std::string("operation"), edit_props[1].first);
    CHECK_EQ(std::string("One of: replace, patch, rename, describe."),
             edit_props[1].second.description);
    CHECK_EQ(std::string("content"), edit_props[2].first);
    CHECK_EQ(std::string("Complete replacement content for replace. May be empty."),
             edit_props[2].second.description);
    CHECK_EQ(std::string("old_string"), edit_props[3].first);
    CHECK_EQ(std::string("Exact non-empty unique text to replace for patch."),
             edit_props[3].second.description);
    CHECK_EQ(std::string("new_string"), edit_props[4].first);
    CHECK_EQ(std::string("Replacement text for patch. May be empty to delete the match."),
             edit_props[4].second.description);
    CHECK_EQ(std::string("new_name"), edit_props[5].first);
    CHECK_EQ(std::string("The target file name for rename."), edit_props[5].second.description);
    CHECK_EQ(std::string("description"), edit_props[6].first);
    CHECK_EQ(std::string("The new description for describe. May be empty to remove it."),
             edit_props[6].second.description);
    CHECK_EQ(std::size_t(2), definitions[3].function.parameters.required.size());
    CHECK_EQ(std::string("name"), definitions[3].function.parameters.required[0]);
    CHECK_EQ(std::string("operation"), definitions[3].function.parameters.required[1]);

    CHECK_EQ(std::string("delete_memory_file"), definitions[4].function.name);
    CHECK_EQ(std::string("Delete a file from the memory database."),
             definitions[4].function.description);
    CHECK_EQ(std::string("The file name to delete."),
             definitions[4].function.parameters.properties[0].second.description);
    CHECK_EQ(std::size_t(1), definitions[4].function.parameters.required.size());
}

TEST(memory_definitions_active_tools_exact) {
    Fixture fixture;
    const auto definitions = fixture.provider.definitions(make_ctx(false, true));
    CHECK_EQ(std::size_t(2), definitions.size());

    CHECK_EQ(std::string("read_active_memory"), definitions[0].function.name);
    CHECK_EQ(
        std::string("Read the current stored active memory. The copy in the system prompt is "
                    "captured once per turn, so edits made during this turn appear here but not "
                    "in the prompt until the next turn."),
        definitions[0].function.description);

    CHECK_EQ(std::string("update_active_memory"), definitions[1].function.name);
    CHECK_EQ(
        std::string("Update the active memory context. Modes: 'replace' (overwrite with "
                    "'content'), 'append' (add 'content' to end), 'prepend' (add 'content' to "
                    "beginning), 'patch' (find 'old_string' exactly once and replace with "
                    "'new_string'). Default is replace."),
        definitions[1].function.description);
    const auto& props = definitions[1].function.parameters.properties;
    CHECK_EQ(std::size_t(4), props.size());
    CHECK_EQ(std::string("content"), props[0].first);
    CHECK_EQ(std::string("The content to write (for replace/append/prepend modes)."),
             props[0].second.description);
    CHECK_EQ(std::string("mode"), props[1].first);
    CHECK_EQ(std::string("One of: replace, append, prepend, patch. Default is replace."),
             props[1].second.description);
    CHECK_EQ(std::string("old_string"), props[2].first);
    CHECK_EQ(std::string("Exact string to find and replace in the active memory. Required for "
                         "patch mode. Must match exactly once."),
             props[2].second.description);
    CHECK_EQ(std::string("new_string"), props[3].first);
    CHECK_EQ(std::string("Replacement string for old_string in patch mode. Pass empty string to "
                         "delete the matched text."),
             props[3].second.description);
    CHECK_EQ(std::size_t(1), definitions[1].function.parameters.required.size());
    CHECK_EQ(std::string("content"), definitions[1].function.parameters.required[0]);
}

// ---- Argument decoding and dispatch ---------------------------------------------------------------------

TEST(memory_execute_malformed_args_throw) {
    Fixture fixture;
    const auto ctx = make_ctx();
    // Decoding happens BEFORE the dispatch and has no runCatching here → it propagates.
    for (const std::string bad : {std::string("{oops"), std::string("[1]"),
                                  std::string("\"str\"")}) {
        bool threw = false;
        try {
            fixture.provider.execute("list_memory_files", bad, ctx);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
    // Blank arguments become "{}" and reach dispatch (unknown name → error result, not throw).
    const auto result = fixture.provider.execute_one_shot_result("nope", "   ", ctx);
    CHECK(result.is_error);
    CHECK_EQ(std::string("Unknown tool: nope"), result.text);
}

TEST(memory_list_memory_files_envelope) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.files = {{"notes.md", "My notes"}, {"tasks.md", ""}};
    CHECK_EQ(std::string(R"({"type":"list_memory_files","files":[)"
                         R"({"name":"notes.md","description":"My notes"},)"
                         R"({"name":"tasks.md","description":""}]})"),
             fixture.provider.execute("list_memory_files", "", ctx));

    // Empty database: same shape with an explicit empty array.
    fixture.seam.files.clear();
    CHECK_EQ(std::string(R"({"type":"list_memory_files","files":[]})"),
             fixture.provider.execute("list_memory_files", "{}", ctx));
}

TEST(memory_read_memory_file_single_name) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.contents["notes.md"] = "Alpha body";

    CHECK_EQ(std::string("Alpha body"),
             fixture.provider.execute("read_memory_file", R"({"name":"notes.md"})", ctx));
    CHECK_EQ(1, static_cast<int>(fixture.seam.reads.size()));
    CHECK_EQ(std::string("notes.md"), fixture.seam.reads.back());

    // Whitespace-only names are NOT blank-filtered (Kotlin isNotEmpty).
    fixture.provider.execute("read_memory_file", R"({"name":" "})", ctx);
    CHECK_EQ(std::string(" "), fixture.seam.reads.back());

    // JsonNull IS a primitive: content "null" is read as a literal file name (kotlinx quirk).
    fixture.provider.execute("read_memory_file", R"({"name":null})", ctx);
    CHECK_EQ(std::string("null"), fixture.seam.reads.back());

    // Nothing provided → the error envelope (isError).
    const auto error =
        fixture.provider.execute_one_shot_result("read_memory_file", "{}", ctx);
    CHECK(error.is_error);
    CHECK_EQ(std::string("Error: No file name provided. Use 'name' for a single file or 'names' "
                         "for multiple files."),
             error.text);
}

TEST(memory_read_memory_file_names_array_join) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.contents["a.md"] = "Alpha";
    fixture.seam.contents["b.md"] = "Beta";
    fixture.seam.contents[" "] = "space";
    fixture.seam.contents["null"] = "nulled";

    CHECK_EQ(std::string("--- a.md ---\nAlpha\n\n--- b.md ---\nBeta"),
             fixture.provider.execute(
                 "read_memory_file", R"({"names":["a.md","b.md"]})", ctx));

    // Objects drop (→"") and filter out; whitespace-only survives isNotEmpty; JsonNull's
    // content "null" survives as a name. "--- $name ---" with a one-space name → 3 spaces.
    CHECK_EQ(std::string("---   ---\nspace\n\n--- null ---\nnulled"),
             fixture.provider.execute(
                 "read_memory_file", R"({"names":[" ",{"x":1},null]})", ctx));

    // An array that filters down to nothing joins to "" (no error, no reads).
    fixture.seam.reads.clear();
    const auto empty = fixture.provider.execute_one_shot_result(
        "read_memory_file", R"({"names":[""]})", ctx);
    CHECK(!empty.is_error);
    CHECK_EQ(std::string(""), empty.text);
    CHECK(fixture.seam.reads.empty());

    // A present-but-empty array falls back to the single-name path.
    CHECK_EQ(std::string("Alpha"),
             fixture.provider.execute(
                 "read_memory_file", R"({"names":[],"name":"a.md"})", ctx));
}

TEST(memory_create_memory_file_passthrough) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.next_result = R"({"type":"create_memory_file","ok":true})";
    CHECK_EQ(std::string(R"({"type":"create_memory_file","ok":true})"),
             fixture.provider.execute(
                 "create_memory_file",
                 R"({"name":"notes.md","content":"# Notes","description":"My notes"})", ctx));
    CHECK_EQ(1, fixture.seam.create_calls);
    CHECK_EQ(std::string("notes.md"), fixture.seam.created_name);
    CHECK_EQ(std::string("# Notes"), fixture.seam.created_content);
    CHECK_EQ(std::string("My notes"), fixture.seam.created_description);

    // arg() defaults to "" for missing fields; numbers stringify through content.
    fixture.provider.execute("create_memory_file", R"({"name":42})", ctx);
    CHECK_EQ(std::string("42"), fixture.seam.created_name);
    CHECK_EQ(std::string(""), fixture.seam.created_content);
    CHECK_EQ(std::string(""), fixture.seam.created_description);
}

TEST(memory_edit_memory_file_operations) {
    Fixture fixture;
    const auto ctx = make_ctx();
    const std::string expected = "edit-result";
    fixture.seam.next_result = expected;

    // operation is trimmed AND lowercased before dispatch.
    fixture.provider.execute(
        "edit_memory_file",
        R"({"name":"notes.md","operation":"  REPLACE  ","content":"new body"})", ctx);
    CHECK_EQ(1, static_cast<int>(fixture.seam.edits.size()));
    CHECK_EQ(std::string("notes.md"), fixture.seam.edits.back().name);
    CHECK(fixture.seam.edits.back().content.has_value());
    CHECK_EQ(std::string("new body"), *fixture.seam.edits.back().content);
    CHECK(!fixture.seam.edits.back().old_string.has_value());

    fixture.provider.execute(
        "edit_memory_file",
        R"({"name":"notes.md","operation":"patch","old_string":"old","new_string":""})",
        ctx);
    CHECK_EQ(2, static_cast<int>(fixture.seam.edits.size()));
    CHECK(fixture.seam.edits.back().old_string.has_value());
    CHECK_EQ(std::string("old"), *fixture.seam.edits.back().old_string);
    CHECK(fixture.seam.edits.back().new_string.has_value());
    CHECK_EQ(std::string(""), *fixture.seam.edits.back().new_string);

    fixture.provider.execute(
        "edit_memory_file",
        R"({"name":"notes.md","operation":"rename","new_name":"  new.md  "})", ctx);
    CHECK_EQ(3, static_cast<int>(fixture.seam.edits.size()));
    // takeIf(isNotBlank) keeps the padding — only blank rejects.
    CHECK(fixture.seam.edits.back().new_name.has_value());
    CHECK_EQ(std::string("  new.md  "), *fixture.seam.edits.back().new_name);

    fixture.provider.execute(
        "edit_memory_file",
        R"({"name":"notes.md","operation":"describe","description":""})", ctx);
    CHECK_EQ(4, static_cast<int>(fixture.seam.edits.size()));
    CHECK(fixture.seam.edits.back().description.has_value());
    CHECK_EQ(std::string(""), *fixture.seam.edits.back().description);

    // Validation failures carry isError.
    auto error = fixture.provider.execute_one_shot_result(
        "edit_memory_file", R"({"name":"f","operation":"patch"})", ctx);
    CHECK(error.is_error);
    CHECK_EQ(std::string("Error: patch requires a non-empty old_string."), error.text);
    CHECK_EQ(4, static_cast<int>(fixture.seam.edits.size()));

    error = fixture.provider.execute_one_shot_result(
        "edit_memory_file", R"({"name":"f","operation":"rename","new_name":"   "})", ctx);
    CHECK(error.is_error);
    CHECK_EQ(std::string("Error: rename requires a non-blank new_name."), error.text);

    error = fixture.provider.execute_one_shot_result(
        "edit_memory_file", R"({"name":"f","operation":"upsert"})", ctx);
    CHECK(error.is_error);
    CHECK_EQ(std::string("Error: operation must be replace, patch, rename, or describe."),
             error.text);
    CHECK_EQ(4, static_cast<int>(fixture.seam.edits.size()));
}

TEST(memory_delete_and_active_read_passthrough) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.next_result = "delete-result";
    CHECK_EQ(std::string("delete-result"),
             fixture.provider.execute("delete_memory_file", R"({"name":"old.md"})", ctx));
    CHECK_EQ(1, static_cast<int>(fixture.seam.deletes.size()));
    CHECK_EQ(std::string("old.md"), fixture.seam.deletes.back());

    CHECK_EQ(std::string("stored active memory"),
             fixture.provider.execute("read_active_memory", "{}", ctx));
}

TEST(memory_update_active_memory_modes) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.next_result = "updated";

    // mode absent or blank → "replace" (ifBlank, Unicode); old/new blank → absent.
    fixture.provider.execute("update_active_memory", R"({"content":"body"})", ctx);
    CHECK_EQ(1, fixture.seam.update_calls);
    CHECK_EQ(std::string("body"), fixture.seam.update_content);
    CHECK_EQ(std::string("replace"), fixture.seam.update_mode);
    CHECK(!fixture.seam.update_old_string.has_value());
    CHECK(!fixture.seam.update_new_string.has_value());

    fixture.provider.execute(
        "update_active_memory", R"({"content":"body","mode":"   "})", ctx);
    CHECK_EQ(std::string("replace"), fixture.seam.update_mode);

    // mode is compared EXACTLY: "PATCH" is not the patch gate (no trim/lowercase here) and
    // travels through unchanged.
    fixture.provider.execute(
        "update_active_memory", R"({"content":"body","mode":"PATCH"})", ctx);
    CHECK_EQ(std::string("PATCH"), fixture.seam.update_mode);
    CHECK(!fixture.seam.update_old_string.has_value());

    // patch without old_string fails…
    auto error = fixture.provider.execute_one_shot_result(
        "update_active_memory", R"({"content":"body","mode":"patch"})", ctx);
    CHECK(error.is_error);
    CHECK_EQ(std::string("Error: 'old_string' is required for patch mode."), error.text);

    // …but JsonNull's content "null" is not blank, so it passes the gate literally (quirk).
    fixture.provider.execute(
        "update_active_memory", R"({"content":"body","mode":"patch","old_string":null})", ctx);
    CHECK_EQ(std::string("patch"), fixture.seam.update_mode);
    CHECK(fixture.seam.update_old_string.has_value());
    CHECK_EQ(std::string("null"), *fixture.seam.update_old_string);

    fixture.provider.execute(
        "update_active_memory",
        R"({"content":"body","mode":"patch","old_string":"a","new_string":"b"})", ctx);
    CHECK(fixture.seam.update_old_string.has_value());
    CHECK_EQ(std::string("a"), *fixture.seam.update_old_string);
    CHECK(fixture.seam.update_new_string.has_value());
    CHECK_EQ(std::string("b"), *fixture.seam.update_new_string);
}

TEST(memory_execute_text_adapter_and_unknown_tool) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.seam.active_memory = "live";
    // execute() is exactly execute_one_shot_result().text on both paths.
    CHECK_EQ(fixture.provider.execute_one_shot_result("read_active_memory", "{}", ctx).text,
             fixture.provider.execute("read_active_memory", "{}", ctx));

    const auto unknown = fixture.provider.execute_one_shot_result("zombie_tool", "{}", ctx);
    CHECK(unknown.is_error);
    CHECK_EQ(std::string("Unknown tool: zombie_tool"), unknown.text);
}