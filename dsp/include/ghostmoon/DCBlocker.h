// SPDX-License-Identifier: MIT
// DCBlocker.h -- First-order DC blocking filter
// Essential after nonlinear stages (MoogLadder, Saturators, waveshaping).
//
// Header-only, zero external dependencies beyond <cmath> and <algorithm>.

#pragma once

#include <algorithm>
#include <cmath>

namespace gm {

/// First-order DC blocking highpass filter.
/// Removes sub-audible DC offset that accumulates after nonlinear processing.
///
/// Difference equation: y[n] = x[n] - x[n-1] + R * y[n-1]
/// Transfer function:   H(z) = (1 - z⁻¹) / (1 - R·z⁻¹) — zero at DC, pole at R.
///
/// `R ≈ 1 - 2π·fc/sr` (first-order approximation of exp(-2π·fc/sr); accurate
/// to <1 ppm for fc/sr < 0.001, i.e. fc ≤ ~40 Hz at 44.1 kHz).
///
/// `prepare()` resets filter state — any in-progress transient is discarded.
/// NaN/Inf input triggers an automatic reset and returns 0, so a single bad
/// upstream sample cannot permanently contaminate the state.
class DCBlocker {
public:
  /// Configure for the given sample rate and cutoff frequency.
  /// ME-1: sampleRate ≤ 0 is a no-op (would produce R=±inf and NaN cascade).
  /// ME-5: cutoffHz is a tunable default (5 Hz) for consumer flexibility.
  /// LO-4: state is reset unconditionally; in-progress transients are lost.
  void prepare(double sampleRate, double cutoffHz = 5.0) {
    if (sampleRate <= 0.0) return;  // keep current R_ and state
    R_ = 1.0 - (6.283185307179586 * cutoffHz / sampleRate);
    // Clamp to stable range. Upper bound 0.9999 keeps the pole safely inside
    // the unit circle even if a caller passes fc very close to 0.
    R_ = std::clamp(R_, 0.0, 0.9999);
    reset();
  }

  double process(double in) {
    // ME-2: a NaN or Inf input (from a blown nonlinear stage) would
    // otherwise permanently poison xPrev_ and yPrev_. Flush state and
    // report silence; recovery begins on the next finite sample.
    if (!std::isfinite(in)) {
      reset();
      return 0.0;
    }
    double out = in - xPrev_ + R_ * yPrev_;
    xPrev_ = in;
    yPrev_ = out;
    // ME-3: snap yPrev_ to zero when it enters subnormal range during
    // sustained silence. On x86 without FTZ/DAZ, subnormal arithmetic
    // is ~100× slower per op. xPrev_ tracks the input so it is zero
    // when the input is zero — no snap needed there.
    if (std::abs(yPrev_) < 1e-15) yPrev_ = 0.0;
    return out;
  }

  float process(float in) {
    return static_cast<float>(process(static_cast<double>(in)));
  }

  // LO-3: guard null/non-positive count.
  // LO-8: inline the double-precision body to avoid paying two casts
  // and two function calls per sample.
  void processBlock(float* buf, int numSamples) {
    if (!buf || numSamples <= 0) return;
    for (int i = 0; i < numSamples; ++i) {
      double in  = static_cast<double>(buf[i]);
      if (!std::isfinite(in)) {
        reset();
        buf[i] = 0.0f;
        continue;
      }
      double out = in - xPrev_ + R_ * yPrev_;
      xPrev_ = in;
      yPrev_ = out;
      if (std::abs(yPrev_) < 1e-15) yPrev_ = 0.0;
      buf[i] = static_cast<float>(out);
    }
  }

  // LO-2: double-precision block overload for double pipelines.
  void processBlock(double* buf, int numSamples) {
    if (!buf || numSamples <= 0) return;
    for (int i = 0; i < numSamples; ++i)
      buf[i] = process(buf[i]);
  }

  void reset() {
    xPrev_ = 0.0;
    yPrev_ = 0.0;
  }

  /// Current pole coefficient R. Exposed for test pinning / diagnostics.
  double getR() const { return R_; }

private:
  // LO-5: default R_ derived from the formula at 44.1 kHz / 5 Hz (previously
  // hardcoded 0.9993, which was a 7 ppm rounding of the exact value).
  double R_ = 1.0 - (6.283185307179586 * 5.0 / 44100.0);
  double xPrev_ = 0.0;
  double yPrev_ = 0.0;
};

} // namespace gm
