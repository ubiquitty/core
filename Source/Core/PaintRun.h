// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/StrongIndex.h"

namespace Ubiquitty {

// PaintRun: one same-pen, contiguous run of dirty cells. PAL receives
// these from PaintSession::NextDirtyRun and renders them with one
// font/color setup. firstColumn..lastColumn inclusive.
//
// selectionOnly: cell content matches shadow but selection state
// changed; PAL can skip glyph render and just repaint selection
// overlay if that's cheaper.
struct PaintRun {
    Row row;
    Column firstColumn;
    Column lastColumn;
    bool selectionOnly;
};

} // namespace Ubiquitty
