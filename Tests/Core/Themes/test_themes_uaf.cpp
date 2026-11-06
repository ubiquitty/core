// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_themes_uaf.cpp — AddressSanitizer negative for the
// custom-theme registry holding element addresses stable across
// growth, so a `const ::Theme*` handed out by customGetInternal survives a
// later registerCustom.
//
// The registry is a std::deque<CustomEntry>. A deque never moves an existing
// element on push_back (it appends into a fresh block and chains it), so a held
// Theme* stays valid across any number of registerCustom/replaceCustom calls;
// only clearCustom (a config reload, which .clear()s) invalidates it.
//
// A std::vector<CustomEntry> would not hold that: when registerCustom's
// push_back outgrows its capacity it reallocates its backing buffer and frees
// the old one — dangling every `Theme*` a prior customGetInternal call
// returned. The next read through that held pointer is a heap-use-after-free.
//
// The negative: register one theme, capture its internal Theme* via
// customGetInternal, then register MANY more — enough that, were the registry a
// vector, its buffer would have reallocated and moved the first element. Then
// read a field through the held pointer. With the deque this is ASan-clean.
// Revert the registry to std::vector and ASan reports a heap-use-after-free READ
// here (the read through the freed Theme*).
//
// Isolation: ThemesCustom.cpp's only cross-TU dependency is builtInCount(),
// which we stub below — so this test links the registry implementation alone,
// with no Preferences/Preferences graph, keeping the ASan picture focused on the
// registry's own allocations.

#include "Core/Themes/Themes.h"

#include <cstdio>

// Stub the one cross-TU symbol ThemesCustom.cpp needs (normally defined in
// Themes.cpp). The exact value is irrelevant to the UAF; registerCustom only
// uses it to compute the returned unified index. A plausible built-in count.

namespace Ubiquitty::Themes {
int builtInCount() { return 16; }
} // namespace Ubiquitty::Themes

// The internal accessor under test — declared at global scope to match the
// definition's mangling (ThemesCustom.cpp defines it there).
const Ubiquitty::Themes::Theme* customGetInternal(int local);

// Build a recognizable theme whose ansi[0] red channel encodes `tag`, so a read
// through a held pointer is both verifiable (right value) and ASan-checked.
static Ubiquitty::Themes::Theme makeTheme(const char* name, unsigned char tag)
{
    Ubiquitty::Themes::Theme t {};
    t.name = name;
    t.ansi[0] = Ubiquitty::ColorRGBA { tag, 0x00, 0x00, 0xFF };
    return t;
}

TEST(themesUaf)
{

    using namespace Ubiquitty::Themes;

    // Start from a clean registry (clearCustom frees any backing storage).
    clearCustom();
    CHECK(customCount() == 0);

    // (1) Register the first custom theme and (2) capture the internal Theme*.
    // Local index 0 (unified index builtInCount()+0).
    const int firstUnified = registerCustom(makeTheme("held", 0x42));
    CHECK(firstUnified == builtInCount());
    CHECK(customCount() == 1);

    const Ubiquitty::Themes::Theme* held = customGetInternal(/*local*/ 0);
    CHECK(held != nullptr);
    // Sanity read while definitely valid.
    CHECK(held->ansi[0].red == 0x42);

    // (3) Register MANY more custom themes. A std::vector starts at capacity 0/1
    // and grows geometrically, reallocating its buffer several times across this
    // loop — each realloc frees the old buffer that `held` points into. 256
    // entries forces well past any small-buffer/initial capacity, so a vector
    // registry is guaranteed to have moved (and freed) the first element. A deque
    // never moves entry 0, so `held` stays valid.
    for (int i = 1; i <= 256; ++i) {
        char nm[32];
        std::snprintf(nm, sizeof(nm), "extra-%d", i);
        registerCustom(makeTheme(nm, static_cast<unsigned char>(i & 0xFF)));
    }
    CHECK(customCount() == 257);

    // (4) Read a field through the HELD pointer, post-growth. With the deque
    // this dereferences live storage (ASan-clean) and still reads the original
    // value. With a vector, `held` dangles into a freed buffer and ASan reports
    // a heap-use-after-free READ on exactly this line.
    const unsigned char redAfterGrowth = held->ansi[0].red; // <-- UAF read under vector
    CHECK(redAfterGrowth == 0x42);

    // The name pointer is likewise refreshed in place; reading it must also be
    // valid (refreshNamePointers walks the registry and rewires name = c_str()).
    CHECK(held->name != nullptr);
    CHECK(held->name[0] == 'h');

    // Cross-check: the public accessor agrees with the held pointer (same entry,
    // stable address in the deque).
    const Ubiquitty::Themes::Theme* viaGet = customGetInternal(0);
    CHECK(viaGet == held);
    CHECK(viaGet != nullptr && viaGet->ansi[0].red == 0x42);

    // clearCustom is the one operation that legitimately invalidates held
    // pointers (config reload). We do not read `held` after this.
    clearCustom();
    CHECK(customCount() == 0);
}

UBIQUITTY_TEST_MAIN()
