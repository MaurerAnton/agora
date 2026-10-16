// Unit pins for the request pipeline pieces the golden traces cannot reach on their own:
// the FIPS 180-4 SHA-256 vectors, the full Float.toString matrix captured from the JVM,
// the RFC 4648 base64 vectors, the streaming-body write/verification loop, and a handful of
// thinking-resolution facts. The scenario-driven golden test covers the rest.
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/openai_request.hpp"
#include "agora/core/sha256.hpp"
#include "check.hpp"

namespace {

namespace request = agora::core::openai;

std::string hex_lower(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::filesystem::path make_temp_dir() {
    const std::filesystem::path base = std::filesystem::temp_directory_path() /
                                       ("agora-openai-request-test-" +
                                        std::to_string(
                                            std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()));
    std::filesystem::create_directories(base);
    return base;
}

void write_bytes_to(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::int64_t mtime_nanos(const std::filesystem::path& path) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::filesystem::last_write_time(path).time_since_epoch())
        .count();
}

}  // namespace

TEST(sha256_fips180_vectors) {
    // FIPS 180-4 / NIST examples, sha256("") == e3b0c44298fc1c14...
    CHECK_EQ(std::string("e3b0c44298fc1c149afbf4c8996fb924"
                         "27ae41e4649b934ca495991b7852b855"),
             hex_lower(agora::core::sha256("")));
    CHECK_EQ(std::string("ba7816bf8f01cfea414140de5dae2223"
                         "b00361a396177a9cb410ff61f20015ad"),
             hex_lower(agora::core::sha256("abc")));
    CHECK_EQ(std::string("248d6a61d20638b8e5c026930c3e6039"
                         "a33ce45964ff2167f6ecedd419db06c1"),
             hex_lower(agora::core::sha256(
                 "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")));
    // One million 'a': the streaming path across block boundaries.
    const std::string million(1'000'000, 'a');
    CHECK_EQ(std::string("cdc76e5c9914fb9281a1c7e284d73e67"
                         "f1809a48a497200e046d39ccc7112cd0"),
             hex_lower(agora::core::sha256(million)));

    // Incremental updates must equal one-shot hashing (the file-hash path feeds chunks).
    agora::core::Sha256Digest digest;
    digest.update("ab");
    digest.update("c");
    CHECK_EQ(hex_lower(agora::core::sha256("abc")), hex_lower(digest.finish()));
}

TEST(kotlin_float_to_string_vectors) {
    // Captured from the JVM via kotlinc: Float.toString on each value below.
    const std::vector<std::pair<float, const char*>> vectors = {
        {1.0e-7f, "1.0E-7"},
        {std::numeric_limits<float>::max(), "3.4028235E38"},
        {std::numeric_limits<float>::denorm_min(), "1.4E-45"},
        {-0.0f, "-0.0"},
        {0.0f, "0.0"},
        {0.1f, "0.1"},
        {1.0f, "1.0"},
        {123456.79f, "123456.79"},
        {0.3f, "0.3"},
        {0.001f, "0.001"},
        {0.0001f, "1.0E-4"},
        {9999999.0f, "9999999.0"},
        {1.0e7f, "1.0E7"},
        {12345678.0f, "1.2345678E7"},
        {12.34f, "12.34"},
        {0.9999999f, "0.9999999"},
        {16777218.0f, "1.6777218E7"},
        {65536.0f, "65536.0"},
        {1.0e30f, "1.0E30"},
        {1.17549435e-38f, "1.1754944E-38"},
        {1.4e-45f, "1.4E-45"},
        {-1.5f, "-1.5"},
        {2.5f, "2.5"},
        {4.9e-3f, "0.0049"},
        {12345.678f, "12345.678"},
        {100.0f, "100.0"},
        {0.01f, "0.01"},
        {1.0e-3f, "0.001"},
        {9.999999e6f, "9999999.0"},
        {3.4028234e38f, "3.4028235E38"},
        {1.1754944e-38f, "1.1754944E-38"},
        {0.0049f, "0.0049"},
        {-123456.79f, "-123456.79"},
    };
    for (const auto& [value, expected] : vectors) {
        CHECK_EQ(std::string(expected), request::format_kotlin_float(value));
    }
    // Non-finite values print in Kotlin spelling (the encoder rejects them before formatting).
    CHECK_EQ(std::string("NaN"), request::format_kotlin_float(std::numeric_limits<float>::quiet_NaN()));
    CHECK_EQ(std::string("Infinity"), request::format_kotlin_float(std::numeric_limits<float>::infinity()));
    CHECK_EQ(std::string("-Infinity"),
             request::format_kotlin_float(-std::numeric_limits<float>::infinity()));
}

TEST(base64_rfc4648_vectors) {
    // java.util.Base64.getEncoder(): RFC 4648 §10 with padding.
    CHECK_EQ(std::string(""), request::encode_base64(""));
    CHECK_EQ(std::string("Zg=="), request::encode_base64("f"));
    CHECK_EQ(std::string("Zm8="), request::encode_base64("fo"));
    CHECK_EQ(std::string("Zm9v"), request::encode_base64("foo"));
    CHECK_EQ(std::string("Zm9vYg=="), request::encode_base64("foob"));
    CHECK_EQ(std::string("Zm9vYmE="), request::encode_base64("fooba"));
    CHECK_EQ(std::string("Zm9vYmFy"), request::encode_base64("foobar"));
    // Attachment sizes from the golden fixtures exercise every padding length.
    CHECK_EQ(std::size_t{8}, request::encode_base64(std::string(4, '\x7A')).size());
    CHECK_EQ(std::size_t{4}, request::encode_base64(std::string(3, '\x7A')).size());
    CHECK_EQ(std::size_t{4}, request::encode_base64(std::string(1, '\x7A')).size());
}

TEST(streaming_json_body_write_loop) {
    const std::filesystem::path dir = make_temp_dir();
    const std::filesystem::path file = dir / "image.png";
    write_bytes_to(file, "payload-bytes");

    request::Base64FileRegistry registry;
    const auto placeholder = registry.register_file(file.string());
    CHECK(placeholder.has_value());

    const std::string body_json =
        "{\"messages\":[{\"content\":\"data:image/png;base64," + *placeholder + "\"}]}";
    const auto body = registry.prepare(body_json);
    CHECK(body.has_value());
    CHECK_EQ(std::size_t{1}, body->segments.size() - body->parts.size());
    CHECK_EQ(request::encode_base64("payload-bytes"),
             request::encode_base64("payload-bytes"));
    // resolved form == transport write on a stable file, and the length gate holds.
    const std::string resolved = body->resolved_bytes();
    CHECK_EQ(static_cast<std::int64_t>(resolved.size()), body->content_length);
    CHECK_EQ(resolved, body->write_bytes());
    CHECK(resolved.find("[STREAMED_BASE64:") == std::string::npos);
    CHECK(body->diagnostic_json.find("[STREAMED_BASE64:13_BYTES]") != std::string::npos);

    // A shorter file fails the metadata check before the write.
    write_bytes_to(file, "short");
    bool threw = false;
    try {
        (void)body->write_bytes();
    } catch (const request::ProviderRequestError& error) {
        threw = true;
        CHECK_EQ(std::string("IOException"), error.class_name);
        CHECK_EQ(std::string("Image attachment changed before request upload"), error.message);
    }
    CHECK(threw);

    // A same-length tamper survives the metadata check but fails the digest re-verification.
    write_bytes_to(file, "payload-BYTES");
    request::StreamingJsonBody tampered = *body;
    tampered.parts[0].byte_count = static_cast<std::int64_t>(std::string("payload-BYTES").size());
    tampered.parts[0].last_modified = mtime_nanos(file);
    threw = false;
    try {
        (void)tampered.write_bytes();
    } catch (const request::ProviderRequestError& error) {
        threw = true;
        CHECK_EQ(std::string("IOException"), error.class_name);
        CHECK_EQ(std::string("Image attachment changed during request upload"), error.message);
    }
    CHECK(threw);

    // A missing path and a directory never register.
    CHECK(!registry.register_file((dir / "missing.png").string()).has_value());
    CHECK(!registry.register_file(dir.string()).has_value());

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST(thinking_resolution_unit_pins) {
    using request::ThinkingProviderFamily;
    using request::capability_for_model;
    using request::resolve_thinking;

    // Disable-able model with thinking off: no effort, no budget.
    const auto qwen_hybrid = capability_for_model(ThinkingProviderFamily::Qwen, "qwen3-30b-a3b");
    const auto off = resolve_thinking(qwen_hybrid, false, "medium", true, 4096);
    CHECK(off.disabled());
    CHECK(!off.effort.has_value());
    CHECK(!off.budget_tokens.has_value());

    // Effort+unsupported budget combination drops the effort (qwen3.8 rejects both together).
    const auto qwen38 = capability_for_model(ThinkingProviderFamily::Qwen, "qwen3.8-max");
    const auto both = resolve_thinking(qwen38, true, "medium", true, 4096);
    CHECK(!both.effort.has_value());  // effort suppressed by the budget, see the golden pin
    CHECK_EQ(4096, *both.budget_tokens);

    // Forced thinking (cannot disable): enabled stays true with the lowest accepted effort.
    const auto astra = capability_for_model(ThinkingProviderFamily::OpenAi, "gpt-6-astra");
    const auto forced = resolve_thinking(astra, false, "high", false, 4096);
    CHECK(forced.enabled);
    CHECK_EQ(std::string("low"), *forced.effort);

    // Budget clamps into the documented window.
    const auto clamped = resolve_thinking(qwen_hybrid, true, "medium", true, 40000);
    CHECK_EQ(32768, *clamped.budget_tokens);
    const auto floor = resolve_thinking(qwen_hybrid, true, "medium", true, 0);
    CHECK_EQ(1, *floor.budget_tokens);

    // Nearest-effort ties resolve upward (deepseek medium -> high; qwen38 high -> xhigh).
    // The optionals are bound to locals first: CHECK_EQ stores a reference, and dereferencing a
    // temporary optional inside the macro would leave that reference dangling.
    const auto deepseek_capability =
        capability_for_model(ThinkingProviderFamily::DeepSeek, "deepseek-v4");
    const auto qwen_flash_capability =
        capability_for_model(ThinkingProviderFamily::Qwen, "qwen3.8-flash");
    const auto open_ai_capability = capability_for_model(ThinkingProviderFamily::OpenAi, "gpt-4.1");
    CHECK_EQ(std::string("high"), *deepseek_capability.nearest_effort("medium"));
    CHECK_EQ(std::string("xhigh"), *qwen_flash_capability.nearest_effort("high"));
    // "none" is not on the ordered selector list: the first accepted value wins.
    CHECK_EQ(std::string("minimal"), *open_ai_capability.nearest_effort("none"));
}

TEST(thinking_levels_unit_pins) {
    // ThinkingLevels helpers the wire never reaches: selector indexing and the budget migration.
    CHECK_EQ(std::string("minimal"), request::effort_for_index(0));
    CHECK_EQ(std::string("max"), request::effort_for_index(5));
    CHECK_EQ(std::string("medium"), request::effort_for_index(6));   // past the end
    CHECK_EQ(std::string("medium"), request::effort_for_index(999));

    CHECK_EQ(std::size_t{3}, request::index_for_effort("HIGH"));
    CHECK_EQ(std::size_t{2}, request::index_for_effort("medium"));
    CHECK_EQ(std::size_t{0}, request::index_for_effort("none"));     // clamped to zero
    CHECK_EQ(std::size_t{2}, request::index_for_effort("garbage"));  // unknown normalizes to medium

    // CHECK_EQ binds const references: keep the optionals alive in locals before dereferencing.
    const auto plain_budget = request::legacy_budget_tokens("budget:2048");
    CHECK(plain_budget.has_value());
    CHECK_EQ(2048, *plain_budget);
    const auto spaced_budget = request::legacy_budget_tokens("  Budget:2048  ");
    CHECK_EQ(2048, *spaced_budget);
    const auto signed_budget = request::legacy_budget_tokens("budget:+7");
    CHECK_EQ(7, *signed_budget);
    CHECK(!request::legacy_budget_tokens("budget:0").has_value());
    CHECK(!request::legacy_budget_tokens("budget:-5").has_value());
    CHECK(!request::legacy_budget_tokens("budget:abc").has_value());
    CHECK(!request::legacy_budget_tokens("budget:").has_value());
    CHECK(!request::legacy_budget_tokens("2048").has_value());
    CHECK(!request::legacy_budget_tokens("").has_value());
    CHECK(!request::legacy_budget_tokens("budget:99999999999").has_value());  // Int overflow

    // forProvider: the display-name family plus the per-model refinement.
    CHECK(!request::capability_for_provider("openai", "gpt-6-astra").can_disable_thinking);
    CHECK(request::capability_for_provider("OpenRouter", "deepseek/deepseek-r1")
              .supports_effort_with_budget == false);
    CHECK(request::capability_for_provider("relay", "anything")
              .supports_thinking_budget);  // unknown name -> OpenAI-compatible permissive
}
