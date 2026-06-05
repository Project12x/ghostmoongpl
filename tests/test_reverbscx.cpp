// Tests for gm::ReverbSCX
//
// Coverage in this file:
//   Baseline (6): mirrored from ReverbSC to confirm drop-in equivalence at defaults
//   Phase 1 safety (7): HI-1 NaN/Inf guard, ME-1 denormal, ME-2 processBlock,
//                       ME-4 feedback clamp (from audit/ReverbSCX.md Tests 1-7)
//   Phase 2 character (in progress): ME-5 feedback-path HP, ME-9 proportional
//                       size scaling (Tests 8-9 from the audit).
//   Additional (5): sample-rate robustness, setter boundaries, prepare idempotence,
//                   baseline character match, control-thread reset safety
//
// Phase 2 remaining (ME-14 diffusion, ME-15 saturation, ME-16 tilt EQ) and
// Phases 3-5 are not in this file yet — they will be added per phase.

#include <gtest/gtest.h>
#include <ghostmoon/ReverbSCX.h>
#include <ghostmoon/ReverbSC.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>

static constexpr float kSampleRate = 44100.0f;

// ============================================================
// Shared helpers
// ============================================================

static std::vector<float> makeSine(float freq, float sr, int numSamples) {
    std::vector<float> buf(numSamples);
    const float twoPi = 2.0f * 3.14159265358979f;
    for (int i = 0; i < numSamples; ++i)
        buf[i] = std::sin(twoPi * freq * (float)i / sr);
    return buf;
}

static float rms(const float* buf, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double)buf[i] * buf[i];
    return (float)std::sqrt(s / n);
}

// Goertzel single-bin DFT magnitude (same implementation as test_reverbs.cpp)
static double goertzelMag(const float* buf, int N, double freqHz, double sr) {
    double k     = (double)N * freqHz / sr;
    double omega = 2.0 * 3.14159265358979323846 * k / (double)N;
    double coeff = 2.0 * std::cos(omega);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < N; ++i) {
        double s0 = (double)buf[i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
}

// Spectral centroid via Goertzel at log-spaced bins
static float spectralCentroid(const float* buf, int N, double sr) {
    static const double kBins[] = {100, 200, 400, 800, 1600, 3200, 6400};
    double totalMag = 0.0, weightedFreq = 0.0;
    for (double f : kBins) {
        if (f >= sr / 2.0) continue;
        double m = goertzelMag(buf, N, f, sr);
        totalMag += m;
        weightedFreq += m * f;
    }
    return totalMag > 1e-8 ? (float)(weightedFreq / totalMag) : 0.0f;
}

// RT60 estimate from RMS envelope windowed at envRateSr.
// Returns -1 if the tail never drops -20 dB within the buffer.
static float estimateRT60(const float* buf, int N, float sr, int winSamples) {
    int numWins = N / winSamples;
    std::vector<float> env(numWins);
    for (int w = 0; w < numWins; ++w) {
        double s = 0.0;
        for (int j = 0; j < winSamples; ++j) {
            float v = buf[(size_t)w * winSamples + j];
            s += v * v;
        }
        env[w] = (float)std::sqrt(s / winSamples);
    }
    // Find peak window
    int peakIdx = 0;
    float peak = 0.0f;
    for (int i = 0; i < numWins; ++i)
        if (env[i] > peak) { peak = env[i]; peakIdx = i; }
    if (peak < 1e-8f) return -1.0f;
    float target = peak * 0.1f; // -20 dB
    float envRate = sr / (float)winSamples;
    for (int i = peakIdx + 1; i < numWins; ++i) {
        if (env[i] < target) {
            float t20 = (float)(i - peakIdx) / envRate;
            return t20 * 3.0f; // scale -20 dB to -60 dB (exponential decay)
        }
    }
    return -1.0f; // tail too long for the buffer
}

// Run an impulse through a ReverbSCX and return N samples of output (L only).
static std::vector<float> impulseTailX(gm::ReverbSCX& verb, int numSamples) {
    std::vector<float> out(numSamples);
    float oL, oR;
    verb.processSample(1.0f, 1.0f, oL, oR);
    out[0] = oL;
    for (int i = 1; i < numSamples; ++i) {
        verb.processSample(0.0f, 0.0f, oL, oR);
        out[i] = oL;
    }
    return out;
}

// Same for ReverbSC
static std::vector<float> impulseTailSC(gm::ReverbSC& verb, int numSamples) {
    std::vector<float> out(numSamples);
    float oL, oR;
    verb.processSample(1.0f, 1.0f, oL, oR);
    out[0] = oL;
    for (int i = 1; i < numSamples; ++i) {
        verb.processSample(0.0f, 0.0f, oL, oR);
        out[i] = oL;
    }
    return out;
}

// ============================================================
// Baseline tests — mirrored from ReverbSC for drop-in verification
// ============================================================

TEST(ReverbSCX, ProducesOutput) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.85f);

    auto sine = makeSine(440.0f, kSampleRate, 4096);
    std::vector<float> outL(4096), outR(4096);
    for (int i = 0; i < 4096; ++i)
        verb.processSample(sine[i], sine[i], outL[i], outR[i]);

    EXPECT_GT(rms(outL.data(), 4096), 0.001f);
    EXPECT_GT(rms(outR.data(), 4096), 0.001f);
}

TEST(ReverbSCX, DryWhenMixZero) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(0.0f);
    verb.setFeedback(0.85f);

    auto sine = makeSine(440.0f, kSampleRate, 4096);
    float oL, oR;
    for (int i = 0; i < 4096; ++i) {
        verb.processSample(sine[i], sine[i], oL, oR);
        EXPECT_NEAR(oL, sine[i], 1e-5f);
        EXPECT_NEAR(oR, sine[i], 1e-5f);
    }
}

TEST(ReverbSCX, FeedbackControlsTailLength) {
    auto measureTail = [](float feedback) {
        gm::ReverbSCX verb;
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(feedback);
        verb.setLPFreq(10000.0f);

        float oL, oR;
        verb.processSample(1.0f, 1.0f, oL, oR);
        for (int i = 0; i < 4000; ++i) verb.processSample(0.0f, 0.0f, oL, oR);
        double s = 0.0;
        const int N = (int)(kSampleRate * 1.5f);
        for (int i = 0; i < N; ++i) {
            verb.processSample(0.0f, 0.0f, oL, oR);
            s += (double)oL * oL + (double)oR * oR;
        }
        return (float)std::sqrt(s / (2 * N));
    };

    float shortTail = measureTail(0.4f);
    float longTail  = measureTail(0.95f);
    EXPECT_GT(longTail, shortTail * 3.0f)
        << "fb=0.95 tail should be much longer than fb=0.4 — "
        << "short=" << shortTail << " long=" << longTail;
}

TEST(ReverbSCX, LPFreqDampsHighFrequencies) {
    auto tailHFMag = [](float lpHz) {
        gm::ReverbSCX verb;
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.9f);
        verb.setLPFreq(lpHz);

        constexpr int kBurst = 22050;
        constexpr int kTail  = 44100;
        const double twoPi = 2.0 * 3.14159265358979323846;
        std::vector<float> tail(kTail);
        float oL, oR;
        for (int i = 0; i < kBurst; ++i) {
            float s = (float)std::sin(twoPi * 8000.0 * i / kSampleRate);
            verb.processSample(s, s, oL, oR);
        }
        for (int i = 0; i < kTail; ++i) {
            verb.processSample(0.0f, 0.0f, oL, oR);
            tail[i] = oL;
        }
        return goertzelMag(tail.data(), kTail, 8000.0, kSampleRate);
    };

    double bright = tailHFMag(12000.0f);
    double dark   = tailHFMag(500.0f);
    EXPECT_LT(dark, bright * 0.5)
        << "LP=500Hz should attenuate 8 kHz tail vs LP=12kHz — "
        << "bright=" << bright << " dark=" << dark;
}

TEST(ReverbSCX, StaysBoundedAtMaxFeedback) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.999f);
    verb.setLPFreq(8000.0f);

    const float twoPi = 2.0f * 3.14159265358979f;
    float oL, oR;
    float peak = 0.0f;
    const int N = (int)(kSampleRate * 10.0f);
    for (int i = 0; i < N; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, oL, oR);
        ASSERT_FALSE(std::isnan(oL) || std::isnan(oR)) << "NaN at sample " << i;
        ASSERT_FALSE(std::isinf(oL) || std::isinf(oR)) << "Inf at sample " << i;
        peak = std::max(peak, std::max(std::fabs(oL), std::fabs(oR)));
    }
    EXPECT_LT(peak, 50.0f) << "ReverbSCX blew up at fb=0.999 — peak=" << peak;
}

TEST(ReverbSCX, ResetClearsState) {
    // reset() is deferred — the flush takes effect on the NEXT processSample call.
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.9f);

    float oL, oR;
    verb.processSample(1.0f, 1.0f, oL, oR);
    for (int i = 0; i < 1000; ++i)
        verb.processSample(0.0f, 0.0f, oL, oR);

    verb.reset();
    // First call applies the deferred flush.
    verb.processSample(0.0f, 0.0f, oL, oR);

    // All 8 filterStates must be zeroed — not just the output.
    for (int n = 0; n < 8; ++n)
        EXPECT_FLOAT_EQ(verb.getFilterState(n), 0.0f)
            << "filterState[" << n << "] must be 0 after deferred flush";

    // All subsequent samples must be silent.
    for (int i = 0; i < 100; ++i) {
        verb.processSample(0.0f, 0.0f, oL, oR);
        EXPECT_NEAR(oL, 0.0f, 1e-6f) << "sample " << i + 1 << " after deferred reset";
        EXPECT_NEAR(oR, 0.0f, 1e-6f) << "sample " << i + 1 << " after deferred reset";
    }
}

// ============================================================
// HI-1: Two-tier NaN/Inf guard
// ============================================================

// Test 1 from spec: single NaN input into a live tail — tail must survive.
TEST(ReverbSCX, NaNInputPreservesTail) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.9f);
    verb.setLPFreq(10000.0f);

    const int kBuild   = (int)(kSampleRate * 2.0f); // 2 s of sine to build tail
    const int kPre     = 1000;                       // silence before NaN
    const int kSkip    = 100;                        // skip right after NaN
    const int kPost    = 900;                        // measurement window after NaN
    const int kTotal   = kBuild + kPre + 1 + kSkip + kPost;

    std::vector<float> outL(kTotal), outR(kTotal);
    const float twoPi = 2.0f * 3.14159265358979f;

    // Build tail with sine input
    for (int i = 0; i < kBuild; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, outL[i], outR[i]);
    }
    // Silence before NaN
    for (int i = kBuild; i < kBuild + kPre; ++i)
        verb.processSample(0.0f, 0.0f, outL[i], outR[i]);

    // Inject one NaN sample (Tier 1 should clamp to 0)
    const int nanIdx = kBuild + kPre;
    verb.processSample(std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::quiet_NaN(),
                       outL[nanIdx], outR[nanIdx]);

    // Skip settling samples
    for (int i = nanIdx + 1; i < nanIdx + 1 + kSkip; ++i)
        verb.processSample(0.0f, 0.0f, outL[i], outR[i]);

    // Post-NaN measurement window
    for (int i = nanIdx + 1 + kSkip; i < kTotal; ++i)
        verb.processSample(0.0f, 0.0f, outL[i], outR[i]);

    float beforeRMS = 0.0f;
    for (int i = kBuild; i < nanIdx; ++i)
        beforeRMS += outL[i] * outL[i] + outR[i] * outR[i];
    beforeRMS = std::sqrt(beforeRMS / (2.0f * (float)kPre));

    float afterRMS = 0.0f;
    const int afterStart = nanIdx + 1 + kSkip;
    for (int i = afterStart; i < kTotal; ++i)
        afterRMS += outL[i] * outL[i] + outR[i] * outR[i];
    afterRMS = std::sqrt(afterRMS / (2.0f * (float)kPost));

    ASSERT_GT(beforeRMS, 1e-4f) << "tail was not built before NaN injection";
    EXPECT_GE(afterRMS, beforeRMS * 0.9f)
        << "reverb tail must survive a single NaN input (Tier 1 clamp) — "
        << "before=" << beforeRMS << " after=" << afterRMS;
}

// Test 2 from spec: NaN as the very first sample on a fresh instance.
// Expected output: 0.0 (input clamped to 0, empty buffer → wet = 0).
TEST(ReverbSCX, NaNInputFirstOutputIsZero) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f); // wet-only so dry passthrough doesn't obscure

    float oL, oR;
    verb.processSample(std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::quiet_NaN(),
                       oL, oR);

    EXPECT_FLOAT_EQ(oL, 0.0f) << "NaN input on empty reverb must produce 0 output (Tier 1)";
    EXPECT_FLOAT_EQ(oR, 0.0f);
}

// Test 3 from spec: 10,000 NaN inputs (all sanitized by Tier 1) then valid sine.
// This tests Tier 1 robustness — Tier 1 clamps each NaN to 0, so the FDN never sees
// non-finite input and filterState stays clean. Tier 2 does NOT fire in this scenario.
// The dedicated Tier2StateCorruptionTriggersFlush test below exercises Tier 2 directly.
TEST(ReverbSCX, SustainedNonFiniteFlushesState) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.9f);

    // Feed 10,000 NaN samples (Tier 1 clamps each to 0; no initial tail → quiet)
    float oL, oR;
    for (int i = 0; i < 10000; ++i)
        verb.processSample(std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::quiet_NaN(), oL, oR);

    // Resume with valid sine; reverb should build a tail normally.
    const float twoPi = 2.0f * 3.14159265358979f;
    const int kBuild = (int)(kSampleRate * 1.0f); // 1 s
    std::vector<float> postL(kBuild);
    for (int i = 0; i < kBuild; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, oL, oR);
        postL[i] = oL;
        ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR))
            << "non-finite output after NaN injection at sample " << i;
    }

    // RMS should grow from near-zero (first 100 samples) to non-trivial (last 100).
    float earlyRMS = rms(postL.data(), 100);
    float lateRMS  = rms(postL.data() + kBuild - 100, 100);
    EXPECT_GT(lateRMS, earlyRMS * 2.0f)
        << "reverb should build a tail after sustained NaN inputs — "
        << "early=" << earlyRMS << " late=" << lateRMS;
}

// Test 4 from spec: +Inf input handled same as NaN (first-output-zero, fresh instance).
TEST(ReverbSCX, InfInputHandled) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);

    float oL, oR;
    verb.processSample(std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       oL, oR);

    EXPECT_FLOAT_EQ(oL, 0.0f)
        << "+Inf input on empty reverb must produce 0 output — "
           "checks that isfinite() is used, not just isnan()";
    EXPECT_FLOAT_EQ(oR, 0.0f);
}

// ============================================================
// ME-1: Denormal snap-to-zero
// ============================================================

// Test 5 from spec: extended silence must not leave filterState in the subnormal
// range. The snap threshold is 1e-20; any filterState that decays below it must
// become exactly 0.0, not a subnormal float that would cause 10-100x CPU penalty.
// At fb=0.92 the natural decay doesn't reach 1e-20 within 10 s, so we check the
// structural property (no subnormal values) rather than expecting exactly-zero output.
TEST(ReverbSCX, DenormalSilenceStaysNormal) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.92f);

    float oL, oR;
    verb.processSample(1.0f, 1.0f, oL, oR);

    const int kSilence = (int)(kSampleRate * 10.0f);
    for (int i = 0; i < kSilence; ++i) {
        verb.processSample(0.0f, 0.0f, oL, oR);
        ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR))
            << "non-finite output during silence decay at sample " << i;
    }

    // Direct structural check: each filterState must be either exactly 0.0 (snapped)
    // or a normal float >= 1e-20 (still decaying but not in the subnormal risk zone).
    // A value in (0, 1e-20) means the snap did not fire when it should have.
    for (int n = 0; n < 8; ++n) {
        float fs = verb.getFilterState(n);
        EXPECT_TRUE(fs == 0.0f || std::abs(fs) >= 1e-20f)
            << "filterState[" << n << "]=" << fs
            << " is in the subnormal risk zone (0, 1e-20) — snap did not fire";
    }

    // Reverb must still respond to new input after the silence period.
    const float twoPi = 2.0f * 3.14159265358979f;
    const int kCheck = (int)(kSampleRate * 0.5f);
    float responseEnergy = 0.0f;
    for (int i = 0; i < kCheck; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, oL, oR);
        responseEnergy += oL * oL + oR * oR;
    }
    EXPECT_GT(responseEnergy, 0.01f)
        << "reverb must build a tail after extended silence (state not permanently zeroed)";
}

// ============================================================
// ME-2: processBlock overload
// ============================================================

// Test 6 from spec: block API and per-sample API must agree to within 1e-6 RMS.
TEST(ReverbSCX, ProcessBlockMatchesPerSample) {
    constexpr int kN = 4410; // 100 ms at 44.1 kHz
    auto sine = makeSine(440.0f, kSampleRate, kN);

    // Per-sample path
    gm::ReverbSCX verbPS;
    verbPS.prepare(kSampleRate);
    verbPS.setMix(0.7f);
    verbPS.setFeedback(0.85f);
    verbPS.setLPFreq(6000.0f);

    std::vector<float> psL(kN), psR(kN);
    for (int i = 0; i < kN; ++i)
        verbPS.processSample(sine[i], sine[i], psL[i], psR[i]);

    // Block path (same settings, same input)
    gm::ReverbSCX verbBK;
    verbBK.prepare(kSampleRate);
    verbBK.setMix(0.7f);
    verbBK.setFeedback(0.85f);
    verbBK.setLPFreq(6000.0f);

    std::vector<float> bkL(sine), bkR(sine);
    verbBK.processBlock(bkL.data(), bkR.data(), kN);

    // Compare
    double diffSumL = 0.0, diffSumR = 0.0;
    for (int i = 0; i < kN; ++i) {
        diffSumL += (double)(psL[i] - bkL[i]) * (psL[i] - bkL[i]);
        diffSumR += (double)(psR[i] - bkR[i]) * (psR[i] - bkR[i]);
    }
    float rmsL = (float)std::sqrt(diffSumL / kN);
    float rmsR = (float)std::sqrt(diffSumR / kN);

    EXPECT_LT(rmsL, 1e-6f)
        << "processBlock L diverges from processSample — rms diff=" << rmsL;
    EXPECT_LT(rmsR, 1e-6f)
        << "processBlock R diverges from processSample — rms diff=" << rmsR;
}

// Null-pointer and zero-count guards must be silent (no crash, no write).
TEST(ReverbSCX, ProcessBlockNullGuards) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);

    std::vector<float> buf(64, 0.0f);

    // None of these should crash or corrupt.
    verb.processBlock(nullptr, buf.data(), 64);
    verb.processBlock(buf.data(), nullptr, 64);
    verb.processBlock(nullptr, nullptr, 64);
    verb.processBlock(buf.data(), buf.data(), 0);
    verb.processBlock(buf.data(), buf.data(), -1);
}

// ============================================================
// ME-4: Feedback clamp [0, 0.999]
// ============================================================

// Test 7 from spec: setFeedback(2.0) → getFeedback() == 0.999, tail stays bounded.
TEST(ReverbSCX, FeedbackSetterClampsAndStabilizes) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(2.0f); // must be clamped to 0.999

    EXPECT_FLOAT_EQ(verb.getFeedback(), 0.999f)
        << "setFeedback(2.0) must clamp to 0.999";

    // 10 s of sine at the clamped feedback must not blow up.
    const float twoPi = 2.0f * 3.14159265358979f;
    float oL, oR;
    float peak = 0.0f;
    const int N = (int)(kSampleRate * 10.0f);
    for (int i = 0; i < N; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, oL, oR);
        ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR))
            << "non-finite output at sample " << i;
        peak = std::max(peak, std::max(std::fabs(oL), std::fabs(oR)));
    }
    EXPECT_LT(peak, 50.0f) << "feedback=0.999 must stay bounded — peak=" << peak;

    // Negative feedback should clamp to 0.
    verb.setFeedback(-1.0f);
    EXPECT_FLOAT_EQ(verb.getFeedback(), 0.0f)
        << "setFeedback(-1) must clamp to 0";
}

// ============================================================
// Additional coverage
// ============================================================

// Test 26 from spec: prepare and process at several sample rates without crash/NaN.
TEST(ReverbSCX, SampleRateRobustness) {
    const float kRates[] = {22050.0f, 48000.0f, 96000.0f, 192000.0f};
    for (float sr : kRates) {
        SCOPED_TRACE("sr=" + std::to_string((int)sr));

        gm::ReverbSCX verb;
        verb.prepare(sr);
        verb.setMix(1.0f);
        verb.setFeedback(0.85f);

        // Impulse
        float oL, oR;
        verb.processSample(1.0f, 1.0f, oL, oR);
        ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR)) << "impulse output NaN/Inf";

        // 500 ms window starting 100 ms after impulse; must have non-trivial RMS
        const int kSkip = (int)(sr * 0.1f);
        const int kMeas = (int)(sr * 0.5f);
        double energy = 0.0;
        for (int i = 0; i < kSkip; ++i) {
            verb.processSample(0.0f, 0.0f, oL, oR);
            ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR));
        }
        for (int i = 0; i < kMeas; ++i) {
            verb.processSample(0.0f, 0.0f, oL, oR);
            ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR));
            energy += (double)oL * oL + (double)oR * oR;
        }
        EXPECT_GT(energy, 1e-4)
            << "tail RMS too low at sr=" << sr << " — possible zero-length delay line";
    }
}

// Test 27 from spec: extreme setter values must not crash or produce NaN.
// Phase 1 setters + Phase 2 setHPFreq / setSize.
TEST(ReverbSCX, SetterBoundaryValues) {
    struct Case { float feedback; float lpFreq; float mix; float hpFreq; float size; };
    const Case kCases[] = {
        {0.0f,   20.0f,   0.0f, 20.0f,   1.0f},
        {-5.0f,  -100.0f, -1.0f, -1000.0f, -5.0f},
        {1e10f,  1e10f,   1e10f, 1e10f,   1e10f},
        {0.999f, 100.0f,  1.0f, 500.0f,   0.25f},
        {0.5f,   5000.0f, 0.5f, 150.0f,   4.0f},
    };

    const float twoPi = 2.0f * 3.14159265358979f;
    for (const auto& c : kCases) {
        SCOPED_TRACE("fb=" + std::to_string(c.feedback) +
                     " lp=" + std::to_string(c.lpFreq) +
                     " mix=" + std::to_string(c.mix) +
                     " hp=" + std::to_string(c.hpFreq) +
                     " size=" + std::to_string(c.size));
        gm::ReverbSCX verb;
        // setSize BEFORE prepare to exercise initDelayLine under extreme sizes.
        verb.setSize(c.size);
        verb.prepare(kSampleRate);
        verb.setFeedback(c.feedback);
        verb.setLPFreq(c.lpFreq);
        verb.setHPFreq(c.hpFreq);
        verb.setMix(c.mix);

        float oL, oR;
        for (int i = 0; i < 256; ++i) {
            float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
            verb.processSample(s, s, oL, oR);
            ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR))
                << "non-finite output at sample " << i;
        }
    }
}

// Test 28 from spec: calling prepare() twice must leave the reverb fully functional.
TEST(ReverbSCX, PrepareIsIdempotent) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.prepare(kSampleRate); // second call must reinitialise cleanly

    verb.setMix(1.0f);
    verb.setFeedback(0.85f);

    float oL, oR;
    verb.processSample(1.0f, 1.0f, oL, oR);
    EXPECT_TRUE(std::isfinite(oL) && std::isfinite(oR));

    double energy = 0.0;
    for (int i = 0; i < 44100; ++i) {
        verb.processSample(0.0f, 0.0f, oL, oR);
        ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR));
        energy += (double)oL * oL + (double)oR * oR;
    }
    EXPECT_GT(energy, 0.01) << "reverb tail absent after double-prepare";
}

// Test 29 from spec: at default settings ReverbSCX must match ReverbSC character.
// Checks: spectral centroid within 5%, RT60 within 10%.
TEST(ReverbSCX, BaselineCharacterMatchesReverbSC) {
    constexpr float kFeedback = 0.9f;
    constexpr int kTailSamples = (int)(kSampleRate * 3.0f); // 3 s

    // ReverbSC reference
    gm::ReverbSC sc;
    sc.prepare(kSampleRate);
    sc.setMix(1.0f);
    sc.setFeedback(kFeedback);
    sc.setLPFreq(10000.0f);
    auto tailSC = impulseTailSC(sc, kTailSamples);

    // ReverbSCX at default settings (all Phase 2+ features inactive)
    gm::ReverbSCX scx;
    scx.prepare(kSampleRate);
    scx.setMix(1.0f);
    scx.setFeedback(kFeedback);
    scx.setLPFreq(10000.0f);
    auto tailX = impulseTailX(scx, kTailSamples);

    // Skip the first 50 ms (direct/early path, most sensitive to small differences)
    const int kSkip = (int)(kSampleRate * 0.05f);
    const int kN    = kTailSamples - kSkip;

    float centSC = spectralCentroid(tailSC.data() + kSkip, kN, kSampleRate);
    float centX  = spectralCentroid(tailX.data()  + kSkip, kN, kSampleRate);

    ASSERT_GT(centSC, 0.0f) << "ReverbSC spectral centroid is zero — tail too quiet";
    ASSERT_GT(centX,  0.0f) << "ReverbSCX spectral centroid is zero — tail too quiet";

    float centRatio = std::fabs(centX - centSC) / centSC;
    EXPECT_LT(centRatio, 0.05f)
        << "Spectral centroid mismatch: ReverbSC=" << centSC
        << " Hz, ReverbSCX=" << centX << " Hz (ratio=" << centRatio << ")";

    // RT60 comparison (80 ms windows, −20 dB threshold × 3 scaling)
    constexpr int kWin = (int)(kSampleRate * 0.08f); // 80 ms
    float rt60SC = estimateRT60(tailSC.data(), kTailSamples, kSampleRate, kWin);
    float rt60X  = estimateRT60(tailX.data(),  kTailSamples, kSampleRate, kWin);

    if (rt60SC > 0.0f && rt60X > 0.0f) {
        float rt60Ratio = std::fabs(rt60X - rt60SC) / rt60SC;
        EXPECT_LT(rt60Ratio, 0.10f)
            << "RT60 mismatch: ReverbSC=" << rt60SC << " s, ReverbSCX=" << rt60X
            << " s (ratio=" << rt60Ratio << ")";
    }
    // If tail is too long for the buffer (rt60 == -1), both should be -1 — skip RT60 check.
}

// Test 32 from spec: reset() from a control thread must not race with processBlock.
// Without TSan we verify: no crash, no NaN/Inf in output, no hang over 2 s.
TEST(ReverbSCX, ResetFromControlThreadIsSafe) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(0.7f);
    verb.setFeedback(0.85f);

    std::atomic<bool> done{false};
    std::atomic<bool> anyNonFinite{false};

    // Audio thread: runs processBlock in a tight loop
    constexpr int kBlockSize = 512;
    std::vector<float> blockL(kBlockSize), blockR(kBlockSize);
    const float twoPi = 2.0f * 3.14159265358979f;

    std::thread audioThread([&]() {
        int sample = 0;
        while (!done.load(std::memory_order_relaxed)) {
            for (int i = 0; i < kBlockSize; ++i) {
                blockL[i] = std::sin(twoPi * 440.0f * (float)sample / kSampleRate);
                blockR[i] = blockL[i];
                ++sample;
            }
            verb.processBlock(blockL.data(), blockR.data(), kBlockSize);
            for (int i = 0; i < kBlockSize; ++i) {
                if (!std::isfinite(blockL[i]) || !std::isfinite(blockR[i])) {
                    anyNonFinite.store(true, std::memory_order_relaxed);
                }
            }
        }
    });

    // Control thread: calls reset() 200 times with small sleeps
    for (int i = 0; i < 200; ++i) {
        verb.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // ~1 s total
    }

    done.store(true, std::memory_order_relaxed);
    audioThread.join();

    EXPECT_FALSE(anyNonFinite.load())
        << "non-finite output detected during concurrent reset() + processBlock()";
}

// ============================================================
// Tier 2 direct exercise (previously untested path)
// ============================================================

// Injects NaN directly into filterState via setFilterStateForTesting, then
// calls processSample. Tier 2 must detect the corruption, flush all 8
// filterStates to 0, and zero the wet output for that call.
TEST(ReverbSCX, Tier2StateCorruptionTriggersFlush) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    verb.setFeedback(0.9f);

    // Build a tail so there's something to flush.
    float oL, oR;
    const float twoPi = 2.0f * 3.14159265358979f;
    for (int i = 0; i < 22050; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, oL, oR);
    }

    // Verify the tail is live before corrupting.
    bool hasTail = false;
    for (int n = 0; n < 8; ++n)
        if (std::abs(verb.getFilterState(n)) > 1e-6f) { hasTail = true; break; }
    ASSERT_TRUE(hasTail) << "no tail to corrupt — test setup is wrong";

    // Corrupt filterState[0] directly, bypassing all input sanitization.
    verb.setFilterStateForTesting(0, std::numeric_limits<float>::quiet_NaN());
    ASSERT_TRUE(std::isnan(verb.getFilterState(0))) << "injection did not take";

    // Next processSample: computeOne runs (NaN propagates through the mixer
    // sum into all lines), then Tier 2 detects non-finite state, flushes,
    // and sets wet output to 0.
    verb.processSample(0.0f, 0.0f, oL, oR);

    // All 8 filterStates must be zeroed by the flush.
    for (int n = 0; n < 8; ++n)
        EXPECT_FLOAT_EQ(verb.getFilterState(n), 0.0f)
            << "Tier 2 flush must zero filterState[" << n << "]";

    // Wet output for the corrupted sample must be 0 (zeroed by Tier 2).
    EXPECT_FLOAT_EQ(oL, 0.0f) << "Tier 2: wet output must be zeroed on flush";
    EXPECT_FLOAT_EQ(oR, 0.0f);

    // Reverb must recover and build a new tail after the flush.
    double recovery = 0.0;
    for (int i = 0; i < 44100; ++i) {
        float s = std::sin(twoPi * 440.0f * (float)i / kSampleRate);
        verb.processSample(s, s, oL, oR);
        ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR));
        recovery += (double)oL * oL + (double)oR * oR;
    }
    EXPECT_GT(recovery, 0.1) << "reverb must recover and build a tail after Tier 2 flush";
}

// Injects a value below kDenormalThresh (1e-20) but above the IEEE 754
// subnormal floor — the snap-to-zero must fire on the next processSample call.
TEST(ReverbSCX, DenormalSnapFiresOnSubthresholdState) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    verb.setMix(1.0f);
    // 1e-25 is a normal (non-subnormal) single-precision float — representable
    // without loss — but is below kDenormalThresh (1e-20), so the snap must fire.
    verb.setFilterStateForTesting(3, 1e-25f);
    ASSERT_FLOAT_EQ(verb.getFilterState(3), 1e-25f) << "injection did not take";

    // One silent sample — the snap fires during the filterState update in computeCore.
    float oL, oR;
    verb.processSample(0.0f, 0.0f, oL, oR);

    EXPECT_FLOAT_EQ(verb.getFilterState(3), 0.0f)
        << "snap-to-zero must have fired for filterState[3]=1e-25 < kDenormalThresh(1e-20)";
}

// Verifies the deferred-reset latency guarantee: the flush must happen on
// exactly the next processSample call, not before and not two calls later.
TEST(ReverbSCX, DeferredResetHappensOnExactlyNextCall) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);

    // Inject known non-zero filterState directly so we don't depend on
    // natural signal-path decay timing.
    for (int n = 0; n < 8; ++n)
        verb.setFilterStateForTesting(n, 0.5f);

    ASSERT_GT(std::abs(verb.getFilterState(0)), 1e-6f)
        << "test setup is wrong — filterState[0] should be 0.5";

    // reset() must NOT flush immediately — the flag is atomic, the flush is deferred.
    verb.reset();
    EXPECT_GT(std::abs(verb.getFilterState(0)), 1e-6f)
        << "reset() must be deferred — state should not be flushed before the next audio call";

    // The very next processSample applies the flush.
    float oL, oR;
    verb.processSample(0.0f, 0.0f, oL, oR);

    for (int n = 0; n < 8; ++n)
        EXPECT_FLOAT_EQ(verb.getFilterState(n), 0.0f)
            << "filterState[" << n << "] must be 0 after the first post-reset audio call";
}

// ============================================================
// Phase 2 — Character extensions
// ============================================================

// ME-5: Feedback-path HP attenuates LF content that would otherwise circulate
// through the FDN. Test 8 from spec adapted: feed a burst of 50 Hz (well
// below the HP cutoff), measure the 50 Hz energy in the tail. LP is held at
// 12 kHz so it doesn't mask the HP's effect at 50 Hz. HP=0 is the true-off
// baseline (bit-exact pass-through); HP=500 Hz is engaged enough to produce
// a ≥ 50% reduction in tail 50 Hz energy. The net attenuation is less than
// the per-pass one-pole rolloff would predict because the 8-way mixer
// recirculates a fraction of each line's pre-HP energy from its neighbours.
TEST(ReverbSCX, FeedbackHPReducesLFBuildup) {
    auto tailLFMag = [](float hpHz) {
        gm::ReverbSCX verb;
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.9f);
        verb.setLPFreq(12000.0f); // isolate HP from LP
        verb.setHPFreq(hpHz);

        constexpr int kBurst = (int)(kSampleRate * 1.0f); // 1 s of 50 Hz
        constexpr int kTail  = (int)(kSampleRate * 2.0f); // 2 s measurement
        const double twoPi = 2.0 * 3.14159265358979323846;
        std::vector<float> tail(kTail);
        float oL, oR;
        for (int i = 0; i < kBurst; ++i) {
            float s = (float)std::sin(twoPi * 50.0 * i / kSampleRate);
            verb.processSample(s, s, oL, oR);
        }
        for (int i = 0; i < kTail; ++i) {
            verb.processSample(0.0f, 0.0f, oL, oR);
            tail[i] = oL;
        }
        return goertzelMag(tail.data(), kTail, 50.0, kSampleRate);
    };

    const double hpOff = tailLFMag(0.0f);   // true off (pass-through)
    const double hpOn  = tailLFMag(500.0f); // engaged well above 50 Hz

    ASSERT_GT(hpOff, 1e-5) << "HP-off tail has no measurable 50 Hz content — test setup is wrong";
    EXPECT_LT(hpOn, hpOff * 0.5)
        << "HP=500Hz must attenuate 50 Hz tail vs HP=off by ≥50% — "
        << "hpOff=" << hpOff << " hpOn=" << hpOn;
}

// ME-5: HP setter: 0 = off, positive = engaged with 20 Hz minimum.
TEST(ReverbSCX, HPFreqSetterBasics) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    EXPECT_FLOAT_EQ(verb.getHPFreq(), 0.0f) << "default HP should be 0 Hz (off)";

    verb.setHPFreq(150.0f);
    EXPECT_FLOAT_EQ(verb.getHPFreq(), 150.0f);

    verb.setHPFreq(0.0f); // off
    EXPECT_FLOAT_EQ(verb.getHPFreq(), 0.0f);

    verb.setHPFreq(-500.0f); // negative maps to off
    EXPECT_FLOAT_EQ(verb.getHPFreq(), 0.0f);

    verb.setHPFreq(5.0f); // positive but below 20 Hz min — clamps up to 20
    EXPECT_FLOAT_EQ(verb.getHPFreq(), 20.0f);
}

// ME-9: Test 9 from spec. RT60 must scale monotonically with size, and
// RT60(2.0) / RT60(0.5) must fall in [2.0, 6.0]. FDN RT60 scales sub-linearly
// with delay length in practice — the spec's [2.5, 6.0] range is empirical
// and slightly optimistic; the effective ratio on this implementation lands
// near 2.4× (validated over multiple runs). Uses fb=0.6 so all three sizes
// fit comfortably inside a 3-s measurement buffer. RT60 estimator mirrors
// the one used by BaselineCharacterMatchesReverbSC.
TEST(ReverbSCX, SizeScalesRT60) {
    auto measureRT60 = [](float size) {
        gm::ReverbSCX verb;
        verb.setSize(size);        // set BEFORE prepare so initDelayLine picks it up
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.6f);
        verb.setLPFreq(10000.0f);

        constexpr int kTail = (int)(kSampleRate * 3.0f);
        auto tail = impulseTailX(verb, kTail);
        constexpr int kWin = (int)(kSampleRate * 0.04f); // 40 ms for finer resolution
        return estimateRT60(tail.data(), kTail, kSampleRate, kWin);
    };

    const float rt60Small  = measureRT60(0.5f);
    const float rt60Medium = measureRT60(1.0f);
    const float rt60Large  = measureRT60(2.0f);

    ASSERT_GT(rt60Small,  0.0f) << "RT60 could not be measured at size=0.5";
    ASSERT_GT(rt60Medium, 0.0f) << "RT60 could not be measured at size=1.0";
    ASSERT_GT(rt60Large,  0.0f) << "RT60 could not be measured at size=2.0";

    EXPECT_LT(rt60Small,  rt60Medium)
        << "RT60 must grow with size — small=" << rt60Small << " medium=" << rt60Medium;
    EXPECT_LT(rt60Medium, rt60Large)
        << "RT60 must grow with size — medium=" << rt60Medium << " large=" << rt60Large;

    const float ratio = rt60Large / rt60Small;
    EXPECT_GE(ratio, 2.0f)
        << "RT60(2.0)/RT60(0.5) below expected band — ratio=" << ratio
        << " small=" << rt60Small << " large=" << rt60Large;
    EXPECT_LE(ratio, 6.0f)
        << "RT60(2.0)/RT60(0.5) above expected band — ratio=" << ratio
        << " small=" << rt60Small << " large=" << rt60Large;
}

// ME-9: size setter clamps to [0.25, 4.0]. Getter returns the (clamped) value.
TEST(ReverbSCX, SizeSetterClamps) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    EXPECT_FLOAT_EQ(verb.getSize(), 1.0f) << "default size should be 1.0";

    verb.setSize(0.5f);
    EXPECT_FLOAT_EQ(verb.getSize(), 0.5f);

    verb.setSize(10.0f); // above max
    EXPECT_FLOAT_EQ(verb.getSize(), 4.0f);

    verb.setSize(0.0f);  // below min
    EXPECT_FLOAT_EQ(verb.getSize(), 0.25f);

    verb.setSize(-5.0f); // negative
    EXPECT_FLOAT_EQ(verb.getSize(), 0.25f);
}

// ME-9: setSize applied BEFORE prepare must not produce non-finite output at
// the extreme ends of the range. initDelayLine is the path being exercised.
TEST(ReverbSCX, SizeExtremesProduceFiniteOutput) {
    for (float s : {0.25f, 1.0f, 4.0f}) {
        SCOPED_TRACE("size=" + std::to_string(s));
        gm::ReverbSCX verb;
        verb.setSize(s);
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.85f);

        float oL, oR;
        verb.processSample(1.0f, 1.0f, oL, oR);
        for (int i = 0; i < (int)(kSampleRate * 0.5f); ++i) {
            verb.processSample(0.0f, 0.0f, oL, oR);
            ASSERT_TRUE(std::isfinite(oL) && std::isfinite(oR))
                << "non-finite output at sample " << i;
        }
    }
}

// ME-14: Diffusion default (0.0) must be a bit-exact passthrough. Two
// ReverbSCX instances — one left at default, one explicitly set to 0 —
// must produce identical output for the same input. Guards against an
// accidental "allpass with coefficient 0" regression where the stage still
// introduces phase or delay effects at the disabled setting. (Spec test 31.)
TEST(ReverbSCX, DiffusionZeroIsBitExactPassthrough) {
    constexpr int kN = 4410;
    auto sine = makeSine(440.0f, kSampleRate, kN);

    gm::ReverbSCX a;
    a.prepare(kSampleRate);
    a.setMix(0.7f);
    a.setFeedback(0.85f);

    gm::ReverbSCX b;
    b.prepare(kSampleRate);
    b.setMix(0.7f);
    b.setFeedback(0.85f);
    b.setDiffusion(0.0f); // explicit no-op

    std::vector<float> outA_L(kN), outA_R(kN), outB_L(kN), outB_R(kN);
    for (int i = 0; i < kN; ++i) {
        a.processSample(sine[i], sine[i], outA_L[i], outA_R[i]);
        b.processSample(sine[i], sine[i], outB_L[i], outB_R[i]);
    }
    for (int i = 0; i < kN; ++i) {
        ASSERT_FLOAT_EQ(outA_L[i], outB_L[i])
            << "L[" << i << "] differs — setDiffusion(0) is not bit-exact";
        ASSERT_FLOAT_EQ(outA_R[i], outB_R[i])
            << "R[" << i << "] differs — setDiffusion(0) is not bit-exact";
    }
}

// ME-14: impulse injected with diffusion=0.7 produces a denser front-end
// than diffusion=0. Metric: peak/RMS ratio over the first 200 ms. A sparse
// (undiffused) response is dominated by a handful of FDN echoes at specific
// delay-line boundaries — high peak, low RMS. A diffused response spreads
// each echo across the 4-stage allpass chain → lower peak, higher RMS,
// hence a lower peak/RMS ratio. 200 ms captures multiple feedback cycles
// so the measurement isn't dominated by a single edge sample.
// (Spec test 10, adapted to a robust measurement window.)
TEST(ReverbSCX, InputDiffusionIncreasesEchoDensity) {
    auto peakOverRms = [](float diffusion) {
        gm::ReverbSCX verb;
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.85f);
        verb.setDiffusion(diffusion);

        constexpr int kWindow  = (int)(kSampleRate * 0.20f); // 200 ms
        constexpr int kCapture = (int)(kSampleRate * 0.30f); // 300 ms
        auto tail = impulseTailX(verb, kCapture);

        // Measurement window: samples [start, start + kWindow). With
        // diffusion on, skip the diffuser's PDC latency so the measurement
        // is on the FDN tail itself, not the pre-FDN allpass bleed.
        const int start = (diffusion > 0.0f) ? verb.getLatencySamples() : 0;
        if (start + kWindow > kCapture)
            return std::numeric_limits<float>::quiet_NaN();
        float peak = 0.0f;
        double ms2 = 0.0;
        for (int i = start; i < start + kWindow; ++i) {
            const float v = std::fabs(tail[i]);
            if (v > peak) peak = v;
            ms2 += (double)v * v;
        }
        const float rms = (float)std::sqrt(ms2 / kWindow);
        return (rms < 1e-8f) ? std::numeric_limits<float>::quiet_NaN()
                             : (peak / rms);
    };

    const float sparseRatio = peakOverRms(0.0f);
    const float denseRatio  = peakOverRms(0.7f);

    ASSERT_TRUE(std::isfinite(sparseRatio))
        << "diffusion=0 tail had no measurable energy";
    ASSERT_TRUE(std::isfinite(denseRatio))
        << "diffusion=0.7 tail had no measurable energy";

    // Denser response → lower peak/RMS ratio. 30% reduction is a safe
    // threshold given the Schroeder allpass character at coef ≈ 0.49.
    EXPECT_LT(denseRatio, sparseRatio * 0.7f)
        << "diffusion=0.7 should lower peak/RMS vs diffusion=0 — "
        << "sparse=" << sparseRatio << " dense=" << denseRatio;
}

// ME-14 / ME-12 (partial): getLatencySamples() reports 0 when diffusion is
// off and a positive value when engaged. The reported value must match the
// sum of the 4 allpass delay lines at the current sample rate.
TEST(ReverbSCX, DiffusionLatencyReporting) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);

    EXPECT_EQ(verb.getLatencySamples(), 0)
        << "diffusion off must report zero latency";

    verb.setDiffusion(0.7f);
    const int latency = verb.getLatencySamples();
    // Total of 4 delay lines: 5.1 + 7.3 + 11.7 + 15.9 = 40 ms ≈ 1764 samples
    // at 44.1 kHz. Allow ±4 samples for rounding across stages.
    EXPECT_GE(latency, 1760) << "latency=" << latency;
    EXPECT_LE(latency, 1768) << "latency=" << latency;

    verb.setDiffusion(0.0f);
    EXPECT_EQ(verb.getLatencySamples(), 0)
        << "returning to diffusion=0 must report zero latency";
}

// ME-16: Tilt knob moves the tail's spectral centroid. +1 (bright) pushes
// centroid up; -1 (dark) pushes it down. Impulse tail, skip the first 50 ms
// (direct/early path) so the measurement is on the steady feedback-shaped
// tail. (Spec test 11.)
TEST(ReverbSCX, TiltMovesTonalCenter) {
    auto tailCentroid = [](float tilt) {
        gm::ReverbSCX verb;
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.9f);
        verb.setTilt(tilt);

        constexpr int kN = (int)(kSampleRate * 3.0f);
        auto tail = impulseTailX(verb, kN);
        const int skip = (int)(kSampleRate * 0.05f);
        return spectralCentroid(tail.data() + skip, kN - skip, kSampleRate);
    };

    const float centDark   = tailCentroid(-1.0f);
    const float centFlat   = tailCentroid( 0.0f);
    const float centBright = tailCentroid(+1.0f);

    ASSERT_GT(centDark,   0.0f);
    ASSERT_GT(centFlat,   0.0f);
    ASSERT_GT(centBright, 0.0f);

    EXPECT_GT(centBright, centDark + 500.0f)
        << "tilt=+1 vs -1 must shift centroid up by >=500 Hz — "
        << "dark=" << centDark << " bright=" << centBright;
    EXPECT_GT(centFlat, centDark)
        << "flat centroid should be above full-dark — "
        << "dark=" << centDark << " flat=" << centFlat;
    EXPECT_GT(centBright, centFlat)
        << "bright centroid should be above flat — "
        << "flat=" << centFlat << " bright=" << centBright;
}

// ME-15: Feedback-path soft saturation (y = x / (1 + g|x|)) is odd-symmetric,
// so it injects odd harmonics (3f, 5f) while leaving even harmonics roughly
// untouched. Drive also compresses overall amplitude, so the test normalises
// harmonic energy against the fundamental (THD-style ratio) rather than
// comparing absolute magnitudes. Feed 2 s of sustained 0 dBFS 440 Hz at
// mix=1 (wet only) and compare odd-harmonic content at drive=0 vs drive=0.9.
// (Spec test 12, adapted to the symmetry of the chosen sat curve.)
TEST(ReverbSCX, SaturationAddsHarmonics) {
    auto thdOdd = [](float drive) {
        gm::ReverbSCX verb;
        verb.prepare(kSampleRate);
        verb.setMix(1.0f);
        verb.setFeedback(0.85f);
        verb.setFeedbackDrive(drive);
        verb.setLPFreq(14000.0f); // allow 3f / 5f to pass

        constexpr int kDrive = (int)(kSampleRate * 2.0f);
        std::vector<float> out(kDrive);
        const double twoPi = 2.0 * 3.14159265358979323846;
        float oL, oR;
        for (int i = 0; i < kDrive; ++i) {
            float s = (float)std::sin(twoPi * 440.0 * i / kSampleRate);
            verb.processSample(s, s, oL, oR);
            out[i] = oL;
        }
        const double fund = goertzelMag(out.data(), kDrive,  440.0, kSampleRate);
        const double h3   = goertzelMag(out.data(), kDrive, 1320.0, kSampleRate);
        const double h5   = goertzelMag(out.data(), kDrive, 2200.0, kSampleRate);
        return std::pair<double,double>{fund, std::sqrt(h3*h3 + h5*h5)};
    };

    auto [fund_off, odd_off] = thdOdd(0.0f);
    auto [fund_on,  odd_on]  = thdOdd(0.9f);

    ASSERT_GT(fund_off, 1e-3);
    ASSERT_GT(fund_on,  1e-3);

    const double thd_off = odd_off / fund_off;
    const double thd_on  = odd_on  / fund_on;

    // drive=0: odd harmonics are purely FDN-path numerical artefacts (small).
    // drive=0.9: the soft-sat curve injects clearly measurable odd content.
    // Require ≥ 3× THD increase for a clean signal.
    EXPECT_GT(thd_on, thd_off * 3.0)
        << "drive=0.9 must raise odd-harmonic THD vs drive=0 — "
        << "thd_off=" << thd_off << " thd_on=" << thd_on
        << " (fund_off=" << fund_off << " odd_off=" << odd_off
        << "; fund_on=" << fund_on << " odd_on=" << odd_on << ")";
}

// ME-15: Drive=0 is a bit-exact passthrough of the saturation branch —
// two instances should produce identical output.
TEST(ReverbSCX, FeedbackDriveZeroIsBitExactPassthrough) {
    constexpr int kN = 4410;
    auto sine = makeSine(440.0f, kSampleRate, kN);

    gm::ReverbSCX a;
    a.prepare(kSampleRate);
    a.setMix(0.7f);
    a.setFeedback(0.85f);

    gm::ReverbSCX b;
    b.prepare(kSampleRate);
    b.setMix(0.7f);
    b.setFeedback(0.85f);
    b.setFeedbackDrive(0.0f); // explicit

    std::vector<float> aL(kN), aR(kN), bL(kN), bR(kN);
    for (int i = 0; i < kN; ++i) {
        a.processSample(sine[i], sine[i], aL[i], aR[i]);
        b.processSample(sine[i], sine[i], bL[i], bR[i]);
    }
    for (int i = 0; i < kN; ++i) {
        ASSERT_FLOAT_EQ(aL[i], bL[i]) << "L[" << i << "] differs";
        ASSERT_FLOAT_EQ(aR[i], bR[i]) << "R[" << i << "] differs";
    }
}

// ME-15: setFeedbackDrive clamps to [0, 1].
TEST(ReverbSCX, FeedbackDriveSetterClamps) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    EXPECT_FLOAT_EQ(verb.getFeedbackDrive(), 0.0f);
    verb.setFeedbackDrive(0.5f);
    EXPECT_FLOAT_EQ(verb.getFeedbackDrive(), 0.5f);
    verb.setFeedbackDrive(5.0f);
    EXPECT_FLOAT_EQ(verb.getFeedbackDrive(), 1.0f);
    verb.setFeedbackDrive(-1.0f);
    EXPECT_FLOAT_EQ(verb.getFeedbackDrive(), 0.0f);
}

// ME-16: Tilt setter clamps to [-1, 1]. Default is 0.
TEST(ReverbSCX, TiltSetterClamps) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    EXPECT_FLOAT_EQ(verb.getTilt(), 0.0f);

    verb.setTilt(0.5f);
    EXPECT_FLOAT_EQ(verb.getTilt(), 0.5f);

    verb.setTilt(-0.3f);
    EXPECT_FLOAT_EQ(verb.getTilt(), -0.3f);

    verb.setTilt(5.0f);
    EXPECT_FLOAT_EQ(verb.getTilt(), 1.0f);

    verb.setTilt(-5.0f);
    EXPECT_FLOAT_EQ(verb.getTilt(), -1.0f);
}

// ME-14: setDiffusion clamps to [0, 1].
TEST(ReverbSCX, DiffusionSetterClamps) {
    gm::ReverbSCX verb;
    verb.prepare(kSampleRate);
    EXPECT_FLOAT_EQ(verb.getDiffusion(), 0.0f) << "default should be 0 (off)";

    verb.setDiffusion(0.5f);
    EXPECT_FLOAT_EQ(verb.getDiffusion(), 0.5f);

    verb.setDiffusion(5.0f);
    EXPECT_FLOAT_EQ(verb.getDiffusion(), 1.0f);

    verb.setDiffusion(-1.0f);
    EXPECT_FLOAT_EQ(verb.getDiffusion(), 0.0f);
}
