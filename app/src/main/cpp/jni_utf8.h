#pragma once

#include <jni.h>
#include <cstdint>
#include <string>
#include <vector>

namespace agora {
namespace jni {

// JNI's GetStringUTFChars returns Modified UTF-8. Native templates, tokenizers and
// filesystem APIs require standard UTF-8, including four-byte supplementary code points.
inline bool read_java_string(JNIEnv * env, jstring value, std::string & result) {
    if (!value) {
        result.clear();
        return true;
    }
    jclass string_class = env->GetObjectClass(value);
    if (!string_class) return false;
    jmethodID get_bytes = env->GetMethodID(
        string_class, "getBytes", "(Ljava/lang/String;)[B"
    );
    env->DeleteLocalRef(string_class);
    if (!get_bytes) return false;
    jstring encoding = env->NewStringUTF("UTF-8");
    if (!encoding) return false;
    jbyteArray bytes = static_cast<jbyteArray>(
        env->CallObjectMethod(value, get_bytes, encoding)
    );
    env->DeleteLocalRef(encoding);
    if (env->ExceptionCheck() || !bytes) {
        if (bytes) env->DeleteLocalRef(bytes);
        return false;
    }
    const jsize size = env->GetArrayLength(bytes);
    result.resize(static_cast<size_t>(size));
    if (size > 0) {
        env->GetByteArrayRegion(bytes, 0, size, reinterpret_cast<jbyte *>(&result[0]));
    }
    env->DeleteLocalRef(bytes);
    return !env->ExceptionCheck();
}

inline bool read_java_path(JNIEnv * env, jstring value, std::string & result) {
    return value && read_java_string(env, value, result) &&
        result.find('\0') == std::string::npos;
}

// The reverse direction: standard UTF-8 into a Java String built from explicit UTF-16 units, so
// supplementary code points survive and no Modified UTF-8 is involved. Invalid bytes decode to
// U+FFFD exactly like the JVM's own UTF-8 decoder.
inline bool write_java_string(JNIEnv * env, const std::string & value, jstring & result) {
    std::vector<jchar> units;
    units.reserve(value.size());
    std::size_t i = 0;
    const std::size_t size = value.size();
    while (i < size) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        std::uint32_t code_point = 0xFFFD;
        std::size_t length = 1;
        if (byte < 0x80) {
            code_point = byte;
        } else if ((byte & 0xE0) == 0xC0 && i + 1 < size) {
            const unsigned char second = static_cast<unsigned char>(value[i + 1]);
            if ((second & 0xC0) == 0x80) {
                code_point = ((byte & 0x1F) << 6) | (second & 0x3F);
                length = code_point < 0x80 ? 1 : 2;
                if (code_point < 0x80) code_point = 0xFFFD;
            }
        } else if ((byte & 0xF0) == 0xE0 && i + 2 < size) {
            const unsigned char second = static_cast<unsigned char>(value[i + 1]);
            const unsigned char third = static_cast<unsigned char>(value[i + 2]);
            if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
                code_point =
                    ((byte & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
                const bool valid = code_point >= 0x800 &&
                    (code_point < 0xD800 || code_point > 0xDFFF);
                length = valid ? 3 : 1;
                if (!valid) code_point = 0xFFFD;
            }
        } else if ((byte & 0xF8) == 0xF0 && i + 3 < size) {
            const unsigned char second = static_cast<unsigned char>(value[i + 1]);
            const unsigned char third = static_cast<unsigned char>(value[i + 2]);
            const unsigned char fourth = static_cast<unsigned char>(value[i + 3]);
            if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 &&
                (fourth & 0xC0) == 0x80) {
                code_point = ((byte & 0x07) << 18) | ((second & 0x3F) << 12) |
                    ((third & 0x3F) << 6) | (fourth & 0x3F);
                const bool valid = code_point >= 0x10000 && code_point <= 0x10FFFF;
                length = valid ? 4 : 1;
                if (!valid) code_point = 0xFFFD;
            }
        }
        i += length;
        if (code_point <= 0xFFFF) {
            units.push_back(static_cast<jchar>(code_point));
        } else {
            const std::uint32_t offset = code_point - 0x10000;
            units.push_back(static_cast<jchar>(0xD800 + (offset >> 10)));
            units.push_back(static_cast<jchar>(0xDC00 + (offset & 0x3FF)));
        }
    }
    result = env->NewString(units.data(), static_cast<jsize>(units.size()));
    return result != nullptr;
}

} // namespace jni
} // namespace agora
