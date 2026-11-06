// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Platform/POSIX/Preferences.cpp — the POSIX half of the PAL preferences
// contract, bundled into Core beside SystemPAL.
//
// BUNDLED, not frontend-supplied, and that is the point of the file. Core's
// configuration is acquired through this seam, so a build that links Core needs
// these symbols to resolve — and requiring an embedder to implement blob storage
// before they can read a config file is a contract nobody would thank us for.
// Storage works the same way: `storageReadFile` is bundled for the same reason.
//
// One implementation for macOS, Linux and the host. The location rule is
// shared: the configuration file lives at `$XDG_CONFIG_HOME/ubiquitty/` — or
// `~/.config/ubiquitty/` when that is unset — on macOS as much as on Linux.
// Filing an opaque record somewhere else would make two rules where the
// user-visible half has one.
//
// Written through `storageWriteAtomic`, which stages and renames: a reader sees
// the whole previous blob or the whole new one, never a torn write.
//

#include "PAL/PAL.h"

#include "Core/Preferences/PreferencesPaths.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

namespace {

/// Whether `key` names a file rather than describing a path.
///
/// Rejecting the separators and the traversal outright is cheaper than
/// canonicalising afterwards, and every key this seam carries is a compile-time
/// constant — so a rejection here is a programming error, not user input.
constexpr bool isKeySafe(std::string_view key)
{
    constexpr auto isPortableFileNameCharacter = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_';
    };
    return !key.empty() && std::ranges::all_of(key, isPortableFileNameCharacter);
}

/// Where a key's bytes live, or empty when the key or the home is unusable.
///
/// The configuration key keeps the name and extension a person edits it under;
/// every other key is an opaque record and takes a generated name, so a
/// directory listing tells the two apart without opening either.
std::string blobPath(Ubiquitty::PAL::PreferencesBlobKey key)
{
    if (key.name == nullptr || !isKeySafe(key.name))
        return {};
    if (std::string_view { key.name } == Ubiquitty::PAL::configurationBlobKey.name)
        return Ubiquitty::Preferences::filePath().value_or(std::string {});
    const auto directory = Ubiquitty::Preferences::directoryPath();
    if (!directory)
        return {};
    return std::format("{}/{}.blob", *directory, key.name);
}

} // namespace

namespace Ubiquitty::PAL {

int preferencesLoad(PreferencesBlobKey key, void* outputBuffer, int outputCapacity)
{
    // A capacity with nowhere to put the bytes is the caller's error. A null
    // buffer with a zero capacity is the size probe, which `storageReadFile`
    // answers directly — passing it through is what lets this seam carry a
    // payload whose length the caller cannot know in advance.
    if (outputBuffer == nullptr && outputCapacity > 0)
        return -1;
    const std::string path = blobPath(key);
    if (path.empty())
        return -1;
    return storageReadFile(path.c_str(), outputBuffer, outputCapacity);
}

int preferencesSaveAtomic(PreferencesBlobKey key, std::span<const std::byte> buffer)
{
    const std::string path = blobPath(key);
    if (path.empty())
        return -EINVAL;
    return storageWriteAtomic(path.c_str(), buffer) ? 0 : -EIO;
}

} // namespace Ubiquitty::PAL
