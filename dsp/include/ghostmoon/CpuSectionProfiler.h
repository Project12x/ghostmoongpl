// SPDX-License-Identifier: MIT
#pragma once
// ghostmoon DSP: CpuSectionProfiler
//
// First-party ghostmoon-oss export for GPL-compatible plugin test/profile
// harnesses. Reuse mode: direct-copy from full ghostmoon
// dsp/include/ghostmoon/CpuSectionProfiler.h at
// f521679b8bd0946bc48b0941c45945e1c65d2369.
//
// Per-section CPU attribution for audio callbacks. Register named sections
// once at construction, wrap stages of the audio callback, and read aggregated
// statistics (total / calls / max) from any other thread.
//
// Threading contract:
//  - registerSection(): message/setup thread only, before audio runs.
//  - ScopedSection zones: any thread (designed for the audio thread).
//    Cost when enabled: two steady_clock reads + three relaxed atomic RMWs.
//    Cost when disabled: one relaxed atomic load (the enable check).
//  - snapshot(): any non-RT thread. Optionally resets accumulators.
//  - No allocation anywhere after construction; section names must be
//    string literals (or otherwise outlive the profiler).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace gm {

class CpuSectionProfiler {
public:
  static constexpr int kMaxSections = 24;

  using Clock = std::chrono::steady_clock;

  CpuSectionProfiler() = default;
  CpuSectionProfiler(const CpuSectionProfiler &) = delete;
  CpuSectionProfiler &operator=(const CpuSectionProfiler &) = delete;

  int registerSection(const char *name) {
    const int idx = numSections_;
    if (name == nullptr || idx >= kMaxSections)
      return -1;
    sections_[idx].name = name;
    numSections_ = idx + 1;
    return idx;
  }

  int getNumSections() const { return numSections_; }

  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

  class ScopedSection {
  public:
    ScopedSection(CpuSectionProfiler &p, int index) noexcept {
      if (index >= 0 && index < p.numSections_ &&
          p.enabled_.load(std::memory_order_relaxed)) {
        profiler_ = &p;
        index_ = index;
        start_ = Clock::now();
      }
    }

    ~ScopedSection() noexcept {
      if (profiler_ == nullptr)
        return;
      const auto ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - start_)
              .count());
      accumulate(profiler_->sections_[index_], ns);
    }

    ScopedSection(const ScopedSection &) = delete;
    ScopedSection &operator=(const ScopedSection &) = delete;

  private:
    CpuSectionProfiler *profiler_ = nullptr;
    int index_ = 0;
    Clock::time_point start_{};
  };

  void beginSection(int index) {
    if (index < 0 || index >= numSections_ ||
        !enabled_.load(std::memory_order_relaxed))
      return;
    sections_[index].start = Clock::now();
    sections_[index].started = true;
  }

  void endSection(int index) {
    if (index < 0 || index >= numSections_)
      return;
    auto &s = sections_[index];
    if (!s.started)
      return;
    s.started = false;
    accumulate(s, static_cast<uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          Clock::now() - s.start)
                          .count()));
  }

  struct Snapshot {
    const char *name = nullptr;
    uint64_t totalNs = 0;
    uint64_t calls = 0;
    uint64_t maxNs = 0;

    double avgUs() const {
      return calls > 0 ? static_cast<double>(totalNs) /
                             static_cast<double>(calls) / 1000.0
                       : 0.0;
    }
    double maxUs() const { return static_cast<double>(maxNs) / 1000.0; }
  };

  int snapshot(Snapshot *out, int maxOut, bool reset = false) {
    const int n = std::min(numSections_, maxOut);
    for (int i = 0; i < n; ++i) {
      auto &s = sections_[i];
      out[i].name = s.name;
      if (reset) {
        out[i].totalNs = s.totalNs.exchange(0, std::memory_order_relaxed);
        out[i].calls = s.calls.exchange(0, std::memory_order_relaxed);
        out[i].maxNs = s.maxNs.exchange(0, std::memory_order_relaxed);
      } else {
        out[i].totalNs = s.totalNs.load(std::memory_order_relaxed);
        out[i].calls = s.calls.load(std::memory_order_relaxed);
        out[i].maxNs = s.maxNs.load(std::memory_order_relaxed);
      }
    }
    return n;
  }

  void reset() {
    for (int i = 0; i < numSections_; ++i) {
      sections_[i].totalNs.store(0, std::memory_order_relaxed);
      sections_[i].calls.store(0, std::memory_order_relaxed);
      sections_[i].maxNs.store(0, std::memory_order_relaxed);
    }
  }

private:
  struct Section {
    const char *name = nullptr;
    std::atomic<uint64_t> totalNs{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> maxNs{0};
    Clock::time_point start{};
    bool started = false;
  };

  static void accumulate(Section &s, uint64_t ns) noexcept {
    s.totalNs.fetch_add(ns, std::memory_order_relaxed);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = s.maxNs.load(std::memory_order_relaxed);
    while (ns > prev &&
           !s.maxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
  }

  Section sections_[kMaxSections];
  int numSections_ = 0;
  std::atomic<bool> enabled_{false};
};

} // namespace gm
