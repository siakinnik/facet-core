// SHA-256 (FIPS 180-4), for checking downloaded packages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace facet::gpu {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string hex();  // finishes; lowercase hex digest

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t bits_ = 0;
};

// Digest of a file, "" if it cannot be read.
std::string sha256_file(const std::string& path);

}  // namespace facet::gpu
