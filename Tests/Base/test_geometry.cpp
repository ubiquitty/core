// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Base/test_geometry.cpp — Size<T>/Point<T> (Base/Geometry.h) declare the
// explicit default constructor the Swift importer's zero-fill deprecation
// requires, and keep both construction paths beside it: brace-init
// (`FloatSize { w, h }`, as OverlayAssembly.cpp writes it) and the memberwise
// constructor that exists only to keep that syntax resolving, because a type
// that declares a default constructor is not an aggregate in C++. A regression
// here reads as a compile error everywhere the brace form is used, not as a
// wrong value — this pins the values a working build could still get wrong:
// which parameter lands in which field, and that `==` compares by value rather
// than by whatever the compiler would synthesize for a type whose construction
// is not implicit.
//
// Header-only, links standalone.

#include "Base/Geometry.h"

using Ubiquitty::FloatPoint;
using Ubiquitty::FloatSize;
using Ubiquitty::IntPoint;
using Ubiquitty::IntSize;

TEST(sizeBraceInitStillResolves)
{
    // The exact call shape OverlayAssembly.cpp depends on: two positional
    // arguments in field order, not the memberwise constructor's names.
    const FloatSize s { 3.0f, 4.0f };
    CHECK(s.width == 3.0f);
    CHECK(s.height == 4.0f);
}

TEST(sizeMemberwiseConstructorOrdersFieldsCorrectly)
{
    const IntSize s(5, 7);
    CHECK_EQ(s.width, 5);
    CHECK_EQ(s.height, 7);
}

TEST(sizeDefaultConstructorZeroInitializes)
{
    const FloatSize s;
    CHECK(s.width == 0.0f);
    CHECK(s.height == 0.0f);
}

TEST(sizeEqualityComparesByValueNotIdentity)
{
    CHECK(FloatSize(1.0f, 2.0f) == FloatSize(1.0f, 2.0f));
    CHECK(!(FloatSize(1.0f, 2.0f) == FloatSize(2.0f, 1.0f)));
}

TEST(pointBraceInitStillResolves)
{
    const FloatPoint p { 10.0f, 20.0f };
    CHECK(p.x == 10.0f);
    CHECK(p.y == 20.0f);
}

TEST(pointMemberwiseConstructorOrdersFieldsCorrectly)
{
    const IntPoint p(11, 13);
    CHECK_EQ(p.x, 11);
    CHECK_EQ(p.y, 13);
}

TEST(pointDefaultConstructorZeroInitializes)
{
    const IntPoint p;
    CHECK_EQ(p.x, 0);
    CHECK_EQ(p.y, 0);
}

TEST(pointEqualityComparesByValueNotIdentity)
{
    CHECK(IntPoint(4, 9) == IntPoint(4, 9));
    CHECK(!(IntPoint(4, 9) == IntPoint(9, 4)));
}

UBIQUITTY_TEST_MAIN()
