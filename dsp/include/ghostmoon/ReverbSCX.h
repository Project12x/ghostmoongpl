// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
// ghostmoon DSP: ReverbSCX
// Extended version of ReverbSC — Sean Costello's 8-delay-line stereo FDN reverb.
// Costello's core algorithm (delay taps, cubic interp, random-walk pitch modulation,
// stereo cross-injection, kJpScale mixing) is preserved verbatim. This file adds
// safety, storage, and ergonomics; `ReverbSC.h` is unchanged.
//
// Original algorithm: Sean Costello & Istvan Varga (Csound reverbsc opcode, 1999/2005)
// C extraction: Paul Batchelor (Soundpipe library)
//
// SPDX-License-Identifier: LGPL-2.1-or-later
// License: GNU LGPL v2.1 or later. CORRECTION (2026-06): previously mislabeled MIT.
//   Contains Sean Costello's Csound `reverbsc` core (LGPL-2.1-or-later, (c) 1999,
//   2005 Costello & Varga) plus original extensions; as a combined work this file
//   is distributed under LGPL-2.1-or-later. See ReverbSC.h and ../../../LICENSE.
//
// Zero JUCE dependency. Header-only.
//
// Phase 1 (safety & storage): HI-1 NaN/Inf two-tier guard, ME-1 denormal snap,
// ME-2 processBlock overload, ME-3 RAII vector storage, ME-4 feedback clamp,
// ME-17 thread-safe deferred reset.
// Phase 2 (character): ME-5 feedback-path HP, ME-9 proportional size scaling,
// ME-14 4-stage input diffusion + getLatencySamples, ME-16 tilt EQ (per-line
// LP+HP), ME-15 feedback-path soft saturation.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gm {

/// Extended Costello 8-delay-line stereo FDN reverb.
///
/// Drop-in-compatible with gm::ReverbSC at default settings:
///   diffusion=0, drive=0, HP=0 Hz (off / pass-through), tilt=0, width=1,
///   size=1, modDepth=1, modRate=1, freeze=off, preDelay=0.
///
/// Thread model (SPSC):
///   - All setters are called from a single control thread.
///   - processSample() / processBlock() are called from a single audio thread.
///   - reset() is safe from a control thread concurrently with audio processing.
///     The flush is deferred to the top of the next audio call (latency: one block).
///
/// Lifecycle:
///   - prepare() allocates the backing buffer (~640 KB at 44.1 kHz with the
///     size-scaling headroom reserved for ME-9 setSize in [0.25, 4.0]). Call
///     off the audio thread. processSample() / processBlock() are
///     allocation-free.
///   - prepare() is idempotent — safe to call multiple times. It is the only
///     writer to `initialized_`, so the non-atomic flag is safe under the
///     documented precondition (prepare() must not race with audio processing).
///   - setFeedback() is clamped to [0, 0.999]. For musical freeze / infinite sustain
///     use setFreeze(bool) (Phase 3), which raises feedback to 1.0 and bypasses all
///     damping filters simultaneously so the tail doesn't drain.
class ReverbSCX {
public:
    ReverbSCX() = default;
    ~ReverbSCX() = default;

    ReverbSCX(const ReverbSCX&) = delete;
    ReverbSCX& operator=(const ReverbSCX&) = delete;

    /// Allocate delay buffers and initialise all internal state.
    /// Must be called before any processing. Not real-time safe.
    void prepare(float sampleRate) {
        rev_.sampleRate = sampleRate;
        rev_.feedback   = 0.97f;
        rev_.lpfreq     = 10000.0f;
        rev_.hpfreq     = 0.0f;   // ME-5: default = off (bit-exact pass-through)
        rev_.tilt       = 0.0f;   // ME-16: default = flat
        rev_.iPitchMod  = 1.0f;
        rev_.dampFact   = 1.0f;
        rev_.hpCoef     = 1.0f;   // pole at 1 → hpState frozen at 0 = HP off
        rev_.tiltDampFact = 0.0f; // ME-16: tilt LP off
        rev_.tiltHpCoef   = 1.0f; // ME-16: tilt HP off
        rev_.prv_LPFreq = 0.0f;
        rev_.prv_HPFreq = 0.0f;
        rev_.prv_Tilt   = 0.0f;

        int totalSamples = 0;
        for (int i = 0; i < 8; i++)
            totalSamples += delayLineMaxSamples(sampleRate, i);

        backingBuf_.assign(totalSamples, 0.0f);

        int offset = 0;
        for (int i = 0; i < 8; i++) {
            rev_.delayLines[i].offset = offset;
            initDelayLine(i);
            offset += delayLineMaxSamples(sampleRate, i);
        }

        // ME-14: allocate diffuser buffers (L + R per stage). Always
        // allocated so setDiffusion() at runtime doesn't need to resize.
        int diffTotal = 0;
        for (int i = 0; i < kNumDiffStages; ++i) {
            int sz = std::max(1, (int)(kDiffusionTimes[i] * sampleRate + 0.5f));
            diffStages_[i].bufSize = sz;
            diffStages_[i].offsetL = diffTotal;
            diffTotal += sz;
            diffStages_[i].offsetR = diffTotal;
            diffTotal += sz;
            diffStages_[i].writePos = 0;
        }
        diffBuf_.assign(diffTotal, 0.0f);
        prv_diff_ = diffusion_;

        initialized_ = true;
        resetPending_.store(false, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Setters
    // -------------------------------------------------------------------------

    /// Feedback gain in [0, 0.999]. Values above 0.999 are clamped — the LP
    /// damping filter does not fully prevent self-oscillation at exactly 1.0.
    /// For intentional infinite sustain see setFreeze(bool) (Phase 3).
    void setFeedback(float fb) { rev_.feedback = std::clamp(fb, 0.0f, 0.999f); }

    /// Returns the current (clamped) feedback value.
    float getFeedback() const { return rev_.feedback; }

    /// Lowpass cutoff in the feedback path (Hz). Default: 10 000 Hz.
    /// Lowering this value darkens the tail; at ~2 000 Hz the reverb is quite warm.
    void setLPFreq(float hz) { rev_.lpfreq = std::max(20.0f, hz); }

    /// Highpass cutoff in the feedback path (Hz). Default: 0 Hz (truly off —
    /// bit-exact pass-through that preserves ReverbSC character). Values > 0
    /// are clamped to a minimum of 20 Hz. Typical engaged setting: 100-150 Hz
    /// to remove LF buildup on long tails and prevent muddy wash on pads.
    /// (ME-5) The ME-16 tilt EQ (Phase 2, remaining) runs as a separate
    /// independent filter pair.
    void setHPFreq(float hz) {
        rev_.hpfreq = (hz <= 0.0f) ? 0.0f : std::max(20.0f, hz);
    }

    /// Returns the current HP cutoff.
    float getHPFreq() const { return rev_.hpfreq; }

    /// Proportional scaling of all 8 delay lengths in [0.25, 4.0].
    /// 1.0 = ReverbSC baseline. Scales RT60 and character: smaller = tighter,
    /// roomier; larger = longer, cathedral-like. Buffers are sized for the
    /// maximum at prepare(), so this is RT-safe — the new size takes effect on
    /// the next random-walk segment (within ~1 s for the slowest-moving line).
    /// (ME-9)
    void setSize(float s) { size_ = std::clamp(s, 0.25f, kMaxSize); }

    /// Returns the current (clamped) size value.
    float getSize() const { return size_; }

    /// Soft-saturation amount in the feedback path, in [0, 1]. Default 0 is
    /// fully linear (bit-exact pass-through). Higher values progressively
    /// warm and compress the tail — at d=1 the per-sample feedback is
    /// compressed by y = x*g / (1 + |x*g|) with g ≈ 4, adding even-symmetric
    /// soft clipping as the tail reaches unity-scale amplitude. (ME-15)
    void setFeedbackDrive(float d) { drive_ = std::clamp(d, 0.0f, 1.0f); }

    /// Returns the current (clamped) feedback drive.
    float getFeedbackDrive() const { return drive_; }

    /// Tilt EQ in the feedback path, in [-1, +1]. A separate per-line LP+HP
    /// pair driven by a single knob, running in series AFTER the explicit
    /// setLPFreq / setHPFreq pair. Default 0 = flat (bit-exact pass-through).
    /// t > 0 (bright): HP cutoff ramps 20 Hz → 500 Hz, internal LP off.
    /// t < 0 (dark):   LP cutoff ramps 20 kHz → 2 kHz, internal HP off.
    /// (ME-16)
    void setTilt(float t) { rev_.tilt = std::clamp(t, -1.0f, 1.0f); }

    /// Returns the current (clamped) tilt value.
    float getTilt() const { return rev_.tilt; }

    /// Input-side echo density via a 4-stage Schroeder allpass chain, in
    /// [0, 1]. Default 0.0 is a true bypass — bit-exact pass-through with
    /// zero latency, matching ReverbSC's drop-in contract. Values > 0 engage
    /// the diffuser with allpass coefficients scaled up to ~0.7 at 1.0,
    /// smearing transients into a denser front-end for percussive material.
    /// Engaging the diffuser adds ~40 ms of algorithmic latency (at 44.1 kHz)
    /// which is reported by getLatencySamples(). (ME-14)
    void setDiffusion(float d) { diffusion_ = std::clamp(d, 0.0f, 1.0f); }

    /// Returns the current (clamped) diffusion amount.
    float getDiffusion() const { return diffusion_; }

    /// Algorithmic latency in samples that consumers should report to hosts
    /// for plugin-delay-compensation (PDC). Currently reflects only the
    /// diffuser's total delay-line length when engaged; 0 when diffusion is
    /// off. Deliberately does NOT include ME-7 pre-delay (Phase 3) — that is
    /// a musical effect, not processing latency. (ME-12, partial — diffusion
    /// contribution.)
    int getLatencySamples() const {
        if (diffusion_ <= 0.0f) return 0;
        int total = 0;
        for (const auto& s : diffStages_) total += s.bufSize;
        return total;
    }

    /// Wet/dry mix in [0, 1]. 0 = dry only, 1 = wet only.
    void setMix(float m) { mix_ = std::clamp(m, 0.0f, 1.0f); }

    /// LP filter state for delay line n [0, 7]. Useful for diagnostics and unit
    /// tests. Returns 0 for out-of-range n.
    float getFilterState(int n) const {
        if (n < 0 || n >= 8) return 0.0f;
        return rev_.delayLines[n].filterState;
    }

    /// HP filter state for delay line n [0, 7]. Useful for diagnostics and
    /// unit tests. Returns 0 for out-of-range n.
    float getHpState(int n) const {
        if (n < 0 || n >= 8) return 0.0f;
        return rev_.delayLines[n].hpState;
    }

    /// Directly writes filterState[n] — for unit tests only (Tier 2 NaN
    /// detection, denormal snap). Single-threaded test contexts only; calling
    /// this concurrently with processSample / processBlock is undefined.
    void setFilterStateForTesting(int n, float v) {
        if (n >= 0 && n < 8)
            rev_.delayLines[n].filterState = v;
    }

    /// Directly writes hpState[n] — for unit tests only. Same threading rules
    /// as setFilterStateForTesting.
    void setHpStateForTesting(int n, float v) {
        if (n >= 0 && n < 8)
            rev_.delayLines[n].hpState = v;
    }

    /// Request a full state flush from any thread. The flush is deferred: the
    /// audio thread applies it at the top of the next processSample or
    /// processBlock call. Latency is bounded to one audio block.
    void reset() { resetPending_.store(true, std::memory_order_release); }

    // -------------------------------------------------------------------------
    // Processing
    // -------------------------------------------------------------------------

    /// Process one stereo sample pair in place.
    ///
    /// NaN/Inf guard (two-tier):
    ///   Tier 1 — non-finite input is clamped to 0.0 before entering the FDN,
    ///             preserving the existing reverb tail (one bad upstream sample
    ///             is treated as silence for that instant).
    ///   Tier 2 — if any of the 8 filterState[] values become non-finite after
    ///             processing, the full FDN state is flushed and wet output for
    ///             this sample is zeroed. Catches corruption that has already
    ///             propagated into feedback state (e.g. via runaway subnormals).
    void processSample(float inL, float inR, float& outL, float& outR) {
        if (!initialized_) { outL = inL; outR = inR; return; }

        if (resetPending_.exchange(false, std::memory_order_acquire))
            flushState();

        if (!std::isfinite(inL)) inL = 0.0f;  // Tier 1
        if (!std::isfinite(inR)) inR = 0.0f;

        // Dry path uses the original input; wet path sees the diffused input.
        float wetInL = inL, wetInR = inR;
        maybeDiffuse(wetInL, wetInR);

        float wetL, wetR;
        computeOne(wetInL, wetInR, wetL, wetR);

        // Tier 2 — per-sample in this path; per-block in processBlock
        if (anyStateNonFinite()) {
            flushState();
            wetL = wetR = 0.0f;
        }

        outL = inL * (1.0f - mix_) + wetL * mix_;
        outR = inR * (1.0f - mix_) + wetR * mix_;
    }

    /// Process a block of stereo audio (separate L and R float arrays, in-place).
    ///
    /// Null-pointer and non-positive sample count are silently ignored.
    /// dampFact, hpCoef, and feedback are hoisted out of the inner loop for
    /// efficiency. Tier 1 NaN guard runs per-sample; Tier 2 state audit runs
    /// once per block.
    ///
    /// If Tier 2 detects corrupted state, the flush takes effect on the next
    /// call; the current block's output may contain non-finite values if the
    /// state was already corrupted before the block began.
    void processBlock(float* leftChannel, float* rightChannel, int numSamples) {
        if (!leftChannel || !rightChannel || numSamples <= 0) return;
        if (!initialized_) return;

        if (resetPending_.exchange(false, std::memory_order_acquire))
            flushState();

        // Hoist dampFact — recompute at most once per block if lpfreq changed.
        if (rev_.lpfreq != rev_.prv_LPFreq) {
            rev_.prv_LPFreq = rev_.lpfreq;
            float d = 2.0f - std::cos(rev_.prv_LPFreq * (2.0f * kPi) / rev_.sampleRate);
            rev_.dampFact = d - std::sqrt(d * d - 1.0f);
        }
        // Same lazy recompute for the HP coefficient (ME-5). hpfreq <= 0
        // means "HP off" — hpCoef = 1.0 freezes hpState at its current value,
        // and since it's 0-initialised and re-zeroed on any off transition,
        // the HP becomes a bit-exact pass-through.
        if (rev_.hpfreq != rev_.prv_HPFreq) {
            rev_.prv_HPFreq = rev_.hpfreq;
            if (rev_.hpfreq <= 0.0f) {
                rev_.hpCoef = 1.0f;
                for (auto& dl : rev_.delayLines) dl.hpState = 0.0f;
            } else {
                float d = 2.0f - std::cos(rev_.prv_HPFreq * (2.0f * kPi) / rev_.sampleRate);
                rev_.hpCoef = d - std::sqrt(d * d - 1.0f);
            }
        }
        // Tilt coef recompute (ME-16). Lazy, runs only on change.
        if (rev_.tilt != rev_.prv_Tilt) recomputeTiltCoefs();

        const float dampFact     = rev_.dampFact;
        const float hpCoef       = rev_.hpCoef;
        const float tiltDampFact = rev_.tiltDampFact;
        const float tiltHpCoef   = rev_.tiltHpCoef;
        const float feedback     = rev_.feedback;
        const float drive        = drive_; // ME-15: feedback-path soft-sat amount
        const float wet      = mix_;
        const float dry      = 1.0f - wet;

        for (int i = 0; i < numSamples; ++i) {
            float inL = leftChannel[i];
            float inR = rightChannel[i];

            if (!std::isfinite(inL)) inL = 0.0f;  // Tier 1
            if (!std::isfinite(inR)) inR = 0.0f;

            float wetInL = inL, wetInR = inR;
            maybeDiffuse(wetInL, wetInR);

            float wetL, wetR;
            computeCore(wetInL, wetInR, wetL, wetR,
                        dampFact, hpCoef, tiltDampFact, tiltHpCoef,
                        feedback, drive);

            leftChannel[i]  = inL * dry + wetL * wet;
            rightChannel[i] = inR * dry + wetR * wet;
        }

        // Tier 2 — once per block
        if (anyStateNonFinite())
            flushState();
    }

private:
    static constexpr float kPi           = 3.14159265358979323846f;
    static constexpr float kOutputGain   = 0.7f;
    // 1/4 = approximate sum-normalization for the 8-way Hadamard-ish feedback mixer;
    // Costello's original constant (reverbsc, 1999).
    static constexpr float kJpScale      = 0.25f;
    static constexpr float kDefaultSRate = 44100.0f;
    static constexpr int   kDelayPosShift = 28;
    static constexpr int   kDelayPosScale = 0x10000000;
    static constexpr int   kDelayPosMask  = 0x0FFFFFFF;
    // Denormal snap threshold: values below this on x86 incur 10-100x microcode cost.
    static constexpr float kDenormalThresh = 1e-20f;
    // ME-9: maximum size multiplier. Determines prepare() allocation headroom —
    // buffers are sized once for the worst case so setSize() is RT-safe.
    static constexpr float kMaxSize = 4.0f;
    // Reserved mod-depth headroom (Phase 3 ME-6 will expose setModDepth in
    // [0, 2]; reserving here avoids a re-prepare when that lands).
    static constexpr float kMaxModDepth = 2.0f;
    // ME-14: 4-stage Schroeder allpass diffuser. Canonical mutually-prime
    // short delays (5.1 / 7.3 / 11.7 / 15.9 ms). Total ≈ 40 ms at 44.1 kHz.
    static constexpr int   kNumDiffStages = 4;
    static constexpr float kDiffusionTimes[kNumDiffStages] =
        {0.0051f, 0.0073f, 0.0117f, 0.0159f};
    // Max coefficient at diffusion=1.0. 0.7 follows the audit spec and sits
    // in the sweet spot between density and ringing for most source material.
    static constexpr float kMaxDiffusionCoef = 0.7f;

    // Sean Costello's original delay-line tuning table — preserved verbatim.
    // Columns: [base_delay_s, mod_depth_s, mod_rate_Hz, initial_seed]
    static constexpr float kParams[8][4] = {
        {2473.0f / kDefaultSRate, 0.0010f, 3.100f, 1966.0f},
        {2767.0f / kDefaultSRate, 0.0011f, 3.500f, 29491.0f},
        {3217.0f / kDefaultSRate, 0.0017f, 1.110f, 22937.0f},
        {3557.0f / kDefaultSRate, 0.0006f, 3.973f, 9830.0f},
        {3907.0f / kDefaultSRate, 0.0010f, 2.341f, 20643.0f},
        {4127.0f / kDefaultSRate, 0.0011f, 1.897f, 22937.0f},
        {2143.0f / kDefaultSRate, 0.0017f, 0.891f, 29491.0f},
        {1933.0f / kDefaultSRate, 0.0006f, 3.221f, 14417.0f}
    };

    struct DelayLine {
        int writePos       = 0;
        int bufferSize     = 0;
        int readPos        = 0;
        int readPosFrac    = 0;
        int readPosFrac_inc = 0;
        int seedVal        = 0;
        int randLine_cnt   = 0;
        float filterState  = 0.0f;  // LP state (feedback path, damping)
        float hpState      = 0.0f;  // HP state (feedback path, DC/LF block) — ME-5
        float tiltLpState  = 0.0f;  // Tilt LP state (Pair B) — ME-16
        float tiltHpState  = 0.0f;  // Tilt HP state (Pair B) — ME-16
        int offset         = 0; // start index into backingBuf_ for this line
    };

    struct RevState {
        float feedback    = 0.97f;
        float lpfreq      = 10000.0f;
        float hpfreq      = 0.0f;   // ME-5: default 0 Hz = off (pass-through)
        float tilt        = 0.0f;   // ME-16: [-1, +1], 0 = flat
        float sampleRate  = 44100.0f;
        float iPitchMod   = 1.0f;
        float dampFact    = 1.0f;
        float hpCoef      = 1.0f;   // ME-5: 1.0 freezes hpState → HP off
        float tiltDampFact = 0.0f;  // ME-16: tilt LP coefficient (0 = off)
        float tiltHpCoef   = 1.0f;  // ME-16: tilt HP coefficient (1 = off)
        float prv_LPFreq  = 0.0f;
        float prv_HPFreq  = 0.0f;   // ME-5: lazy HP coef recompute guard
        float prv_Tilt    = 0.0f;   // ME-16: lazy tilt coef recompute guard
        DelayLine delayLines[8] = {};
    };

    // ME-14: one Schroeder allpass stage — offsets into diffBuf_ for L/R
    // delay lines of length `bufSize`, plus a shared write position.
    struct DiffusionStage {
        int bufSize = 0;
        int writePos = 0;
        int offsetL = 0;
        int offsetR = 0;
    };

    RevState rev_;
    std::vector<float> backingBuf_;
    std::atomic<bool> resetPending_{false};
    float mix_        = 0.5f;
    float size_       = 1.0f;   // ME-9: proportional delay-length scale
    float diffusion_  = 0.0f;   // ME-14: amount in [0, 1]; 0 = bypass
    float prv_diff_   = 0.0f;   // ME-14: transition detection (off <-> on)
    float drive_      = 0.0f;   // ME-15: feedback soft-sat amount in [0, 1]
    DiffusionStage diffStages_[kNumDiffStages] = {};
    std::vector<float> diffBuf_;
    bool initialized_ = false;

    // Zeroes delay-buffer contents, all per-line filter state (LP + HP),
    // AND the diffuser buffers. Deliberately preserves write/read positions,
    // PRNG seeds, and random-walk counters on the FDN delay lines — a live
    // flush drains audio energy; it does NOT re-randomize modulation or
    // restart delay lines from their initial offsets. The diffuser has no
    // modulation state to preserve, so its writePos is also reset to 0.
    void flushState() {
        std::fill(backingBuf_.begin(), backingBuf_.end(), 0.0f);
        for (auto& lp : rev_.delayLines) {
            lp.filterState = 0.0f;
            lp.hpState     = 0.0f;
            lp.tiltLpState = 0.0f;
            lp.tiltHpState = 0.0f;
        }
        std::fill(diffBuf_.begin(), diffBuf_.end(), 0.0f);
        for (auto& s : diffStages_) s.writePos = 0;
    }

    bool anyStateNonFinite() const {
        for (const auto& lp : rev_.delayLines) {
            if (!std::isfinite(lp.filterState)) return true;
            if (!std::isfinite(lp.hpState))     return true;
            if (!std::isfinite(lp.tiltLpState)) return true;
            if (!std::isfinite(lp.tiltHpState)) return true;
        }
        return false;
    }

    // ME-16: recompute tilt LP + HP coefficients from rev_.tilt. Called from
    // the audio thread inside the lazy-recompute guard, so it may safely
    // zero per-line tilt state on "off" / sign-change transitions without
    // needing atomic synchronisation with the control thread.
    void recomputeTiltCoefs() {
        const float prev = rev_.prv_Tilt;
        const float cur  = rev_.tilt;
        rev_.prv_Tilt = cur;

        const bool wentToZero  = (cur == 0.0f) && (prev != 0.0f);
        const bool crossedZero = (prev * cur < 0.0f);
        if (wentToZero || crossedZero) {
            for (auto& dl : rev_.delayLines) {
                dl.tiltLpState = 0.0f;
                dl.tiltHpState = 0.0f;
            }
        }

        if (cur > 0.0f) {
            // Bright: HP engaged, LP off. HP cutoff ramps 20 → 500 Hz.
            rev_.tiltDampFact = 0.0f;
            const float hpHz = 20.0f * std::pow(25.0f, cur); // 20 * 25^t
            const float d = 2.0f - std::cos(hpHz * (2.0f * kPi) / rev_.sampleRate);
            rev_.tiltHpCoef = d - std::sqrt(d * d - 1.0f);
        } else if (cur < 0.0f) {
            // Dark: LP engaged, HP off. LP cutoff ramps 20k → 2k Hz.
            rev_.tiltHpCoef = 1.0f;
            const float lpHz = 20000.0f * std::pow(0.1f, -cur); // 20k * 0.1^|t|
            const float d = 2.0f - std::cos(lpHz * (2.0f * kPi) / rev_.sampleRate);
            rev_.tiltDampFact = d - std::sqrt(d * d - 1.0f);
        } else {
            // tilt == 0: both filters off.
            rev_.tiltDampFact = 0.0f;
            rev_.tiltHpCoef   = 1.0f;
        }
    }

    // ME-14: update diffuser state and return (diffusedL, diffusedR) by
    // mutating the inputs in place. At diffusion_ <= 0 this is a true no-op
    // — not even a read of diffBuf_ — so the bit-exact drop-in guarantee is
    // preserved. On any transition to off, buffers are zeroed so re-enabling
    // the diffuser later starts from silence rather than a stale impulse.
    void maybeDiffuse(float& inL, float& inR) {
        if (diffusion_ != prv_diff_) {
            const bool wasOn  = prv_diff_  > 0.0f;
            const bool nowOff = diffusion_ <= 0.0f;
            prv_diff_ = diffusion_;
            if (wasOn && nowOff) {
                std::fill(diffBuf_.begin(), diffBuf_.end(), 0.0f);
                for (auto& s : diffStages_) s.writePos = 0;
            }
        }
        if (diffusion_ <= 0.0f) return;

        const float a = diffusion_ * kMaxDiffusionCoef;
        for (auto& s : diffStages_) {
            // L channel Schroeder allpass: v[n] = x + a*v_delayed;
            //   y = v_delayed - a*v = -a*x + (1-a²)*v_delayed
            const int idxL = s.offsetL + s.writePos;
            const float sdL = diffBuf_[idxL];
            const float vL  = inL + a * sdL;
            diffBuf_[idxL]  = vL;
            inL = sdL - a * vL;

            // R channel
            const int idxR = s.offsetR + s.writePos;
            const float sdR = diffBuf_[idxR];
            const float vR  = inR + a * sdR;
            diffBuf_[idxR]  = vR;
            inR = sdR - a * vR;

            if (++s.writePos >= s.bufSize) s.writePos = 0;
        }
    }

    // Inner kernel: one stereo sample through all 8 delay lines.
    // Takes pre-hoisted dampFact, hpCoef, tiltDampFact, tiltHpCoef, feedback,
    // and drive to allow the block path to avoid recomputing them per-sample.
    void computeCore(float inL, float inR, float& outL, float& outR,
                     float dampFact, float hpCoef,
                     float tiltDampFact, float tiltHpCoef,
                     float feedback, float drive) {
        float ainL = 0.0f;
        float aoutL = 0.0f, aoutR = 0.0f;
        // Feedback sum uses the combined LP → HP → tilt LP → tilt HP output
        // per line: tiltLpState - tiltHpState. At defaults (hp off, tilt 0)
        // tiltLpState == filterState and tiltHpState == 0, so this matches
        // ReverbSC behavior exactly. (ME-5 + ME-16)
        for (int n = 0; n < 8; n++) {
            const auto& lp = rev_.delayLines[n];
            ainL += (lp.tiltLpState - lp.tiltHpState);
        }

        // 1/4 sum-normalization for the 8-way feedback mixer (Costello 1999)
        ainL *= kJpScale;
        float ainR = ainL + inR;
        ainL       = ainL + inL;

        for (int n = 0; n < 8; n++) {
            DelayLine& lp  = rev_.delayLines[n];
            const int base = lp.offset;
            int bufSize    = lp.bufferSize;

            // Write input + mixed feedback minus this line's own contribution
            // (post-tilt output) so each line sees only others' feedback.
            const float ownFb = lp.tiltLpState - lp.tiltHpState;
            backingBuf_[base + lp.writePos] = ((n & 1) ? ainR : ainL) - ownFb;
            if (++lp.writePos >= bufSize)
                lp.writePos -= bufSize;

            if (lp.readPosFrac >= kDelayPosScale) {
                lp.readPos    += (lp.readPosFrac >> kDelayPosShift);
                lp.readPosFrac &= kDelayPosMask;
            }
            if (lp.readPos >= bufSize)
                lp.readPos -= bufSize;

            int readPos = lp.readPos;
            float frac  = static_cast<float>(lp.readPosFrac) *
                          (1.0f / static_cast<float>(kDelayPosScale));

            // Cubic interpolation (Costello's original formulation — preserved verbatim)
            float a2  = frac * frac;
            a2 -= 1.0f;
            a2 *= (1.0f / 6.0f);
            float a1  = frac;
            a1 += 1.0f;
            a1 *= 0.5f;
            float am1 = a1 - 1.0f;
            float a0  = 3.0f * a2;
            a1  -= a0;
            am1 -= a2;
            a0  -= frac;

            float vm1, v0, v1, v2;
            if (readPos > 0 && readPos < (bufSize - 2)) {
                vm1 = backingBuf_[base + readPos - 1];
                v0  = backingBuf_[base + readPos];
                v1  = backingBuf_[base + readPos + 1];
                v2  = backingBuf_[base + readPos + 2];
            } else {
                if (--readPos < 0) readPos += bufSize;
                vm1 = backingBuf_[base + readPos];
                if (++readPos >= bufSize) readPos -= bufSize;
                v0 = backingBuf_[base + readPos];
                if (++readPos >= bufSize) readPos -= bufSize;
                v1 = backingBuf_[base + readPos];
                if (++readPos >= bufSize) readPos -= bufSize;
                v2 = backingBuf_[base + readPos];
            }
            v0 = (am1 * vm1 + a0 * v0 + a1 * v1 + a2 * v2) * frac + v0;

            lp.readPosFrac += lp.readPosFrac_inc;

            v0 *= feedback;
            // Soft saturation in the feedback path (ME-15). Branch skipped at
            // drive == 0 for bit-exact pass-through. Formulation:
            //   y = softSat(x * g) / g  with softSat(x) = x / (1 + |x|)
            // simplifies to y = x / (1 + g * |x|).
            if (drive > 0.0f) {
                const float g = 1.0f + drive * 3.0f;
                v0 = v0 / (1.0f + g * std::abs(v0));
            }
            // LP (one-pole) — preserved verbatim from ReverbSC.
            v0  = (lp.filterState - v0) * dampFact + v0;
            lp.filterState = v0;

            // HP (one-pole) — hpState is a slow LP of the LP output; HP output
            // = filterState - hpState. hpCoef == 1.0 means "HP off": hpState
            // stays frozen (always 0 by construction — initialised to 0 and
            // re-zeroed on any off transition), so HP is a bit-exact pass-
            // through and ReverbSCX matches ReverbSC at default. (ME-5)
            if (hpCoef < 1.0f) {
                lp.hpState = (lp.hpState - v0) * hpCoef + v0;
                v0 = v0 - lp.hpState;
            }

            // Pair B: tilt LP (ME-16). tiltDampFact == 0 → tiltLpState just
            // tracks v0, so the subsequent subtraction (tiltLpState -
            // tiltHpState) yields v0 unchanged at tilt <= 0 with HP off.
            lp.tiltLpState = (lp.tiltLpState - v0) * tiltDampFact + v0;
            v0 = lp.tiltLpState;

            // Pair B: tilt HP (ME-16). tiltHpCoef == 1 → skipped; tiltHpState
            // stays frozen (re-zeroed on any off / sign-change transition).
            if (tiltHpCoef < 1.0f) {
                lp.tiltHpState = (lp.tiltHpState - v0) * tiltHpCoef + v0;
                v0 = v0 - lp.tiltHpState;
            }

            // Snap subnormals to zero — prevents 10-100x CPU penalty during
            // extended silence on x86 without FTZ/DAZ enabled.
            if (std::abs(lp.filterState) < kDenormalThresh) lp.filterState = 0.0f;
            if (std::abs(lp.hpState)     < kDenormalThresh) lp.hpState     = 0.0f;
            if (std::abs(lp.tiltLpState) < kDenormalThresh) lp.tiltLpState = 0.0f;
            if (std::abs(lp.tiltHpState) < kDenormalThresh) lp.tiltHpState = 0.0f;

            if (n & 1)
                aoutR += v0;
            else
                aoutL += v0;

            if (--(lp.randLine_cnt) <= 0)
                nextRandomLineSeg(n);
        }

        outL = aoutL * kOutputGain;
        outR = aoutR * kOutputGain;
    }

    // Per-sample wrapper used by processSample(). Lazily recomputes dampFact,
    // hpCoef, and the tilt coefficients when their source parameters have
    // changed, then delegates to computeCore().
    void computeOne(float inL, float inR, float& outL, float& outR) {
        if (rev_.lpfreq != rev_.prv_LPFreq) {
            rev_.prv_LPFreq = rev_.lpfreq;
            float d = 2.0f - std::cos(rev_.prv_LPFreq * (2.0f * kPi) / rev_.sampleRate);
            rev_.dampFact = d - std::sqrt(d * d - 1.0f);
        }
        if (rev_.hpfreq != rev_.prv_HPFreq) {
            rev_.prv_HPFreq = rev_.hpfreq;
            if (rev_.hpfreq <= 0.0f) {
                rev_.hpCoef = 1.0f;
                for (auto& dl : rev_.delayLines) dl.hpState = 0.0f;
            } else {
                float d = 2.0f - std::cos(rev_.prv_HPFreq * (2.0f * kPi) / rev_.sampleRate);
                rev_.hpCoef = d - std::sqrt(d * d - 1.0f);
            }
        }
        if (rev_.tilt != rev_.prv_Tilt) recomputeTiltCoefs();
        computeCore(inL, inR, outL, outR,
                    rev_.dampFact, rev_.hpCoef,
                    rev_.tiltDampFact, rev_.tiltHpCoef,
                    rev_.feedback, drive_);
    }

    // Buffer sizing reserves headroom for the maximum possible delay under the
    // maximum size multiplier AND the maximum reserved mod depth. Allocated
    // once at prepare(); setSize() / future setModDepth() slide read-tap
    // positions within this buffer without re-allocating.
    static int delayLineMaxSamples(float sr, int n) {
        float maxDel = kParams[n][0] * kMaxSize + (kParams[n][1] * kMaxModDepth * 1.125f);
        return static_cast<int>(maxDel * sr + 16.5f);
    }

    void nextRandomLineSeg(int n) {
        DelayLine& lp = rev_.delayLines[n];

        if (lp.seedVal < 0) lp.seedVal += 0x10000;
        lp.seedVal = (lp.seedVal * 15625 + 1) & 0xFFFF;
        if (lp.seedVal >= 0x8000) lp.seedVal -= 0x10000;

        lp.randLine_cnt = static_cast<int>((rev_.sampleRate / kParams[n][2]) + 0.5f);

        float prvDel = static_cast<float>(lp.writePos);
        prvDel -= (static_cast<float>(lp.readPos) +
                   (static_cast<float>(lp.readPosFrac) / static_cast<float>(kDelayPosScale)));
        while (prvDel < 0.0f) prvDel += lp.bufferSize;
        prvDel = prvDel / rev_.sampleRate;

        float nxtDel = static_cast<float>(lp.seedVal) * kParams[n][1] / 32768.0f;
        nxtDel = kParams[n][0] * size_ + (nxtDel * rev_.iPitchMod);  // ME-9: size scale

        float phs_incVal = (prvDel - nxtDel) / static_cast<float>(lp.randLine_cnt);
        phs_incVal = phs_incVal * rev_.sampleRate + 1.0f;
        lp.readPosFrac_inc = static_cast<int>(phs_incVal * kDelayPosScale + 0.5f);
    }

    void initDelayLine(int n) {
        DelayLine& lp = rev_.delayLines[n];
        lp.bufferSize = delayLineMaxSamples(rev_.sampleRate, n);
        lp.writePos   = 0;
        lp.seedVal    = static_cast<int>(kParams[n][3] + 0.5f);

        float readPos = static_cast<float>(lp.seedVal) * kParams[n][1] / 32768.0f;
        readPos = kParams[n][0] * size_ + (readPos * rev_.iPitchMod);  // ME-9: size scale
        readPos = static_cast<float>(lp.bufferSize) - (readPos * rev_.sampleRate);
        lp.readPos = static_cast<int>(readPos);
        float fracPart = (readPos - static_cast<float>(lp.readPos)) *
                         static_cast<float>(kDelayPosScale);
        lp.readPosFrac = static_cast<int>(fracPart + 0.5f);

        nextRandomLineSeg(n);
        lp.filterState = 0.0f;
        lp.hpState     = 0.0f;  // ME-5
        lp.tiltLpState = 0.0f;  // ME-16
        lp.tiltHpState = 0.0f;  // ME-16
        // backingBuf_ is zero-initialized by assign() in prepare(); no memset needed.
    }
};

} // namespace gm
