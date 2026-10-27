#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/shell_file_edit.hpp"
#include "agora/core/shell_tools.hpp"

namespace agora::core::shell_client {

// The Conch HTTP client from util/ShellClient.kt: the security handshake, the encrypted
// request assembly, the file/jobs endpoints' request payloads and response coercions, and the
// describeConch*Failure message table. The socket layer is the ConchTransport boundary (the
// Kotlin double stands in for OkHttp on the stand; the real client keeps it in Kotlin) and the
// crypto runs on core's from-scratch primitives (crypto_primitives.hpp). Random inputs
// (challenge, nonce, timestamp, ephemeral key, ciphertext) never reach an unmasked trace byte.

// ---------------------------------------------------------------------------------------------
// Failure models
// ---------------------------------------------------------------------------------------------

/** A transport exception as the Kotlin double throws it (the kind drives describeConch*Failure). */
struct TransportFailure : std::runtime_error {
    enum class Kind { UnknownHost, Connect, SocketTimeout, InterruptedIO, Ssl, Plain };

    Kind kind;
    std::string detail;

    TransportFailure(Kind failure_kind, std::string failure_detail)
        : std::runtime_error(failure_detail),
          kind(failure_kind),
          detail(std::move(failure_detail)) {}
};

/** Kotlin IllegalStateException etc.: the caller (trace driver) records `.message` verbatim. */
struct ClientError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------------------------
// Transport boundary
// ---------------------------------------------------------------------------------------------

/** Kotlin ConchTextResponse. */
struct ConchTextResponse {
    std::int32_t code = 0;
    std::string body;
    bool is_successful = false;
    std::string signature;
};

using HeaderList = std::vector<std::pair<std::string, std::string>>;

/** Kotlin ConchNetwork.getTextResponse/postTextResponse (throws TransportFailure). */
class ConchTransport {
public:
    virtual ~ConchTransport() = default;
    virtual ConchTextResponse get_text_response(const std::string& url,
                                                const HeaderList& headers) = 0;
    virtual ConchTextResponse post_text_response(const std::string& url, const std::string& body,
                                                 const HeaderList& headers,
                                                 const std::optional<std::int64_t>&
                                                     call_timeout_millis) = 0;
};

// ---------------------------------------------------------------------------------------------
// describeConchConnectionFailure / describeConchRequestFailure
// ---------------------------------------------------------------------------------------------

std::string describe_conch_connection_failure(const std::string& server_url,
                                              const TransportFailure& failure);

/** Kotlin `describeConchRequestFailure(serverUrl, operation, error)`. */
std::string describe_conch_request_failure(const std::string& server_url,
                                           const std::string& operation,
                                           const TransportFailure& failure);

// ---------------------------------------------------------------------------------------------
// ShellCrypto compositions (the ShellClient.kt crypto helpers on core's primitives).
// Exposed because the stand's transport double seals responses with the same composition the
// client verifies — and because the Kotlin reference for these lives in ShellCrypto.kt, whose
// own golden stand (shell-crypto) pins the primitives.
// ---------------------------------------------------------------------------------------------

std::string hmac_hex(std::string_view key, std::string_view data);

/** ShellCrypto.deriveAesKey: X25519 + HKDF("conch-agora-v2"). */
crypto::Bytes32 derive_aes_key(const crypto::Bytes32& private_seed,
                               std::span<const std::uint8_t, 32> peer_public);

/** ShellCrypto.encrypt: base64url(nonce12 || ciphertext || tag16), no padding. */
std::string encrypt_payload(const crypto::Bytes32& key, std::string_view plaintext);

/** ShellCrypto.decrypt: throws ClientError with the Kotlin messages. */
std::vector<std::uint8_t> decrypt_payload(const crypto::Bytes32& key,
                                          const std::string& encoded);

/** ShellCrypto.responseSignature (also used by verifyResponseSignature). */
std::string response_signature(const crypto::Bytes32& session_key, std::int32_t status,
                               std::string_view body);

// ---------------------------------------------------------------------------------------------
// ShellClient
// ---------------------------------------------------------------------------------------------

class ShellClient {
public:
    ShellClient(std::string server_url, std::string api_key, std::string cached_public_key,
                ConchTransport& transport);

    // -- security handshake -----------------------------------------------------------------------

    /** Kotlin fetchPublicKey: never throws; the outcome lives in last_error()/server key. */
    bool fetch_public_key();

    std::optional<std::string> last_error() const noexcept { return last_error_; }

    /** Kotlin getServerPublicKeyBase64 (base64url, no padding). */
    std::optional<std::string> get_server_public_key_base64() const;

    // -- execute request assembly -------------------------------------------------------------------

    struct PreparedRequest {
        std::string body;
        HeaderList headers;  // insertion order, like Kotlin's mapOf
        bool is_encrypted = false;
        std::string server_url;
    };

    /** Kotlin prepareRequest: plain JSON body, or the encrypted /execute envelope. */
    PreparedRequest prepare_request(const std::string& command, std::int32_t timeout_ms,
                                    const std::string& workdir);

    /** Kotlin decryptSseData: raw crypto errors propagate unwrapped. */
    std::string decrypt_sse_data(const std::string& encrypted_data);

    /** Kotlin getSessionKey (the live AES session key, when one exists). */
    std::optional<crypto::Bytes32> get_session_key() const { return current_aes_key_; }

    // -- file endpoints ------------------------------------------------------------------------------

    /** Kotlin fileRead: the full ShellFileReadResult coercion matrix. */
    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit);

    /** Kotlin fileWrite: null on success, the server's error string on failure. */
    std::optional<std::string> file_write(const std::string& path, const std::string& content);

    /** Kotlin fileEdit: replacements/sha256 or the exact error string. */
    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& old_string,
                                              const std::string& new_string, bool replace_all);

    /** Kotlin fileGlob folded Result (ok carries the success side; error the failure message). */
    struct GlobOutcome {
        bool ok = false;
        std::vector<std::string> files;
        bool truncated = false;
        std::string error;
    };
    GlobOutcome file_glob(const std::string& pattern, const std::string& path,
                          const std::optional<std::int32_t>& depth);

    struct GrepMatch {
        std::string path;
        std::int32_t line = 0;
        std::string content;
    };
    struct GrepOutcome {
        bool ok = false;
        std::vector<GrepMatch> matches;
        bool truncated = false;
        std::string error;
    };
    GrepOutcome file_grep(const std::string& pattern, const std::string& path,
                          const std::string& glob);

    struct FileImageResult {
        std::string data;
        std::string mime_type;
        std::int64_t size = 0;
        std::optional<std::string> error;
    };
    FileImageResult file_image(const std::string& path);

    // -- jobs endpoints -------------------------------------------------------------------------------

    std::string start_job(const std::string& command, std::int32_t timeout_ms,
                          const std::string& workdir);
    std::string list_jobs();
    std::string get_job(const std::string& job_id);
    std::string stop_job(const std::string& job_id,
                         const std::optional<std::int64_t>& call_timeout_millis);
    std::string acknowledge_job(const std::string& job_id);

private:
    std::string server_url_;
    std::string api_key_;
    ConchTransport& transport_;
    std::optional<crypto::Bytes32> server_public_key_;  // raw X25519 public key
    bool security_verified_ = false;
    std::optional<crypto::Bytes32> current_aes_key_;
    std::optional<crypto::Bytes32> current_ephemeral_seed_;
    std::optional<std::string> last_error_;

    /** Kotlin filePost: encryptedPost without a per-call timeout. */
    std::string file_post(const std::string& path, const std::string& payload);

    /** Kotlin encryptedPost: fetch-on-demand, sign, post, verify, decrypt. */
    std::string encrypted_post(const std::string& path, const std::string& payload,
                               const std::optional<std::int64_t>& call_timeout_millis);

    /** Kotlin buildJsonBody: {"command","timeout_ms"[,"workdir"]}. */
    static std::string build_json_body(const std::string& command, std::int32_t timeout_ms,
                                       const std::string& workdir);
};

}  // namespace agora::core::shell_client
