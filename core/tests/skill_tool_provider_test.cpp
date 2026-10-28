// Unit tests for the SkillToolProvider port: the read/modify definition gates, the
// execution-time access re-check that runs before argument decoding, the list/read/create/
// edit/delete dispatch over a real SkillManager, the arg()/JsonNull quirks, the isError
// envelopes and the manager exceptions that propagate without an envelope.

#include "agora/core/skill_tool_provider.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"

namespace skill = agora::core::skill;
namespace shell_provider = agora::core::shell_provider;

namespace {

struct Fixture {
    std::filesystem::path directory;
    std::unique_ptr<skill::SkillManager> manager;
    std::unique_ptr<skill::SkillToolProvider> provider;

    explicit Fixture(const std::string& name) {
        directory =
            std::filesystem::temp_directory_path() / ("agora_skill_tool_provider_" + name);
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        manager = std::make_unique<skill::SkillManager>(directory.string());
        provider = std::make_unique<skill::SkillToolProvider>(*manager);
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

shell_provider::GenerationContext make_ctx(bool read_access = true, bool modify_access = true) {
    shell_provider::GenerationContext ctx;
    ctx.skill_read_access = read_access;
    ctx.skill_modify_access = modify_access;
    return ctx;
}

}  // namespace

// ---- Definitions -------------------------------------------------------------------------------

TEST(skill_definitions_honour_access_gates) {
    Fixture fixture("defs");
    CHECK(fixture.provider->definitions(make_ctx(false, false)).empty());

    const auto read_only = fixture.provider->definitions(make_ctx(true, false));
    CHECK_EQ(std::size_t(2), read_only.size());
    CHECK_EQ(std::string("list_skill_files"), read_only[0].function.name);
    CHECK_EQ(std::string("read_skill_file"), read_only[1].function.name);

    const auto modify_only = fixture.provider->definitions(make_ctx(false, true));
    CHECK_EQ(std::size_t(3), modify_only.size());
    CHECK_EQ(std::string("create_skill_file"), modify_only[0].function.name);
    CHECK_EQ(std::string("edit_skill_file"), modify_only[1].function.name);
    CHECK_EQ(std::string("delete_skill_file"), modify_only[2].function.name);

    const auto both = fixture.provider->definitions(make_ctx());
    CHECK_EQ(std::size_t(5), both.size());
    CHECK_EQ(std::string("list_skill_files"), both[0].function.name);
    CHECK_EQ(std::string("read_skill_file"), both[1].function.name);
    CHECK_EQ(std::string("create_skill_file"), both[2].function.name);
    CHECK_EQ(std::string("edit_skill_file"), both[3].function.name);
    CHECK_EQ(std::string("delete_skill_file"), both[4].function.name);

    CHECK(fixture.provider->handles("list_skill_files"));
    CHECK(fixture.provider->handles("read_skill_file"));
    CHECK(fixture.provider->handles("create_skill_file"));
    CHECK(fixture.provider->handles("edit_skill_file"));
    CHECK(fixture.provider->handles("delete_skill_file"));
    CHECK(!fixture.provider->handles("memory_read_file"));
}

TEST(skill_definitions_exact_shapes) {
    Fixture fixture("shapes");
    const auto definitions = fixture.provider->definitions(make_ctx());
    CHECK_EQ(std::size_t(5), definitions.size());
    CHECK_EQ(std::string("function"), definitions[0].type);

    // list_skill_files: no properties, no required.
    CHECK_EQ(std::string("List saved skill files with their names and compact descriptions."),
             definitions[0].function.description);
    CHECK(definitions[0].function.parameters.properties.empty());
    CHECK(definitions[0].function.parameters.required.empty());

    // read_skill_file: name + names (array of string), nothing required.
    CHECK_EQ(std::string("Read one or more saved skill files. Read a relevant skill before "
                         "applying it."),
             definitions[1].function.description);
    const auto& read_props = definitions[1].function.parameters.properties;
    CHECK_EQ(std::size_t(2), read_props.size());
    CHECK_EQ(std::string("name"), read_props[0].first);
    CHECK_EQ(std::string("string"), read_props[0].second.type);
    CHECK_EQ(std::string("One skill file name."), read_props[0].second.description);
    CHECK_EQ(std::string("names"), read_props[1].first);
    CHECK_EQ(std::string("array"), read_props[1].second.type);
    CHECK_EQ(std::string("Multiple skill file names."), read_props[1].second.description);
    CHECK(read_props[1].second.items);
    CHECK_EQ(std::string("string"), read_props[1].second.items->type);
    CHECK_EQ(std::string("A skill file name."), read_props[1].second.items->description);
    CHECK(definitions[1].function.parameters.required.empty());

    // create_skill_file.
    CHECK_EQ(std::string("Create a saved Markdown skill file."),
             definitions[2].function.description);
    const auto& create_props = definitions[2].function.parameters.properties;
    CHECK_EQ(std::size_t(3), create_props.size());
    CHECK_EQ(std::string("name"), create_props[0].first);
    CHECK_EQ(std::string("The skill file name."), create_props[0].second.description);
    CHECK_EQ(std::string("content"), create_props[1].first);
    CHECK_EQ(std::string("The complete Markdown instructions."),
             create_props[1].second.description);
    CHECK_EQ(std::string("description"), create_props[2].first);
    CHECK_EQ(std::string("A compact catalog description."),
             create_props[2].second.description);
    CHECK(definitions[2].function.parameters.required ==
          (std::vector<std::string>{"name", "content"}));

    // edit_skill_file: seven properties in declaration order.
    CHECK_EQ(std::string("Edit one aspect of a saved skill file. operation must be replace, "
                         "patch, rename, or describe."),
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
    CHECK_EQ(std::string("The target file name for rename."),
             edit_props[5].second.description);
    CHECK_EQ(std::string("description"), edit_props[6].first);
    CHECK_EQ(std::string("The new compact description for describe. May be empty to remove it."),
             edit_props[6].second.description);
    CHECK(definitions[3].function.parameters.required ==
          (std::vector<std::string>{"name", "operation"}));

    // delete_skill_file.
    CHECK_EQ(std::string("Delete a saved skill file."), definitions[4].function.description);
    const auto& delete_props = definitions[4].function.parameters.properties;
    CHECK_EQ(std::size_t(1), delete_props.size());
    CHECK_EQ(std::string("name"), delete_props[0].first);
    CHECK_EQ(std::string("The skill file name to delete."),
             delete_props[0].second.description);
    CHECK(definitions[4].function.parameters.required == (std::vector<std::string>{"name"}));
}

// ---- Execution gates and argument decoding ---------------------------------------------------------

TEST(skill_access_gates_run_before_argument_decoding) {
    Fixture fixture("gates");
    auto result =
        fixture.provider->execute_one_shot_result("list_skill_files", "{", make_ctx(false, true));
    CHECK_EQ(std::string("Error: Skill read access is disabled."), result.text);
    CHECK(result.is_error);

    result = fixture.provider->execute_one_shot_result("delete_skill_file", "{",
                                                       make_ctx(true, false));
    CHECK_EQ(std::string("Error: Skill modify access is disabled."), result.text);
    CHECK(result.is_error);

    // Names outside the two gate sets skip the check, so decoding runs (and throws).
    bool threw = false;
    try {
        fixture.provider->execute_one_shot_result("nope", "{", make_ctx(false, false));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(skill_argument_decoding_and_text_adapter) {
    Fixture fixture("args");
    CHECK_EQ(std::string(R"({"type":"list_skill_files","files":[]})"),
             fixture.provider->execute("list_skill_files", "", make_ctx()));

    bool threw = false;
    try {
        fixture.provider->execute("list_skill_files", "{", make_ctx());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        fixture.provider->execute("list_skill_files", "[1]", make_ctx());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // execute() is the .text adapter of executeOneShotResult().
    const auto result =
        fixture.provider->execute_one_shot_result("read_skill_file", "{}", make_ctx());
    CHECK_EQ(std::string("Error: Provide name or names."), result.text);
    CHECK(result.is_error);
    CHECK_EQ(result.text, fixture.provider->execute("read_skill_file", "{}", make_ctx()));
}

// ---- list / read -----------------------------------------------------------------------------------

TEST(skill_create_and_list) {
    Fixture fixture("list");
    CHECK_EQ(std::string("Created alpha.md"),
             fixture.provider->execute(
                 "create_skill_file",
                 R"({"name":"alpha","content":"# Alpha","description":"First skill"})",
                 make_ctx()));
    CHECK_EQ(std::string("Created beta.md"),
             fixture.provider->execute("create_skill_file",
                                       R"({"name":"beta.md","content":"# Beta"})", make_ctx()));
    // Names are sanitized and get the .md suffix.
    CHECK_EQ(std::string("Created dir_gamma.md"),
             fixture.provider->execute("create_skill_file",
                                       R"({"name":"dir/gamma","content":"# Gamma"})",
                                       make_ctx()));

    CHECK_EQ(std::string(R"({"type":"list_skill_files","files":[)"
                         R"({"name":"alpha.md","description":"First skill"},)"
                         R"({"name":"beta.md","description":""},)"
                         R"({"name":"dir_gamma.md","description":""}]})"),
             fixture.provider->execute("list_skill_files", "{}", make_ctx()));
    CHECK_EQ(std::string("# Alpha"), fixture.manager->read_file("alpha.md"));

    const auto result =
        fixture.provider->execute_one_shot_result("list_skill_files", "{}", make_ctx());
    CHECK(!result.is_error);
}

TEST(skill_read_joins_names_and_falls_back) {
    Fixture fixture("read");
    fixture.manager->create_file("alpha.md", "Alpha body");
    fixture.manager->create_file("beta.md", "Beta body");

    CHECK_EQ(std::string("--- alpha.md ---\nAlpha body\n\n--- beta.md ---\nBeta body"),
             fixture.provider->execute("read_skill_file",
                                       R"({"names":["alpha.md","beta.md"]})", make_ctx()));
    // Blank entries and non-primitives are dropped; an all-dropped list falls back to name.
    CHECK_EQ(std::string("Alpha body"),
             fixture.provider->execute("read_skill_file",
                                       R"({"names":["  ",{"x":1}],"name":"alpha"})",
                                       make_ctx()));
    // A non-array names value is ignored entirely.
    CHECK_EQ(std::string("Beta body"),
             fixture.provider->execute("read_skill_file",
                                       R"({"names":"alpha","name":"beta"})", make_ctx()));

    // Missing files propagate the manager exception (no envelope).
    bool threw = false;
    try {
        fixture.provider->execute("read_skill_file", R"({"name":"missing"})", make_ctx());
    } catch (const std::invalid_argument& error) {
        threw = std::string(error.what()) == "File not found: missing";
    }
    CHECK(threw);
}

TEST(skill_json_null_arguments_read_as_null) {
    Fixture fixture("nulls");
    // JsonNull is a JsonPrimitive with content "null": create writes a file literally named
    // null.md with content "null".
    CHECK_EQ(std::string("Created null.md"),
             fixture.provider->execute("create_skill_file", R"({"name":null,"content":null})",
                                       make_ctx()));
    CHECK_EQ(std::string("null"), fixture.manager->read_file("null.md"));
    // And a JsonNull names entry is a "null" file name, not a dropped entry.
    CHECK_EQ(std::string("--- null ---\nnull"),
             fixture.provider->execute("read_skill_file", R"({"names":[null]})", make_ctx()));
}

// ---- create / edit / delete ------------------------------------------------------------------------

TEST(skill_create_duplicate_propagates) {
    Fixture fixture("dup");
    CHECK_EQ(std::string("Created one.md"),
             fixture.provider->execute("create_skill_file", R"({"name":"one","content":"x"})",
                                       make_ctx()));
    bool threw = false;
    try {
        fixture.provider->execute("create_skill_file", R"({"name":"one","content":"y"})",
                                  make_ctx());
    } catch (const std::invalid_argument& error) {
        threw = std::string(error.what()) == "File already exists: one.md";
    }
    CHECK(threw);
}

TEST(skill_edit_operations) {
    Fixture fixture("edit");
    fixture.manager->create_file("doc.md", "fresh content", "old desc");

    // operation is trimmed and lowercased.
    CHECK_EQ(std::string("Updated doc.md"),
             fixture.provider->execute(
                 "edit_skill_file",
                 R"({"name":"doc","operation":"  REPLACE  ","content":"fresh"})", make_ctx()));
    CHECK_EQ(std::string("fresh"), fixture.manager->read_file("doc.md"));

    // patch requires a non-empty old_string before touching the manager.
    auto result = fixture.provider->execute_one_shot_result(
        "edit_skill_file", R"({"name":"doc","operation":"patch","old_string":""})",
        make_ctx());
    CHECK_EQ(std::string("Error: patch requires a non-empty old_string."), result.text);
    CHECK(result.is_error);

    CHECK_EQ(std::string("Updated doc.md"),
             fixture.provider->execute(
                 "edit_skill_file",
                 R"({"name":"doc","operation":"patch","old_string":"fresh","new_string":"patched"})",
                 make_ctx()));
    CHECK_EQ(std::string("patched"), fixture.manager->read_file("doc.md"));

    // rename requires a non-blank new_name.
    result = fixture.provider->execute_one_shot_result(
        "edit_skill_file", R"({"name":"doc","operation":"rename","new_name":"   "})",
        make_ctx());
    CHECK_EQ(std::string("Error: rename requires a non-blank new_name."), result.text);
    CHECK(result.is_error);

    CHECK_EQ(std::string("Updated renamed.md"),
             fixture.provider->execute(
                 "edit_skill_file",
                 R"({"name":"doc","operation":"rename","new_name":"renamed"})", make_ctx()));
    CHECK_EQ(std::string("patched"), fixture.manager->read_file("renamed.md"));

    // describe updates, then a blank description removes the metadata.
    CHECK_EQ(std::string("Updated renamed.md"),
             fixture.provider->execute(
                 "edit_skill_file",
                 R"({"name":"renamed","operation":"describe","description":"New desc"})",
                 make_ctx()));
    CHECK_EQ(std::string("New desc"), fixture.manager->get_description("renamed.md"));
    CHECK_EQ(std::string("Updated renamed.md"),
             fixture.provider->execute(
                 "edit_skill_file",
                 R"({"name":"renamed","operation":"describe","description":""})", make_ctx()));
    CHECK_EQ(std::string(""), fixture.manager->get_description("renamed.md"));

    // Unknown and missing operations share the validation envelope.
    result = fixture.provider->execute_one_shot_result(
        "edit_skill_file", R"({"name":"renamed","operation":"append"})", make_ctx());
    CHECK_EQ(std::string("Error: operation must be replace, patch, rename, or describe."),
             result.text);
    CHECK(result.is_error);
    result = fixture.provider->execute_one_shot_result("edit_skill_file",
                                                       R"({"name":"renamed"})", make_ctx());
    CHECK_EQ(std::string("Error: operation must be replace, patch, rename, or describe."),
             result.text);

    // A missing file propagates the manager exception.
    bool threw = false;
    try {
        fixture.provider->execute("edit_skill_file",
                                  R"({"name":"nope","operation":"replace","content":"x"})",
                                  make_ctx());
    } catch (const std::invalid_argument& error) {
        threw = std::string(error.what()) == "File not found: nope";
    }
    CHECK(threw);
}

TEST(skill_delete_and_unknown_tool) {
    Fixture fixture("delete");
    fixture.manager->create_file("gone.md", "x");
    CHECK_EQ(std::string("Deleted gone.md"),
             fixture.provider->execute("delete_skill_file", R"({"name":"gone"})", make_ctx()));
    CHECK_EQ(std::string(R"({"type":"list_skill_files","files":[]})"),
             fixture.provider->execute("list_skill_files", "{}", make_ctx()));

    const auto unknown = fixture.provider->execute_one_shot_result("nope", "{}", make_ctx());
    CHECK_EQ(std::string("Unknown tool: nope"), unknown.text);
    CHECK(unknown.is_error);

    // Deleting again propagates the manager exception.
    bool threw = false;
    try {
        fixture.provider->execute("delete_skill_file", R"({"name":"gone"})", make_ctx());
    } catch (const std::invalid_argument& error) {
        threw = std::string(error.what()) == "File not found: gone";
    }
    CHECK(threw);
}
