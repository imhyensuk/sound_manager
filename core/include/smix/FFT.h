#pragma once

#include <complex>
#include <vector>

namespace smix
{

/** Minimal in-place radix-2 complex FFT. All allocation happens in the constructor. */
class FFT
{
public:
    explicit FFT (int order);

    int size() const noexcept { return n; }

    /** Forward transform, in place. data.size() must equal size(). */
    void perform (std::complex<float>* data) const noexcept;

private:
    int n;
    std::vector<int> bitReverse;
    std::vector<std::complex<float>> twiddles;
};

} // namespace smix
