// Golden parity test for the native provider identity port.
//
// Replays every scenario under core/tools/golden/provider-identity/scenarios through
// agora::core::provider_identity and requires the produced trace to equal the frozen Kotlin trace
// byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/provider-identity/ProviderIdentityTraceDump.kt).
//
// The stand's corpus deliberately supplies pattern-valid ids. The stable-id pattern is strict
// (8-4-4-4-12 hex, version 1..5, variant 89ab), and an id that misses it is not merely "unstable":
// normalize() ignores it and mints one through the policy's own newId(), which is a real
// UUID.randomUUID(). The generator below therefore feeds the scenario's newIds in order and, if a
// scenario ever needed more, would supply a fixed sentinel -- which would make the trace disagree
// with the golden instead of quietly matching a lucky random draw.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/provider_identity.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace identity = agora::core::provider_identity;
using json::Kind;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

std::string text_field(const json::Value& object, std::string_view name, std::string fallback = {}) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::vector<std::string> string_list(const json::Value& object, std::string_view name) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (is_null(value)) return out;
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    for (const json::Value& element : value->elements) out.push_back(element.text);
    return out;
}

identity::ReferenceMap string_map(const json::Value& object, std::string_view name) {
    identity::ReferenceMap out;
    const json::Value* value = field(object, name);
    if (is_null(value)) return out;
    if (value->kind != Kind::Object) scenario_fail(std::string(name) + " must be an object");
    for (const json::Member& entry : value->members) out.emplace_back(entry.key, entry.value.text);
    return out;
}

identity::Provider parse_provider(const json::Value& node) {
    identity::Provider provider;
    provider.name = text_field(node, "name");
    provider.id = text_field(node, "id");
    provider.legacy_names = string_list(node, "legacyNames");
    return provider;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = text_field(root, "name", stem);
    const std::string mode = text_field(root, "mode", "legacyId");

    std::string out;
    const auto line = [&out](const std::string& text) { out += text + "\n"; };
    line("name|" + esc(name));
    line("mode|" + esc(mode));

    if (mode == "legacyId") {
        line("legacyId|" + esc(identity::legacy_id(text_field(root, "providerName"))));
    } else if (mode == "replaceIds") {
        const std::string replacement = text_field(root, "replacement", "PROVIDER");
        line("text|" + esc(identity::replace_stable_ids_for_display(
                            text_field(root, "text"),
                            [&replacement](std::string_view) { return replacement; })));
    } else if (mode == "remap") {
        line("remap|" + esc(identity::remap_provider_reference(text_field(root, "modelId"),
                                                               string_map(root, "migrations"))));
    } else if (mode == "canonical") {
        std::vector<identity::Provider> providers;
        if (const json::Value* node = field(root, "providers"); !is_null(node)) {
            for (const json::Value& entry : node->elements) providers.push_back(parse_provider(entry));
        }
        line("model|" + esc(identity::canonical_custom_model_id(text_field(root, "modelId"), providers)));
    } else if (mode == "preferenceKeys") {
        const identity::ReferenceMap result = identity::remap_model_preference_keys(
            string_map(root, "aliases"), string_map(root, "migrations"));
        line("count|" + std::to_string(result.size()));
        for (const auto& entry : result) line("key|" + esc(entry.first) + "|" + esc(entry.second));
    } else if (mode == "normalize") {
        std::vector<identity::Provider> providers;
        if (const json::Value* node = field(root, "providers"); !is_null(node)) {
            for (const json::Value& entry : node->elements) providers.push_back(parse_provider(entry));
        }
        const std::vector<std::string> occupied = string_list(root, "occupiedIds");
        const std::vector<std::string> new_ids = string_list(root, "newIds");

        std::size_t next = 0;
        const identity::IdGenerator generator = [&new_ids, &next](const identity::Provider&) {
            if (next < new_ids.size()) return new_ids[next++];
            // Unreachable for the frozen corpus; a sentinel keeps a surprise deterministic and
            // therefore visibly wrong instead of invisibly passing.
            return std::string("custom-provider-00000000-0000-4000-8000-000000000000");
        };
        const identity::RandomIdSource random_id = [] {
            return std::string("custom-provider-ffffffff-ffff-4fff-bfff-ffffffffffff");
        };

        const identity::Normalization result =
            identity::normalize(providers, occupied, generator, random_id);
        line("providers|" + std::to_string(result.providers.size()));
        for (std::size_t i = 0; i < result.providers.size(); ++i) {
            const identity::Provider& provider = result.providers[i];
            std::string legacy;
            for (const std::string& entry : provider.legacy_names) {
                if (!legacy.empty()) legacy += ';';
                legacy += entry;
            }
            line("provider|" + std::to_string(i) + "|" + esc(provider.name) + "|" + esc(provider.id) +
                 "|" + esc(legacy));
        }
        line("migrations|" + std::to_string(result.migrations.size()));
        for (std::size_t i = 0; i < result.migrations.size(); ++i) {
            const identity::Migration& migration = result.migrations[i];
            line("migration|" + std::to_string(i) + "|" + esc(migration.legacy_reference) + "|" +
                 esc(migration.provider_id));
        }
    } else {
        scenario_fail(stem + ": unknown mode " + mode);
    }
    return out;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(200, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

/**
 * The retry path cannot be golden-pinned: attempt 1 calls the policy's own newId(), i.e. a real
 * UUID.randomUUID(), which a verbatim stand cannot freeze. It is pinned here instead.
 */
void check_retry_path() {
    const identity::RandomIdSource random_id = [] {
        return std::string("custom-provider-00000002-0002-4002-8002-000000000002");
    };
    // First candidate taken, second one accepted.
    const identity::IdGenerator colliding = [](const identity::Provider&) {
        return std::string("custom-provider-00000001-0001-4001-8001-000000000001");
    };
    const identity::Normalization once = identity::normalize(
        {identity::Provider{"A", "", {}}},
        {"custom-provider-00000001-0001-4001-8001-000000000001"}, colliding, random_id);
    CHECK(once.providers.at(0).id == "custom-provider-00000002-0002-4002-8002-000000000002");

    // A first candidate that is not a stable id at all also falls through to the retry.
    const identity::IdGenerator malformed = [](const identity::Provider&) {
        return std::string("custom-provider-abc123");
    };
    const identity::Normalization twice =
        identity::normalize({identity::Provider{"A", "", {}}}, {}, malformed, random_id);
    CHECK(twice.providers.at(0).id == "custom-provider-00000002-0002-4002-8002-000000000002");

    // A provider whose own id is stable keeps it and never consults a generator.
    bool consulted = false;
    const identity::IdGenerator spy = [&consulted](const identity::Provider&) {
        consulted = true;
        return std::string("custom-provider-00000003-0003-4003-8003-000000000003");
    };
    const identity::Normalization kept = identity::normalize(
        {identity::Provider{"A", "custom-provider-00000001-0001-4001-8001-000000000001", {}}}, {},
        spy, random_id);
    CHECK(!consulted);
    CHECK(kept.providers.at(0).id == "custom-provider-00000001-0001-4001-8001-000000000001");
}

}  // namespace

TEST(provider_identity_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_PROVIDER_IDENTITY_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}

TEST(provider_identity_retry_path) {
    check_retry_path();
}
