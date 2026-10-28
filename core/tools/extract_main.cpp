// tools/extract_main.cpp — C++ replacement for the golden-stand Python extract_*.py scripts
// (the enum/marker family: migration-16-17, migration-17-18, migration-18-19,
// legacy-run-backfill, message-source, regeneration-tree-repair, v18-regeneration-repair,
// migration-19-20, streaming-json-document).
//
// Each profile reproduces its Python script byte for byte; the committed extracted/*.kt files
// are the parity reference (running a profile must leave `git status` clean).
//
// Usage: agora_extract <profile> <stand_dir>
//   stand_dir is core/tools/golden/<stand>; the repo root is derived like the Python HERE/ROOT.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << message << "\n";
    std::exit(1);
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("error: cannot read " + path.string());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, const std::string& content) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) fail("error: cannot create " + path.parent_path().string());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) fail("error: cannot write " + path.string());
    stream << content;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += separator;
        out += parts[i];
    }
    return out;
}

std::string comma_join(const std::vector<std::string>& parts) { return join(parts, ", "); }

// ---- enum extraction -----------------------------------------------------------------------
//
// Python (migration family):
//   re.compile(r"^enum class " + name + r" \{\n(?:.*\n)*?^\}\n", re.MULTILINE)
// The match runs from a line starting with `enum class <name> {` (the `{` must end the line)
// through the first following line that is exactly `}` (which must end with a newline); the
// caller then rstrips the trailing newline. A later occurrence is only tried when an earlier
// head has no terminating `}-line`, exactly like re.search.
//
// Python (participant family): r"^enum class Participant \{\n(?:.*\n)*?\}\n" — the terminator
// `\}\n` sits where the line walk left the cursor (a line start), so both spellings agree.
std::optional<std::string> extract_enum(const std::string& text, const std::string& name,
                                        bool requires_trailing_newline) {
    const std::string head = "enum class " + name + " {";
    std::size_t search = 0;
    while (true) {
        const std::size_t at = text.find(head, search);
        if (at == std::string::npos) return std::nullopt;
        search = at + 1;

        const bool line_start = at == 0 || text[at - 1] == '\n';
        const std::size_t after = at + head.size();
        const bool open_terminated = after < text.size() && text[after] == '\n';
        if (!line_start || !open_terminated) continue;

        std::size_t line_start_pos = after + 1;
        while (line_start_pos <= text.size()) {
            const std::size_t eol = text.find('\n', line_start_pos);
            if (eol == std::string::npos) {
                if (!requires_trailing_newline && text.compare(line_start_pos, std::string::npos,
                                                              "}") == 0) {
                    return text.substr(at, text.size() - at);  // `}` at EOF, newline optional
                }
                break;  // unterminated `}` line: try a later head occurrence
            }
            if (text.compare(line_start_pos, eol - line_start_pos, "}") == 0) {
                return text.substr(at, eol - at);  // through `}`, without the newline
            }
            line_start_pos = eol + 1;
        }
    }
}

std::string extract_enum_or_fail(const std::string& text, const std::string& name,
                                 const fs::path& source, bool requires_trailing_newline) {
    const std::optional<std::string> block = extract_enum(text, name, requires_trailing_newline);
    if (!block) {
        fail("error: enum class " + name + " not found in " + source.string());
    }
    return *block;
}

// ---- profiles ------------------------------------------------------------------------------

// migration-16-17 + legacy-run-backfill: four enums over two model files.
void profile_multi_enums(const fs::path& model, const fs::path& target) {
    struct Source {
        const char* file;
        std::vector<const char*> names;
    };
    const std::vector<Source> sources = {
        {"ChatMessage.kt", {"Participant", "MessageStatus"}},
        {"RuntimeIdentity.kt", {"RunStatus", "RunEndReason"}},
    };

    std::vector<std::string> declarations;
    std::vector<std::string> joined_names;
    for (const Source& source : sources) {
        const fs::path path = model / source.file;
        const std::string text = read_file(path);
        for (const char* name : source.names) {
            declarations.push_back(extract_enum_or_fail(text, name, path, true));
            joined_names.push_back(name);
        }
    }

    write_file(target, "package com.newoether.agora.model\n\n" +
                           join(declarations, "\n\n") + "\n");
    std::cout << "extracted " << comma_join(joined_names) << " into " << target << "\n";
}

// migration-17-18 + migration-18-19: Participant alone into MigrationEnums.kt.
void profile_single_enum(const fs::path& model, const fs::path& target, const std::string& name,
                         const std::string& source_file) {
    const fs::path path = model / source_file;
    const std::string declaration =
        extract_enum_or_fail(read_file(path), name, path, true);
    write_file(target, "package com.newoether.agora.model\n\n" + declaration + "\n");
    std::cout << "extracted " << name << " into " << target << "\n";
}

// message-source + regeneration-tree-repair + v18-regeneration-repair: Participant into
// Participant.kt (the Python scripts differ only in their docstring).
void profile_participant(const fs::path& model, const fs::path& target) {
    const fs::path path = model / "ChatMessage.kt";
    const std::string declaration =
        extract_enum_or_fail(read_file(path), "Participant", path, true);
    write_file(target, "package com.newoether.agora.model\n\n" + declaration + "\n");
    std::cout << "extracted Participant ("
              << std::count(declaration.begin(), declaration.end(), '\n') + 1 << " lines) to "
              << target << "\n";
}

// migration-19-20: the TRUNCATION_MARKER constant wrapped in a stand-local object.
void profile_marker(const fs::path& model, const fs::path& target) {
    const fs::path source = model / "MessagePersistenceGuard.kt";
    const std::string text = read_file(source);

    static constexpr const char* kPrefix = "internal const val TRUNCATION_MARKER = ";
    std::string line;
    std::size_t search = 0;
    while (search < text.size()) {
        const std::size_t eol = text.find('\n', search);
        const std::string candidate =
            text.substr(search, eol == std::string::npos ? std::string::npos : eol - search);
        std::size_t first = candidate.find_first_not_of(" \t");
        if (first != std::string::npos &&
            candidate.compare(first, std::string_view(kPrefix).size(), kPrefix) == 0) {
            line = candidate;
            break;
        }
        if (eol == std::string::npos) break;
        search = eol + 1;
    }
    if (line.empty()) fail("error: TRUNCATION_MARKER not found in " + source.string());

    // Python: match.group(0).strip() — the match spans the whole line, then trims.
    const std::size_t trimmed_start = line.find_first_not_of(" \t\r\n");
    const std::size_t trimmed_end = line.find_last_not_of(" \t\r\n");
    const std::string trimmed = line.substr(trimmed_start, trimmed_end - trimmed_start + 1);

    write_file(target, "package com.newoether.agora.model\n\n"
                           "object MessagePersistenceGuard {\n    " + trimmed + "\n}\n");
    std::cout << "extracted TRUNCATION_MARKER into " << target << "\n";
}

// streaming-json-document: the ToolKind enum with the package line lifted from the source.
void profile_tool_kind(const fs::path& model_root, const fs::path& target) {
    const fs::path source = model_root / "ui/chat/message/ToolPresentation.kt";
    const std::string text = read_file(source);

    static constexpr const char* kHead = "internal enum class ToolKind {";
    std::optional<std::string> block;
    std::size_t search = 0;
    while (true) {
        const std::size_t at = text.find(kHead, search);
        if (at == std::string::npos) break;
        search = at + 1;
        const bool line_start = at == 0 || text[at - 1] == '\n';
        const std::size_t after = at + std::string(kHead).size();
        if (!line_start || after >= text.size() || text[after] != '\n') continue;

        // (?ms) — `.` crosses newlines; the match ends after the first line that is exactly
        // `}` (`$` may sit before the newline or at end of input).
        std::size_t line_at = after + 1;
        while (line_at <= text.size()) {
            const std::size_t eol = text.find('\n', line_at);
            const std::size_t line_end = eol == std::string::npos ? text.size() : eol;
            if (text.compare(line_at, line_end - line_at, "}") == 0) {
                block = text.substr(at, line_end - at);
                break;
            }
            if (eol == std::string::npos) break;
            line_at = eol + 1;
        }
        break;
    }
    if (!block) {
        fail("extract_tool_kind: ToolKind enum block not found in ToolPresentation.kt");
    }

    std::string package;
    search = 0;
    while (search < text.size()) {
        const std::size_t eol = text.find('\n', search);
        const std::string line =
            text.substr(search, eol == std::string::npos ? std::string::npos : eol - search);
        if (line.rfind("package ", 0) == 0) {
            package = line;
            break;
        }
        if (eol == std::string::npos) break;
        search = eol + 1;
    }
    if (package.empty()) package = "package com.newoether.agora.ui.chat.message";

    write_file(target, package + "\n\n" + *block + "\n");
    std::cout << "extracted ToolKind (" << std::count(block->begin(), block->end(), '\n') + 1
              << " lines) to " << target << "\n";
}

void usage() {
    std::cerr << "usage: agora_extract <profile> <stand_dir>\n"
              << "profiles:\n"
              << "  enums-16-17     migration-16-17 extract_enums.py\n"
              << "  enums-17-18     migration-17-18 extract_enums.py\n"
              << "  enums-18-19     migration-18-19 extract_enums.py\n"
              << "  enums-legacy    legacy-run-backfill extract_enums.py\n"
              << "  participant     message-source/v18/regeneration-tree-repair "
                 "extract_participant.py\n"
              << "  marker          migration-19-20 extract_marker.py\n"
              << "  tool-kind       streaming-json-document extract_tool_kind.py\n";
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) usage();
    const std::string profile = argv[1];
    const fs::path stand = fs::path(argv[2]).lexically_normal();
    // stand_dir = <root>/core/tools/golden/<stand> → root is four levels up (Python's HERE/ROOT).
    const fs::path root = (stand / ".." / ".." / ".." / "..").lexically_normal();
    const fs::path model = root / "app/src/main/java/com/newoether/agora/model";
    const fs::path extracted = stand / "extracted";

    if (profile == "enums-16-17") {
        profile_multi_enums(model, extracted / "MigrationEnums.kt");
    } else if (profile == "enums-17-18") {
        profile_single_enum(model, extracted / "MigrationEnums.kt", "Participant",
                            "ChatMessage.kt");
    } else if (profile == "enums-18-19") {
        profile_single_enum(model, extracted / "MigrationEnums.kt", "Participant",
                            "ChatMessage.kt");
    } else if (profile == "enums-legacy") {
        profile_multi_enums(model, extracted / "LegacyEnums.kt");
    } else if (profile == "participant") {
        profile_participant(model, extracted / "Participant.kt");
    } else if (profile == "marker") {
        profile_marker(model, extracted / "MessagePersistenceGuardMarker.kt");
    } else if (profile == "tool-kind") {
        profile_tool_kind(root / "app/src/main/java/com/newoether/agora",
                          extracted / "ToolKind.kt");
    } else {
        usage();
    }
    return 0;
}