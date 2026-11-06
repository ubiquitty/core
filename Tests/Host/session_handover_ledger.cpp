// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/SessionHandoverLedger.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Ubiquitty::Host;
using Ubiquitty::Protocol::Sync::HandoverCarriage;

namespace {

constexpr auto missingMember = [] {
    std::array<SessionHandoverMember, SessionHandoverLedger::members.size() - 1> entries;
    std::copy(SessionHandoverLedger::members.begin() + 1, SessionHandoverLedger::members.end(), entries.begin());
    return entries;
}();

constexpr auto duplicateName = [] {
    auto entries = SessionHandoverLedger::members;
    entries[1].layout.name = entries[0].layout.name;
    return entries;
}();

constexpr auto unknownCarriage = [] {
    auto entries = SessionHandoverLedger::members;
    entries[0].carriage = static_cast<HandoverCarriage>(255);
    return entries;
}();

constexpr auto unsupportedGroundRequirement = [] {
    auto entries = SessionHandoverLedger::members;
    entries[0].carriage = HandoverCarriage::AtGround;
    return entries;
}();

static_assert(!isSessionHandoverLedgerComplete(missingMember, SessionHandoverLedger::size, SessionHandoverLedger::alignment));
static_assert(!isSessionHandoverLedgerComplete(duplicateName, SessionHandoverLedger::size, SessionHandoverLedger::alignment));
static_assert(!isSessionHandoverLedgerComplete(unknownCarriage, SessionHandoverLedger::size, SessionHandoverLedger::alignment));
static_assert(!isSessionHandoverLedgerComplete(unsupportedGroundRequirement, SessionHandoverLedger::size, SessionHandoverLedger::alignment));

const SessionHandoverMember* named(std::string_view name)
{
    const auto& members = SessionHandoverLedger::members;
    const auto found = std::ranges::find_if(members, [name](const auto& member) { return member.layout.name == name; });
    return found == members.end() ? nullptr : &*found;
}

#if defined(__has_builtin)
#if __has_builtin(__builtin_dump_struct)
#define UBQ_HAS_DUMP_STRUCT 1
#endif
#endif

#if defined(UBQ_HAS_DUMP_STRUCT)
struct MemberNames {
    std::vector<std::string>& names;

    template<typename... Arguments>
    void operator()(const char*, Arguments... arguments) const { take(arguments...); }

    void take(...) const { }

    template<typename Value>
    void take(const char* indent, const char*, const char* name, Value) const
    {
        if (std::strcmp(indent, "  ") == 0)
            names.emplace_back(name);
    }

    void take(const char* indent, const char*, const char* name) const
    {
        if (std::strcmp(indent, "  ") == 0)
            names.emplace_back(name);
    }
};
#endif

} // namespace

TEST(everySessionMemberHasAPreservationRequirementEvenInsidePadding)
{
#if defined(UBQ_HAS_DUMP_STRUCT)
    Daemon daemon;
    auto& session = ScreenPlaneAccess::makeSession(daemon, 2, 4);
    std::vector<std::string> names;
    __builtin_dump_struct(&session, MemberNames { names });
    CHECK_EQ(names.size(), SessionHandoverLedger::members.size());
    for (const auto& name : names)
        CHECK_MSG(named(name), ("unclassified Session member: " + name).c_str());
    for (const auto& member : SessionHandoverLedger::members)
        CHECK(std::ranges::count(names, member.layout.name) == 1);
#else
    std::fprintf(stderr, "session_handover_ledger: SKIPPED name reflection; compiler has no __builtin_dump_struct; compile-time layout tiling remains enforced\n");
#endif
}

TEST(authorityExactConsentAndInputOwnershipCannotBeReplacedWithEmptyObjects)
{
    for (const std::string_view name : { "outputOrigin", "driverOriginFloor", "firstInputPrincipal", "hasMultipleInputPrincipals", "capabilityPolicy", "configuration",
             "lastInputClientID", "lastHumanInputClientID", "effects", "stream", "term", "inputEncoder",
             "screenSource", "coherence", "landedPastePaths", "placement" }) {
        const auto* member = named(name);
        REQUIRE(member);
        CHECK(member->carriage == HandoverCarriage::Carried);
    }
    CHECK(named("inputSink")->carriage == HandoverCarriage::Rewired);
    CHECK(named("clipboardClient")->carriage == HandoverCarriage::Rewired);
    CHECK(named("shellClient")->carriage == HandoverCarriage::Rewired);
}

TEST(publicationAndLifecycleContinuationsAreNotTreatedAsScratchCaches)
{
    for (const std::string_view name : { "isFrameDue", "pendingGeometry", "isWindowSizeDue", "presentedDueAt",
             "unpublishedSince", "lastEmittedCommandGeneration", "lastEmittedCommandStartedGeneration",
             "reportedActivityGeneration", "foregroundGeneration", "publishedEchoFacts", "adopted", "shellExited",
             "exitReason", "exitStatusDeadlineMilliseconds", "graceExpiresMilliseconds", "isGraceAwaitingHuman" }) {
        const auto* member = named(name);
        REQUIRE(member);
        CHECK(member->carriage == HandoverCarriage::Carried);
    }
    CHECK(!named("lastImageFingerprint"));
    CHECK(!named("scratchImages"));
    CHECK(named("census")->carriage == HandoverCarriage::Recomputed);
}

TEST(sessionConsentRestoresOnlyFromItsFailClosedDefaults)
{
    Daemon daemon;
    const auto& session = ScreenPlaneAccess::makeSession(daemon, 2, 4);
    CHECK(session.agentSessionGrantIdentities.empty());
    CHECK(named("agentSessionGrantIdentities")->carriage == HandoverCarriage::Recomputed);
}

UBIQUITTY_TEST_MAIN()
