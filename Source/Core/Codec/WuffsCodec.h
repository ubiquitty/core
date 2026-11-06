// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Single seam through which the Codec/ wrappers see the vendored Wuffs library.
// The WUFFS_CONFIG__* macros must agree across every TU that touches wuffs; this
// header pins them once (zlib + PNG surface) so ZlibInflate.cpp, PNGDecoder.cpp
// and the implementation TU (WuffsCodec.cpp) can never drift apart.
//
// Storage contract: without WUFFS_IMPLEMENTATION, Wuffs poisons its C++ struct
// sizeofs so nobody stack-allocates a decoder. Decoder objects live in
// caller-reserved storage of wuffsCodecStorageBytes, initialized
// with the runtime sizeof__wuffs_*__decoder() value. WuffsCodec.cpp
// static_asserts the real sizes fit, so an upstream bump that outgrows the
// storage is a compile break, never a runtime overrun.

// Real sizes on LP64/clang today: 41952 (zlib), 44632 (png). 48 KiB covers both.
namespace Ubiquitty {
// A macro would leak into every translation unit including this header and carry
// no type. Its four uses are static_asserts in C++ files -- nothing tests it with
// a preprocessor conditional, and no C file includes this header, so the
// preprocessor buys nothing here that a scoped constant does not.
constexpr int wuffsCodecStorageBytes = 49152;
} // namespace Ubiquitty

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG

#include "ThirdParty/wuffs/wuffs-v0.4.c"
