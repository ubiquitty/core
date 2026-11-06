// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace Ubiquitty {

struct Cell;

// View over a contiguous range of cells from a single row. Pointer +
// length pair packaged so callers can't accidentally pass a length
// unrelated to the pointed-at buffer. POD; passes by value.
struct RowView {
    const Cell* data;
    int count;
};

} // namespace Ubiquitty
