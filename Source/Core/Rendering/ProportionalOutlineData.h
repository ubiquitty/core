// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace Ubiquitty::Rendering {

/// The outlines of the bitmap tier's proportional face, Noto Sans Regular
/// and Bold, as a zlib stream; the layout is stated in
/// Tools/gen-proportional-outlines/gen-proportional-outlines.cpp, which
/// writes it.
extern const unsigned int proportionalOutlineBlobLength;
extern const unsigned int proportionalOutlineCompressedLength;
extern const unsigned char proportionalOutlineCompressed[];

} // namespace Ubiquitty::Rendering
