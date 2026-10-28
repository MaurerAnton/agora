#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <utility>

#include "agora/core/json.hpp"

namespace agora::tools::scenarios {

namespace json = ::agora::core::json;

/**
 * Scenario builder utility for golden stand test fixture generation.
 * Replaces the Python gen_scenarios.py scripts with C++ equivalents.
 */
class ScenarioBuilder {
public:
    explicit ScenarioBuilder(std::filesystem::path output_dir)
        : output_dir_(std::move(output_dir)) {
        std::filesystem::create_directories(output_dir_);
    }

    /** Add a scenario with key-value pairs. */
    ScenarioBuilder& add(const std::string& name,
                         std::vector<std::pair<std::string, json::Value>> fields) {
        json::Value scenario;
        scenario.kind = json::Kind::Object;
        scenario.members.push_back(json::Member{"name", json_string(name)});
        for (auto& field : fields) {
            scenario.members.push_back(json::Member{std::move(field.first), std::move(field.second)});
        }
        scenarios_.push_back(std::move(scenario));
        return *this;
    }

    /** Write all scenarios to individual .json files. */
    void write() const {
        for (const auto& scenario : scenarios_) {
            std::string name = extract_name(scenario);
            std::ofstream out(output_dir_ / (name + ".json"), std::ios::binary);
            out << json::to_kotlin_string(scenario) << "\n";
        }
    }

private:
    static json::Value json_string(const std::string& text) {
        json::Value v;
        v.kind = json::Kind::String;
        v.text = text;
        return v;
    }

    static std::string extract_name(const json::Value& scenario) {
        for (const auto& member : scenario.members) {
            if (member.key == "name" && member.value.kind == json::Kind::String) {
                return member.value.text;
            }
        }
        return "unknown";
    }

    std::filesystem::path output_dir_;
    std::vector<json::Value> scenarios_;
};

/**
 * Kotlin source declaration extractor.
 * Replaces the Python extract_*.py scripts that lift verbatim declarations
 * from Kotlin sources for golden stand compilation.
 */
class KotlinExtractor {
public:
    /** Load a Kotlin source file. */
    static std::string load(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /** Extract a declaration with its annotation block. */
    static std::string extract_declaration(const std::string& text,
                                           const std::string& declaration_name) {
        // Find the class declaration
        std::size_t pos = find_declaration(text, declaration_name);
        if (pos == std::string::npos) {
            throw std::runtime_error("error: declaration `" + declaration_name + "` not found");
        }

        // Walk backwards to include annotation block
        std::size_t start = include_annotations(text, pos);

        // Find end of declaration
        std::size_t end = declaration_end(text, pos);

        return text.substr(start, end - start);
    }

    /** Write extracted declaration with header. */
    static void write(const std::filesystem::path& output,
                      const std::string& header, const std::string& block) {
        std::ofstream out(output, std::ios::binary);
        out << header << block << "\n";
    }

private:
    static std::size_t find_declaration(const std::string& text, const std::string& name) {
        // Match patterns like "class Name", "data class Name", "internal data class Name"
        // The pattern is always "class " followed by name
        std::string pattern = "class " + name;
        std::size_t pos = 0;
        while ((pos = text.find(pattern, pos)) != std::string::npos) {
            // Ensure word boundary after name
            std::size_t after = pos + pattern.size();
            if (after >= text.size() || !std::isalnum(static_cast<unsigned char>(text[after]))) {
                return pos;
            }
            pos += pattern.size();
        }
        return std::string::npos;
    }

    static std::size_t include_annotations(const std::string& text, std::size_t declaration_pos) {
        // Start of the line holding the declaration.
        std::size_t start = 0;
        if (declaration_pos > 0) {
            const std::size_t nl = text.rfind('\n', declaration_pos - 1);
            start = (nl == std::string::npos) ? 0 : nl + 1;
        }

        // Walk back over contiguous annotation/comment lines, exactly like the Python
        // extractor: '@', '//' or '/*' prefixed lines and '* ' continuations are owned by
        // the declaration; a blank or any other line stops the walk.
        while (start > 0 && start <= text.size()) {
            std::size_t prev_start = 0;
            if (start >= 2) {
                const std::size_t nl = text.rfind('\n', start - 2);
                if (nl != std::string::npos) prev_start = nl + 1;
            }
            if (prev_start >= start) break;  // no progress

            // text[start - 1] is the '\n' terminating the previous line.
            const std::string line = text.substr(prev_start, start - 1 - prev_start);
            if (!is_annotation_or_comment(trim(line))) break;
            start = prev_start;
        }
        return start;
    }

    static std::size_t declaration_end(const std::string& text, std::size_t start) {
        int paren = 0;
        int brace = 0;
        std::size_t at = start;
        while (at < text.size()) {
            char ch = text[at];
            if (ch == '"') {
                at = skip_literal(text, at);
                continue;
            }
            if (text.compare(at, 2, "//") == 0) {
                std::size_t nl = text.find('\n', at);
                at = (nl == std::string::npos) ? text.size() : nl;
                continue;
            }
            if (text.compare(at, 2, "/*") == 0) {
                std::size_t close = text.find("*/", at + 2);
                at = (close == std::string::npos) ? text.size() : close + 2;
                continue;
            }
            if (ch == '(') paren++;
            else if (ch == ')') {
                paren--;
                if (paren == 0 && brace == 0) {
                    std::size_t probe = at + 1;
                    while (probe < text.size() && (text[probe] == ' ' || text[probe] == '\t'))
                        probe++;
                    if (probe >= text.size() || text[probe] == '\n') return at + 1;
                }
            }
            else if (ch == '{') brace++;
            else if (ch == '}') {
                brace--;
                if (brace == 0) return at + 1;
            }
            at++;
        }
        throw std::runtime_error("error: unterminated declaration");
    }

    static std::size_t skip_literal(const std::string& text, std::size_t at) {
        char quote = text[at++];
        while (at < text.size()) {
            if (text[at] == '\\') { at += 2; continue; }
            if (text[at] == quote) return at + 1;
            at++;
        }
        return at;
    }

    static bool is_annotation_or_comment(const std::string& line) {
        if (line.empty()) return false;  // Python: a blank line stops the walk
        // Python OWNED_BY_HEADER: ^(?:@|/\*\*|\*/|\*|//)  ('*' also covers '*/')
        return line[0] == '@' || line[0] == '*' || line.rfind("//", 0) == 0 ||
               line.rfind("/**", 0) == 0;
    }

    static std::string trim(const std::string& s) {
        std::size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        std::size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }
};

}  // namespace agora::tools::scenarios