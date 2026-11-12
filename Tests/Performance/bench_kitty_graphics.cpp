// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Focused non-PGO comparison: ordinary capture, retained image capture and
// complete frame transfer plus deletion. Run without competing builds.
#include "BenchHarness.h"
#include "Core/Codec/Base64.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty;

namespace {
std::string transfer(const std::string& keys, int components)
{
    std::vector<char> raw(128 * 128 * components, static_cast<char>(255));
    std::vector<char> encoded(Codec::base64EncodedLength(raw.size()));
    if (!Codec::base64Encode(raw, encoded))
        std::abort();
    std::string command;
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4096) {
        command += "\033_G";
        command += offset == 0 ? keys : (keys.starts_with("a=f,") ? "a=f" : "");
        command += offset == 0 || keys.starts_with("a=f,") ? ",m=" : "m=";
        command += offset + 4096 < encoded.size() ? "1;" : "0;";
        command.append(encoded.data() + offset, std::min(std::size_t { 4096 }, encoded.size() - offset));
        command += "\033\\";
    }
    return command;
}
}

int main()
{
    using namespace Bench;
    using namespace Rendering;
    std::vector<Result> results;
    UnifontRasterizer rasterizer(1, 0);
    const auto metrics = rasterizer.cellMetrics();
    auto terminal = std::make_unique<Terminal>();
    resize(*terminal, 24, 80);
    processBytes(terminal.get(), "Ordinary terminal text, without images.\r\n");
    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    const auto capture = [&] {
        if (!frame.capture(*terminal, options, metrics, GlyphSlotGeometry {}))
            std::abort();
        auto* cells = frame.inputs().cells;
        doNotOptimize(cells);
    };
    auto timing = measure(10, 101, capture);
    results.push_back({ "capture_without_images", "us", timing.medianSeconds * 1e6, timing.p95Seconds * 1e6 });
    for (int components : { 3, 4 }) {
        hardReset(*terminal);
        const std::string geometry = ",i=31,q=2,f=" + std::to_string(components * 8) + ",s=128,v=128";
        processBytes(terminal.get(), transfer("a=t" + geometry, components));
        capture();
        timing = measure(10, 101, capture);
        results.push_back({ "capture_retained_" + std::to_string(components * 8), "us", timing.medianSeconds * 1e6, timing.p95Seconds * 1e6 });
        const std::string command = transfer("a=f" + geometry, components);
        processBytes(terminal.get(), command);
        if (terminal->images.stored[0].frameCount != 2)
            std::abort();
        processBytes(terminal.get(), "\033_Ga=d,d=f,i=31,q=2\033\\");
        timing = measure(3, 31, [&] {
            processBytes(terminal.get(), command);
            auto count = terminal->images.stored[0].frameCount;
            doNotOptimize(count);
            if (count != 2)
                std::abort();
            processBytes(terminal.get(), "\033_Ga=d,d=f,i=31,q=2\033\\");
        });
        results.push_back({ "transfer_full_frame_" + std::to_string(components * 8), "us", timing.medianSeconds * 1e6, timing.p95Seconds * 1e6 });
    }
    emitJSON(results, "core", std::getenv("UBIQUITTY_BENCH_COMMIT"));
}
