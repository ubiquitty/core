// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty {
class CellGrid;

// See LineAttributeView.h for the CellGrid row-metadata seam.
struct RowMarkView {
    Ubiquitty::CellGrid* owner;
    int screen;
    uint8_t& operator[](int r);
    const uint8_t& operator[](int r) const;
};

} // namespace Ubiquitty
