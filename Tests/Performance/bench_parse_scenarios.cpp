// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Replay canonical ubench streams through either parser entry point.
// Scripts/run-parse-scenarios.sh builds the generator from UBENCH_DIR; it
// supplies the exact streams and manifest. Timing excludes PTY, rendering,
// and generation.

#include "BenchHarness.h"
#include "Core/Terminal.h"

#include "Core/TerminalOperations.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

constexpr std::size_t maxStreamBytes = 256u * 1024u * 1024u;

struct Scenario {
    std::string key;
    std::string filename;
    std::vector<char> bytes;
};

bool readStream(const std::string& path, std::vector<char>& bytes)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return false;
    }
    const std::streamsize length = input.tellg();
    if (length <= 0 || static_cast<std::size_t>(length) > maxStreamBytes) {
        return false;
    }
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(length));
    return input.read(bytes.data(), length).good();
}

bool loadScenarios(const std::string& directory, std::vector<Scenario>& scenarios)
{
    std::ifstream manifest(directory + "/scenarios.tsv");
    if (!manifest) {
        return false;
    }
    std::string line;
    while (std::getline(manifest, line)) {
        const std::size_t firstTab = line.find('\t');
        const std::size_t secondTab = firstTab == std::string::npos ? std::string::npos : line.find('\t', firstTab + 1);
        if (firstTab == std::string::npos || secondTab == std::string::npos) {
            return false;
        }
        Scenario scenario;
        scenario.key = line.substr(0, firstTab);
        scenario.filename = line.substr(firstTab + 1, secondTab - firstTab - 1);
        if (scenario.key.empty() || scenario.filename.empty() || !readStream(directory + "/" + scenario.filename, scenario.bytes)) {
            return false;
        }
        scenarios.push_back(std::move(scenario));
    }
    return !scenarios.empty();
}

// --scroll-lane: enable the deferred-state scroll lane for every scenario.
bool isBenchScrollLaneEnabled = false;

enum class InputEntry {
    ProcessBytes,
    Native,
};

Ubiquitty::Bench::Result benchmarkScenario(const Scenario& scenario, int rows, int columns, int warmup, int iterations, InputEntry inputEntry)
{
    Ubiquitty::Terminal term;
    Ubiquitty::hardReset(term);
    term.isScrollLaneEnabled = isBenchScrollLaneEnabled;
    Ubiquitty::resize(term, rows, columns);
    const auto timing = Ubiquitty::Bench::measure(warmup, iterations, [&] {
        std::string_view remaining(scenario.bytes);
        if (inputEntry == InputEntry::ProcessBytes) {
            Ubiquitty::processBytes(&term, remaining);
            return;
        }
        while (!remaining.empty()) {
            const int consumed = Ubiquitty::processBytesUpToSyncClear(&term, remaining);
            if (consumed <= 0 || static_cast<std::size_t>(consumed) > remaining.size())
                std::abort();
            remaining.remove_prefix(static_cast<std::size_t>(consumed));
        }
    });
    const char* prefix = inputEntry == InputEntry::Native ? "parse_native_" : "parse_";
    return Ubiquitty::Bench::throughput(prefix + scenario.key, static_cast<double>(scenario.bytes.size()), timing);
}

} // namespace

int main(int argc, char** argv)
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    const char* platform = "unknown";
    const char* commit = "unknown";
    std::string streamDirectory;
    int rows = 52;
    int columns = 196;
    int warmup = 1;
    int iterations = 5;
    InputEntry inputEntry = InputEntry::ProcessBytes;
    for (int i = 1; i < argc; ++i) {
        auto equals = [&](const char* argument) { return std::strcmp(argv[i], argument) == 0; };
        if (equals("--stream-dir") && i + 1 < argc) {
            streamDirectory = argv[++i];
        } else if (equals("--platform") && i + 1 < argc) {
            platform = argv[++i];
        } else if (equals("--commit") && i + 1 < argc) {
            commit = argv[++i];
        } else if (equals("--rows") && i + 1 < argc) {
            rows = std::atoi(argv[++i]);
        } else if (equals("--columns") && i + 1 < argc) {
            columns = std::atoi(argv[++i]);
        } else if (equals("--warmup") && i + 1 < argc) {
            warmup = std::atoi(argv[++i]);
        } else if (equals("--iters") && i + 1 < argc) {
            iterations = std::atoi(argv[++i]);
        } else if (equals("--scroll-lane")) {
            isBenchScrollLaneEnabled = true;
        } else if (equals("--native-input")) {
            inputEntry = InputEntry::Native;
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (streamDirectory.empty() || rows <= 0 || columns <= 0 || warmup < 0 || iterations <= 0) {
        std::fprintf(stderr,
            "usage: bench_parse_scenarios --stream-dir DIR "
            "[--rows N --columns N --iters N --native-input]\n");
        return 2;
    }

    std::vector<Scenario> scenarios;
    if (!loadScenarios(streamDirectory, scenarios)) {
        std::fprintf(stderr, "cannot load scenario manifest from %s\n", streamDirectory.c_str());
        return 1;
    }

    std::vector<Ubiquitty::Bench::Result> results;
    results.reserve(scenarios.size());
    for (const Scenario& scenario : scenarios) {
        results.push_back(benchmarkScenario(scenario, rows, columns, warmup, iterations, inputEntry));
    }
    Ubiquitty::Bench::emitJSON(results, platform, commit);
    return 0;
}
