#include "agora/core/text_counter.hpp"

#include <cmath>
#include <stdexcept>

#include "agora/core/token_count.hpp"
#include "agora/core/unicode.hpp"

namespace agora::core {
namespace {

constexpr std::size_t kMinMemoChars = 64;
constexpr std::int64_t kMaxMemoChars = 4'000'000;

constexpr double kAsciiCharsPerToken = 4.0;

bool is_ascii_alnum(std::uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

}  // namespace

const HeuristicTextCounter& HeuristicTextCounter::instance() {
    static const HeuristicTextCounter counter;
    return counter;
}

std::int64_t HeuristicTextCounter::count(std::string_view utf8) const {
    if (utf8.empty()) return 0;
    std::int64_t tokens = 0;
    std::int64_t ascii_run = 0;
    const auto flush = [&]() {
        if (ascii_run > 0) {
            tokens += static_cast<std::int64_t>(
                std::ceil(static_cast<double>(ascii_run) / kAsciiCharsPerToken));
            ascii_run = 0;
        }
    };

    std::size_t at_offset = 0;
    while (at_offset < utf8.size()) {
        const unicode::Decoded decoded = unicode::decode_utf8(utf8.data() + at_offset, utf8.size() - at_offset);
        const std::uint32_t cp = decoded.code_point;
        if (cp <= 0x7F && is_ascii_alnum(cp)) {
            ascii_run++;
        } else if (unicode::is_jvm_whitespace(cp)) {
            flush();
        } else {
            flush();
            tokens++;
        }
        at_offset += decoded.length;
    }
    flush();
    return tokens;
}

BpeTextCounter::BpeTextCounter(const BpeVocabulary* vocabulary, double scale, const TextCounter* fallback)
    : vocabulary_(vocabulary), scale_(scale), fallback_(fallback) {
    if (!(scale >= 1.0)) {
        throw std::invalid_argument("scale must be at least 1.0");
    }
}

std::int64_t BpeTextCounter::count(std::string_view utf8) const {
    if (utf8.empty()) return 0;
    if (vocabulary_ == nullptr) return fallback_->count(utf8);
    const std::int64_t counted = memoized(utf8);
    if (scale_ == 1.0) return counted;
    return static_cast<std::int64_t>(
        std::ceil(static_cast<double>(counted) * scale_));
}

std::int64_t BpeTextCounter::count_uncached(std::string_view utf8) const {
    return bpe_token_count(utf8, *vocabulary_);
}

std::int64_t BpeTextCounter::memoized(std::string_view utf8) const {
    // Short texts are cheaper to count than to hash and store, so they bypass the memo.
    if (utf8.size() < kMinMemoChars) return count_uncached(utf8);

    {
        const std::lock_guard lock(mutex_);
        const auto found = index_.find(utf8);
        if (found != index_.end()) {
            entries_.splice(entries_.begin(), entries_, found->second);
            return found->second->second;
        }
    }
    // Counted outside the lock so concurrent callers never wait on each other's merges.
    const std::int64_t counted = count_uncached(utf8);
    {
        const std::lock_guard lock(mutex_);
        if (index_.find(utf8) == index_.end()) {
            entries_.emplace_front(std::string(utf8), counted);
            index_.emplace(entries_.front().first, entries_.begin());
            stored_chars_ += static_cast<std::int64_t>(utf8.size());
            while (stored_chars_ > kMaxMemoChars && !entries_.empty()) {
                const Entry& eldest = entries_.back();
                stored_chars_ -= static_cast<std::int64_t>(eldest.first.size());
                index_.erase(eldest.first);
                entries_.pop_back();
            }
        }
    }
    return counted;
}

std::size_t BpeTextCounter::TransparentHash::operator()(std::string_view text) const {
    return std::hash<std::string_view>{}(text);
}

}  // namespace agora::core
