// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/SelfWriteGuard.h"

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Internal.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesPaths.h"

#include "PAL/PAL.h"

#include <cstddef>
#include <span>

namespace Ubiquitty::Preferences {

namespace {

OwnWriteListener ownWriteListener = nullptr;

void notifyOwnWrite()
{
    if (ownWriteListener != nullptr)
        ownWriteListener();
}

/// Stores the live document where the platform keeps it — the PAL
/// configuration blob, which on a desktop is the file a person edits — and
/// records the bytes as the app's own.
bool persistDocument(std::string document)
{
    const bool didWrite = PAL::preferencesSaveAtomic(PAL::configurationBlobKey, std::as_bytes(std::span { document })) == 0;
    if (didWrite)
        selfWriteGuard().noteOwnWrite(std::move(document));
    return didWrite;
}

} // namespace

SelfWriteGuard& selfWriteGuard()
{
    static SelfWriteGuard guard;
    return guard;
}

ChangeOrigin originOfFileChange()
{
    const std::optional<std::string> path = filePath();
    return selfWriteGuard().originOf(path ? readWholeFile(*path) : std::nullopt);
}

void setOwnWriteListener(OwnWriteListener listener) { ownWriteListener = listener; }

ConfigureResult configureOwnDocument(const std::string& document)
{
    ConfigureResult result = configure(document, ParseMode::Permissive);
    (void)persistDocument(liveDocument());
    notifyOwnWrite();
    return result;
}

std::optional<std::string> configureOwnKey(Key key, const Value& value)
{
    std::optional<std::string> document = configureKey(key, value);
    if (!document)
        return std::nullopt;
    (void)persistDocument(*document);
    notifyOwnWrite();
    return document;
}

std::optional<std::string> configureOwnSectionKey(const std::string& section, const std::string& key, const std::string& tomlValue)
{
    std::string document = setSectionKey(liveDocument(), section, key, tomlValue);
    (void)configureOwnDocument(document);
    return document;
}

std::optional<std::string> removeConfiguredSectionKey(const std::string& section, const std::string& key)
{
    std::string document = removeSectionKey(liveDocument(), section, key);
    (void)configureOwnDocument(document);
    return document;
}

} // namespace Ubiquitty::Preferences
