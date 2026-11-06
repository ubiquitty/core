// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_host_directory — the one seam every frontend reaches the pairing and the
// connect rule through.
//
// The rule itself is asserted in test_remembered_sessions. What is asserted
// here is the crossing, and every part of it survives a wrong answer looking
// right.
//
// The rule reads the host's tree beside the record this seam recalls, so a
// recall that crossed the wrong record would still answer a plan — just the
// wrong one. Each branch is therefore reached through `HostDirectory::decide`
// over a tree, and the decision a frontend makes before the host has answered
// through `decideBeforeListing`, which must never read the unanswered host as
// an empty one.
//
// The plan crosses with the id and name it chose, in the one `decide` answer:
// a decision that answered the right plan beside a stale id would attach to
// whatever the previous connect landed on, so the triple is asserted whole
// against the input that produced it.
//
// The store crosses as a byte buffer and a length rather than as the
// std::string the rule takes, because no `@convention(c)` function may name a
// C++ reference. A crossing that lost a byte still decodes to a record for
// every truncation the version byte survives, and recalling a wrong id is what
// lands a connect on somebody else's session.

#include "Harness.h"

#include "Core/Runtime/HostDirectory.h"
#include "Core/Session/RememberedSessions.h"
#include "Core/Session/SessionTree.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Ubiquitty::Runtime::HostDirectory;
using Ubiquitty::Session::AttachDecision;
using Ubiquitty::Session::AttachPlan;
using Ubiquitty::Session::SessionTree;

/// A store the case can read and corrupt, standing in for the embedder's.
struct FakeStore {
    /// The bytes, by host key.
    std::map<std::string, std::string> entries;

    /// Reads a host's bytes. ``HostDirectory::Load``.
    ///
    /// - Parameters:
    ///   - context: The store.
    ///   - hostKey: The host.
    ///   - buffer: Receives up to `capacity` bytes.
    ///   - capacity: What the buffer holds.
    /// - Returns: The full stored length, 0 for a host with nothing.
    static std::size_t load(void* context, const char* hostKey, std::uint8_t* buffer, std::size_t capacity)
    {
        FakeStore& store = *static_cast<FakeStore*>(context);
        const auto found = store.entries.find(hostKey);
        if (found == store.entries.end())
            return 0;
        const std::string& bytes = found->second;
        if (buffer != nullptr && capacity > 0 && !bytes.empty())
            std::memcpy(buffer, bytes.data(), std::min(capacity, bytes.size()));
        return bytes.size();
    }

    /// Writes a host's bytes. ``HostDirectory::Store``.
    ///
    /// - Parameters:
    ///   - context: The store.
    ///   - hostKey: The host.
    ///   - bytes: The record.
    ///   - byteCount: Its length, 0 to forget the host.
    static void store(void* context, const char* hostKey, const std::uint8_t* bytes, std::size_t byteCount)
    {
        static_cast<FakeStore*>(context)->entries[hostKey]
            = std::string(reinterpret_cast<const char*>(bytes), byteCount);
    }
};

/// Installs a store, so a case starts on a directory that carries nothing from
/// the one before it.
void openDirectoryOver(FakeStore& store)
{
    HostDirectory::setPersistence(&FakeStore::load, &FakeStore::store, &store);
}

/// One live workspace for each session, under the given name, as a host
/// lists sessions that each stand alone; the workspace is numbered apart
/// from its session.
SessionTree listing(std::vector<std::pair<std::uint32_t, std::string>> sessions)
{
    SessionTree tree;
    for (auto& [session, name] : sessions) {
        Ubiquitty::Session::TreePane pane;
        pane.sessionID = session;
        Ubiquitty::Session::TreeTab tab;
        tab.panes.push_back(pane);
        Ubiquitty::Session::TreeWindow window;
        window.tabs.push_back(std::move(tab));
        Ubiquitty::Session::TreeWorkspace workspace;
        workspace.id = 1000 + session;
        workspace.name = std::move(name);
        workspace.windows.push_back(std::move(window));
        tree.workspaces.push_back(std::move(workspace));
    }
    return tree;
}

} // namespace

// A saved host's own name wins over the id the last connect left behind, and
// both cross this seam: the name arrives as an argument, the id through the
// store. A recall that answered over the wrong record would still pick the
// named workspace; one that ranked the id first would answer AttachByID.
TEST(aPredefinedNameTheListingBearsAttachesToIt)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::remember("laptop", 7, "");
    const SessionTree listed = listing({ { 7, "" }, { 9, "work" } });

    const AttachDecision decision = HostDirectory::decide("laptop", "work", &listed);

    CHECK_EQ(decision.plan, AttachPlan::AttachNamed);
    CHECK_EQ(decision.sessionID, 9u);
    CHECK_STR_EQ(decision.workspaceName, "work");
}

// A predefined name nothing listed bears creates one under it. Listed beside a
// workspace carrying a different name, so a comparison that matched any named
// workspace would answer AttachNamed and attach to somebody else's.
TEST(aPredefinedNameNothingBearsCreatesTheWorkspace)
{
    FakeStore store;
    openDirectoryOver(store);
    const SessionTree listed = listing({ { 9, "other" } });

    const AttachDecision decision = HostDirectory::decide("laptop", "work", &listed);

    CHECK_EQ(decision.plan, AttachPlan::CreateNamed);
    CHECK_STR_EQ(decision.workspaceName, "work");
}

// The remembered pairing reaches the rule through the store, and both halves of
// it reach the caller back in the answer. A decision whose id or name did not
// follow the plan would reattach to whatever the last decision named.
TEST(aRememberedSessionTheHostStillListsIsAttachedByID)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::remember("laptop", 42, "notes");
    const SessionTree listed = listing({ { 11, "" }, { 42, "notes" } });

    const AttachDecision decision = HostDirectory::decide("laptop", "", &listed);

    CHECK_EQ(decision.plan, AttachPlan::AttachByID);
    CHECK_EQ(decision.sessionID, 42u);
    CHECK_STR_EQ(decision.workspaceName, "notes");
}

// The human's choice and the vanished fact cross in the same answer as the
// plan. The remembered id is not listed and a different session is chosen, so
// a crossing that dropped the choice would answer ChooseFromList and one that
// dropped the fact would leave the ended session unshown.
TEST(aDecisionCarriesTheChoiceAndTheVanishedSession)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::remember("laptop", 42, "deploy");
    const SessionTree listed = listing({ { 11, "" }, { 12, "builds" } });

    const AttachDecision decision = HostDirectory::decide("laptop", "", &listed, 12);

    CHECK_EQ(decision.plan, AttachPlan::AttachByID);
    CHECK_EQ(decision.sessionID, 12u);
    CHECK_EQ(decision.vanishedSessionID, 42u);
    CHECK_STR_EQ(decision.workspaceName, "builds");
    HostDirectory::forget("laptop");
}

// Whether New Session stands beside the plan crosses the same answer. An empty
// host's plan is the new session, so the offer reads false there and true over
// a host with something to choose.
TEST(aDecisionCarriesWhetherNewSessionIsASecondAction)
{
    FakeStore store;
    openDirectoryOver(store);

    const SessionTree empty;
    const AttachDecision fresh = HostDirectory::decide("desk", "", &empty);
    CHECK_EQ(fresh.plan, AttachPlan::CreateNew);
    CHECK(!fresh.isNewSessionOffered);

    const SessionTree listed = listing({ { 11, "" } });
    const AttachDecision asked = HostDirectory::decide("desk", "", &listed);
    CHECK_EQ(asked.plan, AttachPlan::ChooseFromList);
    CHECK(asked.isNewSessionOffered);
}

// A remembered id the host no longer lists names a session that ended, so the
// human is asked. The answer carries no id or name with it: a caller that took
// a previous decision's target here would offer a session nothing listed.
TEST(aRememberedSessionTheHostNoLongerListsAsksTheHuman)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::remember("laptop", 42, "notes");
    const SessionTree listed = listing({ { 11, "" } });

    const AttachDecision decision = HostDirectory::decide("laptop", "", &listed);

    CHECK_EQ(decision.plan, AttachPlan::ChooseFromList);
    CHECK_EQ(decision.sessionID, 0u);
    CHECK(decision.workspaceName.empty());
}

// A host with nothing listed and nothing remembered is a new session.
TEST(aHostWithNothingListedAndNothingRememberedCreatesANewSession)
{
    FakeStore store;
    openDirectoryOver(store);
    const SessionTree empty;

    const AttachDecision decision = HostDirectory::decide("laptop", "", &empty);

    CHECK_EQ(decision.plan, AttachPlan::CreateNew);
    CHECK_EQ(decision.sessionID, 0u);
    CHECK(decision.workspaceName.empty());
}

// An unanswered host and an answered empty one are different inputs and answer
// different plans. With nothing remembered the first waits and the second
// creates; with a remembered id the first attaches to it, because a host that
// has not answered has not said the session ended. The decision a frontend
// makes before any listing is the unanswered one, through the same record.
TEST(anUnansweredHostIsNotAnAnsweredEmptyOne)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::forget("laptop");

    CHECK_EQ(HostDirectory::decide("laptop", "", nullptr).plan, AttachPlan::AwaitListing);
    CHECK_EQ(HostDirectory::decideBeforeListing("laptop", "").plan, AttachPlan::AwaitListing);
    const SessionTree empty;
    CHECK_EQ(HostDirectory::decide("laptop", "", &empty).plan, AttachPlan::CreateNew);

    HostDirectory::remember("laptop", 42, "notes");
    const AttachDecision remembered = HostDirectory::decideBeforeListing("laptop", "");
    CHECK_EQ(remembered.plan, AttachPlan::AttachByID);
    CHECK_EQ(remembered.sessionID, 42u);
    CHECK_STR_EQ(remembered.workspaceName, "notes");
    CHECK_EQ(remembered.vanishedSessionID, 0u);

    // A saved host's name is created under, before or without an answer.
    const AttachDecision named = HostDirectory::decideBeforeListing("laptop", "work");
    CHECK_EQ(named.plan, AttachPlan::CreateNamed);
    CHECK_STR_EQ(named.workspaceName, "work");
}

// The pairing survives the crossing into the embedder's byte-buffer store and
// back, id and name together. The length crosses separately from the bytes, so
// a record read one byte short would decode to a plausible record rather than
// to nothing.
TEST(aRememberedPairingRoundTripsThroughTheEmbeddersStore)
{
    FakeStore store;
    openDirectoryOver(store);

    HostDirectory::remember("laptop", 42, "notes");

    CHECK_EQ(static_cast<int>(HostDirectory::rememberedSessionID("laptop")), 42);
    CHECK_STR_EQ(HostDirectory::rememberedWorkspaceName("laptop"), "notes");
}

// A host never remembered recalls nothing rather than a zeroed record, so the
// rule sees no memory and not a remembered session 0.
TEST(anUnknownHostRecallsNothing)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::remember("laptop", 42, "notes");

    CHECK_EQ(static_cast<int>(HostDirectory::rememberedSessionID("desktop")), 0);
    CHECK(HostDirectory::rememberedWorkspaceName("desktop").empty());
}

// Forgetting a host makes the next connect decide as a first one: the store
// still holds an entry for it, and what makes the pairing gone is that the
// entry decodes to no record.
TEST(aForgottenHostRecallsNothing)
{
    FakeStore store;
    openDirectoryOver(store);
    HostDirectory::remember("laptop", 42, "notes");

    HostDirectory::forget("laptop");

    CHECK_EQ(static_cast<int>(HostDirectory::rememberedSessionID("laptop")), 0);
    CHECK(HostDirectory::rememberedWorkspaceName("laptop").empty());
}

// An embedder with nowhere to keep a record connects as though it had never
// been to the host: every recall answers nothing and every remember is dropped,
// rather than the directory keeping a memory of its own that a restart loses.
TEST(aDirectoryWithNoStoreRemembersNothing)
{
    HostDirectory::setPersistence(nullptr, nullptr, nullptr);

    HostDirectory::remember("laptop", 42, "notes");

    CHECK_EQ(static_cast<int>(HostDirectory::rememberedSessionID("laptop")), 0);
    CHECK(HostDirectory::rememberedWorkspaceName("laptop").empty());
}

// ---------------------------------------------------------------------------
// Replacement vs. recall stress: the lease owns coherence.
// ---------------------------------------------------------------------------
//
// One thread replaces persistence while others decide over the host's tree. Every decision must answer from whole state — a plan that agrees
// with the id and name it carries — and after the storm the newest install
// wins. Two installs alternate, seeded with same-shape but different-content
// records, so a torn read across installs decodes to a mixed pair (or
// refuses into a plan the pair disagrees with) instead of passing quietly.
//
// No static harness pointers: every hook receives its context and casts it,
// so the only shared test state is counters the workers own and capture.
//
// The stores serialize their own entries (their map is theirs, not the
// directory's); what the directory owes is that each operation runs one
// install's pair whole, which holds by construction once publishing and
// snapshotting share one lock and no embedder call runs under it.

namespace {

/// A store proving coherence by content: any pair it serves is one it holds
/// whole, so a mixed pair names a torn read across installs.
struct ProvingStore {
    /// Entries, under their own lock (the store's, not the directory's).
    std::map<std::string, std::string> entries;
    std::mutex entriesMutex;

    static std::size_t load(void* context, const char* hostKey, std::uint8_t* buffer, std::size_t capacity)
    {
        ProvingStore& self = *static_cast<ProvingStore*>(context);
        const std::lock_guard<std::mutex> guard(self.entriesMutex);
        const auto found = self.entries.find(hostKey);
        if (found == self.entries.end())
            return 0;
        const std::string& bytes = found->second;
        if (buffer != nullptr && capacity > 0 && !bytes.empty())
            std::memcpy(buffer, bytes.data(), std::min(capacity, bytes.size()));
        return bytes.size();
    }

    static void store(void* context, const char* hostKey, const std::uint8_t* bytes, std::size_t byteCount)
    {
        ProvingStore& self = *static_cast<ProvingStore*>(context);
        const std::lock_guard<std::mutex> guard(self.entriesMutex);
        self.entries[hostKey] = std::string(reinterpret_cast<const char*>(bytes), byteCount);
    }

    void seed(const std::string& host, const std::string& encoded)
    {
        const std::lock_guard<std::mutex> guard(entriesMutex);
        entries[host] = encoded;
    }
};

/// A store whose closures re-enter the directory: the load recalls, the store
/// recalls and replaces with the same lease. No directory lock is ever held
/// across an embedder call, so all of it completes inline instead of
/// deadlocking — and the operation in flight keeps the lease it started with
/// while the next one sees the replacement.
///
/// Re-entry is keyed, not counted: closures re-enter only for hosts other
/// than "other", so the nested call serves directly instead of recursing.
struct ReentrantStore {
    std::map<std::string, std::string> entries;

    static std::size_t load(void* context, const char* hostKey, std::uint8_t* buffer, std::size_t capacity)
    {
        // Re-enter before answering: a lock held across this call would
        // deadlock here, non-recursively, on the first recall.
        if (std::string_view(hostKey) != "other")
            (void)HostDirectory::rememberedSessionID("other");
        ReentrantStore& self = *static_cast<ReentrantStore*>(context);
        const auto found = self.entries.find(hostKey);
        if (found == self.entries.end())
            return 0;
        const std::string& bytes = found->second;
        if (buffer != nullptr && capacity > 0 && !bytes.empty())
            std::memcpy(buffer, bytes.data(), std::min(capacity, bytes.size()));
        return bytes.size();
    }
    static void store(void* context, const char* hostKey, const std::uint8_t* bytes, std::size_t byteCount)
    {
        // Re-enter with a read and a same-lease replacement: both take a free
        // lock, because neither runs under one.
        if (std::string_view(hostKey) != "other") {
            (void)HostDirectory::rememberedSessionID("other");
            HostDirectory::setPersistence(&ReentrantStore::load, &ReentrantStore::store, context);
        }
        ReentrantStore& self = *static_cast<ReentrantStore*>(context);
        self.entries[hostKey] = std::string(reinterpret_cast<const char*>(bytes), byteCount);
    }
};

} // namespace

// Replacing persistence while others decide over the host's tree: every
// decision answers from whole state — a plan that agrees with the id and name
// it carries — and the newest install wins after the storm. Two installs
// alternate, seeded with same-length but different-content records, so a torn
// read across installs decodes to a mixed pair (or refuses into a plan the
// pair disagrees with) instead of passing quietly.
TEST(aReplacementStormDecidesWholeAnswersAndLandsTheNewest)
{
    using Lease = std::pair<std::uint32_t, std::string>;
    const Lease seedA { 7, "a" };
    const Lease seedB { 8, "b" };

    ProvingStore storeA;
    ProvingStore storeB;
    // Same byte length, different content: a torn probe/sized-read pair still
    // decodes, into a MIX — which is exactly what must never happen.
    storeA.seed("storm", Ubiquitty::Session::RememberedSessions::encodeRecord({ seedA.first, seedA.second }));
    storeB.seed("storm", Ubiquitty::Session::RememberedSessions::encodeRecord({ seedB.first, seedB.second }));
    CHECK(storeA.entries.at("storm").size() == storeB.entries.at("storm").size());

    HostDirectory::setPersistence(&ProvingStore::load, &ProvingStore::store, &storeA);

    std::atomic<bool> stop { false };
    std::atomic<std::uint64_t> operations { 0 };
    std::atomic<std::uint64_t> incoherent { 0 };

    // The replacer: alternates installs as fast as it can.
    std::thread replacer([&] {
        bool useA = false;
        while (!stop.load()) {
            useA = !useA;
            if (useA)
                HostDirectory::setPersistence(&ProvingStore::load, &ProvingStore::store, &storeA);
            else
                HostDirectory::setPersistence(&ProvingStore::load, &ProvingStore::store, &storeB);
        }
    });

    // Workers: decide over the tree, and check the triple agrees.
    // The rule echoes the recalled pair only on AttachByID; every other plan
    // carries (0, ""). A torn recall mixes installs — an attached id beside
    // the other install's name, or a refusal the plan disagrees with — and any
    // of it fails the agreement below.
    constexpr int workerCount = 3;
    constexpr int iterations = 20000;
    const SessionTree listed = listing({ { seedA.first, seedA.second }, { seedB.first, seedB.second } });
    std::vector<std::thread> workers;
    for (int w = 0; w < workerCount; ++w) {
        workers.emplace_back([&, w] {
            for (int i = 0; i < iterations; ++i) {
                const AttachDecision decided = HostDirectory::decide("storm", "", &listed);
                const Lease answer { decided.sessionID, decided.workspaceName };
                if (decided.plan == AttachPlan::AttachByID) {
                    if (!(answer == seedA || answer == seedB))
                        ++incoherent;
                } else if (decided.plan == AttachPlan::ChooseFromList) {
                    if (!(answer.first == 0 && answer.second.empty()))
                        ++incoherent;
                } else {
                    ++incoherent;
                }
                // Keep the store path hot with same-length worker records whose
                // ids never collide with the listed seeds: a worker pair that
                // attached would be coherent but indistinguishable from a
                // mixed one, so the ranges are disjoint by construction.
                HostDirectory::remember("storm", static_cast<std::uint32_t>(1000000 + w * 100000 + i), "w");
                ++operations;
            }
        });
    }
    for (std::thread& worker : workers)
        worker.join();
    stop.store(true);
    replacer.join();

    CHECK_EQ(incoherent.load(), 0u);
    CHECK(operations.load() == static_cast<std::uint64_t>(workerCount) * iterations);

    // Latest wins: quiesce on a final install and the directory answers it.
    ProvingStore storeFinal;
    HostDirectory::setPersistence(&ProvingStore::load, &ProvingStore::store, &storeFinal);
    HostDirectory::remember("storm", 99, "final");
    CHECK_EQ(static_cast<int>(HostDirectory::rememberedSessionID("storm")), 99);
    CHECK_STR_EQ(HostDirectory::rememberedWorkspaceName("storm"), "final");

    // Leave nothing installed for the cases after this one.
    HostDirectory::setPersistence(nullptr, nullptr, nullptr);
}

// An embedder closure that re-enters the directory must not deadlock: no
// directory lock is ever held across an embedder call, so a load that recalls
// and a store that recalls and replaces complete inline.
TEST(anEmbedderClosureThatReentersTheDirectoryDoesNotDeadlock)
{
    ReentrantStore store;

    HostDirectory::setPersistence(&ReentrantStore::load, &ReentrantStore::store, &store);
    // Seed through the reentrant store so both directions run at least once.
    HostDirectory::remember("laptop", 42, "notes");

    CHECK_EQ(static_cast<int>(HostDirectory::rememberedSessionID("laptop")), 42);
    CHECK_STR_EQ(HostDirectory::rememberedWorkspaceName("laptop"), "notes");

    HostDirectory::setPersistence(nullptr, nullptr, nullptr);
}

UBIQUITTY_TEST_MAIN()
