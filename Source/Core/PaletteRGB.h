// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// 16-bit channels match the OSC wire precision; frontends scale to
// 8-bit when they sync their paint cache.

namespace Ubiquitty {

struct PaletteRGB {
    uint16_t red;
    uint16_t green;
    uint16_t blue;

    bool operator==(const PaletteRGB&) const = default;
};

} // namespace Ubiquitty
