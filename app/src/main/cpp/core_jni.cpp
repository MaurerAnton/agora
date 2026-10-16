// The JNI door into the C++ core (see core/). Every function here is a thin marshal: the logic,
// including the fallback and the memo, lives on the native side and is pinned by core/tests to the
// Kotlin implementation it replaces.

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "agora/core/bpe_vocabulary.hpp"
#include "agora/core/text_counter.hpp"

namespace {

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

/**
 * UTF-16 to UTF-8, with unpaired surrogates becoming '?' exactly like Java's own UTF-8 encoder,
 * so a string counts the same whichever side of the bridge it is counted on.
 */
std::string to_utf8(JNIEnv* env, jstring text) {
    const jsize length = env->GetStringLength(text);
    const jchar* units = env->GetStringChars(text, nullptr);
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 2);
    for (jsize index = 0; index < length; ++index) {
        const std::uint32_t unit = units[index];
        std::uint32_t code_point = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < length) {
            const std::uint32_t low = units[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code_point = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                index++;
            } else {
                code_point = '?';
            }
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            code_point = '?';
        }
        append_utf8(out, code_point);
    }
    env->ReleaseStringChars(text, units);
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_newoether_agora_core_NativeTokens_createVocabulary(
    JNIEnv* env,
    jobject,
    jbyteArray data
) {
    if (data == nullptr) return 0;
    try {
        const jsize length = env->GetArrayLength(data);
        std::string bytes(static_cast<std::size_t>(length), '\0');
        if (length > 0) {
            env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
        }
        auto* vocabulary = new agora::core::BpeVocabulary(
            agora::core::BpeVocabulary::parse_tiktoken(bytes)
        );
        return reinterpret_cast<jlong>(vocabulary);
    } catch (...) {
        return 0;
    }
}

JNIEXPORT void JNICALL Java_com_newoether_agora_core_NativeTokens_destroyVocabulary(
    JNIEnv*,
    jobject,
    jlong vocabulary
) {
    delete reinterpret_cast<agora::core::BpeVocabulary*>(vocabulary);
}

JNIEXPORT jlong JNICALL Java_com_newoether_agora_core_NativeTokens_createCounter(
    JNIEnv*,
    jobject,
    jlong vocabulary,
    jdouble scale
) {
    try {
        auto* loaded = reinterpret_cast<agora::core::BpeVocabulary*>(vocabulary);
        if (loaded == nullptr) return 0;
        auto* counter = new agora::core::BpeTextCounter(
            loaded,
            scale,
            &agora::core::HeuristicTextCounter::instance()
        );
        return reinterpret_cast<jlong>(counter);
    } catch (...) {
        return 0;
    }
}

JNIEXPORT void JNICALL Java_com_newoether_agora_core_NativeTokens_destroyCounter(
    JNIEnv*,
    jobject,
    jlong counter
) {
    delete reinterpret_cast<agora::core::BpeTextCounter*>(counter);
}

JNIEXPORT jlong JNICALL Java_com_newoether_agora_core_NativeTokens_count(
    JNIEnv* env,
    jobject,
    jlong counter,
    jstring text
) {
    auto* loaded = reinterpret_cast<agora::core::BpeTextCounter*>(counter);
    if (loaded == nullptr || text == nullptr) return 0;
    const std::string utf8 = to_utf8(env, text);
    try {
        return static_cast<jlong>(loaded->count(utf8));
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
