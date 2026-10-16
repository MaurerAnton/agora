#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agora/core/openai_stream.hpp"

namespace agora::core::openai {

/**
 * Binary wire format between the native consumer and its Kotlin wrapper.
 *
 * The wrapper hands one raw line in and gets one self-contained payload back; no JNI upcalls,
 * no shared mutable state beyond the consumer handle. All integers are big-endian, strings are
 * a u32 byte length followed by standard UTF-8 (native strings are already surrogate-normalized,
 * so they always round-trip through a Java String unchanged), and a nullable string is a u32
 * length of 0xFFFFFFFF meaning null.
 *
 * consumeLine payload:
 *   u8   keepReading
 *   u16  logCount, then logCount × string (the exact debug lines the consumer logged)
 *   u16  eventCount, then eventCount × Event
 *
 * Event:
 *   u8 kind, then the kind's fields:
 *     1 text             string text
 *     2 thought          string thought, nstring title, nstring signature
 *     3 toolCallUpdate   string streamKey, nstring id, string name, string arguments
 *     4 toolCallRequest  ToolCall
 *     5 toolCallsRequest u16 count, count × ToolCall
 *     6 usage            i32 total, then input/cached/cacheWrite/uncached/output/reasoning,
 *                        each as u8 presence followed by i32 when present
 *     7 error            Error
 *
 * ToolCall:  string id, string name, string arguments, nstring signature, string streamKey
 *
 * Error:
 *   u8 kind: 0 none
 *            1 network           i32 statusCode, string message
 *            2 api               nstring code, nstring type, string message
 *            3 sseParse          string rawLine, string cause
 *            4 malformedToolCall string cause
 *            5 incomplete        string provider, nstring stopReason, u8 inFlight, u8 produced
 *            6 outputTruncated   string provider, nstring stopReason
 *            7 timeout
 *
 * finish() payload (StreamTermination):
 *   u8 sawTerminalMarker, nstring stopReason, u8 producedContent, u8 toolCallInFlight,
 *   u8 alreadyReportedError, u8 timedOut, Error streamError
 *
 * The format is exercised from both ends by the committed fixtures under
 * app/src/test/resources/openai_stream_codec (generator: core/tools/golden/openai/codec).
 */

/** Encodes one consumeLine result; [logs] are the debug lines logged during the call. */
std::vector<std::uint8_t> encode_line_outcome(const ChatStreamConsumer::LineOutcome& outcome,
                                              const std::vector<std::string>& logs);

/** Encodes a stream termination. */
std::vector<std::uint8_t> encode_termination(const Termination& termination);

}  // namespace agora::core::openai
