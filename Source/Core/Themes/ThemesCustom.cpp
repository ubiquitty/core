// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Runtime registry of custom themes parsed from config.toml. A frontend without
// a custom registry can supply a no-op definition.

#include "Core/Themes/Themes.h"

#include <deque>
#include <string>

namespace {

// Each entry pairs the public Theme with its owning name storage. A deque never
// moves an existing entry on growth, so the storage (and its c_str()) stays put.
struct CustomEntry {
    std::string nameStorage;
    Ubiquitty::Themes::Theme theme;
};

// A deque keeps element addresses stable across push_back, so a Theme* returned
// by customGetInternal stays valid across a later registerCustom/replaceCustom;
// only clearCustom invalidates it. (A vector would dangle on reallocation.)
struct CustomRegistry {
    std::deque<CustomEntry> entries;
    uint64_t generation = 0;
};

CustomRegistry& registry()
{
    static CustomRegistry r;
    return r;
}

void refreshNamePointers()
{
    auto& r = registry().entries;
    for (auto& entry : r)
        entry.theme.name = entry.nameStorage.c_str();
}

} // namespace

namespace Ubiquitty::Themes {

int customCount() { return static_cast<int>(registry().entries.size()); }

uint64_t generation() { return registry().generation; }

void clearCustom()
{
    registry().entries.clear();
    ++registry().generation;
}

int registerCustom(const Theme& theme)
{
    auto& r = registry().entries;
    CustomEntry e;
    e.nameStorage = (theme.name != nullptr) ? theme.name : "";
    e.theme = theme;
    r.push_back(std::move(e));
    refreshNamePointers();
    ++registry().generation;
    return builtInCount() + static_cast<int>(r.size()) - 1;
}

bool replaceCustom(int unifiedIndex, const Theme& theme)
{
    const int base = builtInCount();
    if (unifiedIndex < base)
        return false;
    auto& r = registry().entries;
    int local = unifiedIndex - base;
    if (local < 0 || local >= static_cast<int>(r.size()))
        return false;
    r[local].nameStorage = (theme.name != nullptr) ? theme.name : "";
    r[local].theme = theme;
    refreshNamePointers();
    ++registry().generation;
    return true;
}

} // namespace Ubiquitty::Themes

// Internal accessor used by Themes.cpp's get(); kept in the global
// namespace via an extern "C++" declaration there.
const Ubiquitty::Themes::Theme* customGetInternal(int local);
const Ubiquitty::Themes::Theme* customGetInternal(int local)
{
    auto& r = registry().entries;
    if (local < 0 || local >= static_cast<int>(r.size()))
        return nullptr;
    return &r[local].theme;
}
