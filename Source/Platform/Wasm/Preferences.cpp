// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Platform/Wasm/Preferences.cpp — the wasm32 half of the PAL preferences
// contract, the emscripten counterpart of Platform/POSIX/Preferences.cpp.
//
// Carries the same symbol set, as every bundled backend does, so the portable
// library links unchanged. What differs is where the bytes come from: a browser
// has no filesystem to read a path out of, and reaching for localStorage here
// would bind Core to one storage choice on behalf of every embedder.
//
// So the page hands bytes in under a name and this holds them for the module's
// lifetime. Whether they are persisted, and in what — localStorage, IndexedDB, a
// server round-trip — stays the page's decision, the same latitude the POSIX
// backend takes when it decides a key means a file.
//
// The browser's configuration arrives through this seam and no other. A
// hand-built settings channel beside it diverges on any value that fails to
// arrive through it, and the browser then draws underlines through descenders
// while every other arm breaks them around the ink.
//

#include "PAL/PAL.h"

#include "Base/Types.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ubiquitty::Wasm {

std::unordered_map<std::string, std::vector<std::byte>>& preferenceBlobs()
{
    static std::unordered_map<std::string, std::vector<std::byte>> blobs;
    return blobs;
}

} // namespace Ubiquitty::Wasm

namespace Ubiquitty::PAL {

int preferencesLoad(PreferencesBlobKey key, void* outputBuffer, int outputCapacity)
{
    if (key.name == nullptr)
        return -1;
    if (outputBuffer == nullptr && outputCapacity > 0)
        return -1; // a capacity with nowhere to put the bytes is a caller error

    const auto& blobs = Wasm::preferenceBlobs();
    const auto found = blobs.find(key.name);
    if (found == blobs.cend())
        return -1; // absent, which Core reads as "nothing stored" rather than as failure

    const std::span blob { found->second };
    // The probe: a null buffer, or a zero capacity, asks how many bytes there are.
    if (outputBuffer == nullptr || outputCapacity <= 0)
        return Ubiquitty::saturatingCast<int>(blob.size());

    const std::span destination { static_cast<std::byte*>(outputBuffer), static_cast<std::size_t>(outputCapacity) };
    const std::size_t count = std::min(blob.size(), destination.size());
    std::ranges::copy(blob.first(count), destination.begin());
    return static_cast<int>(count);
}

int preferencesSaveAtomic(PreferencesBlobKey key, std::span<const std::byte> buffer)
{
    if (key.name == nullptr)
        return -EINVAL;
    // Atomic by construction: the assignment replaces the whole value, so a
    // reader sees the old bytes or the new ones and never a half-written blob.
    // Durability belongs to the page, which reads them back out to persist them.
    Wasm::preferenceBlobs()[key.name].assign_range(buffer);
    return 0;
}

} // namespace Ubiquitty::PAL
