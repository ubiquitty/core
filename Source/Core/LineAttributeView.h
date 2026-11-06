// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty {
class CellGrid;

// Read-through views over CellGrid's per-row metadata. A Screen does not
// store lineAttribute / lineWrapped; these forward `[r]` into the owning
// CellGrid's RowMeta for this screen (0 = primary, 1 = alternate), so there
// is a single store of record. `owner` + `screen` are bound once in
// Ubiquitty::Terminal construction (screenInit zeroes them; construction rebinds after the CellGrid is
// allocated). operator[] is defined inline in CellGrid.h, where the RowMeta
// layout it indexes is complete.
struct LineAttributeView {
    Ubiquitty::CellGrid* owner;
    int screen;
    uint8_t& operator[](int r);
    const uint8_t& operator[](int r) const;
};

} // namespace Ubiquitty
