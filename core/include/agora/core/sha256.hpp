#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace agora::core {

/**
 * Incremental SHA-256 (FIPS 180-4).
 *
 * Both users in the request pipeline need it streaming: tool-call ids digest one short string and
 * attachment parts digest whole files without materializing them.
 */
class Sha256Digest {
public:
    Sha256Digest();

    void update(const std::uint8_t* data, std::size_t size);
    void update(std::string_view data);

    /** Ends the digest. The object must not be reused afterwards. */
    std::array<std::uint8_t, 32> finish();

private:
    void process_block(const std::uint8_t* block);

    std::uint32_t state_[8];
    std::uint64_t bit_length_ = 0;
    std::uint8_t buffer_[64];
    std::size_t buffered_ = 0;
};

std::array<std::uint8_t, 32> sha256(std::string_view data);

}  // namespace agora::core
