// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable compiler/language-attribute macros. Each wraps a compiler-specific
// `[[...]]` attribute behind a `__has_cpp_attribute` guard, so a Core header
// carries the attribute where understood and compiles untouched elsewhere
// (a bare vendor attribute is -Werror=attributes on a compiler that lacks it).

// Closed enumeration. Two effects from the same clang attribute:
//   1. Swift importer maps the enum to a @frozen Swift enum (exhaustive Swift
//      switch needs no @unknown default).
//   2. Clang's -Wswitch treats the enumerator set as complete.
// clang-only, guarded so it's a no-op on GCC. Place between `enum class` and
// the name.
#if defined(__has_cpp_attribute) && __has_cpp_attribute(clang::enum_extensibility)
#define UBIQUITTY_CLOSED_ENUM [[clang::enum_extensibility(closed)]]
#else
#define UBIQUITTY_CLOSED_ENUM
#endif
