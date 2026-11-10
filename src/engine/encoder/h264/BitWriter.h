// schneidi's own H.264 encoder (prototype): bit writer and NAL packing.
// Pure C++20, no external libraries.
#pragma once

#include <cstdint>
#include <vector>

namespace schneidi::h264 {

class BitWriter {
public:
    // Writes the lowest `bits` bits of `value`, most significant first (bits <= 32).
    void u(uint32_t value, int bits)
    {
        if (bits <= 0)
            return;
        acc_ = (acc_ << bits) | (uint64_t(value) & ((uint64_t(1) << bits) - 1));
        pending_ += bits;
        while (pending_ >= 8) {
            pending_ -= 8;
            bytes_.push_back(uint8_t(acc_ >> pending_));
        }
    }
    void flag(bool b) { u(b ? 1u : 0u, 1); }

    // Unsigned Exp-Golomb.
    void ue(uint32_t v)
    {
        const uint64_t x = uint64_t(v) + 1;
        int len = 0;
        while ((x >> len) > 1)
            ++len;
        u(0, len);
        u(uint32_t(x), len + 1);
    }
    // Signed Exp-Golomb.
    void se(int32_t v) { ue(v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * int64_t(v))); }

    bool byteAligned() const { return pending_ == 0; }
    void alignZero()
    {
        if (pending_)
            u(0, 8 - pending_);
    }
    void rbspTrailing()
    {
        u(1, 1);
        alignZero();
    }
    size_t bitCount() const { return bytes_.size() * 8 + size_t(pending_); }

    // Only valid when byte aligned.
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    uint64_t acc_ = 0;
    int pending_ = 0;
};

// Appends an Annex B NAL unit (start code, header, emulation-prevented payload).
inline void appendNal(std::vector<uint8_t>& out, int nalRefIdc, int nalType, const std::vector<uint8_t>& rbsp)
{
    out.insert(out.end(), {0, 0, 0, 1});
    out.push_back(uint8_t((nalRefIdc << 5) | nalType));
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
}

} // namespace schneidi::h264
