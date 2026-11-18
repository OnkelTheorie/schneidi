// schneidi's own H.264 encoder: CABAC arithmetic coding engine (ITU-T H.264 clause 9.3.4).
#pragma once

#include "BitWriter.h"
#include "CabacTables.h"

#include <bit>
#include <cmath>
#include <cstdint>

namespace schneidi::h264 {

class CabacCounter;

class CabacEncoder {
    friend class CabacCounter;

public:
    // Starts the arithmetic coder on a byte-aligned writer (after cabac_alignment_one_bit) and
    // initialises all contexts for the slice (9.3.1.1). cabacInitIdc < 0 = I slice.
    void start(BitWriter* out, int sliceQp, int cabacInitIdc)
    {
        out_ = out;
        low_ = 0;
        range_ = 510;
        free_ = 23;
        heldByte_ = 0xff;
        held_ = 0;
        const int qp = sliceQp < 0 ? 0 : sliceQp > 51 ? 51 : sliceQp;
        for (int i = 0; i < kCabacContexts; ++i) {
            const int8_t* mn = cabacInitIdc < 0 ? kCabacInitI[i] : kCabacInitP[cabacInitIdc][i];
            int pre = ((mn[0] * qp) >> 4) + mn[1];
            pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
            ctx_[i] = pre <= 63 ? uint8_t((63 - pre) << 1) : uint8_t(((pre - 64) << 1) | 1);
        }
    }

    // Turns this (copied) engine into a bit counter: no output, contexts keep adapting.
    void startCounting()
    {
        counting_ = true;
        cost_ = 0;
    }
    double countedBits() const { return cost_ / 256.0; }
    // Cost of one bin in the current state, in 1/256 bits (no adaptation).
    uint32_t binCost(int ctx, int bin) const { return kFast.cost[ctx_[ctx]][bin]; }

    // EncodeDecision (9.3.4.2).
    void decision(int ctx, int bin)
    {
        const uint8_t c = ctx_[ctx];
        ctx_[ctx] = kFast.next[c][bin];
        if (counting_) {
            cost_ += kFast.cost[c][bin];
            return;
        }
        const uint32_t lps = kRangeTabLps[c >> 1][(range_ >> 6) & 3];
        range_ -= lps;
        if (bin != (c & 1)) {
            const int shift = std::countl_zero(lps) - 23; // renormalisation: until the range is 256 or more
            low_ = (low_ + range_) << shift;
            range_ = lps << shift;
            free_ -= shift;
        } else {
            if (range_ >= 256)
                return;
            low_ <<= 1; // the range after a most probable symbol is never below 128
            range_ <<= 1;
            --free_;
        }
        if (free_ < 12)
            flushByte();
    }

    // EncodeBypass (9.3.4.4).
    void bypass(int bin)
    {
        if (counting_) {
            cost_ += 256;
            return;
        }
        low_ <<= 1;
        if (bin)
            low_ += range_;
        if (--free_ < 12)
            flushByte();
    }

    // EncodeTerminate (9.3.4.5); bin 1 also flushes and writes the rbsp_stop_one_bit.
    void terminate(int bin)
    {
        if (counting_) {
            cost_ += bin ? 7 * 256 : 0;
            return;
        }
        range_ -= 2;
        if (bin) {
            low_ = (low_ + range_) << 7;
            range_ = 2 << 7;
            free_ -= 7;
            if (free_ < 12)
                flushByte();
            finish();
            out_->u(1, 1);
        } else if (range_ < 256) {
            low_ <<= 1;
            range_ <<= 1;
            if (--free_ < 12)
                flushByte();
        }
    }

private:
    // Context = state << 1 | MPS. Per context and bin: the next context (9.3.3.2.2) and the cost
    // in 1/256 bits from pLPS(s) ~= 0.5 * (0.01875 / 0.5)^(s / 63) (the design rule of 9.3.3.2).
    struct FastTables {
        uint8_t next[128][2];
        uint32_t cost[128][2];
        FastTables()
        {
            for (int s = 0; s < 64; ++s) {
                const double lps = 0.5 * std::pow(0.01875 / 0.5, s / 63.0);
                const uint32_t costLps = uint32_t(std::lround(-std::log2(lps) * 256));
                const uint32_t costMps = uint32_t(std::lround(-std::log2(1 - lps) * 256));
                for (int m = 0; m < 2; ++m) {
                    const int c = s << 1 | m;
                    for (int bin = 0; bin < 2; ++bin) {
                        if (bin == m) {
                            next[c][bin] = uint8_t(kTransIdxMps[s] << 1 | m);
                            cost[c][bin] = costMps;
                        } else {
                            next[c][bin] = uint8_t(kTransIdxLps[s] << 1 | (s == 0 ? 1 - m : m));
                            cost[c][bin] = costLps;
                        }
                    }
                }
            }
        }
    };
    static inline const FastTables kFast{};

    // The interval's lower end is kept with its settled bits above the nine working bits of the specification's
    // codILow (free_ = bits of the 32 still unused). Whole bytes leave from the top; a byte may still change by a
    // carry from below, so the last byte stays back, with a count of the 0xff bytes after it (they turn to 0x00
    // when the carry comes). The bits are the ones PutBit of 9.3.4.2 would write one at a time.
    void flushByte()
    {
        const uint32_t lead = low_ >> (24 - free_);
        free_ += 8;
        low_ &= 0xffffffffu >> free_;
        if (lead == 0xff) {
            ++held_;
        } else if (held_ > 0) {
            const uint32_t carry = lead >> 8;
            out_->byte(uint8_t(heldByte_ + carry));
            heldByte_ = lead & 0xff;
            const uint8_t fill = uint8_t(0xff + carry);
            for (; held_ > 1; --held_)
                out_->byte(fill);
        } else {
            held_ = 1;
            heldByte_ = lead;
        }
    }
    // The end of the slice data: the bytes held back and the rest of the settled bits (down to bit 8 of codILow;
    // the bit after it is the rbsp_stop_one_bit).
    void finish()
    {
        if (low_ >> (32 - free_)) {
            out_->byte(uint8_t(heldByte_ + 1));
            for (; held_ > 1; --held_)
                out_->byte(0x00);
            low_ -= uint32_t(1) << (32 - free_);
        } else {
            if (held_ > 0)
                out_->byte(uint8_t(heldByte_));
            for (; held_ > 1; --held_)
                out_->byte(0xff);
        }
        out_->u(low_ >> 8, 24 - free_);
    }

    bool counting_ = false;
    uint64_t cost_ = 0;
    BitWriter* out_ = nullptr;
    uint32_t low_ = 0, range_ = 510;
    int free_ = 23;                // unused bits of low_ (the first bit PutBit would write is dropped: 9.3.4.2)
    uint32_t heldByte_ = 0xff;     // the last whole byte, not written yet
    int held_ = 0;                 // that byte and the 0xff bytes after it
    uint8_t ctx_[kCabacContexts] = {}; // state << 1 | MPS
};

// Bit counter for the mode decision: a copy of an encoder's contexts that adapts like it and sums
// the bins' costs (the encoder's counting mode without a test per bin and without its coder state).
class CabacCounter {
public:
    explicit CabacCounter(const CabacEncoder& from)
    {
        for (int i = 0; i < kCabacContexts; ++i)
            ctx_[i] = from.ctx_[i];
    }
    void decision(int ctx, int bin)
    {
        const uint8_t c = ctx_[ctx];
        ctx_[ctx] = CabacEncoder::kFast.next[c][bin];
        cost_ += CabacEncoder::kFast.cost[c][bin];
    }
    void bypass(int) { cost_ += 256; }
    void terminate(int bin) { cost_ += bin ? 7 * 256 : 0; }
    uint32_t binCost(int ctx, int bin) const { return CabacEncoder::kFast.cost[ctx_[ctx]][bin]; }
    double countedBits() const { return cost_ / 256.0; }

private:
    uint64_t cost_ = 0;
    uint8_t ctx_[kCabacContexts];
};

} // namespace schneidi::h264
