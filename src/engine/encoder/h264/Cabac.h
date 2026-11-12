// schneidi's own H.264 encoder: CABAC arithmetic coding engine (ITU-T H.264 clause 9.3.4).
#pragma once

#include "BitWriter.h"
#include "CabacTables.h"

#include <cmath>
#include <cstdint>

namespace schneidi::h264 {

class CabacEncoder {
public:
    // Starts the arithmetic coder on a byte-aligned writer (after cabac_alignment_one_bit) and
    // initialises all contexts for the slice (9.3.1.1). cabacInitIdc < 0 = I slice.
    void start(BitWriter* out, int sliceQp, int cabacInitIdc)
    {
        out_ = out;
        low_ = 0;
        range_ = 510;
        firstBit_ = true;
        outstanding_ = 0;
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
            low_ += range_;
            range_ = lps;
        }
        renorm();
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
        if (low_ >= 1024) {
            putBit(1);
            low_ -= 1024;
        } else if (low_ < 512) {
            putBit(0);
        } else {
            low_ -= 512;
            ++outstanding_;
        }
    }

    // EncodeTerminate (9.3.4.5); bin 1 also flushes (the last bit written is the rbsp_stop_one_bit).
    void terminate(int bin)
    {
        if (counting_) {
            cost_ += bin ? 7 * 256 : 0;
            return;
        }
        range_ -= 2;
        if (bin) {
            low_ += range_;
            range_ = 2;
            renorm();
            putBit((low_ >> 9) & 1);
            out_->u(((low_ >> 7) & 3) | 1, 2);
        } else {
            renorm();
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

    void renorm()
    {
        while (range_ < 256) {
            if (low_ < 256) {
                putBit(0);
            } else if (low_ >= 512) {
                low_ -= 512;
                putBit(1);
            } else {
                low_ -= 256;
                ++outstanding_;
            }
            range_ <<= 1;
            low_ <<= 1;
        }
    }
    void putBit(int b)
    {
        if (firstBit_)
            firstBit_ = false;
        else
            out_->u(uint32_t(b), 1);
        while (outstanding_ > 0) {
            out_->u(uint32_t(1 - b), 1);
            --outstanding_;
        }
    }

    bool counting_ = false;
    uint64_t cost_ = 0;
    BitWriter* out_ = nullptr;
    uint32_t low_ = 0, range_ = 510;
    bool firstBit_ = true;
    int outstanding_ = 0;
    uint8_t ctx_[kCabacContexts] = {}; // state << 1 | MPS
};

} // namespace schneidi::h264
