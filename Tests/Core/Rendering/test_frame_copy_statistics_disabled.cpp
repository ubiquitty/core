// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

TEST(testReleaseCaptureCarriesNoCopyAccounting)
{
    auto terminal = std::make_unique<Terminal>();
    Ubiquitty::resize(*terminal, 6, 20);

    TerminalFrameInputs frame;
    TerminalSnapshotOptions options;
    CellMetrics metrics;
    REQUIRE(frame.capture(*terminal, options, metrics, GlyphSlotGeometry {}));

    const FrameCopyStatistics& statistics = frame.copyStatistics();
    CHECK(statistics.cellBytesCopied == 0);
    CHECK(statistics.imageBytesCopied == 0);
    CHECK(statistics.rowsInspected == 0);
    CHECK(statistics.rowsCopied == 0);
}

} // namespace

UBIQUITTY_TEST_MAIN()
