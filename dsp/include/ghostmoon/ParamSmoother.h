// SPDX-License-Identifier: MIT
// ParamSmoother.h — One-pole exponential parameter smoother
// Header-only, no dependencies beyond <cmath>, <algorithm>, <type_traits>.
//
// Usage:
//   gm::ParamSmoother<float> smoother(0.02f);  // 20ms time constant
//   smoother.prepare(sampleRate);
//   for each sample: float out = smoother.process(target);
//   smoother.snapTo(value);  // instant jump (e.g. on preset load)

#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace gm {

/// One-pole exponential smoother for parameter interpolation.
/// Prevents zipper noise when automating knobs, sliders, etc.
/// Templated on float or double (enforced by static_assert).
///
/// `tau` is an RC time constant, not a ramp duration: the smoother reaches
/// `1 - 1/e ≈ 63%` of a step change after `tau` seconds, and asymptotically
/// approaches the target thereafter (never reaching it exactly — see the
/// snap-to-target guard in `process()` for how we handle the subnormal
/// floor). Callers expecting a fixed-duration ramp should use JUCE's
/// `SmoothedValue<T, Linear>` instead.
///
/// Thread safety: `process()` is audio-thread only. `prepare()`,
/// `setTimeConstant()`, and `snapTo()` must NOT be called concurrently
/// with `process()` — they are control-thread only. No internal locking;
/// caller is responsible for the thread ownership contract.
template <typename T = float>
class ParamSmoother {
    static_assert(std::is_floating_point_v<T>,
        "gm::ParamSmoother<T> requires float or double");

public:
    /// Construct with smoothing time in seconds (default 20ms).
    /// Negative values are clamped to 0 (which gives passthrough).
    explicit ParamSmoother(T timeConstantSeconds = static_cast<T>(0.02))
        : tau_(std::max(static_cast<T>(0), timeConstantSeconds)) {}

    /// Call once with the host sample rate. Not RT-safe (calls std::exp
    /// via recalcCoeff). Calling with a new sampleRate mid-stream
    /// recomputes the coefficient — legal but non-RT-safe.
    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        recalcCoeff();
    }

    /// Set smoothing time constant in seconds.
    /// Control-thread only: calls std::exp. Do not invoke from process().
    /// Negative values clamp to 0. Safe to call before prepare(); the
    /// coefficient will be recomputed when prepare() runs.
    void setTimeConstant(T seconds) {
        tau_ = std::max(static_cast<T>(0), seconds);
        recalcCoeff();
    }

    /// Current time constant in seconds.
    T getTimeConstant() const noexcept { return tau_; }

    /// Process one sample: smoothly approach target.
    /// Passthrough (returns target directly) until prepare() is called —
    /// the smoother cannot compute a coefficient without a sample rate.
    ///
    /// NaN/Inf guard: if target is not finite (NaN or Inf from a misbehaving
    /// automation source), the call is a no-op — `current_` is preserved and
    /// returned. This prevents a single bad parameter value from permanently
    /// contaminating the smoother state (NaN is sticky under arithmetic).
    ///
    /// Snap threshold (ME-1) vs isSmoothing epsilon: the snap guard fires
    /// when `|current - target| < 1e-7`, hard-jumping current to target.
    /// That is 10× below the default `isSmoothing` epsilon of 1e-6, so for a
    /// caller checking `isSmoothing(target)` with defaults, the snap fires
    /// after `isSmoothing` would already report settled — no observable edge.
    /// Callers using a custom epsilon below 1e-7 may see an artificial
    /// "hard" settle where pure exponential decay would continue indefinitely.
    T process(T target) noexcept {
        if (!std::isfinite(target))
            return current_;                           // NaN/Inf: no state update
        current_ = target + coeff_ * (current_ - target);
        // ME-1: snap to target when the residual enters subnormal range.
        // Without this guard, idle voices with target=0 pay a 10-100× microcode
        // penalty per op once current_ drops into IEEE subnormals (~40s at
        // 20ms tau, 44.1 kHz).
        if (std::abs(current_ - target) < static_cast<T>(1e-7))
            current_ = target;
        return current_;
    }

    /// Instantly set current value (no ramp). Use on preset/program change.
    void snapTo(T value) noexcept {
        current_ = value;
    }

    /// Reset the smoother to `value` (default 0). Alias for snapTo() to
    /// match JUCE/Signalsmith naming conventions.
    void reset(T value = T{}) noexcept {
        current_ = value;
    }

    /// Get current smoothed value without advancing state.
    T getCurrentValue() const noexcept { return current_; }

    /// True if |current - target| > epsilon. Epsilon units match the
    /// parameter's units: the default `1e-6` suits normalized [0, 1]
    /// parameters. For Hz (e.g. filter cutoff in [20, 20000]), pass ~0.01.
    /// For dB, ~0.001. For raw integer-like values, scale accordingly.
    bool isSmoothing(T target, T epsilon = static_cast<T>(1e-6)) const noexcept {
        return std::abs(current_ - target) > epsilon;
    }

private:
    void recalcCoeff() {
        // LO-2: compute in double to preserve precision for long tau
        // (e.g., tau=10s at sr=44100 → exponent = -1/441000 ≈ -2.27e-6).
        if (sampleRate_ > 0.0 && tau_ > static_cast<T>(0)) {
            const double exponent = -1.0 / (static_cast<double>(tau_) * sampleRate_);
            coeff_ = static_cast<T>(std::exp(exponent));
        } else {
            coeff_ = static_cast<T>(0); // no smoothing (passthrough)
        }
    }

    T tau_        = static_cast<T>(0.02);
    T coeff_      = static_cast<T>(0);
    T current_    = static_cast<T>(0);
    double sampleRate_ = 0.0;
};

} // namespace gm
