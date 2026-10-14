#include "core/Fft.h"

#include <cmath>
#include <numbers>
#include <utility>

namespace Fft {

void transform(std::vector<double>& re, std::vector<double>& im, bool inverse)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2 * std::numbers::pi / double(len) * (inverse ? 1 : -1);
        const double wr = std::cos(angle), wi = std::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k, b = i + k + len / 2;
                const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

size_t size(size_t n)
{
    size_t s = 1;
    while (s < n) s <<= 1;
    return s;
}

} // namespace Fft
