// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/HandoverLedger.h"

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr auto missingTerminalMember = [] {
    std::array<HandoverLedgerEntry, terminalHandoverLedger.size() - 1> entries;
    std::copy(terminalHandoverLedger.begin() + 1, terminalHandoverLedger.end(), entries.begin());
    return entries;
}();

constexpr auto duplicateTerminalMember = [] {
    auto entries = terminalHandoverLedger;
    entries[1] = entries[0];
    return entries;
}();

constexpr auto unknownTerminalMember = [] {
    auto entries = terminalHandoverLedger;
    entries[0].name = "inventedMember";
    return entries;
}();

constexpr auto unclassifiedNewHostMember = [] {
    auto entries = terminalLedger;
    for (auto& entry : entries) {
        if (entry.carriage == Carriage::Derived) {
            entry.carriage = Carriage::HostOnly;
            break;
        }
    }
    return entries;
}();

constexpr auto groundWithoutBlocker = [] {
    auto entries = terminalHandoverLedger;
    entries[0].carriage = HandoverCarriage::AtGround;
    entries[0].blocker = HandoverBlocker::None;
    return entries;
}();

constexpr auto unknownCarriage = [] {
    auto entries = terminalHandoverLedger;
    entries[0].carriage = static_cast<HandoverCarriage>(255);
    return entries;
}();

constexpr auto unknownBlocker = [] {
    auto entries = terminalHandoverLedger;
    entries[0].blocker = static_cast<HandoverBlocker>(1u << 31);
    return entries;
}();

constexpr auto missingParserMember = [] {
    std::array<HandoverLedgerEntry, parserHandoverLedger.size() - 1> entries;
    std::copy(parserHandoverLedger.begin() + 1, parserHandoverLedger.end(), entries.begin());
    return entries;
}();

static_assert(isHandoverLedgerComplete(terminalLedger, terminalHandoverLedger));
static_assert(!isHandoverLedgerComplete(terminalLedger, missingTerminalMember));
static_assert(!isHandoverLedgerComplete(terminalLedger, duplicateTerminalMember));
static_assert(!isHandoverLedgerComplete(terminalLedger, unknownTerminalMember));
static_assert(!isHandoverLedgerComplete(unclassifiedNewHostMember, terminalHandoverLedger));
static_assert(!isHandoverLedgerComplete(terminalLedger, groundWithoutBlocker));
static_assert(!isHandoverLedgerComplete(terminalLedger, unknownCarriage));
static_assert(!isHandoverLedgerComplete(terminalLedger, unknownBlocker));
static_assert(!isHandoverLedgerComplete(entriesOf(parserLedger), missingParserMember));

template<std::size_t count>
const HandoverLedgerEntry* named(const std::array<HandoverLedgerEntry, count>& entries, std::string_view name)
{
    const auto found = std::ranges::find(entries, name, &HandoverLedgerEntry::name);
    return found == entries.end() ? nullptr : &*found;
}

} // namespace

TEST(authorityRepliesIdentitiesAndEffectsMustBeCarriedRatherThanReset)
{
    for (const std::string_view name : { "currentCapabilityOrigin", "replyAuthority", "sequenceGate",
             "osc52ClipboardReadPolicy", "osc52ClipboardWritePolicy", "outputBuffer", "bulkOutput",
             "lineStore", "historyEpochBase", "linkNextID", "linkFreeIDs", "commandExitCodeRing",
             "linkReclaimBackoffMints", "runTextReclaimBackoffInterns", "textSizeReclaimBackoffInterns",
             "bellPending", "pendingSound", "sampleBank", "typography" }) {
        const auto* entry = named(terminalHandoverLedger, name);
        REQUIRE(entry);
        CHECK(entry->carriage == HandoverCarriage::Carried);
    }
    REQUIRE(named(terminalHandoverLedger, "capabilityPolicy"));
    CHECK(named(terminalHandoverLedger, "capabilityPolicy")->carriage == HandoverCarriage::Rewired);
}

TEST(graphemeAndDecoderPolicySurviveEvenWhenParserIsAtGround)
{
    for (const std::string_view name : { "parseState", "inputEncoding", "invalidUTF8Policy", "parserInputIsUTF8",
             "clusterActive", "clusterState", "clusterCodePoints", "clusterCodepointLength" }) {
        const auto* entry = named(parserHandoverLedger, name);
        REQUIRE(entry);
        CHECK(entry->carriage == HandoverCarriage::Carried);
    }
    CHECK(named(parserHandoverLedger, "inputEncoding")->blocker == HandoverBlocker::IncompleteCharacter);
    CHECK(named(parserHandoverLedger, "clusterActive")->blocker == HandoverBlocker::None);
}

TEST(opaqueSinksAndPendingAssemblyHaveExplicitGroundRequirements)
{
    for (const std::string_view name : { "printSinkFile", "sessionLogFile", "osc52Chunk",
             "extendedClipboardWriteItems", "asyncDecodes", "kittyNotifications" }) {
        const auto* entry = named(terminalHandoverLedger, name);
        REQUIRE(entry);
        CHECK(entry->carriage == HandoverCarriage::AtGround);
        CHECK(entry->blocker != HandoverBlocker::None);
    }
    CHECK(named(terminalHandoverLedger, "images")->carriage == HandoverCarriage::Carried);
    CHECK(named(terminalHandoverLedger, "images")->blocker == HandoverBlocker::ImageTransferOpen);
    CHECK(named(terminalHandoverLedger, "scrollSearchState")->carriage == HandoverCarriage::Recomputed);
}

UBIQUITTY_TEST_MAIN()
