// Golden parity test for the C++ ShellCrypto port.
//
// Replays every scenario under core/tools/golden/shell-crypto/scenarios through
// agora::core::shell_crypto and requires the produced trace to equal the frozen Kotlin trace
// byte for byte after the stand's non-determinism mask
// (core/tools/golden/shell-crypto/mask_trace.py) is applied -- exactly as run_golden.sh masks
// both the fresh and the frozen side before diffing. The trace format mirrors the reference
// driver (core/tools/golden/shell-crypto/ShellCryptoTraceDump.kt), including the driver-level
// base64url decoding of the scenario inputs (java.util.Base64 URL decoder semantics).
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/json.hpp"
#include "agora/core/shell_crypto.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace crypto = agora::core::shell_crypto;
namespace prim = agora::core::crypto;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) return &entry.value;
    }
    return nullptr;
}

std::string required_string(const json::Value& object, std::string_view name,
                            const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::String) {
        scenario_fail(scenario, "missing string " + std::string(name));
    }
    return value->text;
}

std::int64_t required_number(const json::Value& object, std::string_view name,
                             const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) {
        scenario_fail(scenario, "missing number " + std::string(name));
    }
    std::size_t consumed = 0;
    try {
        const std::int64_t parsed = std::stoll(value->text, &consumed);
        if (consumed == value->text.size()) return parsed;
    } catch (const std::exception&) {
    }
    scenario_fail(scenario, "not a number: " + value->text);
}

// Driver esc(): only \\, ", \n, \r and \t are escaped (ShellCryptoTraceDump.kt).
std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string json_string(std::string_view key, std::string_view value) {
    return "\"" + std::string(key) + "\":\"" + esc(value) + "\"";
}

std::string json_one(std::string_view key, std::string_view value) {
    return "{" + json_string(key, value) + "}";
}

std::string json_two(std::string_view key1, std::string_view value1, std::string_view key2,
                     std::string_view value2) {
    return "{" + json_string(key1, value1) + "," + json_string(key2, value2) + "}";
}

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

// java.util.Base64.getEncoder().withoutPadding() -- the driver emits the PKCS#8 private key
// with the *standard* alphabet (mask_trace.py distinguishes the two alphabets for exactly this
// reason), while everything else on the wire is base64url.
std::string base64_standard_nopad(std::span<const std::uint8_t> input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= input.size(); i += 3) {
        const std::uint32_t group = (static_cast<std::uint32_t>(input[i]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(input[i + 2]);
        out.push_back(table[(group >> 18) & 0x3F]);
        out.push_back(table[(group >> 12) & 0x3F]);
        out.push_back(table[(group >> 6) & 0x3F]);
        out.push_back(table[group & 0x3F]);
    }
    const std::size_t rest = input.size() - i;
    if (rest == 1) {
        const std::uint32_t group = static_cast<std::uint32_t>(input[i]) << 16;
        out.push_back(table[(group >> 18) & 0x3F]);
        out.push_back(table[(group >> 12) & 0x3F]);
    } else if (rest == 2) {
        const std::uint32_t group = (static_cast<std::uint32_t>(input[i]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8);
        out.push_back(table[(group >> 18) & 0x3F]);
        out.push_back(table[(group >> 12) & 0x3F]);
        out.push_back(table[(group >> 6) & 0x3F]);
    }
    return out;
}

std::string b64url(std::span<const std::uint8_t> bytes) {
    return prim::base64_encode(bytes, /*url_safe=*/true, /*padding=*/false);
}

std::string b64url(const crypto::PublicKeyRaw& raw) {
    return prim::base64_encode(std::span<const std::uint8_t>(raw.data(), raw.size()),
                               /*url_safe=*/true, /*padding=*/false);
}

// java.util.Base64 URL decoder (as vendored by platform/Base64.java): accepts padded and
// unpadded input, maps -/ back to +/ and rejects characters outside the alphabet.
std::vector<std::uint8_t> url_decode(const std::string& input, const std::string& scenario) {
    const prim::Base64Decode decoded = prim::base64_decode(input, /*url_safe=*/true);
    if (!decoded.bytes) scenario_fail(scenario, "driver base64url decode failed for: " + input);
    return *decoded.bytes;
}

crypto::AESKey aes_key(const std::string& input, const std::string& scenario) {
    const std::vector<std::uint8_t> decoded = url_decode(input, scenario);
    if (decoded.size() != crypto::AES_KEY_SIZE) scenario_fail(scenario, "key is not 32 bytes");
    crypto::AESKey key{};
    std::copy(decoded.begin(), decoded.end(), key.begin());
    return key;
}

std::span<const std::uint8_t> span_of(const std::vector<std::uint8_t>& bytes) {
    return std::span<const std::uint8_t>(bytes.data(), bytes.size());
}

// mask_trace.py, line for line: four key-specific substitutions over the rendered trace. The
// placeholder keeps the encoded length, so the sizes the Kotlin side draws (12-byte nonce,
// 48-byte DER, 32-byte raw key, nonce+ct+tag) stay pinned through the mask.
void mask_trace(std::string& text) {
    struct Rule {
        const char* key;
        std::string_view alphabet;
        std::size_t min_length;
    };
    static constexpr std::string_view kUrlSafe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    static constexpr std::string_view kStandard =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    const Rule rules[] = {
        {"nonce", kUrlSafe, 10},
        {"ciphertext", kUrlSafe, 20},
        {"public_key_raw", kUrlSafe, 20},
        {"private_key_raw", kStandard, 20},
    };
    for (const Rule& rule : rules) {
        const std::string needle = std::string("\"") + rule.key + "\":\"";
        std::size_t at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) {
            const std::size_t value_at = at + needle.size();
            std::size_t end = value_at;
            while (end < text.size() &&
                   rule.alphabet.find(text[end]) != std::string_view::npos) {
                ++end;
            }
            if (end - value_at >= rule.min_length && end < text.size() && text[end] == '"') {
                const std::string replacement = "\"" + std::string(rule.key) + "\":\"" +
                                                "<" + rule.key + ":" +
                                                std::to_string(end - value_at) + ">" + "\"";
                text.replace(at, end + 1 - at, replacement);
                at += replacement.size();
            } else {
                at = value_at;
            }
        }
    }
}

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string op = required_string(scenario, "op", stem);
    std::string trace;

    if (op == "generate_ephemeral_keypair") {
        const crypto::KeyPair pair = crypto::generate_ephemeral_keypair();
        trace = json_two("public_key_raw", b64url(pair.public_key), "private_key_raw",
                         base64_standard_nopad(pair.private_key));
    } else if (op == "encode_public_key") {
        const std::vector<std::uint8_t> raw = url_decode(
            required_string(scenario, "public_key_raw", stem), stem);
        if (raw.size() != crypto::X25519_KEY_SIZE) scenario_fail(stem, "public key is not 32 bytes");
        crypto::PublicKeyRaw key{};
        std::copy(raw.begin(), raw.end(), key.begin());
        trace = json_one("encoded", crypto::encode_public_key(key));
    } else if (op == "decode_public_key") {
        const crypto::DecodeResult result =
            crypto::decode_public_key(required_string(scenario, "base64url_key", stem));
        trace = result.error ? json_one("error", *result.error)
                             : json_one("public_key_raw", b64url(result.public_key_raw));
    } else if (op == "derive_aes_key") {
        const std::vector<std::uint8_t> private_der = url_decode(
            required_string(scenario, "private_key_pkcs8", stem), stem);
        const std::string public_raw_b64 = required_string(scenario, "public_key_raw", stem);
        const crypto::DecodeResult decoded = crypto::decode_public_key(public_raw_b64);
        if (decoded.error) scenario_fail(stem, "public key rejected: " + *decoded.error);
        const crypto::DerivedKeyResult derived = crypto::derive_aes_key(
            std::string_view(reinterpret_cast<const char*>(private_der.data()), private_der.size()),
            decoded.public_key_raw);
        trace = json_one("derived_key", b64url(derived.key));
    } else if (op == "encrypt_decrypt") {
        const crypto::AESKey key = aes_key(required_string(scenario, "key", stem), stem);
        const std::string plaintext = required_string(scenario, "plaintext", stem);
        const crypto::EncryptResult encrypted = crypto::encrypt(key, plaintext);
        const crypto::DecryptResult decrypted = crypto::decrypt(key, encrypted.ciphertext);
        if (decrypted.error) scenario_fail(stem, "roundtrip failed: " + *decrypted.error);
        trace = json_two("ciphertext", encrypted.ciphertext, "decrypted", decrypted.decrypted);
    } else if (op == "decrypt") {
        const crypto::AESKey key = aes_key(required_string(scenario, "key", stem), stem);
        const std::string ciphertext = required_string(scenario, "ciphertext", stem);
        if (ciphertext == "PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER") {
            // The driver always round-trips the literal "test" in this branch.
            const crypto::EncryptResult encrypted = crypto::encrypt(key, "test");
            const crypto::DecryptResult decrypted = crypto::decrypt(key, encrypted.ciphertext);
            if (decrypted.error) scenario_fail(stem, "roundtrip failed: " + *decrypted.error);
            trace = json_two("ciphertext", encrypted.ciphertext, "decrypted", decrypted.decrypted);
        } else {
            const crypto::DecryptResult decrypted = crypto::decrypt(key, ciphertext);
            trace = decrypted.error ? json_one("error", *decrypted.error)
                                    : json_one("decrypted", decrypted.decrypted);
        }
    } else if (op == "sign") {
        const crypto::SignResult result = crypto::sign(
            required_string(scenario, "api_key", stem), required_number(scenario, "timestamp", stem),
            required_string(scenario, "method", stem), required_string(scenario, "path", stem),
            required_string(scenario, "body_sha256", stem), required_string(scenario, "nonce", stem),
            required_string(scenario, "client_pub_key", stem));
        trace = json_one("signature", result.signature);
    } else if (op == "sha256_hex") {
        const crypto::SHA256Result result =
            crypto::sha256_hex(required_string(scenario, "data", stem));
        trace = json_one("sha256", hex(result.hash));
    } else if (op == "response_signature") {
        const std::vector<std::uint8_t> session_key =
            url_decode(required_string(scenario, "session_key", stem), stem);
        const auto status = static_cast<int>(required_number(scenario, "status", stem));
        trace = json_one("signature",
                         crypto::response_signature(span_of(session_key), status,
                                                    required_string(scenario, "body", stem)));
    } else if (op == "verify_response_signature") {
        const std::vector<std::uint8_t> session_key =
            url_decode(required_string(scenario, "session_key", stem), stem);
        const auto status = static_cast<int>(required_number(scenario, "status", stem));
        const std::string body = required_string(scenario, "body", stem);
        std::string signature = required_string(scenario, "signature", stem);
        if (signature == "PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER") {
            signature = crypto::response_signature(span_of(session_key), status, body);
        }
        const bool valid = crypto::verify_response_signature(span_of(session_key), status, body,
                                                             signature);
        trace = std::string("{\"valid\":") + (valid ? "true" : "false") + "}";
    } else if (op == "generate_nonce") {
        trace = json_one("nonce", crypto::generate_nonce());
    } else if (op == "base64_url_encode") {
        trace = json_one("encoded",
                         crypto::base64_url_encode(required_string(scenario, "input", stem)));
    } else if (op == "base64_url_decode") {
        const std::string input = required_string(scenario, "input", stem);
        const std::optional<std::string> decoded = crypto::base64_url_decode(input);
        if (!decoded) scenario_fail(stem, "base64url decode failed for: " + input);
        trace = json_one("decoded", *decoded);
    } else {
        scenario_fail(stem, "unknown op: " + op);
    }

    trace += "\n";
    mask_trace(trace);
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(160, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(shell_crypto_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_CRYPTO_DIR;
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
        checked++;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
