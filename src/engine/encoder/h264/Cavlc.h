// schneidi's own H.264 encoder (prototype): CAVLC residual coding (ITU-T H.264 clause 9.2).
#pragma once

#include "BitWriter.h"

namespace schneidi::h264 {

// Writes residual_block_cavlc for `count` coefficients in scan order.
// nC: predicted non-zero count (-1 = chroma DC 4:2:0). Returns TotalCoeff.
int writeResidualBlock(BitWriter& bw, const int* coeffs, int count, int nC);

// Largest level magnitude the encoder may produce (level_prefix stays <= 15,
// as Baseline/Main require).
constexpr int kMaxLevel = 2063;

// Checks the VLC tables (prefix-free, Kraft sum). Returns false and prints on error.
bool selfTestTables();

} // namespace schneidi::h264
