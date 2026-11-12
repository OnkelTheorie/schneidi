// schneidi's own H.264 encoder (prototype): CAVLC tables and residual coding.
// Tables are transcribed from ITU-T H.264 tables 9-5, 9-7, 9-8, 9-9 and 9-10.
#include "Cavlc.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace schneidi::h264 {
namespace {

// coeff_token, index [table][TotalCoeff * 4 + TrailingOnes].
// Tables: 0 <= nC < 2, 2 <= nC < 4, 4 <= nC < 8, 8 <= nC.
const uint8_t kCoeffTokenLen[4][4 * 17] = {
    {
        1, 0, 0, 0,   6, 2, 0, 0,   8, 6, 3, 0,   9, 8, 7, 5,   10, 9, 8, 6,
        11, 10, 9, 7, 13, 11, 10, 8, 13, 13, 11, 9, 13, 13, 13, 10,
        14, 14, 13, 11, 14, 14, 14, 13, 15, 15, 14, 14, 15, 15, 15, 14,
        16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16, 16, 16, 16, 16,
    },
    {
        2, 0, 0, 0,   6, 2, 0, 0,   6, 5, 3, 0,   7, 6, 6, 4,   8, 6, 6, 4,
        8, 7, 7, 5,   9, 8, 8, 6,   11, 9, 9, 6,  11, 11, 11, 7,
        12, 11, 11, 9, 12, 12, 12, 11, 12, 12, 12, 11, 13, 13, 13, 12,
        13, 13, 13, 13, 13, 14, 13, 13, 14, 14, 14, 13, 14, 14, 14, 14,
    },
    {
        4, 0, 0, 0,   6, 4, 0, 0,   6, 5, 4, 0,   6, 5, 5, 4,   7, 5, 5, 4,
        7, 5, 5, 4,   7, 6, 6, 4,   7, 6, 6, 4,   8, 7, 7, 5,
        8, 8, 7, 6,   9, 8, 8, 7,   9, 9, 8, 8,   9, 9, 9, 8,
        10, 9, 9, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    },
    {
        6, 0, 0, 0,   6, 6, 0, 0,   6, 6, 6, 0,   6, 6, 6, 6,   6, 6, 6, 6,
        6, 6, 6, 6,   6, 6, 6, 6,   6, 6, 6, 6,   6, 6, 6, 6,
        6, 6, 6, 6,   6, 6, 6, 6,   6, 6, 6, 6,   6, 6, 6, 6,
        6, 6, 6, 6,   6, 6, 6, 6,   6, 6, 6, 6,   6, 6, 6, 6,
    },
};
const uint8_t kCoeffTokenBits[4][4 * 17] = {
    {
        1, 0, 0, 0,   5, 1, 0, 0,   7, 4, 1, 0,   7, 6, 5, 3,   7, 6, 5, 3,
        7, 6, 5, 4,   15, 6, 5, 4,  11, 14, 5, 4, 8, 10, 13, 4,
        15, 14, 9, 4, 11, 10, 13, 12, 15, 14, 9, 12, 11, 10, 13, 8,
        15, 1, 9, 12, 11, 14, 13, 8, 7, 10, 9, 12, 4, 6, 5, 8,
    },
    {
        3, 0, 0, 0,   11, 2, 0, 0,  7, 7, 3, 0,   7, 10, 9, 5,  7, 6, 5, 4,
        4, 6, 5, 6,   7, 6, 5, 8,   15, 6, 5, 4,  11, 14, 13, 4,
        15, 10, 9, 4, 11, 14, 13, 12, 8, 10, 9, 8, 15, 14, 13, 12,
        11, 10, 9, 12, 7, 11, 6, 8, 9, 8, 10, 1, 7, 6, 5, 4,
    },
    {
        15, 0, 0, 0,  15, 14, 0, 0, 11, 15, 13, 0, 8, 12, 14, 12, 15, 10, 11, 11,
        11, 8, 9, 10, 9, 14, 13, 9, 8, 10, 9, 8,  15, 14, 13, 13,
        11, 14, 10, 12, 15, 10, 13, 12, 11, 14, 9, 12, 8, 10, 13, 8,
        13, 7, 9, 12, 9, 12, 11, 10, 5, 8, 7, 6, 1, 4, 3, 2,
    },
    {
        3, 0, 0, 0,   0, 1, 0, 0,   4, 5, 6, 0,   8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    },
};

// Chroma DC (nC == -1), index [TotalCoeff * 4 + TrailingOnes].
const uint8_t kChromaDcTokenLen[4 * 5] = {
    2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6, 6, 8, 8, 7,
};
const uint8_t kChromaDcTokenBits[4 * 5] = {
    1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5, 2, 3, 2, 0,
};

// total_zeros for 4x4 blocks, index [TotalCoeff - 1][total_zeros].
const uint8_t kTotalZerosLen[15][16] = {
    {1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9},
    {3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6},
    {4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6},
    {5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5},
    {4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5},
    {6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6},
    {6, 5, 3, 3, 3, 2, 3, 4, 3, 6},
    {6, 4, 5, 3, 2, 2, 3, 3, 6},
    {6, 6, 4, 2, 2, 3, 2, 5},
    {5, 5, 3, 2, 2, 2, 4},
    {4, 4, 3, 3, 1, 3},
    {4, 4, 2, 1, 3},
    {3, 3, 1, 2},
    {2, 2, 1},
    {1, 1},
};
const uint8_t kTotalZerosBits[15][16] = {
    {1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1},
    {7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0},
    {5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0},
    {3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0},
    {5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0},
    {1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0},
    {1, 1, 5, 4, 3, 3, 2, 1, 1, 0},
    {1, 1, 1, 3, 3, 2, 2, 1, 0},
    {1, 0, 1, 3, 2, 1, 1, 1},
    {1, 0, 1, 3, 2, 1, 1},
    {0, 1, 1, 2, 1, 3},
    {0, 1, 1, 1, 1},
    {0, 1, 1, 1},
    {0, 1, 1},
    {0, 1},
};

// total_zeros for chroma DC 2x2, index [TotalCoeff - 1][total_zeros].
const uint8_t kChromaDcTotalZerosLen[3][4] = {{1, 2, 3, 3}, {1, 2, 2, 0}, {1, 1, 0, 0}};
const uint8_t kChromaDcTotalZerosBits[3][4] = {{1, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}};

// run_before, index [min(zerosLeft, 7) - 1][run_before].
const uint8_t kRunLen[7][16] = {
    {1, 1},
    {1, 2, 2},
    {2, 2, 2, 2},
    {2, 2, 2, 3, 3},
    {2, 2, 3, 3, 3, 3},
    {2, 3, 3, 3, 3, 3, 3},
    {3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11},
};
const uint8_t kRunBits[7][16] = {
    {1, 0},
    {1, 1, 0},
    {3, 2, 1, 0},
    {3, 2, 1, 1, 0},
    {3, 2, 3, 2, 1, 0},
    {3, 0, 1, 3, 2, 5, 4},
    {7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1},
};

void writeLevel(BitWriter& bw, int levelCode, int suffixLength)
{
    if (suffixLength == 0) {
        if (levelCode < 14) {
            bw.u(1, levelCode + 1); // levelCode zeros then a one
        } else if (levelCode < 30) {
            bw.u(1, 15);
            bw.u(uint32_t(levelCode - 14), 4);
        } else {
            bw.u(1, 16);
            bw.u(uint32_t(levelCode - 30), 12);
        }
    } else {
        if (levelCode < (15 << suffixLength)) {
            bw.u(1, (levelCode >> suffixLength) + 1);
            bw.u(uint32_t(levelCode & ((1 << suffixLength) - 1)), suffixLength);
        } else {
            bw.u(1, 16);
            bw.u(uint32_t(levelCode - (15 << suffixLength)), 12);
        }
    }
}

} // namespace

int writeResidualBlock(BitWriter& bw, const int* coeffs, int count, int nC)
{
    // Non-zero coefficients from high to low frequency.
    int levels[16];
    int positions[16];
    int total = 0;
    for (int i = count - 1; i >= 0; --i) {
        if (coeffs[i] != 0) {
            levels[total] = coeffs[i];
            positions[total] = i;
            ++total;
        }
    }
    int trailingOnes = 0;
    while (trailingOnes < total && trailingOnes < 3 && std::abs(levels[trailingOnes]) == 1)
        ++trailingOnes;

    const int tokenIdx = total * 4 + trailingOnes;
    if (nC == -1) {
        bw.u(kChromaDcTokenBits[tokenIdx], kChromaDcTokenLen[tokenIdx]);
    } else {
        const int table = nC < 2 ? 0 : nC < 4 ? 1 : nC < 8 ? 2 : 3;
        bw.u(kCoeffTokenBits[table][tokenIdx], kCoeffTokenLen[table][tokenIdx]);
    }
    if (total == 0)
        return 0;

    for (int i = 0; i < trailingOnes; ++i)
        bw.flag(levels[i] < 0);

    int suffixLength = (total > 10 && trailingOnes < 3) ? 1 : 0;
    for (int i = trailingOnes; i < total; ++i) {
        const int level = levels[i];
        int levelCode = level > 0 ? 2 * level - 2 : -2 * level - 1;
        if (i == trailingOnes && trailingOnes < 3)
            levelCode -= 2;
        writeLevel(bw, levelCode, suffixLength);
        if (suffixLength == 0)
            suffixLength = 1;
        if (std::abs(level) > (3 << (suffixLength - 1)) && suffixLength < 6)
            ++suffixLength;
    }

    const int totalZeros = positions[0] + 1 - total; // zeros before the highest non-zero
    if (total < count) {
        if (nC == -1)
            bw.u(kChromaDcTotalZerosBits[total - 1][totalZeros], kChromaDcTotalZerosLen[total - 1][totalZeros]);
        else
            bw.u(kTotalZerosBits[total - 1][totalZeros], kTotalZerosLen[total - 1][totalZeros]);
    }

    int zerosLeft = totalZeros;
    for (int i = 0; i < total - 1 && zerosLeft > 0; ++i) {
        const int run = positions[i] - positions[i + 1] - 1;
        const int t = (zerosLeft > 7 ? 7 : zerosLeft) - 1;
        bw.u(kRunBits[t][run], kRunLen[t][run]);
        zerosLeft -= run;
    }
    return total;
}

namespace {

struct Code {
    uint32_t bits;
    int len;
};

bool checkCodeSet(const std::vector<Code>& codes, const std::string& name, bool expectComplete)
{
    double kraft = 0;
    for (size_t a = 0; a < codes.size(); ++a) {
        kraft += 1.0 / double(uint64_t(1) << codes[a].len);
        for (size_t b = 0; b < codes.size(); ++b) {
            if (a == b)
                continue;
            const Code& s = codes[a];
            const Code& l = codes[b];
            if (s.len <= l.len && (l.bits >> (l.len - s.len)) == s.bits) {
                std::fprintf(stderr, "%s: code %zu is a prefix of code %zu\n", name.c_str(), a, b);
                return false;
            }
        }
    }
    if (kraft > 1.0 + 1e-12 || (expectComplete && kraft < 1.0 - 1e-12)) {
        std::fprintf(stderr, "%s: Kraft sum %.6f\n", name.c_str(), kraft);
        return false;
    }
    return true;
}

} // namespace

bool selfTestTables()
{
    bool ok = true;
    for (int t = 0; t < 4; ++t) {
        std::vector<Code> codes;
        for (int tc = 0; tc <= 16; ++tc)
            for (int t1 = 0; t1 <= 3 && t1 <= tc; ++t1)
                codes.push_back({kCoeffTokenBits[t][tc * 4 + t1], kCoeffTokenLen[t][tc * 4 + t1]});
        ok &= checkCodeSet(codes, "coeff_token table " + std::to_string(t), false);
    }
    {
        std::vector<Code> codes;
        for (int tc = 0; tc <= 4; ++tc)
            for (int t1 = 0; t1 <= 3 && t1 <= tc; ++t1)
                codes.push_back({kChromaDcTokenBits[tc * 4 + t1], kChromaDcTokenLen[tc * 4 + t1]});
        ok &= checkCodeSet(codes, "chroma DC coeff_token", false);
    }
    for (int tc = 1; tc <= 15; ++tc) {
        std::vector<Code> codes;
        for (int tz = 0; tz <= 16 - tc; ++tz)
            codes.push_back({kTotalZerosBits[tc - 1][tz], kTotalZerosLen[tc - 1][tz]});
        // tc = 1 leaves the all-zero 9-bit code unused on purpose.
        ok &= checkCodeSet(codes, "total_zeros tc=" + std::to_string(tc), tc > 1);
    }
    for (int tc = 1; tc <= 3; ++tc) {
        std::vector<Code> codes;
        for (int tz = 0; tz <= 4 - tc; ++tz)
            codes.push_back({kChromaDcTotalZerosBits[tc - 1][tz], kChromaDcTotalZerosLen[tc - 1][tz]});
        ok &= checkCodeSet(codes, "chroma DC total_zeros tc=" + std::to_string(tc), true);
    }
    for (int z = 1; z <= 7; ++z) {
        std::vector<Code> codes;
        const int maxRun = z < 7 ? z : 14;
        for (int r = 0; r <= maxRun; ++r)
            codes.push_back({kRunBits[z - 1][r], kRunLen[z - 1][r]});
        ok &= checkCodeSet(codes, "run_before zl=" + std::to_string(z), z < 7);
    }
    return ok;
}

} // namespace schneidi::h264
