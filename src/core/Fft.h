// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// In-place radix-2 FFT (complex, double), shared by the denoiser (engine/Denoise) and the audio sync (core/AudioSync)

#include <cstddef>
#include <vector>

namespace Fft {

// re/im: same size, a power of two; inverse without the 1/n scaling
void transform(std::vector<double>& re, std::vector<double>& im, bool inverse);
// Smallest power of two >= n (at least 1)
std::size_t size(std::size_t n);

} // namespace Fft
