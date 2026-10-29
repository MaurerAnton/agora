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
#include <array>
#include <cctype>
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

// ---- declaration-walker family -------------------------------------------------------------
//
// task-schedule/extract_task_entity.py, backup-media-policy/extract_message_segment.py and
// run-recovery/extract_model_types.py share one algorithm: regex-ish declaration match at a line
// start, walk back over the annotation/KDoc block, then scan forward to the declaration end.

std::size_t skip_literal(const std::string& text, std::size_t at) {
    const char quote = text[at];
    at += 1;
    while (at < text.size()) {
        if (text[at] == '\\') {
            at += 2;
            continue;
        }
        if (text[at] == quote) return at + 1;
        at += 1;
    }
    return at;
}

// style_newline: probe after ')' skips only " \t", stops at newline/EOF (task-schedule and
// backup-media-policy). style_brace: probe skips " \t\r\n" and only stops unless a `{` follows
// (run-recovery — keeps scanning through a body brace that starts on the next line).
[[noreturn]] void unterminated_declaration(const std::string& message) { fail(message); }

std::size_t declaration_end(const std::string& text, std::size_t start,
                            bool style_brace_follows, const std::string& unterminated) {
    int paren = 0;
    int brace = 0;
    std::size_t at = start;
    while (at < text.size()) {
        const char ch = text[at];
        if (ch == '"') {
            at = skip_literal(text, at);
            continue;
        }
        if (text.compare(at, 2, "//") == 0) {
            const std::size_t newline = text.find('\n', at);
            at = newline == std::string::npos ? text.size() : newline;
            continue;
        }
        if (text.compare(at, 2, "/*") == 0) {
            const std::size_t close = text.find("*/", at + 2);
            at = close == std::string::npos ? text.size() : close + 2;
            continue;
        }
        if (ch == '(') {
            paren += 1;
        } else if (ch == ')') {
            paren -= 1;
            if (paren == 0 && brace == 0) {
                std::size_t probe = at + 1;
                if (style_brace_follows) {
                    while (probe < text.size() &&
                           (text[probe] == ' ' || text[probe] == '\t' || text[probe] == '\r' ||
                            text[probe] == '\n')) {
                        probe += 1;
                    }
                    if (probe >= text.size() || text[probe] != '{') return at + 1;
                    // A class body follows: keep scanning so its closing brace is included.
                } else {
                    while (probe < text.size() && (text[probe] == ' ' || text[probe] == '\t')) {
                        probe += 1;
                    }
                    if (probe >= text.size() || text[probe] == '\n') return at + 1;
                }
            }
        } else if (ch == '{') {
            brace += 1;
        } else if (ch == '}') {
            brace -= 1;
            if (brace == 0) return at + 1;
        }
        at += 1;
    }
    unterminated_declaration(unterminated);
}

std::size_t trim_right_end(std::size_t to, const std::string& text) {
    while (to > 0 && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r' ||
                      text[to - 1] == '\n' || text[to - 1] == '\f' || text[to - 1] == '\v')) {
        to -= 1;
    }
    return to;
}

// Python OWNED_BY_HEADER on candidate.strip(): ^(?:@|/\*\*|\*/|\*|//) — plus the (redundant)
// lstrip().startswith("*") check.
bool owned_annotation_line(std::string_view candidate) {
    std::size_t start = 0;
    while (start < candidate.size() &&
           (candidate[start] == ' ' || candidate[start] == '\t' || candidate[start] == '\r' ||
            candidate[start] == '\n' || candidate[start] == '\f' || candidate[start] == '\v')) {
        start += 1;
    }
    if (start >= candidate.size()) return false;
    const std::string_view stripped = candidate.substr(start);
    return stripped.rfind("@", 0) == 0 || stripped.rfind("/**", 0) == 0 ||
           stripped.rfind("//", 0) == 0 || stripped.rfind("*", 0) == 0;
}

struct DeclMatch {
    std::size_t start = 0;
    std::size_t end = 0;
};

// re.search(r"^(<prefix alternatives>) NAME\b", text, re.MULTILINE): the prefix must sit at a
// line start, NAME must follow, and a word boundary must close it.
std::optional<DeclMatch> find_declaration(const std::string& text, const std::string& name,
                                           const std::vector<std::string>& prefixes) {
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        for (const std::string& prefix : prefixes) {
            if (text.compare(line_start, prefix.size(), prefix) == 0) {
                const std::size_t name_at = line_start + prefix.size();
                if (name_at > text.size()) continue;
                if (text.compare(name_at, name.size(), name) == 0) {
                    const std::size_t after = name_at + name.size();
                    const bool boundary =
                        after >= text.size() ||
                        !(std::isalnum(static_cast<unsigned char>(text[after])) ||
                          text[after] == '_');
                    if (boundary) return DeclMatch{line_start, after};
                }
            }
        }
        const std::size_t newline = text.find('\n', line_start);
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }
    return std::nullopt;
}

std::string extract_declaration(const std::string& text, const std::string& name,
                                const std::vector<std::string>& prefixes,
                                bool require_annotation, bool style_brace_follows,
                                const std::string& not_found, const std::string& no_annotation,
                                const std::string& unterminated) {
    const std::optional<DeclMatch> match = find_declaration(text, name, prefixes);
    if (!match) fail(not_found);

    // lines = text[:match.start].split("\n"); a trailing "" (the newline that terminates the
    // preceding line) is dropped, so lines[-1] is the line directly above the declaration.
    std::vector<std::string_view> lines;
    {
        std::size_t pos = 0;
        while (pos < match->start) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t stop = newline == std::string::npos ? match->start : newline;
            lines.push_back(std::string_view(text).substr(pos, stop - pos));
            if (newline == std::string::npos) break;
            pos = newline + 1;
        }
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
    }

    // Walk back over the annotation/KDoc block that belongs to THIS declaration.
    std::size_t owned = 0;
    while (!lines.empty() && owned_annotation_line(lines.back())) {
        lines.pop_back();
        owned += 1;
    }
    if (require_annotation && owned == 0) fail(no_annotation);

    // Walk back `owned` newlines in the original text; re-joining the split would lose a
    // character and silently slice the wrong range. Python: text.rfind("\n", 0, start - 1) + 1
    // (exclusive end — the newline at start-1 itself is outside the range, so each step lands
    // on the previous line's start).
    std::size_t start = match->start;
    for (std::size_t i = 0; i < owned; ++i) {
        if (start < 2) {
            start = 0;
            break;
        }
        const std::size_t newline = text.rfind('\n', start - 2);
        start = newline == std::string::npos ? 0 : newline + 1;
    }

    return text.substr(start, declaration_end(text, match->end, style_brace_follows,
                                              unterminated) - start);
}

// ---- batch-2 profiles -----------------------------------------------------------------------
// Each writes HEADER + extracted declarations; the GENERATED provenance line names the C++
// profile instead of the deleted Python script (the only intended byte difference).

// task-schedule/extract_task_entity.py
void profile_task_entity(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "package com.newoether.agora.data.local\n"
        "\n"
        "// GENERATED by agora_extract task-entity from\n"
        "// app/src/main/java/com/newoether/agora/data/local/ChatEntities.kt -- do not edit. The "
        "declaration\n"
        "// below is copied verbatim; only the Room markers are pointed at the stand-local "
        "stubs.\n"
        "import androidx.room.Entity\n"
        "import androidx.room.PrimaryKey\n"
        "\n";
    const std::string text = read_file(source);
    const std::string block = extract_declaration(
        text, "TaskEntity",
        {"class ", "data class ", "internal data class ", "public data class "},
        /*require_annotation=*/true, /*style_brace_follows=*/false,
        "error: declaration `TaskEntity` not found in ChatEntities.kt",
        "error: `TaskEntity` has no annotation block; the extractor would drift",
        "error: unterminated declaration in ChatEntities.kt");

    const std::size_t trimmed = trim_right_end(block.size(), block);
    write_file(out, std::string(kHeader) + block.substr(0, trimmed) + "\n");
    std::cout << "extracted TaskEntity into " << out.string() << "\n";
}

std::string join_blocks(const std::string& header, const std::vector<std::string>& blocks) {
    std::string joined = header;
    for (const std::string& block : blocks) joined += block + "\n\n";
    const std::size_t trimmed = trim_right_end(joined.size(), joined);
    return joined.substr(0, trimmed) + "\n";
}

// backup-media-policy/extract_message_segment.py (also invoked by message-persistence-guard)
void profile_message_segment(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "package com.newoether.agora.model\n"
        "\n"
        "// GENERATED by agora_extract message-segment from "
        "app/src/main/java/com/newoether/agora/model/\n"
        "// ChatMessage.kt -- do not edit. The declarations below are copied verbatim.\n"
        "import kotlinx.serialization.Serializable\n"
        "import kotlinx.serialization.Transient\n"
        "import kotlinx.serialization.json.JsonObject\n"
        "\n";
    static constexpr std::array<const char*, 3> kWanted = {"ToolImageAttachment",
                                                           "StreamingTextDelta", "MessageSegment"};
    const std::string text = read_file(source);
    std::vector<std::string> blocks;
    for (const char* name : kWanted) {
        std::string block = extract_declaration(
            text, name, {"class ", "data class ", "internal data class ", "public data class "},
            /*require_annotation=*/true, /*style_brace_follows=*/false,
            std::string("error: declaration `") + name + "` not found in ChatMessage.kt",
            std::string("error: `") + name +
                "` has no annotation block; the extractor would drift",
            std::string("error: unterminated declaration `") + name + "` in ChatMessage.kt");
        // The Compose marker is substituted by a stand-local stub; nothing else may change.
        const std::string from = "@Immutable\n";
        const std::string to = "@androidx.compose.runtime.Immutable\n";
        std::size_t at = 0;
        while ((at = block.find(from, at)) != std::string::npos) {
            block.replace(at, from.size(), to);
            at += to.size();
        }
        blocks.push_back(std::move(block));
    }
    write_file(out, join_blocks(kHeader, blocks));
    std::cout << "extracted ToolImageAttachment, StreamingTextDelta, MessageSegment into "
              << out.string() << "\n";
}

// run-recovery/extract_model_types.py
void profile_run_recovery_models(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "package com.newoether.agora.model\n"
        "\n"
        "// GENERATED by agora_extract run-recovery-models from "
        "app/src/main/java/com/newoether/agora/model/\n"
        "// ChatMessage.kt -- do not edit. The declarations below are copied verbatim.\n"
        "import androidx.compose.runtime.Immutable\n"
        "import kotlinx.serialization.Serializable\n"
        "import kotlinx.serialization.Transient\n"
        "import kotlinx.serialization.json.JsonObject\n"
        "\n";
    static constexpr std::array<const char*, 6> kWanted = {
        "ToolImageAttachment", "StreamingTextDelta", "MessageSegment",
        "ToolExecutionStates", "Participant",        "MessageStatus"};
    const std::string text = read_file(source);
    std::vector<std::string> blocks;
    for (const char* name : kWanted) {
        blocks.push_back(extract_declaration(
            text, name,
            {"class ", "data class ", "internal data class ", "public data class ", "object ",
             "enum class "},
            /*require_annotation=*/false, /*style_brace_follows=*/true,
            std::string("error: declaration `") + name + "` not found in ChatMessage.kt", "",
            std::string("error: unterminated declaration `") + name + "` in ChatMessage.kt"));
    }
    write_file(out, join_blocks(kHeader, blocks));
    std::cout << "extracted ToolImageAttachment, StreamingTextDelta, MessageSegment, "
                 "ToolExecutionStates, Participant, MessageStatus into "
              << out.string() << "\n";
}

// ---- widen family ---------------------------------------------------------------------------
// generation-policies/extract_policies.py, generation-streaming-segments/extract_policy.py and
// malformed-tool-call/extract_policy.py copy whole app files verbatim, widening only a leading
// `internal ` to the default visibility so a driver in another module can call the API.
//
// Usage: agora_extract widen <source_dir> <out_dir> <file> [<file>...]
[[noreturn]] void usage();

int profile_widen(int argc, char** argv) {
    if (argc < 5) usage();
    const fs::path source_dir = argv[2];
    const fs::path out_dir = argv[3];
    std::error_code error;
    fs::create_directories(out_dir, error);
    if (error) fail("error: cannot create " + out_dir.string());

    int widened = 0;
    for (int i = 4; i < argc; ++i) {
        const std::string name = argv[i];
        const fs::path source = source_dir / name;
        if (!fs::exists(source)) fail("error: missing source " + source.string());
        std::string text = read_file(source);

        // Python widen(): split("\n"), lstrip each line, drop a leading "internal ", re-join.
        std::string joined;
        std::size_t pos = 0;
        while (true) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t stop = newline == std::string::npos ? text.size() : newline;
            std::string line = text.substr(pos, stop - pos);
            std::size_t lead = 0;
            while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
            if (line.compare(lead, std::string("internal ").size(), "internal ") == 0) {
                line = line.substr(0, lead) + line.substr(lead + std::string("internal ").size());
            }
            joined += line;
            if (newline == std::string::npos) break;
            joined += "\n";
            pos = newline + 1;
        }

        write_file(out_dir / name, joined);
        widened += 1;
    }
    std::cout << "Widened " << widened << " policy files\n";
    return 0;
}

// ---- small region profiles -------------------------------------------------------------------

// run-branch-selection/extract_run_entity.py — the whole RunEntity declaration to end of file.
void profile_run_entity(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "package com.newoether.agora.data.local\n"
        "\n"
        "// GENERATED by agora_extract run-entity from\n"
        "// app/src/main/java/com/newoether/agora/data/local/RunEntity.kt -- do not edit. The "
        "declaration\n"
        "// below is copied verbatim; only the Room marker is pointed at the stand-local stub.\n"
        "import androidx.room.PrimaryKey\n"
        "import com.newoether.agora.model.RunEndReason\n"
        "import com.newoether.agora.model.RunStatus\n"
        "\n";
    static constexpr std::array<const char*, 3> kInvariants = {
        "require((activeSlot == 1) == !status.isTerminal)",
        "require(status.isTerminal == (endedAt != null && endReason != null))",
        "require(status != RunStatus.STOPPING || stopRequestedAt != null)"};

    const std::string text = read_file(source);
    const std::size_t at = text.find("data class RunEntity(");
    if (at == std::string::npos) {
        fail("error: `data class RunEntity(` not found in RunEntity.kt");
    }
    const std::string block = text.substr(at);
    const std::size_t trimmed = trim_right_end(block.size(), block);
    if (trimmed == 0 || block[trimmed - 1] != '}') {
        fail("error: unexpected RunEntity tail; the extractor would drift");
    }
    for (const char* invariant : kInvariants) {
        if (block.find(invariant) == std::string::npos) {
            fail(std::string("error: RunEntity invariant changed: ") + invariant);
        }
    }
    write_file(out, std::string(kHeader) + block.substr(0, trimmed) + "\n");
    std::cout << "extracted RunEntity into " << out.string() << "\n";
}

// conch-sse/extract_shell_prefix.py — the output-cap constant and the shellUtf8Prefix helper.
void profile_shell_prefix(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract shell-prefix from util/ShellClient.kt -- do not edit.\n"
        "//\n"
        "// Copied verbatim; only the surrounding file (which cannot compile host-side) is "
        "dropped.\n"
        "package com.newoether.agora.util\n"
        "\n";
    static constexpr std::array<const char*, 5> kRequired = {
        "maxBytes <= 0", "Character.codePointAt", "0x7ff", "0xffff", "charCount"};

    const std::string text = read_file(source);
    std::vector<std::string_view> lines;
    {
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t stop = newline == std::string::npos ? text.size() : newline;
            lines.push_back(std::string_view(text).substr(pos, stop - pos));
            if (newline == std::string::npos) break;
            pos = newline + 1;
        }
    }

    std::optional<std::size_t> const_index;
    std::optional<std::size_t> fun_index;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!const_index && lines[i].find("const val SHELL_COMMAND_OUTPUT_MAX_BYTES ") !=
                                std::string_view::npos) {
            const_index = i;
        }
        if (!fun_index &&
            lines[i].rfind("internal fun String.shellUtf8Prefix(", 0) == 0) {
            fun_index = i;
        }
    }
    if (!const_index) fail("error: SHELL_COMMAND_OUTPUT_MAX_BYTES not found");
    if (!fun_index) fail("error: internal fun String.shellUtf8Prefix( not found");

    std::optional<std::size_t> fun_end;
    for (std::size_t i = *fun_index + 1; i < lines.size(); ++i) {
        if (lines[i] == "}") {
            fun_end = i;
            break;
        }
    }
    if (!fun_end) fail("error: end of shellUtf8Prefix not found");

    std::string block;
    for (std::size_t i = *fun_index; i <= *fun_end; ++i) {
        if (i > *fun_index) block += "\n";
        block += std::string(lines[i]);
    }
    for (const char* required : kRequired) {
        if (block.find(required) == std::string::npos) {
            fail(std::string("error: extracted function lost '") + required + "'");
        }
    }
    write_file(out, std::string(kHeader) + std::string(lines[*const_index]) + "\n\n" + block +
                        "\n");
}

// native-backup-archive/extract_graph_interface.py — the three-method interface.
void profile_graph_interface(const fs::path& source, const fs::path& out) {
    const std::string text = read_file(source);
    const std::size_t start = text.find("internal interface NativeGraphEntrySource");
    if (start == std::string::npos) fail("error: NativeGraphEntrySource not found");
    const std::size_t close = text.find('}', start);
    if (close == std::string::npos) fail("error: unterminated NativeGraphEntrySource");
    write_file(out, "package com.newoether.agora.data\n\nimport java.io.InputStream\n\n" +
                        text.substr(start, close + 1 - start) + "\n");
}

// Shared: extract a top-level declaration from the line starting with `prefix` through the first
// following line that is exactly "}" (Python splitlines(keepends=True) + rstrip("\n") == "}").
void extract_function(const fs::path& source, const fs::path& out, const std::string& prefix,
                      const std::string& header, const std::string& not_found,
                      const std::string& unterminated) {
    const std::string text = read_file(source);
    std::size_t pos = 0;
    std::optional<std::size_t> start;
    std::optional<std::size_t> end;
    while (pos <= text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t stop = newline == std::string::npos ? text.size() : newline;
        const std::string_view line(text.data() + pos, stop - pos);
        if (!start) {
            if (line.rfind(prefix, 0) == 0) start = pos;
        } else if (!end) {
            if (line == "}") {
                end = newline == std::string::npos ? text.size() : newline + 1;
                break;
            }
        }
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    if (!start) fail(not_found);
    if (!end) fail(unterminated);
    write_file(out, header + text.substr(*start, *end - *start));
}

// native-conversation-graph-source/extract_read_to_end.py — one extension function.
void profile_read_to_end(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// Generated by agora_extract read-to-end from "
        "app/src/main/java/com/newoether/agora/data/NativeBackupArchive.kt.\n"
        "// The function body below is copied byte-for-byte; only the package and import are "
        "added here.\n"
        "package com.newoether.agora.data\n"
        "\n"
        "import java.io.InputStream\n"
        "\n";
    extract_function(source, out, "internal fun InputStream.readToEnd()", kHeader,
                     "readToEnd", "readToEnd");
}

// data-exporter/extract_sanitize_imported_loop.py — one top-level function.
void profile_sanitize_loop(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// Generated by agora_extract sanitize-loop from "
        "app/src/main/java/com/newoether/agora/data/DataImporter.kt.\n"
        "// The function body below is copied byte-for-byte from DataImporter.kt; only the "
        "package and the\n"
        "// two imports it needs are added here.\n"
        "package com.newoether.agora.data\n"
        "\n"
        "import com.newoether.agora.automation.LoopPolicy\n"
        "import com.newoether.agora.data.local.LoopEntity\n"
        "\n";
    extract_function(source, out, "internal fun sanitizeImportedLoop(", kHeader,
                     "sanitizeImportedLoop", "sanitizeImportedLoop");
}

// openai-responses-request/extract_responses_models.py — four @Serializable data classes.
void profile_responses_models(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract responses-models from api/LlmProvider.kt -- do not edit.\n"
        "//\n"
        "// The OpenAI Responses request models are copied verbatim; only the file they live in "
        "(the\n"
        "// provider abstraction) is left behind, because that file pulls in the "
        "message-preparation and\n"
        "// cost-model clusters.\n"
        "package com.newoether.agora.api\n"
        "\n"
        "import kotlinx.serialization.SerialName\n"
        "import kotlinx.serialization.Serializable\n"
        "import kotlinx.serialization.json.JsonElement\n"
        "import kotlinx.serialization.json.JsonObject\n"
        "\n";
    static constexpr std::array<const char*, 4> kClasses = {
        "OpenAiResponsesRequest", "OpenAiResponseInputItem", "OpenAiResponseInputContent",
        "OpenAiResponseTool"};

    const std::string text = read_file(source);
    std::vector<std::string_view> lines;
    {
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t stop = newline == std::string::npos ? text.size() : newline;
            lines.push_back(std::string_view(text).substr(pos, stop - pos));
            if (newline == std::string::npos) break;
            pos = newline + 1;
        }
    }

    std::vector<std::string> blocks;
    for (const char* name : kClasses) {
        std::optional<std::size_t> start;
        const std::string declaration = std::string("data class ") + name + "(";
        for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
            if (lines[i].rfind("@Serializable", 0) == 0 &&
                lines[i + 1].rfind(declaration, 0) == 0) {
                start = i;
                break;
            }
        }
        if (!start) fail(std::string("error: ") + name + " not found");
        std::optional<std::size_t> end;
        for (std::size_t i = *start + 1; i < lines.size(); ++i) {
            if (lines[i] == "}" || lines[i] == ")") {
                end = i;
                break;
            }
        }
        if (!end) fail(std::string("error: end of ") + name + " not found");
        std::string block;
        for (std::size_t i = *start; i <= *end; ++i) {
            if (i > *start) block += "\n";
            block += std::string(lines[i]);
        }
        blocks.push_back(std::move(block));
    }

    std::string body;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (i > 0) body += "\n\n";
        body += blocks[i];
    }
    for (const char* name : kClasses) {
        if (body.find(name) == std::string::npos) {
            fail(std::string("error: ") + name + " missing from the extracted region");
        }
    }
    write_file(out, std::string(kHeader) + body + "\n");
}

// data-importer/extract_export_category.py — the nested enum wrapped in a stand-local class.
void profile_export_category(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract export-category from DataExporter.kt -- do not edit.\n"
        "//\n"
        "// DataImporter's decision map is keyed by DataExporter.ExportCategory. Only the nested "
        "enum is\n"
        "// lifted (byte-for-byte) so the import composition root does not pull in the export "
        "root.\n"
        "package com.newoether.agora.data\n"
        "\n"
        "class DataExporter {\n";
    static constexpr const char* kFooter = "}\n";
    static constexpr const char* kStart = "enum class ExportCategory(val manifestKey: String) {";

    const std::string text = read_file(source);
    std::size_t pos = 0;
    std::size_t index = 0;
    std::optional<std::size_t> start;
    std::optional<std::size_t> start_index;
    std::optional<std::size_t> end;
    int depth = 0;

    while (pos <= text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t stop = newline == std::string::npos ? text.size() : newline;
        const std::string_view line(text.data() + pos, stop - pos);

        if (!start) {
            std::size_t lead = 0;
            while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
            std::size_t tail = line.size();
            while (tail > lead &&
                   (line[tail - 1] == ' ' || line[tail - 1] == '\t' || line[tail - 1] == '\r')) {
                tail -= 1;
            }
            if (line.substr(lead, tail - lead) == kStart) {
                start = pos;
                start_index = index;
                depth += static_cast<int>(std::count(line.begin(), line.end(), '{')) -
                         static_cast<int>(std::count(line.begin(), line.end(), '}'));
            }
        } else if (!end) {
            depth += static_cast<int>(std::count(line.begin(), line.end(), '{')) -
                     static_cast<int>(std::count(line.begin(), line.end(), '}'));
            if (depth == 0 && index > *start_index) {
                end = newline == std::string::npos ? text.size() : newline + 1;
            }
        }
        if (end) break;
        if (newline == std::string::npos) break;
        pos = newline + 1;
        index += 1;
    }

    if (!start) fail("error: ExportCategory declaration not found");
    if (!end) fail("error: unterminated ExportCategory");

    write_file(out, std::string(kHeader) + text.substr(*start, *end - *start) + kFooter);
}

// model-thinking-resolver/extract_identity_policy.py — two slices of the identity object.
void profile_identity_policy(const fs::path& source, const fs::path& out) {
    const std::string text = read_file(source);
    const std::size_t header_at = text.find("internal object CustomProviderIdentityPolicy");
    if (header_at == std::string::npos) {
        fail("error: CustomProviderIdentityPolicy not found");
    }
    const std::string body = text.substr(header_at);

    const auto block = [&](const char* first, const char* end_marker,
                           const std::string& label) -> std::string {
        const std::size_t start = body.find(first);
        if (start == std::string::npos) fail("error: " + label + " not found");
        const std::size_t end = body.find(end_marker, start);
        if (end == std::string::npos) fail("error: end of " + label + " not found");
        return body.substr(start, end - start);
    };

    std::string pattern = block("    private const val STABLE_ID_PATTERN =",
                                "    private val stableIdInTextPattern", "STABLE_ID_PATTERN");
    std::string is_stable = block("    fun isStableId(value: String): Boolean",
                                  "    fun replaceStableIdsForDisplay", "isStableId");
    // The function uses an expression body, so the outer object close below is the only brace.
    is_stable = is_stable.substr(0, trim_right_end(is_stable.size(), is_stable));

    write_file(out,
               "package com.newoether.agora.data\n"
               "\n"
               "// GENERATED by agora_extract identity-policy -- copied verbatim from\n"
               "// app/src/main/java/com/newoether/agora/data/CustomProviderIdentity.kt.\n"
               "internal object CustomProviderIdentityPolicy {\n" +
                   pattern + is_stable + "\n}\n");
    std::cout << "extracted CustomProviderIdentityPolicy.isStableId into " << out.string()
              << "\n";
}

// ---- batch-5 region profiles ----------------------------------------------------------------

std::string trim_copy(std::string_view value) {
    const auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t start = 0;
    while (start < value.size() && ws(value[start])) start += 1;
    std::size_t end = value.size();
    while (end > start && ws(value[end - 1])) end -= 1;
    return std::string(value.substr(start, end - start));
}

std::string trim_right_copy(std::string_view value) {
    std::size_t end = value.size();
    while (end > 0 && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                       value[end - 1] == '\n' || value[end - 1] == '\r')) {
        end -= 1;
    }
    return std::string(value.substr(0, end));
}

std::vector<std::string_view> split_lines(const std::string& text) {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t stop = newline == std::string::npos ? text.size() : newline;
        lines.push_back(std::string_view(text).substr(pos, stop - pos));
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    return lines;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string join_lines(const std::vector<std::string_view>& lines, std::size_t start,
                       std::size_t end_inclusive) {
    std::string out;
    for (std::size_t i = start; i <= end_inclusive && i < lines.size(); ++i) {
        if (i > start) out += "\n";
        out += std::string(lines[i]);
    }
    return out;
}

// embedding/extract_embedding_models.py — EmbeddingRequest through BatchEmbeddingRequest,
// widening `private data class` to `internal data class`.
void profile_embedding_models(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract embedding-models from EmbeddingClient.kt -- do not edit.\n"
        "//\n"
        "// The request models are copied verbatim from the client file, which cannot be "
        "compiled\n"
        "// standalone (coroutines + HttpClient); the embedding golden stand pins them here. "
        "The only\n"
        "// transformation is widening `private data class` to `internal data class`.\n"
        "package com.newoether.agora.api\n"
        "\n"
        "import kotlinx.serialization.Serializable\n"
        "\n";
    static constexpr const char* kStartClass = "private data class EmbeddingRequest(";
    static constexpr const char* kEndClass = "private data class BatchEmbeddingRequest(";
    static constexpr std::array<const char*, 2> kExpected = {"EmbeddingRequest",
                                                             "BatchEmbeddingRequest"};

    const std::vector<std::string_view> lines = split_lines(read_file(source));
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(kStartClass) != std::string_view::npos) {
            if (i == 0 || trim_copy(lines[i - 1]) != "@Serializable") {
                fail(std::string("error: no @Serializable directly above ") + kStartClass);
            }
            start = i - 1;
            break;
        }
    }
    if (!start) fail(std::string("error: ") + kStartClass + " not found");

    std::optional<std::size_t> end;
    bool seen_batch = false;
    for (std::size_t i = *start; i < lines.size(); ++i) {
        if (lines[i].find(kEndClass) != std::string_view::npos) seen_batch = true;
        if (seen_batch && trim_copy(lines[i]) == ")") {
            end = i;
            break;
        }
    }
    if (!end) fail(std::string("error: closing paren of ") + kEndClass + " not found");

    std::string region;
    for (std::size_t i = *start; i <= *end; ++i) {
        std::string line(lines[i]);
        if (starts_with(line, "private data class ")) {
            line = "internal data class " + line.substr(std::string("private data class ").size());
        }
        if (i > *start) region += "\n";
        region += line;
    }
    for (const char* cls : kExpected) {
        if (region.find(std::string("data class ") + cls + "(") == std::string::npos) {
            fail(std::string("error: ") + cls + " is not inside the extracted region");
        }
    }
    write_file(out, std::string(kHeader) + region + "\n");
}

// shell-tools/extract_shell_models.py — ShellFileReadResult + shellFileLineCount.
void profile_shell_models(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract shell-models from ShellClient.kt -- do not edit.\n"
        "//\n"
        "// ShellFileReadResult and shellFileLineCount are copied verbatim from the "
        "transport-heavy\n"
        "// ShellClient.kt so the shell-tools golden stand can compile the real "
        "BoundedFileReadJson.\n"
        "package com.newoether.agora.util\n"
        "\n";
    static constexpr const char* kClass = "data class ShellFileReadResult(";
    static constexpr const char* kFunction = "internal fun shellFileLineCount(content: String): Int =";

    const std::vector<std::string_view> lines = split_lines(read_file(source));

    std::optional<std::size_t> class_start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], kClass)) {
            class_start = i;
            break;
        }
    }
    if (!class_start) fail(std::string("error: ") + kClass + " not found");
    std::optional<std::size_t> class_end;
    for (std::size_t i = *class_start; i < lines.size(); ++i) {
        if (trim_copy(lines[i]) == ")") {
            class_end = i;
            break;
        }
    }
    if (!class_end) fail(std::string("error: closing paren of ") + kClass + " not found");

    std::optional<std::size_t> function_at;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], kFunction)) {
            function_at = i;
            break;
        }
    }
    if (!function_at) fail(std::string("error: ") + kFunction + " not found");

    std::string function_block;
    {
        std::string first(lines[*function_at]);
        // Expression body on one line (`= if ... else ...` spans two lines here).
        if (trim_right_copy(first).back() == '=' && *function_at + 1 < lines.size()) {
            function_block = first + "\n" + std::string(lines[*function_at + 1]);
        } else {
            function_block = first;
        }
    }

    const std::string text =
        join_lines(lines, *class_start, *class_end) + "\n\n" + function_block;
    for (const char* name : {"ShellFileReadResult", "shellFileLineCount"}) {
        if (text.find(name) == std::string::npos) {
            fail(std::string("error: ") + name + " missing from the extracted region");
        }
    }
    write_file(out, std::string(kHeader) + text + "\n");
}

// model-aliases/extract_aliases.py — the alias region with `private` widened to `internal`.
void profile_aliases(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract aliases from CustomProviderIdentity.kt -- do not edit.\n"
        "//\n"
        "// The alias inference block (patterns, display-name tables, inferModelAlias and its "
        "Gemini /\n"
        "// Claude / Nova branches) is copied verbatim; only `private` is widened to "
        "`internal`.\n"
        "package com.newoether.agora.data\n"
        "\n"
        "import java.time.LocalDate\n"
        "import java.time.format.DateTimeFormatter\n"
        "import java.util.Locale\n"
        "\n";
    static constexpr const char* kStart = "private val aliasSeparators";
    static constexpr const char* kEndMarker = "private fun isValidSnapshotDate";
    static constexpr std::array<const char*, 7> kExpected = {
        "inferModelAlias", "formatAliasToken", "humanizeModelAlias", "aliasSeparators",
        "claudeFamilyFirstAliasPattern", "exactAliasTokenDisplayNames", "isValidSnapshotDate"};

    const std::vector<std::string_view> lines = split_lines(read_file(source));
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], kStart)) {
            start = i;
            break;
        }
    }
    if (!start) fail(std::string("error: ") + kStart + " not found");

    std::optional<std::size_t> marker_index;
    for (std::size_t i = *start; i < lines.size(); ++i) {
        if (starts_with(lines[i], kEndMarker)) {
            marker_index = i;
            break;
        }
    }
    if (!marker_index) fail(std::string("error: ") + kEndMarker + " not found");

    // The region ends at the next top-level declaration.
    std::size_t end = lines.size() - 1;
    for (std::size_t i = *marker_index + 1; i < lines.size(); ++i) {
        if (starts_with(lines[i], "fun ") || starts_with(lines[i], "private fun ") ||
            starts_with(lines[i], "internal fun ") || starts_with(lines[i], "private val ") ||
            starts_with(lines[i], "internal val ") || starts_with(lines[i], "private const val ") ||
            starts_with(lines[i], "@")) {
            end = i - 1;
            break;
        }
    }

    std::string text;
    for (std::size_t i = *start; i <= end; ++i) {
        std::string line(lines[i]);
        if (starts_with(line, "private val ")) {
            line = "internal val " + line.substr(std::string("private val ").size());
        } else if (starts_with(line, "private fun ")) {
            line = "internal fun " + line.substr(std::string("private fun ").size());
        }
        if (i > *start) text += "\n";
        text += line;
    }
    for (const char* name : kExpected) {
        if (text.find(name) == std::string::npos) {
            fail(std::string("error: ") + name + " missing from the extracted region");
        }
    }
    write_file(out, std::string(kHeader) + text + "\n");
}

// context-compact/extract_message_entity.py — MessageEntity with its annotation block.
void profile_message_entity(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract message-entity from data/local/ChatEntities.kt -- do not "
        "edit.\n"
        "//\n"
        "// MessageEntity is copied verbatim; the Room annotations are satisfied by the "
        "stand-local stubs\n"
        "// (Room is a persistence framework: the annotations have no runtime behaviour the "
        "compact graph\n"
        "// transactions can observe).\n"
        "package com.newoether.agora.data.local\n"
        "\n"
        "import androidx.room.Entity\n"
        "import androidx.room.ForeignKey\n"
        "import androidx.room.Index\n"
        "import androidx.room.PrimaryKey\n"
        "import com.newoether.agora.model.MessageStatus\n"
        "import com.newoether.agora.model.Participant\n"
        "\n";
    static constexpr const char* kDeclaration = "data class MessageEntity(";

    const std::vector<std::string_view> lines = split_lines(read_file(source));
    std::optional<std::size_t> declaration_index;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], kDeclaration)) {
            declaration_index = i;
            break;
        }
    }
    if (!declaration_index) fail(std::string("error: MessageEntity not found"));

    // Walk back over the annotation block directly above the declaration.
    std::size_t probe = *declaration_index;
    while (probe > 0) {
        const std::string previous = trim_copy(lines[probe - 1]);
        if (previous.empty() || previous == "}") break;
        const std::string_view raw = lines[probe - 1];
        if (starts_with(raw, "@") || starts_with(raw, "    ") || previous == ")" ||
            previous == "],") {
            probe -= 1;
            continue;
        }
        break;
    }

    std::optional<std::size_t> end;
    for (std::size_t i = *declaration_index + 1; i < lines.size(); ++i) {
        if (lines[i] == "}" || lines[i] == ")") {
            end = i;
            break;
        }
    }
    if (!end) fail("error: end of MessageEntity not found");

    write_file(out, std::string(kHeader) + join_lines(lines, probe, *end) + "\n");
    std::cout << "Extracted MessageEntity\n";
}

// anthropic-stream/extract_models.py — the stream-event models through AnthropicUsage.
void profile_anthropic_models(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract anthropic-models from AnthropicProvider.kt -- do not "
        "edit.\n"
        "//\n"
        "// The stream-event model declarations are copied verbatim from the provider file, "
        "which cannot be\n"
        "// compiled standalone (Android-only imports); the router golden stand pins them "
        "here.\n"
        "package com.newoether.agora.api.anthropic\n"
        "\n"
        "import kotlinx.serialization.SerialName\n"
        "import kotlinx.serialization.Serializable\n"
        "import kotlinx.serialization.json.JsonElement\n"
        "import kotlinx.serialization.json.JsonObject\n"
        "\n";
    static constexpr const char* kStartClass = "internal data class AnthropicStreamEvent(";
    static constexpr const char* kEndClass = "internal data class AnthropicUsage(";
    static constexpr std::array<const char*, 6> kExpected = {
        "AnthropicStreamEvent", "AnthropicStreamError", "AnthropicDelta",
        "AnthropicContentBlock", "AnthropicMessageInfo", "AnthropicUsage"};

    const std::vector<std::string_view> lines = split_lines(read_file(source));
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(kStartClass) != std::string_view::npos) {
            if (i == 0 || trim_copy(lines[i - 1]) != "@Serializable") {
                fail(std::string("error: no @Serializable directly above ") + kStartClass);
            }
            start = i - 1;
            break;
        }
    }
    if (!start) fail(std::string("error: ") + kStartClass + " not found");

    std::optional<std::size_t> end;
    for (std::size_t i = *start; i < lines.size(); ++i) {
        if (lines[i].find(kEndClass) != std::string_view::npos) {
            for (std::size_t j = i; j < lines.size(); ++j) {
                if (trim_copy(lines[j]) == ")") {
                    end = j;
                    break;
                }
            }
            break;
        }
    }
    if (!end) fail(std::string("error: closing paren of ") + kEndClass + " not found");

    const std::string region = join_lines(lines, *start, *end);
    for (const char* cls : kExpected) {
        if (region.find(std::string("internal data class ") + cls + "(") == std::string::npos) {
            fail(std::string("error: ") + cls + " is not inside the extracted region");
        }
    }
    write_file(out, std::string(kHeader) + region + "\n");
}

// ---- batch-6 profiles ------------------------------------------------------------------------

// semantic-index-ledger/extract_entities.py — EmbeddingEntity with its annotation block.
void profile_embedding_entity(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract embedding-entity from data/local/ChatEntities.kt -- do not "
        "edit.\n"
        "//\n"
        "// EmbeddingEntity is copied verbatim; the Room annotations are satisfied by the "
        "stand-local stubs\n"
        "// (Room is a persistence framework: the annotations have no runtime behaviour the "
        "ledger observes).\n"
        "package com.newoether.agora.data.local\n"
        "\n"
        "import androidx.room.Entity\n"
        "import androidx.room.Index\n"
        "import androidx.room.PrimaryKey\n"
        "\n";
    static constexpr const char* kDeclaration = "data class EmbeddingEntity(";

    const std::vector<std::string_view> lines = split_lines(read_file(source));
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], kDeclaration)) {
            start = i;
            break;
        }
    }
    if (!start) fail("error: EmbeddingEntity not found");

    std::size_t annotation_start = *start;
    std::size_t probe = *start;
    while (probe > 0) {
        const std::string stripped = trim_copy(lines[probe - 1]);
        if (stripped.empty()) break;
        if (starts_with(lines[probe - 1], "@")) {
            annotation_start = probe - 1;
            probe -= 1;
            continue;
        }
        if (starts_with(lines[probe - 1], "    ") || stripped == ")" || stripped == "],") {
            annotation_start = probe - 1;
            probe -= 1;
            continue;
        }
        break;
    }

    std::optional<std::size_t> end;
    for (std::size_t i = *start; i < lines.size(); ++i) {
        if (lines[i] == "}") {
            end = i;
            break;
        }
    }
    if (!end) fail("error: end of EmbeddingEntity not found");

    write_file(out, std::string(kHeader) + join_lines(lines, annotation_start, *end) + "\n");
    std::cout << "Extracted EmbeddingEntity\n";
}

// shell-command-validation/extract_validation.py — one widened function + two constants.
void profile_shell_validation(const fs::path& tool_source, const fs::path& client_source,
                              const fs::path& out) {
    static constexpr const char* kHeader =
        "// GENERATED by agora_extract shell-validation -- do not edit.\n"
        "//\n"
        "// shellCommandValidationError is copied from ShellToolProvider.kt and its constants\n"
        "// from ShellClient.kt so the shell-command-validation golden stand can compile the "
        "real logic.\n"
        "package com.newoether.agora.tool\n"
        "\n";
    static constexpr const char* kFunctionMarker = "private fun shellCommandValidationError(";
    static constexpr std::array<const char*, 2> kConstants = {
        "const val SHELL_COMMAND_MAX_BYTES", "const val SHELL_WORKDIR_MAX_BYTES"};

    const std::string tool = read_file(tool_source);
    const std::string client = read_file(client_source);

    auto lines_of = [](const std::string& text) {
        struct Line {
            std::size_t begin;
            std::size_t end;  // including the newline when present
        };
        std::vector<Line> lines;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t newline = text.find('\n', pos);
            const std::size_t stop = newline == std::string::npos ? text.size() : newline + 1;
            lines.push_back(Line{pos, stop});
            if (newline == std::string::npos) break;
            pos = stop;
        }
        return lines;
    };

    std::string extracted;
    const auto client_lines = lines_of(client);
    for (const char* marker : kConstants) {
        bool found = false;
        for (const auto& line : client_lines) {
            const std::string_view content =
                std::string_view(client).substr(line.begin, line.end - line.begin);
            if (content.find(marker) != std::string_view::npos) {
                extracted += std::string(content);
                if (content.empty() || content.back() != '\n') extracted += "\n";
                extracted += "\n";
                found = true;
                break;
            }
        }
        if (!found) fail(std::string("error: ") + marker + " not found");
    }

    const auto tool_lines = lines_of(tool);
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < tool_lines.size(); ++i) {
        const std::string_view content = std::string_view(tool).substr(
            tool_lines[i].begin, tool_lines[i].end - tool_lines[i].begin);
        if (starts_with(content, kFunctionMarker)) {
            start = i;
            break;
        }
    }
    if (!start) fail(std::string("error: ") + kFunctionMarker + " not found");

    int brace_depth = 0;
    std::optional<std::size_t> end;
    for (std::size_t i = *start; i < tool_lines.size(); ++i) {
        const std::string_view content = std::string_view(tool).substr(
            tool_lines[i].begin, tool_lines[i].end - tool_lines[i].begin);
        brace_depth += static_cast<int>(std::count(content.begin(), content.end(), '{')) -
                       static_cast<int>(std::count(content.begin(), content.end(), '}'));
        if (i > *start && brace_depth <= 0) {
            end = i;
            break;
        }
    }
    if (!end) fail(std::string("error: closing brace of ") + kFunctionMarker + " not found");

    for (std::size_t i = *start; i <= *end; ++i) {
        std::string content(tool.substr(tool_lines[i].begin,
                                        tool_lines[i].end - tool_lines[i].begin));
        if (i == *start) {
            const std::size_t at = content.find("private fun");
            if (at != std::string::npos) content.replace(at, std::string("private fun").size(),
                                                         "internal fun");
        }
        extracted += content;
    }

    write_file(out, std::string(kHeader) + extracted);
    std::cout << "extracted 3 regions into " << out.string() << "\n";
}

// native-conversation-graph-importer/extract_import_pieces.py — sanitize helpers + nested enum.
void profile_import_pieces(const fs::path& source, const fs::path& out) {
    static constexpr const char* kHeader =
        "// Generated by agora_extract import-pieces from "
        "app/src/main/java/com/newoether/agora/data/DataImporter.kt.\n"
        "// The function bodies and the enum below are copied byte-for-byte; only the package "
        "line and the\n"
        "// imports the pieces use are added here.\n"
        "package com.newoether.agora.data\n"
        "\n"
        "import com.newoether.agora.automation.LoopPolicy\n"
        "import com.newoether.agora.data.local.ChatEntity\n"
        "import com.newoether.agora.data.local.LoopEntity\n"
        "import com.newoether.agora.data.local.TaskEntity\n"
        "\n";

    const std::string text = read_file(source);
    const std::vector<std::string_view> lines = split_lines(text);

    std::optional<std::size_t> task_doc;
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
        if (starts_with(lines[i], "/**") && starts_with(lines[i + 1], " * Imported automations are content")) {
            task_doc = i;
            break;
        }
    }
    if (!task_doc) fail("sanitizeImportedTask KDoc not found");

    std::optional<std::size_t> conversation_start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], "internal fun sanitizeImportedConversation(")) {
            conversation_start = i;
            break;
        }
    }
    if (!conversation_start) fail("sanitizeImportedConversation not found");

    std::optional<std::size_t> conversation_end;
    for (std::size_t i = *conversation_start; i < lines.size(); ++i) {
        if (lines[i] == "}") {
            conversation_end = i;
            break;
        }
    }
    if (!conversation_end) fail("sanitizeImportedConversation closing brace not found");

    std::optional<std::size_t> enum_start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], "    enum class ImportStrategy")) {
            enum_start = i;
            break;
        }
    }
    if (!enum_start) fail("ImportStrategy enum not found");

    std::optional<std::size_t> enum_end;
    for (std::size_t i = *enum_start; i < lines.size(); ++i) {
        if (lines[i] == "    }") {
            enum_end = i;
            break;
        }
    }
    if (!enum_end) fail("ImportStrategy closing brace not found");

    std::string out_text = kHeader;
    out_text += join_lines(lines, *task_doc, *conversation_end);
    out_text += "\n\n";
    out_text += "/** The import strategy enum nested in DataImporter (see DataImporter.kt). */\n";
    out_text += "class DataImporter {\n";
    out_text += join_lines(lines, *enum_start, *enum_end);
    out_text += "\n}\n";
    write_file(out, out_text);
    std::cout << "extracted sanitize helpers + ImportStrategy\n";
}

// attachment-clone-session/extract_clone_policy.py — MessageEntity + the widened policy file.
int profile_clone_policy(int argc, char** argv) {
    if (argc != 4) usage();
    const fs::path app_dir = argv[2];
    const fs::path out_dir = argv[3];
    static constexpr const char* kEntityHeader =
        "// GENERATED by agora_extract clone-policy from data/local/ChatEntities.kt -- do not "
        "edit.\n"
        "//\n"
        "// MessageEntity is copied verbatim; the Room annotations are satisfied by the "
        "stand-local stubs\n"
        "// (Room is a persistence framework: the annotations have no runtime behaviour the "
        "clone policy\n"
        "// can observe).\n"
        "package com.newoether.agora.data.local\n"
        "\n"
        "import androidx.room.Entity\n"
        "import androidx.room.ForeignKey\n"
        "import androidx.room.Index\n"
        "import androidx.room.PrimaryKey\n"
        "import com.newoether.agora.model.MessageSource\n"
        "import com.newoether.agora.model.MessageStatus\n"
        "import com.newoether.agora.model.Participant\n"
        "\n";
    static constexpr const char* kDeclaration = "data class MessageEntity(";

    const std::string entities = read_file(app_dir / "data/local/ChatEntities.kt");
    const std::vector<std::string_view> lines = split_lines(entities);
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], kDeclaration)) {
            start = i;
            break;
        }
    }
    if (!start) fail("error: MessageEntity not found");

    std::size_t annotation_start = *start;
    std::size_t probe = *start;
    while (probe > 0) {
        const std::string_view previous = lines[probe - 1];
        if (starts_with(previous, "@") || starts_with(previous, "        ") ||
            trim_copy(previous).empty()) {
            if (trim_copy(previous).empty()) break;
            probe -= 1;
            annotation_start = probe;
            continue;
        }
        break;
    }

    std::optional<std::size_t> end;
    for (std::size_t i = *start; i < lines.size(); ++i) {
        if (lines[i] == "}") {
            end = i;
            break;
        }
    }
    if (!end) fail("error: end of MessageEntity not found");

    std::error_code error;
    fs::create_directories(out_dir, error);
    if (error) fail("error: cannot create " + out_dir.string());
    write_file(out_dir / "MessageEntity.kt",
               std::string(kEntityHeader) + join_lines(lines, annotation_start, *end) + "\n");

    const fs::path policy_path = app_dir / "data/MessageAttachmentCloneSession.kt";
    if (!fs::exists(policy_path)) fail("error: missing " + policy_path.string());
    const std::string policy = read_file(policy_path);
    std::string widened;
    std::size_t pos = 0;
    while (true) {
        const std::size_t newline = policy.find('\n', pos);
        const std::size_t stop = newline == std::string::npos ? policy.size() : newline;
        std::string line = policy.substr(pos, stop - pos);
        std::size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        if (line.compare(lead, std::string("internal ").size(), "internal ") == 0) {
            line = line.substr(0, lead) +
                   line.substr(lead + std::string("internal ").size());
        }
        widened += line;
        if (newline == std::string::npos) break;
        widened += "\n";
        pos = newline + 1;
    }
    write_file(out_dir / "MessageAttachmentCloneSession.kt", widened);
    std::cout << "Extracted MessageEntity and the clone policy\n";
    return 0;
}

void usage() {
    std::cerr << "usage: agora_extract <profile> <stand_dir>\n"
                 "       agora_extract <profile> <source.kt> <out.kt>\n"
              << "profiles (stand_dir):\n"
              << "  enums-16-17     migration-16-17 extract_enums.py\n"
              << "  enums-17-18     migration-17-18 extract_enums.py\n"
              << "  enums-18-19     migration-18-19 extract_enums.py\n"
              << "  enums-legacy    legacy-run-backfill extract_enums.py\n"
              << "  participant     message-source/v18/regeneration-tree-repair "
                 "extract_participant.py\n"
              << "  marker          migration-19-20 extract_marker.py\n"
              << "  tool-kind       streaming-json-document extract_tool_kind.py\n"
              << "profiles (source, out):\n"
              << "  task-entity         task-schedule extract_task_entity.py\n"
              << "  message-segment     backup-media-policy extract_message_segment.py\n"
              << "  run-recovery-models run-recovery extract_model_types.py\n"
              << "  run-entity          run-branch-selection extract_run_entity.py\n"
              << "  shell-prefix        conch-sse extract_shell_prefix.py\n"
              << "  graph-interface     native-backup-archive extract_graph_interface.py\n"
              << "  read-to-end         native-conversation-graph-source extract_read_to_end.py\n"
              << "  sanitize-loop       data-exporter extract_sanitize_imported_loop.py\n"
              << "  responses-models    openai-responses-request extract_responses_models.py\n"
              << "  export-category     data-importer extract_export_category.py\n"
              << "  identity-policy     model-thinking-resolver extract_identity_policy.py\n"
              << "  embedding-models    embedding extract_embedding_models.py\n"
              << "  shell-models        shell-tools extract_shell_models.py\n"
              << "  aliases             model-aliases extract_aliases.py\n"
              << "  message-entity      context-compact extract_message_entity.py\n"
              << "  anthropic-models    anthropic-stream extract_models.py\n"
              << "  shell-validation    shell-command-validation extract_validation.py "
                 "(3 paths)\n"
              << "  embedding-entity    semantic-index-ledger extract_entities.py\n"
              << "  import-pieces       native-conversation-graph-importer "
                 "extract_import_pieces.py\n"
              << "  clone-policy <app_dir> <out_dir>  attachment-clone-session\n"
              << "profiles (dirs):\n"
              << "  widen <src_dir> <out_dir> <file>...\n";
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string profile = argc >= 2 ? argv[1] : "";

    // Widen profiles: agora_extract widen <source_dir> <out_dir> <file> [<file>...]
    if (profile == "widen") return profile_widen(argc, argv);
    // Clone-policy: agora_extract clone-policy <app_dir> <out_dir>
    if (profile == "clone-policy") return profile_clone_policy(argc, argv);
    // Shell-validation: agora_extract shell-validation <ShellToolProvider.kt> <ShellClient.kt>
    // <out.kt>
    if (profile == "shell-validation") {
        if (argc != 5) usage();
        profile_shell_validation(argv[2], argv[3], argv[4]);
        return 0;
    }

    // Source/out profiles: agora_extract <profile> <source.kt> <out.kt>
    if (argc == 4) {
        const fs::path source = argv[2];
        const fs::path out = argv[3];
        if (profile == "task-entity") {
            profile_task_entity(source, out);
        } else if (profile == "message-segment") {
            profile_message_segment(source, out);
        } else if (profile == "run-recovery-models") {
            profile_run_recovery_models(source, out);
        } else if (profile == "run-entity") {
            profile_run_entity(source, out);
        } else if (profile == "shell-prefix") {
            profile_shell_prefix(source, out);
        } else if (profile == "graph-interface") {
            profile_graph_interface(source, out);
        } else if (profile == "read-to-end") {
            profile_read_to_end(source, out);
        } else if (profile == "sanitize-loop") {
            profile_sanitize_loop(source, out);
        } else if (profile == "responses-models") {
            profile_responses_models(source, out);
        } else if (profile == "export-category") {
            profile_export_category(source, out);
        } else if (profile == "identity-policy") {
            profile_identity_policy(source, out);
        } else if (profile == "embedding-models") {
            profile_embedding_models(source, out);
        } else if (profile == "shell-models") {
            profile_shell_models(source, out);
        } else if (profile == "aliases") {
            profile_aliases(source, out);
        } else if (profile == "message-entity") {
            profile_message_entity(source, out);
        } else if (profile == "anthropic-models") {
            profile_anthropic_models(source, out);
        } else if (profile == "embedding-entity") {
            profile_embedding_entity(source, out);
        } else if (profile == "import-pieces") {
            profile_import_pieces(source, out);
        } else {
            usage();
        }
        return 0;
    }

    // Stand profiles: agora_extract <profile> <stand_dir>
    if (argc != 3) usage();
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