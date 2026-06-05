// SPDX-License-Identifier: MIT
// SafetyLimiter.h — 8-stage audio safety chain
//
// Revision 4 — audit fixes: DC detection moved pre-HPF (HI-6), panic() no longer
// self-unmutes (CR-2), per-sample atomic loads eliminated (HI-2), denormal bias
// added to all EMAs (HI-3), hazardous-sample leak at trigger closed (HI-4),
// threshold setters made atomic to fix data race (HI-5), dcOffsetCounter reset on
// recovery (HI-7), memory ordering on pendingNotification_ corrected (HI-1),
// dangerEventCount incremented inside triggerPermanentMute (ME-6), threshold
// setters clamped (ME-4), UI atomics use relaxed ordering (ME-7), redundant peak
// recomputation removed (ME-8), missing API hooks restored (CR-1), sample-rate
// validation added (ME-1), State enum made public (ME-10).
//
// Stage order:
//   0  NaN/Inf protection       → zero + permanent mute immediately
//   1  DC offset detection       → signed EMA on raw input; mute on sustained DC
//   2  DC Blocker (HPF 20 Hz)   → remove DC before downstream stages
//   3  Ultrasonic LPF 18 kHz    → remove ultrasonic content before detection
//   4  Sustained peak detection  → tiered mute (soft-first, permanent on repeat)
//   5  Sustained loudness riding → backstop gain riding
//   6  Soft-mute ramp            → smooth transition during tiered mute states
//   7  Hard clip                 → final brickwall guarantee
//
// Tiered mute behaviour:
//   First sustained-peak trip  → ramp gain to 0 over 500ms, hold 2s, auto-recover once.
//   Second trip (same session) → permanent mute.
//   NaN/Inf or DC offset       → permanent mute immediately.
//
// Permanent mute requires explicit unlockPermanentMute() to clear.
//
// Header-only, zero external dependencies.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace gm {

class SafetyLimiter {
public:
  // Public enums ─────────────────────────────────────────────────────────────
  enum class State        { Normal = 0, SoftMuting = 1, AutoRecovering = 2, PermanentMuted = 3 };
  enum class MuteReason   { None, SustainedPeak, DCOffset, NaNInf };

  SafetyLimiter() = default;

  void prepare(double sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
      sampleRate = 44100.0;
    // Clamp to realistic audio range
    sampleRate = std::max(kSampleRateMin, std::min(kSampleRateMax, sampleRate));
    sampleRate_ = sampleRate;

    sustainedPeakCoeff_            = std::exp(-1.0  / (kSustainedPeakEmaSec  * sampleRate));
    sustainedPeakThresholdSamples_ = static_cast<int>(kSustainedPeakWindowSec * sampleRate);

    // DC detection: 2 Hz EMA, operates on raw (pre-HPF) signal.
    // Sine waves have zero mean so this does not false-positive on LF audio.
    dcEmaCoeff_               = std::exp(-kTwoPi * kDcEmaFreqHz / sampleRate);
    dcOffsetThresholdSamples_ = static_cast<int>(kDcWindowSec * sampleRate);

    sustainedCoeff_ = std::exp(-1.0 / (kSustainedCoeffSec * sampleRate));

    float hpfFreq = std::min(kHpfFreqHz, static_cast<float>(sampleRate) * 0.49f);
    calcHighpass(hpfFreq,   sampleRate, hpfB0_, hpfB1_, hpfB2_, hpfA1_, hpfA2_);
    float lpfFreq = std::min(kLpfFreqHz, static_cast<float>(sampleRate) * 0.49f);
    calcLowpass (lpfFreq,   sampleRate, lpfB0_, lpfB1_, lpfB2_, lpfA1_, lpfA2_);

    softMuteRampSamples_     = static_cast<int>(kSoftMuteRampSec * sampleRate);
    softMuteRampRate_        = softMuteRampSamples_ > 0
                               ? 1.0f / static_cast<float>(softMuteRampSamples_) : 1.0f;
    autoRecoveryHoldSamples_ = static_cast<int>(kAutoRecoveryHoldSec * sampleRate);

    resetDsp();
    // Restore the one-free-mute budget so a sample-rate change doesn't spend it.
    autoRecoverAllowed_.store(true, std::memory_order_relaxed);
  }

  void reset() {
    // Preserve mute state and auto-recover history across host stop/start.
    resetDsp();
  }

  void process(float* left, float* right, int numSamples) {
    if (pendingReset_.exchange(false, std::memory_order_acquire)) {
      resetDsp();
      if (state_.load(std::memory_order_relaxed) == State::PermanentMuted)
        state_.store(State::Normal, std::memory_order_relaxed);
    }

    float blockMinGain = 1.0f;
    // Snapshot parameter atomics once per block — message thread writes these.
    const float sustainedTh  = sustainedThreshold_.load(std::memory_order_relaxed);
    const float dangerPeakTh = dangerPeakThreshold_.load(std::memory_order_relaxed);
    const float limiterCeil  = limiterCeiling_.load(std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i) {
      // Audio thread is sole writer in this path — relaxed loads are sufficient.
      State st = state_.load(std::memory_order_relaxed);

      // ── Permanent mute: bypass all DSP ───────────────────────────────────
      if (st == State::PermanentMuted) {
        left[i] = right[i] = 0.0f;
        blockMinGain = 0.0f;
        continue;
      }

      // ── Stage 0: NaN/Inf → immediate permanent mute ──────────────────────
      if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
        left[i] = right[i] = 0.0f;
        triggerPermanentMute(MuteReason::NaNInf);
        blockMinGain = 0.0f;
        continue;
      }

      // ── Stage 1: DC offset detection (pre-filter, signed EMA) ────────────
      dcEstL_ = kDenormalBias + static_cast<float>(dcEmaCoeff_) * dcEstL_
                + (1.0f - static_cast<float>(dcEmaCoeff_)) * left[i];
      dcEstR_ = kDenormalBias + static_cast<float>(dcEmaCoeff_) * dcEstR_
                + (1.0f - static_cast<float>(dcEmaCoeff_)) * right[i];
      if (std::max(std::abs(dcEstL_), std::abs(dcEstR_)) > kDcOffsetThreshold) {
        ++dcOffsetCounter_;
        if (dcOffsetCounter_ >= dcOffsetThresholdSamples_) {
          left[i] = right[i] = 0.0f;
          triggerPermanentMute(MuteReason::DCOffset);
          blockMinGain = 0.0f;
          continue;
        }
      } else {
        dcOffsetCounter_ = 0;
      }

      // ── Stage 2: DC Blocker (HPF 20 Hz) ──────────────────────────────────
      left[i]  = applyBiquad(left[i],  0, hpfB0_, hpfB1_, hpfB2_, hpfA1_, hpfA2_,
                             hpfX1_, hpfX2_, hpfY1_, hpfY2_);
      right[i] = applyBiquad(right[i], 1, hpfB0_, hpfB1_, hpfB2_, hpfA1_, hpfA2_,
                             hpfX1_, hpfX2_, hpfY1_, hpfY2_);

      // ── Stage 3: Ultrasonic LPF 18 kHz ───────────────────────────────────
      left[i]  = applyBiquad(left[i],  0, lpfB0_, lpfB1_, lpfB2_, lpfA1_, lpfA2_,
                             lpfX1_, lpfX2_, lpfY1_, lpfY2_);
      right[i] = applyBiquad(right[i], 1, lpfB0_, lpfB1_, lpfB2_, lpfA1_, lpfA2_,
                             lpfX1_, lpfX2_, lpfY1_, lpfY2_);

      // ── Stage 4: Sustained peak detection → tiered mute ──────────────────
      float instantPeak = std::max(std::abs(left[i]), std::abs(right[i]));
      sustainedPeakLevel_ = kDenormalBias
                          + static_cast<float>(sustainedPeakCoeff_) * sustainedPeakLevel_
                          + (1.0f - static_cast<float>(sustainedPeakCoeff_)) * instantPeak;
      if (sustainedPeakLevel_ > dangerPeakTh) {
        ++sustainedPeakCounter_;
        if (sustainedPeakCounter_ >= sustainedPeakThresholdSamples_ && st == State::Normal) {
          sustainedPeakCounter_ = 0;
          if (triggerTieredMute()) {
            left[i] = right[i] = 0.0f;
            blockMinGain = 0.0f;
            continue;  // went directly to PermanentMuted (second trip)
          }
          st = state_.load(std::memory_order_relaxed);  // reload → SoftMuting
        }
      } else {
        sustainedPeakCounter_ = 0;
      }

      const float preProtectionPeak = instantPeak;

      // ── Stage 5: Sustained loudness gain riding ───────────────────────────
      sustainedLevel_ = kDenormalBias
                      + static_cast<float>(sustainedCoeff_) * sustainedLevel_
                      + (1.0f - static_cast<float>(sustainedCoeff_)) * instantPeak;
      float sampleGain = 1.0f;
      if (sustainedLevel_ > sustainedTh)
        sampleGain = sustainedTh / sustainedLevel_;

      // ── Stage 6: Soft-mute ramp ───────────────────────────────────────────
      if (st == State::SoftMuting) {
        softMuteGain_ -= softMuteRampRate_;
        if (softMuteGain_ <= 0.0f) {
          softMuteGain_ = 0.0f;
          state_.store(State::AutoRecovering, std::memory_order_relaxed);
          autoRecoveryCounter_ = 0;
          st = State::AutoRecovering;
        }
        sampleGain *= softMuteGain_;
      }

      if (sampleGain < 1.0f) {
        left[i]  *= sampleGain;
        right[i] *= sampleGain;
      }

      if (st == State::AutoRecovering) {
        left[i] = right[i] = 0.0f;
        blockMinGain = 0.0f;
        ++autoRecoveryCounter_;
        if (autoRecoveryCounter_ >= autoRecoveryHoldSamples_) {
          autoRecoverAllowed_.store(false, std::memory_order_relaxed);
          sustainedPeakCounter_ = 0;
          sustainedPeakLevel_   = 0.0f;
          dcOffsetCounter_      = 0;
          dcEstL_ = dcEstR_     = 0.0f;
          softMuteGain_ = 1.0f;
          muteReason_.store(MuteReason::None, std::memory_order_relaxed);
          state_.store(State::Normal, std::memory_order_relaxed);
          // Release: message thread reads muteReason_/state_ after acquire on this.
          pendingNotification_.store(true, std::memory_order_release);
        }
      }

      // ── Stage 7: Hard clip (final brickwall guarantee) ───────────────────
      left[i]  = left[i]  >  limiterCeil ?  limiterCeil : (left[i]  < -limiterCeil ? -limiterCeil : left[i]);
      right[i] = right[i] >  limiterCeil ?  limiterCeil : (right[i] < -limiterCeil ? -limiterCeil : right[i]);

      if (preProtectionPeak > kGainFloor) {
        const float postProtectionPeak = std::max(std::abs(left[i]), std::abs(right[i]));
        const float protectionGain = postProtectionPeak / preProtectionPeak;
        blockMinGain = std::min(blockMinGain, protectionGain);
      }
    }

    envelopeLevelMeter_.store(sustainedLevel_, std::memory_order_relaxed);
    gainReductionDb_.store(
        blockMinGain < 1.0f
            ? 20.0f * std::log10(blockMinGain > kGainFloor ? blockMinGain : kGainFloor)
            : 0.0f,
        std::memory_order_relaxed);
  }

  // ── Configuration ────────────────────────────────────────────────────────

  // Sustained loudness threshold in linear gain. Clamped to [0.01, 1.0].
  void setSustainedThreshold(float level) {
    sustainedThreshold_.store(
        level <= 0.01f ? 0.01f : (level > 1.0f ? 1.0f : level),
        std::memory_order_relaxed);
  }

  // Convenience overload: threshold in dB (e.g. -6.0 dB = 0.5 linear).
  void setThreshold(float dB) {
    setSustainedThreshold(std::pow(10.0f, dB / 20.0f));
  }

  // Stage 4 danger peak threshold in linear gain. Clamped to [0.5, 100.0].
  void setDangerousThreshold(float linearGain) {
    if (!std::isfinite(linearGain) || linearGain <= 0.0f)
      linearGain = kDefaultDangerPeakThreshold;
    dangerPeakThreshold_.store(
        linearGain < 0.5f ? 0.5f : (linearGain > 100.0f ? 100.0f : linearGain),
        std::memory_order_relaxed);
  }

  // Stage 7 hard clip ceiling. Clamped to [0.01, 1.0].
  void setLimiterCeiling(float level) {
    limiterCeiling_.store(
        level < 0.01f ? 0.01f : (level > 1.0f ? 1.0f : level),
        std::memory_order_relaxed);
  }

  // Returns the sustained loudness envelope level (used by Stage 5).
  float getEnvelopeLevel() const {
    return envelopeLevelMeter_.load(std::memory_order_relaxed);
  }

  // ── State ─────────────────────────────────────────────────────────────────

  State getState() const {
    return state_.load(std::memory_order_relaxed);
  }

  bool isPermanentlyMuted() const {
    return state_.load(std::memory_order_relaxed) == State::PermanentMuted;
  }

  MuteReason getMuteReason() const {
    return muteReason_.load(std::memory_order_relaxed);
  }

  // Async: sets pendingReset_ so the audio thread clears state on the next
  // process() call. Call isPermanentlyMuted() only after process() has run.
  void unlockPermanentMute() {
    muteReason_.store(MuteReason::None, std::memory_order_relaxed);
    autoRecoverAllowed_.store(true, std::memory_order_relaxed);
    pendingReset_.store(true, std::memory_order_release);
  }

  void panic() {
    // Do NOT set pendingReset_ here — that would clear PermanentMuted on the next
    // process() call, silently un-muting. Just store the muted state directly.
    muteReason_.store(MuteReason::None, std::memory_order_relaxed);
    state_.store(State::PermanentMuted, std::memory_order_release);
    pendingNotification_.store(true, std::memory_order_release);
  }

  // Returns true (and clears the flag) if a noteworthy event occurred since last
  // call. Consumer must use this with acquire semantics to safely read state_
  // and muteReason_ via relaxed loads afterwards.
  bool consumePendingNotification() {
    return pendingNotification_.exchange(false, std::memory_order_acq_rel);
  }

  // ── Metering ─────────────────────────────────────────────────────────────

  float getGainReductionDb()   const { return gainReductionDb_.load(std::memory_order_relaxed); }
  int   getDangerEventCount()  const { return dangerEventCount_.load(std::memory_order_relaxed); }
  void  resetDangerEventCount()      { dangerEventCount_.store(0, std::memory_order_relaxed); }

private:
  // ── Constants ─────────────────────────────────────────────────────────────
  static constexpr float  kTwoPi                   = 6.283185307f;
  static constexpr float  kInvSqrt2                = 0.7071f;   // Butterworth Q
  static constexpr float  kDcEmaFreqHz             = 2.0f;
  static constexpr float  kDcOffsetThreshold       = 0.5f;
  static constexpr double kDcWindowSec             = 0.2;
  static constexpr double kSustainedPeakEmaSec     = 0.020;
  static constexpr double kSustainedPeakWindowSec  = 0.050;
  static constexpr double kSustainedCoeffSec       = 0.5;
  static constexpr double kSoftMuteRampSec         = 0.5;
  static constexpr double kAutoRecoveryHoldSec     = 2.0;
  static constexpr float  kDefaultDangerPeakThreshold = 2.0f;  // +6 dBFS
  static constexpr float  kHpfFreqHz               = 20.0f;
  static constexpr float  kLpfFreqHz               = 18000.0f;
  static constexpr float  kDenormalBias            = 1e-25f;
  static constexpr float  kGainFloor               = 0.000015f;  // -96 dB
  static constexpr double kSampleRateMin           = 8000.0;
  static constexpr double kSampleRateMax           = 384000.0;

  // ── Internal helpers ──────────────────────────────────────────────────────

  void triggerPermanentMute(MuteReason reason) {
    auto st = state_.load(std::memory_order_relaxed);
    if (st == State::SoftMuting || st == State::AutoRecovering)
      autoRecoverAllowed_.store(false, std::memory_order_relaxed);
    dangerEventCount_.fetch_add(1, std::memory_order_relaxed);
    muteReason_.store(reason, std::memory_order_relaxed);
    state_.store(State::PermanentMuted, std::memory_order_relaxed);
    // Release so message-thread acquire on pendingNotification_ sees consistent state_/muteReason_.
    pendingNotification_.store(true, std::memory_order_release);
  }

  // Returns true if we went directly to PermanentMuted (second trip), false for SoftMuting.
  bool triggerTieredMute() {
    dangerEventCount_.fetch_add(1, std::memory_order_relaxed);
    muteReason_.store(MuteReason::SustainedPeak, std::memory_order_relaxed);
    if (autoRecoverAllowed_.load(std::memory_order_relaxed)) {
      softMuteGain_ = 1.0f;
      state_.store(State::SoftMuting, std::memory_order_relaxed);
      pendingNotification_.store(true, std::memory_order_release);
      return false;
    }
    state_.store(State::PermanentMuted, std::memory_order_relaxed);
    pendingNotification_.store(true, std::memory_order_release);
    return true;
  }

  void resetDsp() {
    sustainedLevel_     = 0.0f;
    sustainedPeakLevel_ = 0.0f;
    dcEstL_ = dcEstR_   = 0.0f;
    dcOffsetCounter_    = 0;
    sustainedPeakCounter_ = 0;
    autoRecoveryCounter_  = 0;
    softMuteGain_         = 1.0f;
    for (int ch = 0; ch < 2; ++ch) {
      hpfX1_[ch] = hpfX2_[ch] = hpfY1_[ch] = hpfY2_[ch] = 0.0f;
      lpfX1_[ch] = lpfX2_[ch] = lpfY1_[ch] = lpfY2_[ch] = 0.0f;
    }
    envelopeLevelMeter_.store(0.0f, std::memory_order_relaxed);
    gainReductionDb_.store(0.0f, std::memory_order_relaxed);
  }

  static float applyBiquad(float input, int ch,
                            float b0, float b1, float b2, float a1, float a2,
                            float x1[2], float x2[2], float y1[2], float y2[2]) {
    float out = b0 * input + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
    x2[ch] = x1[ch]; x1[ch] = input;
    y2[ch] = y1[ch]; y1[ch] = out;
    return out;
  }

  static void calcHighpass(float freq, double sr,
                            float& b0, float& b1, float& b2, float& a1, float& a2) {
    float w0    = kTwoPi * freq / static_cast<float>(sr);
    float cosW0 = std::cos(w0), sinW0 = std::sin(w0);
    float alpha = sinW0 / (2.0f * kInvSqrt2);
    float a0Inv = 1.0f / (1.0f + alpha);
    b0 = ((1.0f + cosW0) * 0.5f) * a0Inv;
    b1 = -(1.0f + cosW0) * a0Inv;
    b2 = b0;
    a1 = (-2.0f * cosW0) * a0Inv;
    a2 = (1.0f - alpha)  * a0Inv;
  }

  static void calcLowpass(float freq, double sr,
                           float& b0, float& b1, float& b2, float& a1, float& a2) {
    float w0    = kTwoPi * freq / static_cast<float>(sr);
    float cosW0 = std::cos(w0), sinW0 = std::sin(w0);
    float alpha = sinW0 / (2.0f * kInvSqrt2);
    float a0Inv = 1.0f / (1.0f + alpha);
    b0 = ((1.0f - cosW0) * 0.5f) * a0Inv;
    b1 = (1.0f  - cosW0) * a0Inv;
    b2 = b0;
    a1 = (-2.0f * cosW0) * a0Inv;
    a2 = (1.0f - alpha)  * a0Inv;
  }

  // ── DSP state (audio thread only) ────────────────────────────────────────
  double sampleRate_ = 44100.0;

  float  hpfB0_ = 1.0f, hpfB1_ = 0.0f, hpfB2_ = 0.0f, hpfA1_ = 0.0f, hpfA2_ = 0.0f;
  float  lpfB0_ = 1.0f, lpfB1_ = 0.0f, lpfB2_ = 0.0f, lpfA1_ = 0.0f, lpfA2_ = 0.0f;
  float  hpfX1_[2]={}, hpfX2_[2]={}, hpfY1_[2]={}, hpfY2_[2]={};
  float  lpfX1_[2]={}, lpfX2_[2]={}, lpfY1_[2]={}, lpfY2_[2]={};

  double sustainedCoeff_  = 0.0;
  float  sustainedLevel_  = 0.0f;

  double sustainedPeakCoeff_            = 0.0;
  float  sustainedPeakLevel_            = 0.0f;
  int    sustainedPeakCounter_          = 0;
  int    sustainedPeakThresholdSamples_ = 0;

  double dcEmaCoeff_               = 0.0;  // 2 Hz pole
  float  dcEstL_                   = 0.0f, dcEstR_ = 0.0f;
  int    dcOffsetCounter_          = 0;
  int    dcOffsetThresholdSamples_ = 0;

  float  softMuteGain_          = 1.0f;
  float  softMuteRampRate_      = 0.0f;
  int    softMuteRampSamples_   = 0;
  int    autoRecoveryCounter_   = 0;
  int    autoRecoveryHoldSamples_ = 0;

  // ── Cross-thread atomics ──────────────────────────────────────────────────
  std::atomic<State>       state_              {State::Normal};
  std::atomic<MuteReason>  muteReason_         {MuteReason::None};
  std::atomic<bool>        autoRecoverAllowed_ {true};
  std::atomic<bool>        pendingNotification_{false};
  std::atomic<bool>        pendingReset_       {false};
  std::atomic<int>         dangerEventCount_   {0};
  std::atomic<float>       envelopeLevelMeter_ {0.0f};
  std::atomic<float>       gainReductionDb_    {0.0f};
  // Thresholds written by message thread, read each block by audio thread.
  std::atomic<float>       sustainedThreshold_   {0.95f};
  std::atomic<float>       dangerPeakThreshold_  {kDefaultDangerPeakThreshold};
  std::atomic<float>       limiterCeiling_       {1.0f};
};

} // namespace gm
