// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/test_aggregate_fields.cpp — the field-count helper counts, and the
// cases that make it useless are the ones asserted hardest.
//
// The assertions are not "does it count" but "does it distinguish": types of
// different arity must report different numbers, and a field added WITH A DEFAULT
// must move the count, since that is the change that alters nothing else and so
// the only one worth a checker.
//
// What these do not prove is that AnyFieldBut's constraint matters. It does not
// change the count in this formulation — measured, both ways, at four arities —
// because the search takes the largest well-formed initialiser list and a
// spurious success at one cannot lower a maximum. The constraint guards a search
// strategy nobody has written yet, so no test here can fail without it.
//

#include "AggregateFields.h"
#include "Harness.h"

#include <cstdint>

namespace {

using UbiquittyTest::aggregateFieldCount;

struct Empty { };
struct One {
    int a;
};
struct Three {
    float advance;
    float ascent;
    int flags;
};
// Three's shape, plus a field that defaults — the case a test constructing this
// type by hand would not notice, because every existing site keeps compiling.
struct ThreePlusDefaulted {
    float advance;
    float ascent;
    int flags;
    double added {};
};
struct Nested {
    Three inner;
    std::uint8_t tag;
};

} // namespace

TEST(aggregateFieldCountDistinguishesArity)
{
    CHECK(aggregateFieldCount<Empty>() == 0);
    CHECK(aggregateFieldCount<One>() == 1);
    CHECK(aggregateFieldCount<Three>() == 3);

    // Inequalities as well as equalities: a helper that returned any constant
    // would satisfy one of the lines above by luck, and these say the answer
    // moves with the type rather than being the same number three times.
    CHECK(aggregateFieldCount<Three>() != aggregateFieldCount<One>());
    CHECK(aggregateFieldCount<Empty>() != aggregateFieldCount<One>());
}

TEST(aggregateFieldCountSeesADefaultedField)
{
    // A field nobody has to pass still changes the count, which is what this
    // case exists to catch.
    CHECK(aggregateFieldCount<ThreePlusDefaulted>() == 4);
    CHECK(aggregateFieldCount<ThreePlusDefaulted>() == aggregateFieldCount<Three>() + 1);
}

TEST(aggregateFieldCountCountsMembersNotLeaves)
{
    // A nested aggregate is one field, not its own arity flattened — so pinning a
    // count says "this many members", which is what a construction site names.
    CHECK(aggregateFieldCount<Nested>() == 2);
}

// Constant expressions too, since a static_assert beside a fake is the only
// form that fails at the person adding the field.
static_assert(aggregateFieldCount<Three>() == 3);
static_assert(aggregateFieldCount<ThreePlusDefaulted>() == 4);

UBIQUITTY_TEST_MAIN()
