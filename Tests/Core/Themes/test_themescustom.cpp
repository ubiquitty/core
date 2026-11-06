// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_themescustom.cpp — the custom-theme registry's CONTRACT
// (Core/Themes/ThemesCustom.cpp): unified-index arithmetic, replaceCustom's
// two rejection guards, customGetInternal's bounds, the name-storage ownership
// (the registry copies the caller's name; the entry's Theme::name never points
// at caller memory), and clearCustom's reset. The pointer-stability invariant
// that motivates the deque has its own ASan negative (test_themes_uaf); this is
// the behavioural half.
//
// Isolation: ThemesCustom.cpp's only cross-TU dependency is builtInCount(),
// given a fixed value in this file, so the registry links alone with no Preferences/Preferences graph.

#include "Core/Themes/Themes.h"

#include <string>

// The stub value is what the assertions rest on: every unified index below is asserted
// against it, so a non-zero base proves the base offset is really applied.
namespace Ubiquitty::Themes {
int builtInCount() { return 16; }
} // namespace Ubiquitty::Themes

// The internal accessor under test — declared at global scope to match the
// definition's mangling (ThemesCustom.cpp defines it there).
const Ubiquitty::Themes::Theme* customGetInternal(int local);

static Ubiquitty::Themes::Theme makeTheme(const char* name, unsigned char tag)
{
    Ubiquitty::Themes::Theme theme {};
    theme.name = name;
    theme.ansi[0] = Ubiquitty::ColorRGBA { tag, 0x00, 0x00, 0xFF };
    return theme;
}

TEST(testRegisterAssignsUnifiedIndices)
{
    using namespace Ubiquitty::Themes;
    clearCustom();
    CHECK_EQ(customCount(), 0);

    // The returned index is unified (built-ins first), not registry-local.
    CHECK_EQ(registerCustom(makeTheme("first", 0x11)), builtInCount() + 0);
    CHECK_EQ(registerCustom(makeTheme("second", 0x22)), builtInCount() + 1);
    CHECK_EQ(registerCustom(makeTheme("third", 0x33)), builtInCount() + 2);
    CHECK_EQ(customCount(), 3);

    // customGetInternal takes the LOCAL index and returns entries in
    // registration order.
    CHECK_EQ(customGetInternal(0)->ansi[0].red, 0x11);
    CHECK_EQ(customGetInternal(2)->ansi[0].red, 0x33);
    CHECK(std::string(customGetInternal(1)->name) == "second");
}

TEST(testGetInternalBounds)
{
    using namespace Ubiquitty::Themes;
    clearCustom();
    registerCustom(makeTheme("only", 0x44));

    CHECK(customGetInternal(0) != nullptr);
    CHECK(customGetInternal(-1) == nullptr); // below range
    CHECK(customGetInternal(1) == nullptr); // one past the end
    CHECK(customGetInternal(9999) == nullptr);

    clearCustom();
    CHECK(customGetInternal(0) == nullptr); // empty registry: every index misses
}

TEST(testReplaceGuards)
{
    using namespace Ubiquitty::Themes;
    const int base = builtInCount();
    clearCustom();
    registerCustom(makeTheme("a", 0x55));
    registerCustom(makeTheme("b", 0x66));

    // A built-in slot is not replaceable — the registry owns custom entries only.
    CHECK(!replaceCustom(0, makeTheme("nope", 0x00)));
    CHECK(!replaceCustom(base - 1, makeTheme("nope", 0x00)));
    // Past the last custom entry.
    CHECK(!replaceCustom(base + 2, makeTheme("nope", 0x00)));
    CHECK(!replaceCustom(base + 9999, makeTheme("nope", 0x00)));
    // A rejected replace leaves the registry untouched.
    CHECK_EQ(customCount(), 2);
    CHECK_EQ(customGetInternal(0)->ansi[0].red, 0x55);

    // In-range replace substitutes the entry in place, keeping the count.
    CHECK(replaceCustom(base + 1, makeTheme("b-revised", 0x77)));
    CHECK_EQ(customCount(), 2);
    CHECK_EQ(customGetInternal(1)->ansi[0].red, 0x77);
    CHECK(std::string(customGetInternal(1)->name) == "b-revised");
    CHECK_EQ(customGetInternal(0)->ansi[0].red, 0x55); // its sibling is undisturbed
}

TEST(testNameStorageIsOwned)
{
    using namespace Ubiquitty::Themes;
    const int base = builtInCount();
    clearCustom();

    // The registry copies the name: the stored Theme::name must not alias the
    // caller's buffer, so a caller freeing/overwriting its string is harmless.
    {
        std::string transient = "borrowed-name";
        registerCustom(makeTheme(transient.c_str(), 0x88));
        transient = "clobbered-after-registration";
    }
    CHECK(std::string(customGetInternal(0)->name) == "borrowed-name");

    // Same ownership contract on the replace path.
    {
        std::string transient = "borrowed-replacement";
        replaceCustom(base, makeTheme(transient.c_str(), 0x99));
        transient.clear();
    }
    CHECK(std::string(customGetInternal(0)->name) == "borrowed-replacement");

    // A null name normalizes to empty — never a null Theme::name for a consumer.
    registerCustom(makeTheme(nullptr, 0xAA));
    CHECK(customGetInternal(1)->name != nullptr);
    CHECK(std::string(customGetInternal(1)->name).empty());
    CHECK(replaceCustom(base, makeTheme(nullptr, 0xBB)));
    CHECK(customGetInternal(0)->name != nullptr);
    CHECK(std::string(customGetInternal(0)->name).empty());
}

TEST(testClearResets)
{
    using namespace Ubiquitty::Themes;
    clearCustom();
    registerCustom(makeTheme("x", 0xCC));
    registerCustom(makeTheme("y", 0xDD));
    CHECK_EQ(customCount(), 2);

    clearCustom();
    CHECK_EQ(customCount(), 0);
    CHECK(customGetInternal(0) == nullptr);
    CHECK(!replaceCustom(builtInCount(), makeTheme("z", 0xEE))); // nothing to replace

    // Indices restart from the base after a reload — a cleared registry is
    // indistinguishable from a fresh one.
    CHECK_EQ(registerCustom(makeTheme("post-reload", 0xFF)), builtInCount());
    CHECK_EQ(customCount(), 1);

    clearCustom(); // an already-empty clear is a no-op, not a fault
    clearCustom();
    CHECK_EQ(customCount(), 0);
}

UBIQUITTY_TEST_MAIN()
