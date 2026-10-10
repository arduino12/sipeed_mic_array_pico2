#pragma once
// In-place iterative radix-2 complex FFT, forward (X[k] = sum x[n] e^{-j2pi kn/N}).

namespace fft {

constexpr int N = 512;
constexpr int LOG2N = 9;

void init();
void forward(float *re, float *im);

}  // namespace fft
