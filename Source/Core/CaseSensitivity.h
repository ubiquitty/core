// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/CaseSensitivity.h — whether a text match distinguishes case.
//
// Its own header because two unrelated subsystems ask the question: the
// scrollback search and the trigger engine. Declared inside either one, the
// other must include a matcher to reach an enumeration — the search header
// carries a compiled-regex handle and the RE2 program behind it, which is a
// large dependency to acquire for two enumerators.
//

#include <cstdint>

namespace Ubiquitty {

/// Whether a text match treats upper and lower case as the same character.
enum class CaseSensitivity : uint8_t {
    Sensitive,
    Insensitive,
};

} // namespace Ubiquitty
