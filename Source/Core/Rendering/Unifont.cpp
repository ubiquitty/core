// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Rendering/Unifont.cpp — see Unifont.h.
//

#include "Core/Rendering/Unifont.h"

#include "Core/Codec/ZlibInflate.h"
#include "Core/Rendering/UnifontData.h"

#include <cstdlib>
#include <vector>

namespace Ubiquitty::Rendering::Unifont {

namespace {

bool loaded = false;
bool loadFailed = false;
std::vector<uint8_t> blob; // inflated record stream
std::vector<uint32_t> glyphOffsets; // 0x10000: bitmap byte offset + 1 (0 = absent)
std::vector<uint8_t> glyphWidths; // 0x10000: 8 or 16

void ensureLoaded()
{
    if (loaded || loadFailed)
        return;

    blob.resize(unifontBlobLength);
    // One complete stream into an exact-size buffer — inflateExact's contract;
    // the decoder state lives on this call's stack (~33 KB).
    if (!Ubiquitty::Codec::Zlib::inflateExact({ unifontCompressed, static_cast<std::size_t>(unifontCompressedLength) }, { blob.data(), static_cast<std::size_t>(unifontBlobLength) })) {
        loadFailed = true;
        return;
    }

    glyphOffsets.assign(0x10000, 0);
    glyphWidths.assign(0x10000, 0);
    std::size_t pos = 0;
    const std::size_t total = blob.size();
    while (pos + 3 <= total) {
        uint32_t codePoint = static_cast<uint32_t>(blob[pos]) | (static_cast<uint32_t>(blob[pos + 1]) << 8);
        uint8_t width = blob[pos + 2];
        std::size_t bitmapBytes = (width == 8) ? 16 : 32;
        std::size_t bitmapAt = pos + 3;
        if (bitmapAt + bitmapBytes > total)
            break;
        if (codePoint < 0x10000) {
            glyphOffsets[codePoint] = static_cast<uint32_t>(bitmapAt + 1);
            glyphWidths[codePoint] = width;
        }
        pos = bitmapAt + bitmapBytes;
    }
    loaded = true;
}

} // namespace

Glyph lookup(uint32_t codePoint)
{
    ensureLoaded();
    Glyph glyph { nullptr, 0 };
    if (!loaded || codePoint >= 0x10000)
        return glyph;
    uint32_t offset = glyphOffsets[codePoint];
    if (offset == 0)
        return glyph;
    glyph.bits = blob.data() + (offset - 1);
    glyph.width = glyphWidths[codePoint];
    return glyph;
}

} // namespace Ubiquitty::Rendering::Unifont
