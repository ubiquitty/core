// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The one TU that compiles the vendored Wuffs implementation. With
// WUFFS_IMPLEMENTATION the decoder structs have their real layout here, so these
// static_asserts are the only compile-time link proving the reserved storage
// fits (consumer TUs never see the real sizeofs — wuffs poisons them there).

#define WUFFS_IMPLEMENTATION
#include "Core/Codec/WuffsCodec.h"

static_assert(sizeof(wuffs_zlib__decoder) <= Ubiquitty::wuffsCodecStorageBytes, "wuffs zlib decoder must fit the codec storage Codec::Zlib::Inflater reserves");
static_assert(sizeof(wuffs_png__decoder) <= Ubiquitty::wuffsCodecStorageBytes, "wuffs PNG decoder must fit the codec storage Codec::Zlib::Inflater reserves");
