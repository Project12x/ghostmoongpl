// SPDX-License-Identifier: MIT
#pragma once
// ghostmoon tools: CpuProfileHarness
//
// First-party ghostmoon-oss export for GPL-compatible plugin test/profile
// harnesses. Reuse mode: direct-copy from full ghostmoon
// tools/include/ghostmoon_tools/CpuProfileHarness.h at
// f521679b8bd0946bc48b0941c45945e1c65d2369.
//
// Generic headless per-section CPU attribution runner for JUCE plugins.
// Consumer test binaries supply scenarios and processor/profiler accessors;
// this header drives warmup/measurement, prints per-section tables, and writes
// stable-keyed JSON for agent diffing.

#include <ghostmoon/CpuSectionProfiler.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace gm::tools {

struct CpuProfileScenario {
  const char *name;
  const char *description;
  std::function<void(juce::AudioProcessor &, juce::AudioBuffer<float> &)> setup;
  std::function<void(juce::AudioProcessor &, juce::AudioBuffer<float> &,
                     juce::MidiBuffer &, int64_t)>
      prepareBlock;
};

struct CpuProfileSpec {
  const char *pluginName = "plugin";
  double sampleRate = 48000.0;
  int blockSize = 512;
  int warmupBlocks = 200;
  int measureBlocks = 2000;
  std::function<std::unique_ptr<juce::AudioProcessor>()> makeProcessor;
  std::function<gm::CpuSectionProfiler &(juce::AudioProcessor &)> getProfiler;
  std::function<void(juce::AudioProcessor &)> resetCounters;
  std::function<std::string(juce::AudioProcessor &, int)> getCountersJson;
  std::vector<CpuProfileScenario> scenarios;
};

inline int runCpuProfile(const CpuProfileSpec &spec, const char *jsonPath) {
  if (!spec.makeProcessor || !spec.getProfiler || spec.scenarios.empty()) {
    std::cerr
        << "CpuProfileHarness: spec is missing makeProcessor/getProfiler/scenarios\n";
    return 2;
  }

  const double budgetUs = 1.0e6 * spec.blockSize / spec.sampleRate;

  std::cout << spec.pluginName << " CPU Attribution (headless)\n";
  std::cout << std::string(std::strlen(spec.pluginName) + 27, '=') << "\n";

  std::ostringstream json;
  json << "{\n  \"plugin\": \"" << spec.pluginName << "\","
       << " \"sampleRate\": " << spec.sampleRate
       << ", \"blockSize\": " << spec.blockSize
       << ", \"measuredBlocks\": " << spec.measureBlocks
       << ",\n  \"scenarios\": {\n";

  bool firstScenario = true;
  for (const auto &sc : spec.scenarios) {
    auto proc = spec.makeProcessor();
    proc->prepareToPlay(spec.sampleRate, spec.blockSize);
    juce::AudioBuffer<float> buf(2, spec.blockSize);
    if (sc.setup)
      sc.setup(*proc, buf);

    auto processProfileBlock = [&](int64_t blockIndex) {
      buf.clear();
      juce::MidiBuffer midi;
      if (sc.prepareBlock)
        sc.prepareBlock(*proc, buf, midi, blockIndex);
      proc->processBlock(buf, midi);
    };

    for (int b = 0; b < spec.warmupBlocks; ++b)
      processProfileBlock(b);

    if (spec.resetCounters)
      spec.resetCounters(*proc);

    auto &prof = spec.getProfiler(*proc);
    prof.reset();
    prof.setEnabled(true);

    uint64_t wallNs = 0;
    for (int b = 0; b < spec.measureBlocks; ++b) {
      const auto t0 = gm::CpuSectionProfiler::Clock::now();
      processProfileBlock(spec.warmupBlocks + b);
      wallNs += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              gm::CpuSectionProfiler::Clock::now() - t0)
              .count());
    }
    prof.setEnabled(false);

    gm::CpuSectionProfiler::Snapshot
        snaps[gm::CpuSectionProfiler::kMaxSections];
    const int n = prof.snapshot(snaps, gm::CpuSectionProfiler::kMaxSections);

    const double wallAvgUs =
        static_cast<double>(wallNs) / spec.measureBlocks / 1000.0;
    double sectionSumUs = 0.0;
    for (int i = 0; i < n; ++i)
      sectionSumUs +=
          static_cast<double>(snaps[i].totalNs) / 1000.0 / spec.measureBlocks;

    int order[gm::CpuSectionProfiler::kMaxSections];
    for (int i = 0; i < n; ++i)
      order[i] = i;
    std::sort(order, order + n, [&](int a, int b) {
      return snaps[a].totalNs > snaps[b].totalNs;
    });

    std::cout << "\nScenario: " << sc.name << " - " << sc.description
              << "\n";
    std::cout << "  " << spec.measureBlocks << " blocks @ "
              << spec.sampleRate << "/" << spec.blockSize << "  budget "
              << std::fixed << std::setprecision(1) << budgetUs
              << " us/block, wall avg " << wallAvgUs << " us ("
              << std::setprecision(2) << (100.0 * wallAvgUs / budgetUs)
              << "% of budget)\n";
    std::cout << "  Section            avg us    max us   % budget   % measured\n";
    std::cout << "  ---------------- --------- --------- ---------- -----------\n";
    for (int k = 0; k < n; ++k) {
      const auto &s = snaps[order[k]];
      const double avgUs =
          static_cast<double>(s.totalNs) / spec.measureBlocks / 1000.0;
      std::cout << "  " << std::left << std::setw(16) << s.name << std::right
                << std::setw(10) << std::setprecision(2) << avgUs
                << std::setw(10) << std::setprecision(1) << s.maxUs()
                << std::setw(10) << std::setprecision(2)
                << (100.0 * avgUs / budgetUs) << std::setw(11)
                << std::setprecision(1)
                << (wallAvgUs > 0 ? 100.0 * avgUs / wallAvgUs : 0.0)
                << "\n";
    }
    const double otherUs = wallAvgUs - sectionSumUs;
    std::cout << "  " << std::left << std::setw(16) << "(Other)"
              << std::right << std::setw(10) << std::setprecision(2)
              << otherUs << std::setw(10) << "-" << std::setw(10)
              << std::setprecision(2) << (100.0 * otherUs / budgetUs)
              << std::setw(11) << std::setprecision(1)
              << (wallAvgUs > 0 ? 100.0 * otherUs / wallAvgUs : 0.0)
              << "\n";

    if (!firstScenario)
      json << ",\n";
    firstScenario = false;
    json << "    \"" << sc.name << "\": { \"wall_avg_us\": "
         << std::setprecision(3) << wallAvgUs << ", \"sections\": {";
    for (int i = 0; i < n; ++i) {
      if (i > 0)
        json << ", ";
      json << "\"" << snaps[i].name << "\": { \"avg_us\": "
           << (static_cast<double>(snaps[i].totalNs) / spec.measureBlocks /
               1000.0)
           << ", \"max_us\": " << snaps[i].maxUs()
           << ", \"calls\": " << snaps[i].calls << " }";
    }
    json << " }";
    if (spec.getCountersJson) {
      const auto countersJson = spec.getCountersJson(*proc, spec.measureBlocks);
      if (!countersJson.empty())
        json << ", \"counters\": " << countersJson;
    }
    json << " }";

    proc->releaseResources();
  }
  json << "\n  }\n}\n";

  if (jsonPath != nullptr) {
    std::ofstream f(jsonPath);
    if (f) {
      f << json.str();
      std::cout << "\nJSON written: " << jsonPath << "\n";
    } else {
      std::cerr << "\nCould not write JSON: " << jsonPath << "\n";
      return 1;
    }
  }
  std::cout << "\n";
  return 0;
}

inline bool handleCpuProfileArgs(int argc, char *argv[],
                                 const CpuProfileSpec &spec, int &exitCode) {
  bool wantsProfile = false;
  const char *jsonPath = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--cpu-profile")
      wantsProfile = true;
    if (std::string(argv[i]) == "--json" && i + 1 < argc)
      jsonPath = argv[i + 1];
  }
  if (!wantsProfile)
    return false;
  exitCode = runCpuProfile(spec, jsonPath);
  return true;
}

} // namespace gm::tools
