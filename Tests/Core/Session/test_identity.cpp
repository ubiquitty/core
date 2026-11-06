// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_identity — typed identities, labels, and the one target resolver: an
// ID is never reused, a label never shadows an ID, and an ambiguous label
// resolves to nothing and names every object it matched.

#include "Harness.h"

#include "Core/Session/Identity.h"

#include <array>
#include <string>
#include <vector>

namespace {

using Ubiquitty::Session::Identity::Allocator;
using Ubiquitty::Session::Identity::Candidate;
using Ubiquitty::Session::Identity::checkLabel;
using Ubiquitty::Session::Identity::format;
using Ubiquitty::Session::Identity::LabelBook;
using Ubiquitty::Session::Identity::LabelVerdict;
using Ubiquitty::Session::Identity::ObjectID;
using Ubiquitty::Session::Identity::ObjectKind;
using Ubiquitty::Session::Identity::parseObjectID;
using Ubiquitty::Session::Identity::ResolutionOutcome;
using Ubiquitty::Session::Identity::resolveTarget;

constexpr ObjectID pane3 { ObjectKind::Pane, 3 };
constexpr ObjectID pane7 { ObjectKind::Pane, 7 };
constexpr ObjectID window2 { ObjectKind::Window, 2 };
constexpr ObjectID session5 { ObjectKind::Session, 5 };

} // namespace

TEST(anIdentityRoundTripsThroughItsTypedSpelling)
{
    CHECK_STR_EQ(format(pane7), "pane:7");
    const auto parsed = parseObjectID("window:2");
    CHECK(parsed.has_value());
    if (parsed)
        CHECK(*parsed == window2);
    for (const char* malformed : { "pane", "pane:", "pane:0", "pane:07", "pane:-1", "pane:7x", "Pane:7", "frame:7", "pane:18446744073709551616", ":7" })
        CHECK_MSG(!parseObjectID(malformed), malformed);
}

TEST(anAllocatorNeverReusesANumberAndRefusesInsteadOfWrapping)
{
    Allocator allocator(3);
    CHECK_EQ(allocator.mint(ObjectKind::Pane)->number, 1u);
    CHECK_EQ(allocator.mint(ObjectKind::Pane)->number, 2u);
    // Kinds count independently.
    CHECK_EQ(allocator.mint(ObjectKind::Tab)->number, 1u);
    allocator.reserveThrough(ObjectID { ObjectKind::Pane, 2 });
    CHECK_EQ(allocator.mint(ObjectKind::Pane)->number, 3u);
    CHECK_MSG(!allocator.mint(ObjectKind::Pane), "an exhausted kind refuses rather than wrapping to 1");
    CHECK_MSG(!allocator.mint(ObjectKind::Pane), "and keeps refusing");

    Allocator adopted;
    adopted.reserveThrough(ObjectID { ObjectKind::Session, 41 });
    CHECK_EQ(adopted.mint(ObjectKind::Session)->number, 42u);
    adopted.reserveThrough(ObjectID { ObjectKind::Session, 10 });
    CHECK_EQ(adopted.mint(ObjectKind::Session)->number, 43u);
}

TEST(aLabelThatWouldShadowAnIdentityIsRefused)
{
    CHECK(checkLabel("logs") == LabelVerdict::Accepted);
    CHECK(checkLabel("") == LabelVerdict::Accepted);
    CHECK(checkLabel("pane:3") == LabelVerdict::LooksLikeID);
    CHECK(checkLabel("12") == LabelVerdict::LooksLikeID);
    CHECK(checkLabel("HERE") == LabelVerdict::Reserved);
    CHECK(checkLabel("tab\tname") == LabelVerdict::ControlCharacter);
    CHECK(checkLabel(std::string(65, 'x')) == LabelVerdict::TooLong);

    LabelBook book;
    CHECK(book.set(pane3, "logs") == LabelVerdict::Accepted);
    CHECK(book.set(pane3, "pane:7") == LabelVerdict::LooksLikeID);
    CHECK_STR_EQ(std::string(book.label(pane3)), "logs");
    CHECK(book.set(pane3, "") == LabelVerdict::Accepted);
    CHECK_EQ(book.count(), 0u);
}

TEST(aFullLabelBookRefusesANewLabelButStillRenames)
{
    LabelBook book;
    for (std::uint64_t number = 1; number <= LabelBook::capacity; ++number)
        CHECK(book.set(ObjectID { ObjectKind::Pane, number }, "p") == LabelVerdict::Accepted);
    CHECK(book.set(ObjectID { ObjectKind::Pane, LabelBook::capacity + 1 }, "one too many") == LabelVerdict::Full);
    CHECK(book.set(ObjectID { ObjectKind::Pane, 1 }, "renamed") == LabelVerdict::Accepted);
    book.forget(ObjectID { ObjectKind::Pane, 2 });
    CHECK(book.set(ObjectID { ObjectKind::Pane, LabelBook::capacity + 1 }, "fits now") == LabelVerdict::Accepted);
}

TEST(theResolverRefusesAnAmbiguousLabelAndNamesBothCandidates)
{
    const std::array candidates {
        Candidate { session5, "" },
        Candidate { window2, "" },
        Candidate { pane3, "logs" },
        Candidate { pane7, "LOGS" },
    };
    const auto resolution = resolveTarget("logs", candidates, ObjectKind::Pane, {});
    CHECK(resolution.outcome == ResolutionOutcome::Ambiguous);
    CHECK_MSG(!resolution.id.isValid(), "an ambiguous label resolves to nothing");
    CHECK_EQ(resolution.candidates.size(), 2u);
    CHECK_CONTAINS(resolution.message, "pane:3");
    CHECK_CONTAINS(resolution.message, "pane:7");
}

TEST(theResolverPrefersAnIdentityAndOnlyNamesLiveObjects)
{
    const std::array candidates {
        Candidate { pane3, "logs" },
        Candidate { pane7, "build" },
        Candidate { window2, "logs" },
    };
    auto resolution = resolveTarget("pane:7", candidates, std::nullopt, {});
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == pane7);

    resolution = resolveTarget("7", candidates, ObjectKind::Pane, {});
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == pane7);

    resolution = resolveTarget("pane:9", candidates, std::nullopt, {});
    CHECK(resolution.outcome == ResolutionOutcome::NotFound);
    CHECK_CONTAINS(resolution.message, "pane:9");

    resolution = resolveTarget("pane:3", candidates, ObjectKind::Window, {});
    CHECK_MSG(resolution.outcome == ResolutionOutcome::NotFound, "an identity of the wrong kind is refused, not converted");

    // The kind filter is what disambiguates a label two kinds share.
    resolution = resolveTarget("logs", candidates, ObjectKind::Window, {});
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == window2);
    resolution = resolveTarget("logs", candidates, std::nullopt, {});
    CHECK(resolution.outcome == ResolutionOutcome::Ambiguous);

    resolution = resolveTarget("nothing", candidates, std::nullopt, {});
    CHECK(resolution.outcome == ResolutionOutcome::NotFound);
}

TEST(hereResolvesToTheCallersOwnObject)
{
    const std::array candidates {
        Candidate { session5, "" },
        Candidate { window2, "" },
        Candidate { pane3, "" },
    };
    const std::array here { session5, window2, pane3 };
    auto resolution = resolveTarget("here", candidates, std::nullopt, here);
    CHECK_MSG(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == pane3, "with no kind wanted, here is the innermost: the pane");
    resolution = resolveTarget("here", candidates, ObjectKind::Window, here);
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == window2);

    resolution = resolveTarget("here", candidates, std::nullopt, {});
    CHECK_MSG(resolution.outcome == ResolutionOutcome::NoContext, "outside a pane, here names nothing");

    const std::array stale { pane7 };
    resolution = resolveTarget("here", candidates, ObjectKind::Pane, stale);
    CHECK_MSG(resolution.outcome == ResolutionOutcome::NotFound, "a here that names an ended pane is not resolved");
}

TEST(aTabAndAWorkspaceAreLabelledLikeEveryOtherObject)
{
    using Ubiquitty::Session::Identity::kindName;
    using Ubiquitty::Session::Identity::kindNamed;
    constexpr ObjectID tab2 { ObjectKind::Tab, 2 };
    constexpr ObjectID workspace2 { ObjectKind::Workspace, 2 };

    // A workspace is an identity like the others: written, read and minted.
    CHECK_STR_EQ(std::string(kindName(ObjectKind::Workspace)), "workspace");
    CHECK(kindNamed("workspace") == ObjectKind::Workspace);
    CHECK_STR_EQ(format(workspace2), "workspace:2");
    CHECK(parseObjectID("workspace:2") == workspace2);
    CHECK_MSG(checkLabel("workspace:3") == LabelVerdict::LooksLikeID, "a label may not read as a workspace's identity either");
    Allocator allocator;
    CHECK_EQ(allocator.mint(ObjectKind::Tab)->number, 1u);
    CHECK_MSG(allocator.mint(ObjectKind::Workspace)->number == 1u, "a workspace counts on its own");
    CHECK_EQ(allocator.mint(ObjectKind::Workspace)->number, 2u);

    // The same number of two kinds is two objects with two labels.
    LabelBook book;
    CHECK(book.set(tab2, "build") == LabelVerdict::Accepted);
    CHECK(book.set(workspace2, "work") == LabelVerdict::Accepted);
    CHECK_STR_EQ(std::string(book.label(tab2)), "build");
    CHECK_STR_EQ(std::string(book.label(workspace2)), "work");
    CHECK_STR_EQ(std::string(book.label(pane3)), "");

    // The bounds and the refusals are the ones every label has.
    for (const ObjectID id : { tab2, workspace2 }) {
        CHECK(book.set(id, std::string(64, 'x')) == LabelVerdict::Accepted);
        CHECK(book.set(id, std::string(65, 'x')) == LabelVerdict::TooLong);
        CHECK(book.set(id, "two\nlines") == LabelVerdict::ControlCharacter);
        CHECK(book.set(id, "tab:9") == LabelVerdict::LooksLikeID);
        CHECK(book.set(id, "44") == LabelVerdict::LooksLikeID);
        CHECK(book.set(id, "here") == LabelVerdict::Reserved);
        CHECK_MSG(book.label(id) == std::string(64, 'x'), "a refused label leaves the one it had");
    }
    CHECK(book.set(tab2, "") == LabelVerdict::Accepted);
    CHECK_MSG(book.count() == 1 && book.label(workspace2) == std::string(64, 'x'), "clearing the tab's label left the workspace's");
    book.forget(workspace2);
    CHECK_EQ(book.count(), 0u);

    // They take slots of the same bounded book.
    for (std::uint64_t number = 1; number <= LabelBook::capacity / 2; ++number) {
        CHECK(book.set(ObjectID { ObjectKind::Tab, number }, "t") == LabelVerdict::Accepted);
        CHECK(book.set(ObjectID { ObjectKind::Workspace, number }, "w") == LabelVerdict::Accepted);
    }
    CHECK(book.set(ObjectID { ObjectKind::Workspace, LabelBook::capacity }, "one too many") == LabelVerdict::Full);
    book.prune([](ObjectID id) { return id.kind != ObjectKind::Tab; });
    CHECK_EQ(book.count(), LabelBook::capacity / 2);
    CHECK(book.set(ObjectID { ObjectKind::Workspace, LabelBook::capacity }, "fits now") == LabelVerdict::Accepted);

    // A label finds its tab or workspace among the candidates of that kind.
    const std::array candidates {
        Candidate { tab2, "build" },
        Candidate { workspace2, "build" },
        Candidate { pane3, "" },
    };
    auto resolution = resolveTarget("build", candidates, ObjectKind::Workspace, {});
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == workspace2);
    resolution = resolveTarget("BUILD", candidates, ObjectKind::Tab, {});
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == tab2);
    resolution = resolveTarget("workspace:2", candidates, std::nullopt, {});
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == workspace2);

    // `here` reaches the workspace when it is wanted, and last when nothing is.
    const std::array inside { workspace2, tab2 };
    resolution = resolveTarget("here", candidates, ObjectKind::Workspace, inside);
    CHECK(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == workspace2);
    resolution = resolveTarget("here", candidates, std::nullopt, inside);
    CHECK_MSG(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == tab2, "the tab is inside the workspace");
    const std::array onlyWorkspace { workspace2 };
    resolution = resolveTarget("here", candidates, std::nullopt, onlyWorkspace);
    CHECK_MSG(resolution.outcome == ResolutionOutcome::Resolved && resolution.id == workspace2, "the workspace is the outermost thing a process runs in");
}

TEST(everyRefusedSetOfTagsHasASentenceThatSaysWhy)
{
    using Ubiquitty::Session::Identity::TagVerdict;
    using Ubiquitty::Session::Identity::tagVerdictSentence;
    CHECK(tagVerdictSentence(TagVerdict::Accepted).empty());
    const TagVerdict refusals[] = { TagVerdict::TooMany, TagVerdict::Empty, TagVerdict::TooLong, TagVerdict::ControlCharacter, TagVerdict::Full };
    for (std::size_t index = 0; index < std::size(refusals); ++index) {
        CHECK(!tagVerdictSentence(refusals[index]).empty());
        for (std::size_t other = 0; other < index; ++other)
            CHECK(tagVerdictSentence(refusals[index]) != tagVerdictSentence(refusals[other]));
    }
    // The bounds a sentence states are the ones the book holds to.
    CHECK(tagVerdictSentence(TagVerdict::TooMany).find(std::to_string(Ubiquitty::Session::Identity::maxTagsPerObject)) != std::string_view::npos);
    CHECK(tagVerdictSentence(TagVerdict::TooLong).find(std::to_string(Ubiquitty::Session::Identity::maxLabelLength)) != std::string_view::npos);
}

TEST(theTagsAPersonPutsOnAnObjectAreHostStateUnderBounds)
{
    using Ubiquitty::Session::Identity::maxTagsPerObject;
    using Ubiquitty::Session::Identity::TagBook;
    using Ubiquitty::Session::Identity::TagVerdict;
    const auto names = [](std::initializer_list<const char*> list) { return std::vector<std::string>(list.begin(), list.end()); };
    const auto held = [](const TagBook& book, ObjectID id) {
        const auto tags = book.tags(id);
        return std::vector<std::string>(tags.begin(), tags.end());
    };

    TagBook book;
    CHECK(book.tags(window2).empty());
    CHECK(book.set(window2, names({ "production", "billing" })) == TagVerdict::Accepted);
    CHECK(held(book, window2) == names({ "production", "billing" }));
    CHECK_MSG(held(book, pane3).empty() && book.count() == 1, "tags are one object's");

    // Setting replaces; a name given twice, whatever its case, is one tag
    // under its first spelling.
    CHECK(book.set(window2, names({ "Staging", "billing", "STAGING", "Billing" })) == TagVerdict::Accepted);
    CHECK(held(book, window2) == names({ "Staging", "billing" }));

    // What a tag may not be; a refused set leaves the tags as they were.
    CHECK(book.set(window2, names({ "ok", "" })) == TagVerdict::Empty);
    CHECK(book.set(window2, names({ std::string(65, 'x').c_str() })) == TagVerdict::TooLong);
    CHECK(book.set(window2, names({ std::string(64, 'x').c_str() })) == TagVerdict::Accepted);
    CHECK(book.set(window2, names({ "two\nlines" })) == TagVerdict::ControlCharacter);
    std::vector<std::string> many;
    for (std::size_t index = 0; index <= maxTagsPerObject; ++index)
        many.push_back("t" + std::to_string(index));
    CHECK(book.set(window2, many) == TagVerdict::TooMany);
    CHECK(held(book, window2) == names({ std::string(64, 'x').c_str() }));
    many.pop_back();
    CHECK(book.set(window2, many) == TagVerdict::Accepted);
    CHECK_EQ(book.tags(window2).size(), maxTagsPerObject);
    // Unlike a label, a tag is not a target: it may read as anything.
    CHECK(book.set(window2, names({ "pane:3", "12", "here" })) == TagVerdict::Accepted);

    // No tags clears, and an object that ends is forgotten.
    CHECK(book.set(window2, {}) == TagVerdict::Accepted);
    CHECK_EQ(book.count(), 0u);
    CHECK(book.set(pane3, names({ "a" })) == TagVerdict::Accepted);
    book.forget(pane3);
    CHECK_EQ(book.count(), 0u);

    // The book is bounded and a prune makes room.
    for (std::uint64_t number = 1; number <= TagBook::capacity; ++number)
        CHECK(book.set(ObjectID { ObjectKind::Window, number }, names({ "w" })) == TagVerdict::Accepted);
    CHECK(book.set(ObjectID { ObjectKind::Window, TagBook::capacity + 1 }, names({ "one too many" })) == TagVerdict::Full);
    CHECK_MSG(book.set(ObjectID { ObjectKind::Window, 1 }, names({ "retagged" })) == TagVerdict::Accepted, "a full book still retags what it holds");
    book.prune([](ObjectID id) { return id.number % 2 == 0; });
    CHECK_EQ(book.count(), TagBook::capacity / 2);
    CHECK(book.set(ObjectID { ObjectKind::Window, TagBook::capacity + 1 }, names({ "fits now" })) == TagVerdict::Accepted);
}

UBIQUITTY_TEST_MAIN()
