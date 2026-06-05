#pragma once
// ghostmoon DSP: ReverbSC
// Sean Costello's 8-delay-line stereo FDN reverb (Csound reverbsc opcode).
// Origin: Poompatoom ReverbSC.h, via Breadbin, via Soundpipe (Paul Batchelor).
//
// Original algorithm: Sean Costello & Istvan Varga (Csound, 1999/2005)
// C extraction: Paul Batchelor (Soundpipe library)
//
// SPDX-License-Identifier: LGPL-2.1-or-later
// License: GNU Lesser General Public License v2.1 or later.
//   CORRECTION (2026-06): previously mislabeled MIT. This code is "extracted
//   from" the Csound `reverbsc` opcode, which is LGPL-2.1-or-later
//   (Copyright (c) 1999, 2005 Sean Costello & Istvan Varga). Soundpipe's
//   top-level MIT relicensing does NOT validly cover this Csound-derived
//   module, so it is distributed here under its actual license. See
//   ../../../LICENSE for full terms.
//
// Zero JUCE dependency. Header-only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gm {

class ReverbSC {
public:
    ReverbSC() = default;
    ~ReverbSC() { destroy(); }

    ReverbSC(const ReverbSC&) = delete;
    ReverbSC& operator=(const ReverbSC&) = delete;

    void prepare(float sampleRate) {
        destroy();
        rev_.sampleRate = sampleRate;
        rev_.feedback = 0.97f;
        rev_.lpfreq = 10000.0f;
        rev_.iPitchMod = 1.0f;
        rev_.dampFact = 1.0f;
        rev_.prv_LPFreq = 0.0f;
        rev_.initDone = 1;

        int nBytes = 0;
        for (int i = 0; i < 8; i++)
            nBytes += delayLineBytes(sampleRate, 1.0f, i);

        rev_.auxBuf = static_cast<float*>(std::malloc(nBytes));
        if (!rev_.auxBuf) return;
        std::memset(rev_.auxBuf, 0, nBytes);

        int offset = 0;
        for (int i = 0; i < 8; i++) {
            rev_.delayLines[i].buf =
                reinterpret_cast<float*>(reinterpret_cast<char*>(rev_.auxBuf) + offset);
            initDelayLine(i);
            offset += delayLineBytes(sampleRate, 1.0f, i);
        }

        initialized_ = true;
    }

    /// Feedback gain (0.0-1.0, typical 0.4-0.97)
    void setFeedback(float fb) { rev_.feedback = std::max(0.0f, std::min(1.0f, fb)); }

    /// Lowpass filter frequency in feedback path (Hz)
    void setLPFreq(float hz) { rev_.lpfreq = std::max(20.0f, hz); }

    /// Wet/dry mix 0..1
    void setMix(float m) { mix_ = std::max(0.0f, std::min(1.0f, m)); }

    void processSample(float inL, float inR, float& outL, float& outR) {
        if (!initialized_) {
            outL = inL;
            outR = inR;
            return;
        }

        float wetL, wetR;
        compute(inL, inR, wetL, wetR);

        outL = inL * (1.0f - mix_) + wetL * mix_;
        outR = inR * (1.0f - mix_) + wetR * mix_;
    }

    void reset() {
        if (!initialized_) return;
        // Zero all delay buffers and filter states
        for (int i = 0; i < 8; i++) {
            auto& lp = rev_.delayLines[i];
            if (lp.buf)
                std::memset(lp.buf, 0, sizeof(float) * lp.bufferSize);
            lp.filterState = 0.0f;
        }
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kOutputGain = 0.7f;
    static constexpr float kJpScale = 0.25f;
    static constexpr float kDefaultSRate = 44100.0f;
    static constexpr int kDelayPosShift = 28;
    static constexpr int kDelayPosScale = 0x10000000;
    static constexpr int kDelayPosMask = 0x0FFFFFFF;

    // Sean Costello's original delay line tuning
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
        int writePos = 0;
        int bufferSize = 0;
        int readPos = 0;
        int readPosFrac = 0;
        int readPosFrac_inc = 0;
        int seedVal = 0;
        int randLine_cnt = 0;
        float filterState = 0.0f;
        float* buf = nullptr;
    };

    struct RevState {
        float feedback = 0.97f;
        float lpfreq = 10000.0f;
        float sampleRate = 44100.0f;
        float iPitchMod = 1.0f;
        float dampFact = 1.0f;
        float prv_LPFreq = 0.0f;
        int initDone = 0;
        DelayLine delayLines[8] = {};
        float* auxBuf = nullptr;
    };

    RevState rev_;
    float mix_ = 0.5f;
    bool initialized_ = false;

    void destroy() {
        if (rev_.auxBuf) {
            std::free(rev_.auxBuf);
            rev_.auxBuf = nullptr;
        }
        initialized_ = false;
        rev_.initDone = 0;
    }

    void compute(float inL, float inR, float& outL, float& outR) {
        float dampFact = rev_.dampFact;

        if (rev_.lpfreq != rev_.prv_LPFreq) {
            rev_.prv_LPFreq = rev_.lpfreq;
            dampFact = 2.0f - std::cos(rev_.prv_LPFreq * (2.0f * kPi) / rev_.sampleRate);
            dampFact = rev_.dampFact = dampFact - std::sqrt(dampFact * dampFact - 1.0f);
        }

        float ainL = 0.0f;
        float aoutL = 0.0f, aoutR = 0.0f;
        for (int n = 0; n < 8; n++)
            ainL += rev_.delayLines[n].filterState;

        ainL *= kJpScale;
        float ainR = ainL + inR;
        ainL = ainL + inL;

        for (int n = 0; n < 8; n++) {
            DelayLine& lp = rev_.delayLines[n];
            int bufSize = lp.bufferSize;

            lp.buf[lp.writePos] = ((n & 1) ? ainR : ainL) - lp.filterState;
            if (++lp.writePos >= bufSize)
                lp.writePos -= bufSize;

            if (lp.readPosFrac >= kDelayPosScale) {
                lp.readPos += (lp.readPosFrac >> kDelayPosShift);
                lp.readPosFrac &= kDelayPosMask;
            }
            if (lp.readPos >= bufSize)
                lp.readPos -= bufSize;

            int readPos = lp.readPos;
            float frac = static_cast<float>(lp.readPosFrac) *
                         (1.0f / static_cast<float>(kDelayPosScale));

            // Cubic interpolation
            float a2 = frac * frac;
            a2 -= 1.0f;
            a2 *= (1.0f / 6.0f);
            float a1 = frac;
            a1 += 1.0f;
            a1 *= 0.5f;
            float am1 = a1 - 1.0f;
            float a0 = 3.0f * a2;
            a1 -= a0;
            am1 -= a2;
            a0 -= frac;

            float vm1, v0, v1, v2;
            if (readPos > 0 && readPos < (bufSize - 2)) {
                vm1 = lp.buf[readPos - 1];
                v0 = lp.buf[readPos];
                v1 = lp.buf[readPos + 1];
                v2 = lp.buf[readPos + 2];
            } else {
                if (--readPos < 0) readPos += bufSize;
                vm1 = lp.buf[readPos];
                if (++readPos >= bufSize) readPos -= bufSize;
                v0 = lp.buf[readPos];
                if (++readPos >= bufSize) readPos -= bufSize;
                v1 = lp.buf[readPos];
                if (++readPos >= bufSize) readPos -= bufSize;
                v2 = lp.buf[readPos];
            }
            v0 = (am1 * vm1 + a0 * v0 + a1 * v1 + a2 * v2) * frac + v0;

            lp.readPosFrac += lp.readPosFrac_inc;

            v0 *= rev_.feedback;
            v0 = (lp.filterState - v0) * dampFact + v0;
            lp.filterState = v0;

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

    static int delayLineMaxSamples(float sr, float iPitchMod, int n) {
        float maxDel = kParams[n][0] + (kParams[n][1] * iPitchMod * 1.125f);
        return static_cast<int>(maxDel * sr + 16.5f);
    }

    static int delayLineBytes(float sr, float iPitchMod, int n) {
        return delayLineMaxSamples(sr, iPitchMod, n) * static_cast<int>(sizeof(float));
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
        nxtDel = kParams[n][0] + (nxtDel * rev_.iPitchMod);

        float phs_incVal = (prvDel - nxtDel) / static_cast<float>(lp.randLine_cnt);
        phs_incVal = phs_incVal * rev_.sampleRate + 1.0f;
        lp.readPosFrac_inc = static_cast<int>(phs_incVal * kDelayPosScale + 0.5f);
    }

    void initDelayLine(int n) {
        DelayLine& lp = rev_.delayLines[n];
        lp.bufferSize = delayLineMaxSamples(rev_.sampleRate, 1.0f, n);
        lp.writePos = 0;
        lp.seedVal = static_cast<int>(kParams[n][3] + 0.5f);

        float readPos = static_cast<float>(lp.seedVal) * kParams[n][1] / 32768.0f;
        readPos = kParams[n][0] + (readPos * rev_.iPitchMod);
        readPos = static_cast<float>(lp.bufferSize) - (readPos * rev_.sampleRate);
        lp.readPos = static_cast<int>(readPos);
        float fracPart = (readPos - static_cast<float>(lp.readPos)) *
                         static_cast<float>(kDelayPosScale);
        lp.readPosFrac = static_cast<int>(fracPart + 0.5f);

        nextRandomLineSeg(n);
        lp.filterState = 0.0f;
        std::memset(lp.buf, 0, sizeof(float) * lp.bufferSize);
    }
};

} // namespace gm
