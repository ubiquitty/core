// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The built-in theme table as TOML, compiled into the binary from
// Source/Core/Themes/builtin-themes.toml (see Scripts/embed-builtin-themes.py).
// Parsed once at startup by the same reader as user [[themes]] blocks, so
// built-in and custom themes share one definition format. NUL-terminated.

namespace Ubiquitty::Themes {

extern const char builtinThemesTOML[];
extern const unsigned builtinThemesTOMLLength;

} // namespace Ubiquitty::Themes
