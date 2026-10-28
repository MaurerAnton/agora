#include "agora/tools/scenario_builder.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "agora/core/json.hpp"
#include "check.hpp"

namespace scenarios = agora::tools::scenarios;
namespace json = agora::core::json;

namespace {

// Helper for creating json::Value strings
json::Value json_string_value(const std::string& text) {
    json::Value v;
    v.kind = json::Kind::String;
    v.text = text;
    return v;
}

TEST(scenario_builder_writes_json_files) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "test_scenarios";
    std::filesystem::remove_all(temp_dir);

    {
        scenarios::ScenarioBuilder builder(temp_dir);
        builder.add("test1", {
            std::pair<std::string, json::Value>("mode", json_string_value("read")),
            std::pair<std::string, json::Value>("file_name", json_string_value("test.md"))
        });
        builder.add("test2", {
            std::pair<std::string, json::Value>("mode", json_string_value("write"))
        });
        builder.write();
    }

    CHECK(std::filesystem::exists(temp_dir / "test1.json"));
    CHECK(std::filesystem::exists(temp_dir / "test2.json"));

    std::ifstream in(temp_dir / "test1.json");
    std::string content;
    std::getline(in, content);
    CHECK(content.find("\"name\":\"test1\"") != std::string::npos);
    CHECK(content.find("\"mode\":\"read\"") != std::string::npos);

    std::filesystem::remove_all(temp_dir);
}

TEST(kotlin_extractor_finds_class_declaration) {
    std::string kotlin_source =
        "package com.test\n"
        "\n"
        "import androidx.room.Entity\n"
        "import androidx.room.PrimaryKey\n"
        "\n"
        "/**\n"
        " * A test entity.\n"
        " */\n"
        "@Entity(tableName = \"test\")\n"
        "data class TestEntity(\n"
        "    @PrimaryKey\n"
        "    val id: String,\n"
        "    val name: String,\n"
        "    val value: Int = 0\n"
        ")\n";

    auto extracted = scenarios::KotlinExtractor::extract_declaration(kotlin_source, "TestEntity");
    CHECK(extracted.find("data class TestEntity") != std::string::npos);
    CHECK(extracted.find("@Entity(tableName = \"test\")") != std::string::npos);
    CHECK(extracted.find("/**") != std::string::npos);  // includes annotation block
    CHECK(extracted.find("@PrimaryKey") != std::string::npos);
}

TEST(kotlin_extractor_handles_nested_braces) {
    std::string kotlin_source =
        "class Outer {\n"
        "    fun test(): Int {\n"
        "        return 42\n"
        "    }\n"
        "}\n";

    auto extracted = scenarios::KotlinExtractor::extract_declaration(kotlin_source, "Outer");
    CHECK(extracted.find("class Outer") != std::string::npos);
    CHECK(extracted.find("}") != std::string::npos);
    // Should include the full class body
    size_t first_brace = extracted.find('{');
    size_t last_brace = extracted.rfind('}');
    CHECK(first_brace != std::string::npos);
    CHECK(last_brace != std::string::npos);
    CHECK(last_brace > first_brace);
}

TEST(kotlin_extractor_skips_comments_in_strings) {
    std::string kotlin_source =
        "class Foo {\n"
        "    val s = \"// not a comment\"\n"
        "}\n";

    auto extracted = scenarios::KotlinExtractor::extract_declaration(kotlin_source, "Foo");
    CHECK(extracted.find("// not a comment") != std::string::npos);
    CHECK(extracted.size() > 10);  // Should have full content
}

}  // namespace