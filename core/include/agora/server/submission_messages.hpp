#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace agora::server {

/** Optional per-package message attached to a successful submission. */
struct SubmissionMessage {
    std::string id;
    std::string title;
    std::string body;
    std::optional<std::string> button_text;
};

/**
 * Reads AGORA_SUBMISSION_MESSAGES on every call (like the Python reference), parses the JSON
 * config and returns the validated message for an exact package-name match.
 *
 * Parity rules kept verbatim from server/submission_messages.py:
 *  - missing/unreadable/oversized (>1 MiB) config, malformed JSON, or a non-object root -> none;
 *  - the package key must match exactly (a longer dotted name is a different package);
 *  - id/title/body must be present, be strings, and strip() to a non-empty value within
 *    128/200/8000 *code points* (Python len() counts code points, not UTF-16 units);
 *  - buttonText is optional: a non-string, blank or >80-code-point value is dropped without
 *    failing the whole message (only id/title/body failures return none);
 *  - stored values are the stripped ones, emitted in id/title/body/buttonText order.
 */
std::optional<SubmissionMessage> message_for(std::string_view package_name);

}  // namespace agora::server
