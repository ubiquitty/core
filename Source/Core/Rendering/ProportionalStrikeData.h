// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace Ubiquitty::Rendering {

/// The bitmap tier's proportional face, Noto Sans pre-rendered at two
/// scales and every subpixel phase, as a zlib stream; the layout is stated
/// in Tools/gen-proportional-strike/gen-proportional-strike.cpp, which
/// writes it.
extern const unsigned int proportionalStrikeBlobLength;
extern const unsigned int proportionalStrikeCompressedLength;
extern const unsigned char proportionalStrikeCompressed[];

} // namespace Ubiquitty::Rendering
