#include "smix/FFT.h"

#include <cmath>
#include <utility>

namespace smix
{

FFT::FFT (int order) : n (1 << order), bitReverse (static_cast<size_t> (n)), twiddles (static_cast<size_t> (n / 2))
{
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < order; ++b)
            if (i & (1 << b))
                r |= 1 << (order - 1 - b);
        bitReverse[static_cast<size_t> (i)] = r;
    }

    const double twoPi = 6.283185307179586;
    for (int k = 0; k < n / 2; ++k)
        twiddles[static_cast<size_t> (k)] = std::polar (1.0f, static_cast<float> (-twoPi * k / n));
}

void FFT::perform (std::complex<float>* data) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitReverse[static_cast<size_t> (i)];
        if (j > i)
            std::swap (data[i], data[j]);
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2;
        const int step = n / len;
        for (int start = 0; start < n; start += len)
        {
            for (int k = 0; k < half; ++k)
            {
                const auto w = twiddles[static_cast<size_t> (k * step)];
                const auto u = data[start + k];
                const auto v = data[start + k + half] * w;
                data[start + k] = u + v;
                data[start + k + half] = u - v;
            }
        }
    }
}

} // namespace smix
