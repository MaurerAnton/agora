// The JNI door into the native OpenAI chat stream consumer (core/src/openai_stream.cpp).
//
// Thin marshaling only: every function forwards to agora_core_lib, which owns all of the
// per-line meaning. The Kotlin wrapper (NativeOpenAiChatStreamConsumer) decodes what comes back
// with the shared binary codec documented in core/include/agora/core/openai_stream_codec.hpp,
// and the codec fixtures pinned from both sides keep this format from drifting.

#include <jni.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/openai_stream.hpp"
#include "agora/core/openai_stream_codec.hpp"
#include "jni_utf8.h"

namespace {

using agora::core::openai::ChatStreamConsumer;
using agora::core::openai::Error;

/**
 * One live consumer plus the debug lines it logged during the current call. The lines are
 * returned in the consumeLine payload instead of being pushed back into Kotlin, so there are no
 * JNI upcalls and no global references to leak.
 */
struct StreamHandle {
    std::vector<std::string> logs;
    ChatStreamConsumer consumer;

    explicit StreamHandle(std::string provider_name)
        : consumer(make_options(this, std::move(provider_name))) {}

private:
    static ChatStreamConsumer::Options make_options(StreamHandle * self, std::string provider_name) {
        ChatStreamConsumer::Options options;
        options.provider_name = std::move(provider_name);
        options.log_malformed = [self](const std::string & message) {
            self->logs.push_back(message);
        };
        return options;
    }
};

StreamHandle * handle_from(jlong handle) {
    return reinterpret_cast<StreamHandle *>(handle);
}

jbyteArray to_byte_array(JNIEnv * env, const std::vector<std::uint8_t> & bytes) {
    jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!array) return nullptr;
    if (!bytes.empty()) {
        env->SetByteArrayRegion(
            array, 0, static_cast<jsize>(bytes.size()),
            reinterpret_cast<const jbyte *>(bytes.data())
        );
    }
    return array;
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_create(
    JNIEnv * env,
    jobject,
    jstring providerName
) {
    std::string provider;
    if (!agora::jni::read_java_string(env, providerName, provider)) return 0;
    try {
        return reinterpret_cast<jlong>(new StreamHandle(std::move(provider)));
    } catch (...) {
        return 0;
    }
}

JNIEXPORT void JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_destroy(
    JNIEnv *,
    jobject,
    jlong handle
) {
    delete handle_from(handle);
}

JNIEXPORT jbyteArray JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_consumeLine(
    JNIEnv * env,
    jobject,
    jlong handle,
    jstring line
) {
    StreamHandle * state = handle_from(handle);
    if (!state || !line) return nullptr;
    std::string text;
    if (!agora::jni::read_java_string(env, line, text)) return nullptr;
    try {
        state->logs.clear();
        const ChatStreamConsumer::LineOutcome outcome = state->consumer.consume_line(text);
        return to_byte_array(
            env, agora::core::openai::encode_line_outcome(outcome, state->logs)
        );
    } catch (...) {
        return nullptr;
    }
}

JNIEXPORT jboolean JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_onReadTimeout(
    JNIEnv *,
    jobject,
    jlong handle,
    jboolean terminalGraceActive
) {
    StreamHandle * state = handle_from(handle);
    if (!state) return JNI_FALSE;
    return state->consumer.on_read_timeout(terminalGraceActive == JNI_TRUE)
        ? JNI_TRUE
        : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_onReadError(
    JNIEnv * env,
    jobject,
    jlong handle,
    jint statusCode,
    jstring message
) {
    StreamHandle * state = handle_from(handle);
    if (!state) return;
    std::string text;
    if (!agora::jni::read_java_string(env, message, text)) return;
    Error error;
    error.kind = Error::Kind::Network;
    error.status_code = statusCode;
    error.message = std::move(text);
    state->consumer.on_read_error(std::move(error));
}

JNIEXPORT jboolean JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_hasSemanticTerminal(
    JNIEnv *,
    jobject,
    jlong handle
) {
    StreamHandle * state = handle_from(handle);
    return state && state->consumer.has_semantic_terminal() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL Java_com_newoether_agora_core_NativeOpenAiStream_finish(
    JNIEnv * env,
    jobject,
    jlong handle
) {
    StreamHandle * state = handle_from(handle);
    if (!state) return nullptr;
    try {
        return to_byte_array(
            env, agora::core::openai::encode_termination(state->consumer.finish())
        );
    } catch (...) {
        return nullptr;
    }
}

} // extern "C"
