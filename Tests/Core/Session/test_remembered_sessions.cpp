// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_remembered_sessions — the host-to-session pairing and the one connect
// rule every frontend answers through.
//
// Two things here are decisions rather than plumbing, and both survive a wrong
// answer looking right.
//
// `decide` ranks three inputs over the host's tree. A predefined workspace name
// outranks a remembered id, and a remembered id counts only while the host
// still lists it.
// Every wrong ranking still produces a connect that works — it just connects to
// the wrong session, or opens a second shell beside the one the human meant to
// return to. So each branch is asserted against inputs where the branches
// disagree: a remembered id present beside a predefined name, a remembered
// id the list does not carry, a name the list does not carry.
//
// The record round trip carries a version byte and a length-prefixed name.
// A store holding a byte string from another build, or a truncated one, must
// decode to nothing rather than to a plausible id — recalling a wrong id is
// what makes a connect land on somebody else's session.

#include "Harness.h"

#include "Core/Session/RememberedSessions.h"
#include "Core/Session/SessionTree.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using Ubiquitty::Session::AttachPlan;
using Ubiquitty::Session::RememberedSessions;

/// A store the case can read and corrupt, standing in for the embedder's.
struct FakeStore {
    /// The bytes, by host key.
    std::map<std::string, std::string> entries;

    /// Reads a host's bytes.
    ///
    /// - Parameters:
    ///   - context: The store.
    ///   - hostKey: The host.
    ///   - bytesOut: Receives the bytes.
    /// - Returns: Whether the host has an entry.
    static bool load(void* context, const std::string& hostKey, std::string& bytesOut)
    {
        FakeStore& store = *static_cast<FakeStore*>(context);
        const auto found = store.entries.find(hostKey);
        if (found == store.entries.end())
            return false;
        bytesOut = found->second;
        return true;
    }

    /// Writes a host's bytes.
    ///
    /// - Parameters:
    ///   - context: The store.
    ///   - hostKey: The host.
    ///   - bytes: The record, or empty to forget it.
    static void store(void* context, const std::string& hostKey, const std::string& bytes)
    {
        static_cast<FakeStore*>(context)->entries[hostKey] = bytes;
    }
};

} // namespace

namespace {

using Ubiquitty::Session::AttachAsk;
using Ubiquitty::Session::AttachCandidate;
using Ubiquitty::Session::AttachDecision;
using Ubiquitty::Session::SessionTree;
using Ubiquitty::Session::TreePane;
using Ubiquitty::Session::TreeTab;
using Ubiquitty::Session::TreeWindow;
using Ubiquitty::Session::TreeWorkspace;
using Ubiquitty::Session::TreeWorkspaceState;
using Record = RememberedSessions::Record;

/// A tab whose panes the given sessions draw into; 0 is a pane no session
/// draws into.
TreeTab tabOf(std::vector<std::uint32_t> sessions, std::size_t focused = 0)
{
    TreeTab tab;
    for (const std::uint32_t session : sessions) {
        TreePane pane;
        pane.sessionID = session;
        tab.panes.push_back(pane);
    }
    tab.hostFocusedPane = focused;
    return tab;
}

/// A live workspace of one window holding the given tabs.
TreeWorkspace live(std::uint32_t id, std::string name, std::vector<TreeTab> tabs, std::size_t activeTab = 0)
{
    TreeWorkspace workspace;
    workspace.id = id;
    workspace.name = std::move(name);
    TreeWindow window;
    window.tabs = std::move(tabs);
    window.hostActiveTab = activeTab;
    workspace.windows.push_back(std::move(window));
    return workspace;
}

/// A workspace the host could restore: one recorded pane, no session.
TreeWorkspace restorable(std::uint32_t id, std::string name)
{
    TreeWorkspace workspace = live(id, std::move(name), { tabOf({ 0 }) });
    workspace.state = TreeWorkspaceState::Restorable;
    return workspace;
}

SessionTree treeOf(std::vector<TreeWorkspace> workspaces)
{
    SessionTree tree;
    tree.workspaces = std::move(workspaces);
    return tree;
}

/// A decision's candidates as the workspace and the session each lands on.
std::vector<std::pair<std::uint32_t, std::uint32_t>> pairsOf(const std::vector<AttachCandidate>& candidates)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    for (const AttachCandidate& candidate : candidates) {
        // What picking one opens is what its row would open.
        CHECK(candidate.open.workspaceID == candidate.workspaceID && candidate.open.sessionID == candidate.sessionID);
        pairs.emplace_back(candidate.workspaceID, candidate.sessionID);
    }
    return pairs;
}

/// One live workspace for each session, under the given name, as a host
/// lists sessions that each stand alone; the workspace is numbered apart
/// from its session.
SessionTree listing(std::vector<std::pair<std::uint32_t, std::string>> sessions)
{
    std::vector<TreeWorkspace> workspaces;
    for (auto& [session, name] : sessions)
        workspaces.push_back(live(1000 + session, std::move(name), { tabOf({ session }) }));
    return treeOf(std::move(workspaces));
}

/// What the host answered: nothing yet.
constexpr const SessionTree* unanswered = nullptr;

AttachDecision named(const std::string& name, const SessionTree& tree) { return RememberedSessions::decide(std::nullopt, name, &tree); }
AttachDecision returning(Record record, const SessionTree& tree) { return RememberedSessions::decide(record, "", &tree); }

} // namespace

// ---- The connect rule's ranking --------------------------------------------

// A saved host's own name wins over the id the last connect left behind: the
// name is what the human asked this host to be, and the id is only where they
// happened to land. Both inputs are present and both are listed, so a rule that
// read the id first would answer AttachByID on a scene the name already
// decides.
TEST(aPredefinedWorkspaceNameOutranksTheRememberedSession)
{
    const SessionTree listed = listing({ { 7, "" }, { 9, "work" } });

    const AttachDecision decision = RememberedSessions::decide(Record { 7, "" }, "work", &listed);

    CHECK_EQ(decision.plan, AttachPlan::AttachNamed);
    CHECK_EQ(decision.sessionID, 9u);
    CHECK_STR_EQ(decision.workspaceName, "work");
}

// A predefined name the host does not carry creates one under it rather than
// falling back to the picker: one ask, either outcome, so a saved host reaches
// its own workspace on a daemon that has just started.
TEST(aPredefinedNameNothingBearsCreatesTheWorkspace)
{
    const SessionTree listed = listing({ { 9, "other" } });

    const AttachDecision decision = named("work", listed);

    CHECK_EQ(decision.plan, AttachPlan::CreateNamed);
    CHECK_STR_EQ(decision.workspaceName, "work");
}

// With no predefined name the remembered id decides, and only while the host
// still lists it. The tree carries the id beside another session, so a rule
// that stopped at "the host has sessions" would answer ChooseFromList and ask a
// question already answered.
TEST(aRememberedSessionTheHostStillListsIsAttachedByID)
{
    const SessionTree listed = listing({ { 11, "" }, { 42, "notes" } });

    const AttachDecision decision = returning({ 42, "notes" }, listed);

    CHECK_EQ(decision.plan, AttachPlan::AttachByID);
    CHECK_EQ(decision.sessionID, 42u);
    CHECK_EQ(decision.workspaceID, 1042u);
    CHECK_STR_EQ(decision.workspaceName, "notes");
}

// A remembered id the host has forgotten names a session that ended. Re-homing
// to it would open a fresh shell while the human believed they were returning
// to their own, so the memory is dropped and the human is asked.
TEST(aRememberedSessionTheHostNoLongerListsFallsBackToTheList)
{
    const SessionTree listed = listing({ { 11, "" } });

    const AttachDecision decision = returning({ 42, "" }, listed);

    CHECK_EQ(decision.plan, AttachPlan::ChooseFromList);
    CHECK_EQ(decision.sessionID, 0u);
}

// A host that restarted holds the remembered workspace as one it could
// restore rather than the session that ended. Returning to it by name
// recreates the arrangement, where asking the human would offer the same
// workspace as one row among others.
TEST(aRememberedSessionWhoseWorkspaceTheHostCanRestoreIsAttachedByName)
{
    const SessionTree listed = treeOf({ live(1011, "", { tabOf({ 11 }) }), restorable(3, "notes") });

    const AttachDecision decision = returning({ 42, "notes" }, listed);

    CHECK_EQ(decision.plan, AttachPlan::AttachNamed);
    CHECK_MSG(decision.sessionID == 0 && decision.workspaceID == 3, "a workspace nothing runs in is marked by its id and landed on by no session");
    CHECK_STR_EQ(decision.workspaceName, "notes");
    CHECK_EQ(decision.vanishedSessionID, 42u);

    // A live session under the name is not one the remembered id was: that is
    // still the human's to choose.
    const SessionTree running = listing({ { 11, "" }, { 3, "notes" } });
    CHECK_EQ(returning({ 42, "notes" }, running).plan, AttachPlan::ChooseFromList);
}

// A host with no sessions and nothing remembered is a new session, not an empty
// picker: there is nothing to choose from, and showing a list of none asks the
// human to answer a question with no answers.
TEST(aHostWithNothingListedAndNothingRememberedCreatesANewSession)
{
    const SessionTree empty = treeOf({});

    const AttachDecision decision = RememberedSessions::decide(std::nullopt, "", &empty);

    CHECK_EQ(decision.plan, AttachPlan::CreateNew);
    CHECK_EQ(decision.sessionID, 0u);
    CHECK(decision.workspaceName.empty());
}

// An empty host offers one action. The plan is already the unnamed new session,
// so a New Session offered beside it would be a second button doing the same
// thing — which is what the Connect window showed on an empty This Mac.
TEST(anEmptyHostOffersTheNewSessionOnceAsItsPlan)
{
    const SessionTree empty = treeOf({});

    const AttachDecision decision = RememberedSessions::decide(std::nullopt, "", &empty);

    CHECK_EQ(decision.plan, AttachPlan::CreateNew);
    CHECK(!decision.isNewSessionOffered);
}

// Every other plan does something a new unnamed session does not, so New
// Session stands beside it as a second, different action. One input per plan,
// so a rule that withheld the offer from any of them is caught here.
TEST(everyPlanButTheNewSessionOffersNewSessionBesideIt)
{
    const SessionTree listed = listing({ { 42, "" }, { 43, "builds" } });
    const SessionTree empty = treeOf({});

    struct Case {
        AttachDecision decision;
        AttachPlan expected;
    };
    const Case cases[] = {
        { returning({ 42, "" }, listed), AttachPlan::AttachByID },
        { named("notes", listed), AttachPlan::CreateNamed },
        { named("builds", listed), AttachPlan::AttachNamed },
        { RememberedSessions::decide(std::nullopt, "", &listed), AttachPlan::ChooseFromList },
        { RememberedSessions::decide(std::nullopt, "", &empty), AttachPlan::CreateNew },
        { RememberedSessions::decide(std::nullopt, "", unanswered), AttachPlan::AwaitListing },
    };
    for (const Case& entry : cases) {
        const bool isDifferentAction = entry.expected != AttachPlan::CreateNew;
        CHECK_EQ(entry.decision.plan, entry.expected);
        CHECK_EQ(entry.decision.isNewSessionOffered, isDifferentAction);
    }
}

// Nothing remembered and sessions on the host is the human's question. Asserted
// apart from the forgotten-id case because the two reach the same plan by
// different routes, and a rule that lost the id check would still pass that one.
TEST(aHostWithSessionsAndNothingRememberedAsksTheHuman)
{
    const SessionTree listed = listing({ { 11, "" }, { 12, "" } });

    const AttachDecision decision = RememberedSessions::decide(std::nullopt, "", &listed);

    CHECK_EQ(decision.plan, AttachPlan::ChooseFromList);
    CHECK(decision.ask == AttachAsk::WhichWorkspace);
    CHECK(pairsOf(decision.candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 1011, 11 }, { 1012, 12 } }));
}

// A connect made before the host has answered still returns to the session the
// host was last attached on. Reading the unanswered listing as an empty one
// opens a fresh shell on every connect: the remembered id is dropped as "no
// longer listed" by a host that has not been asked yet.
TEST(aRememberedSessionWithAnUnansweredListingIsAttachedToByID)
{
    const AttachDecision decision = RememberedSessions::decide(Record { 42, "" }, "", unanswered);

    CHECK_EQ(decision.plan, AttachPlan::AttachByID);
    CHECK_EQ(decision.sessionID, 42u);
    CHECK(decision.workspaceName.empty());
}

// With nothing remembered, an unanswered listing is waited for and an answered
// empty one is a new session. The two inputs are asserted side by side because
// a rule that cannot tell them apart answers both the same way, and one of the
// two answers is a fresh shell on a host that may be holding the human's own.
TEST(anUnansweredListingWithNothingRememberedWaitsRatherThanCreates)
{
    const AttachDecision waiting = RememberedSessions::decide(std::nullopt, "", unanswered);
    CHECK_EQ(waiting.plan, AttachPlan::AwaitListing);
    CHECK_EQ(waiting.sessionID, 0u);

    const SessionTree empty = treeOf({});
    CHECK_EQ(RememberedSessions::decide(std::nullopt, "", &empty).plan, AttachPlan::CreateNew);
}

// A predefined name does not wait for the listing: the named HELLO attaches or
// creates on the daemon either way, so there is nothing for the answer to add.
TEST(aPredefinedNameDecidesWithoutAListing)
{
    const AttachDecision decision = RememberedSessions::decide(Record { 7, "" }, "work", unanswered);

    CHECK_EQ(decision.plan, AttachPlan::CreateNamed);
    CHECK_STR_EQ(decision.workspaceName, "work");
}

// A session the human selected is their own answer, so it outranks both the
// saved name and the remembered id. All three are present and listed, so a
// rule that ranked either above the choice would land on a session nobody
// selected.
TEST(aChosenSessionOutranksThePredefinedNameAndTheRememberedSession)
{
    const SessionTree listed = listing({ { 7, "" }, { 9, "work" }, { 12, "builds" } });

    const AttachDecision withName = RememberedSessions::decide(Record { 7, "" }, "work", &listed, 12);
    CHECK_EQ(withName.plan, AttachPlan::AttachByID);
    CHECK_EQ(withName.sessionID, 12u);
    CHECK_EQ(withName.workspaceID, 1012u);
    CHECK_STR_EQ(withName.workspaceName, "builds");

    const AttachDecision withoutName = RememberedSessions::decide(Record { 7, "" }, "", &listed, 12);
    CHECK_EQ(withoutName.plan, AttachPlan::AttachByID);
    CHECK_EQ(withoutName.sessionID, 12u);
}

// A chosen id the host no longer lists is never attached to: the row was
// picked from an answer that has since been replaced, and attaching to it
// blind is how a person lands on a session that ended. The rule answers as
// though nothing were chosen.
TEST(aChosenSessionTheHostNoLongerListsFallsThroughToTheRule)
{
    const SessionTree listed = listing({ { 11, "" }, { 13, "" } });

    const AttachDecision decision = RememberedSessions::decide(std::nullopt, "", &listed, 12);

    CHECK_EQ(decision.plan, AttachPlan::ChooseFromList);
    CHECK_EQ(decision.sessionID, 0u);

    // Before any answer, a chosen id has nothing to be checked against, so it
    // is not a choice at all.
    const AttachDecision waiting = RememberedSessions::decide(std::nullopt, "", unanswered, 12);
    CHECK_EQ(waiting.plan, AttachPlan::AwaitListing);
    CHECK_EQ(waiting.sessionID, 0u);
}

// The named plan names the session the name resolves to, so a frontend can
// mark its row without matching names. The workspace is not first in the
// tree, so a rule that answered the first one's session would mark the wrong
// row.
TEST(aNamedAttachCarriesTheListedSessionBearingTheName)
{
    const SessionTree listed = listing({ { 9, "other" }, { 21, "ci" } });

    const AttachDecision decision = named("ci", listed);

    CHECK_EQ(decision.plan, AttachPlan::AttachNamed);
    CHECK_EQ(decision.sessionID, 21u);
    CHECK_EQ(decision.workspaceID, 1021u);
    CHECK_STR_EQ(decision.workspaceName, "ci");

    const AttachDecision created = named("nightly", listed);
    CHECK_EQ(created.plan, AttachPlan::CreateNamed);
    CHECK_EQ(created.sessionID, 0u);
}

// The vanished fact is the remembered id only once the host has answered and
// left it out. Before the answer the host has not been asked, and while it
// still lists the id the session is alive; reporting either as ended would
// show a ghost row for a session the person can still reach.
TEST(aVanishedSessionIsReportedOnlyWhenTheAnswerOmitsIt)
{
    const Record remembered { 42, "deploy" };

    CHECK_EQ(RememberedSessions::decide(remembered, "", unanswered).vanishedSessionID, 0u);

    const SessionTree stillListed = listing({ { 42, "deploy" } });
    CHECK_EQ(returning(remembered, stillListed).vanishedSessionID, 0u);

    const SessionTree withoutIt = listing({ { 11, "" } });
    const AttachDecision gone = returning(remembered, withoutIt);
    CHECK_EQ(gone.vanishedSessionID, 42u);
    CHECK_EQ(gone.plan, AttachPlan::ChooseFromList);

    // The fact rides every plan, a named one included: the record still names
    // a session that ended, whatever this connect does instead.
    const SessionTree empty = treeOf({});
    const AttachDecision withName = RememberedSessions::decide(remembered, "ci", &empty);
    CHECK_EQ(withName.vanishedSessionID, 42u);
    CHECK_EQ(withName.plan, AttachPlan::CreateNamed);
}

// ---- What only a tree can say ------------------------------------------------

TEST(aNamedWorkspaceIsJoinedOnItsFocusedPaneWhicheverPaneIsFirst)
{
    // Session 4 is the first pane of the first tab; the host's focus is on
    // session 9, in the second tab.
    const SessionTree tree = treeOf({ live(70, "other", { tabOf({ 2 }) }), live(80, "work", { tabOf({ 4, 5 }), tabOf({ 8, 9 }, 1) }, 1) });
    const AttachDecision decision = named("work", tree);
    CHECK(decision.plan == AttachPlan::AttachNamed);
    CHECK_EQ(decision.sessionID, 9u);
    CHECK_EQ(decision.workspaceID, 80u);
    CHECK_STR_EQ(decision.workspaceName, "work");
    CHECK(decision.isNewSessionOffered);
    // A focused pane no session draws into is not landed on: the first pane
    // with a session is.
    const SessionTree unbacked = treeOf({ live(80, "work", { tabOf({ 0, 6 }) }) });
    CHECK_EQ(named("work", unbacked).sessionID, 6u);
    // Nor is a focus the tree states past what it holds.
    SessionTree bent = treeOf({ live(80, "work", { tabOf({ 4, 5 }, 7) }) });
    CHECK_EQ(named("work", bent).sessionID, 4u);
    bent.workspaces[0].hostActiveWindow = 3;
    CHECK_EQ(named("work", bent).sessionID, 4u);
    bent.workspaces[0].hostActiveWindow = 0;
    bent.workspaces[0].windows[0].hostActiveTab = 3;
    CHECK_EQ(named("work", bent).sessionID, 4u);
}

TEST(aNameTwoLiveWorkspacesBearAsksThePersonAndAttachesToNeither)
{
    const SessionTree tree = treeOf({ live(80, "work", { tabOf({ 4 }) }), live(81, "work", { tabOf({ 5 }) }) });
    const AttachDecision decision = named("work", tree);
    CHECK(decision.plan == AttachPlan::ChooseFromList);
    CHECK(decision.sessionID == 0 && decision.workspaceID == 0 && decision.workspaceName.empty());
    // The question is which of the two, each with the session picking it
    // lands on.
    CHECK(decision.ask == AttachAsk::SeveralBearTheName);
    CHECK_STR_EQ(decision.askedName, "work");
    CHECK(pairsOf(decision.candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 80, 4 }, { 81, 5 } }));
    // Nor does a remembered name pick one of them once its session is gone:
    // the person is asked of everything the host holds.
    const AttachDecision back = returning({ 3, "work" }, tree);
    CHECK(back.plan == AttachPlan::ChooseFromList && back.vanishedSessionID == 3 && back.workspaceName.empty());
    CHECK(back.ask == AttachAsk::WhichWorkspace && back.askedName.empty() && back.candidates.size() == 2);
    // A record of the same name beside them is no tie-break: it would be
    // recreated beside two that run.
    const SessionTree withRecord = treeOf({ live(80, "work", { tabOf({ 4 }) }), live(81, "work", { tabOf({ 5 }) }), restorable(60, "work") });
    CHECK(named("work", withRecord).plan == AttachPlan::ChooseFromList && named("work", withRecord).workspaceID == 0);
    CHECK_MSG(pairsOf(named("work", withRecord).candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 80, 4 }, { 81, 5 }, { 60, 0 } }), "the ones that run come first, and a record is picked by no session");
    CHECK(returning({ 3, "work" }, withRecord).plan == AttachPlan::ChooseFromList);
    // The person's own choice still does.
    const AttachDecision chosen = RememberedSessions::decide(std::nullopt, "work", &tree, 5);
    CHECK(chosen.plan == AttachPlan::AttachByID && chosen.sessionID == 5 && chosen.workspaceID == 81);
}

TEST(aLiveWorkspaceIsJoinedThoughARestorableOneBearsItsName)
{
    const SessionTree tree = treeOf({ restorable(60, "work"), live(80, "work", { tabOf({ 4 }) }) });
    const AttachDecision decision = named("work", tree);
    CHECK_MSG(decision.plan == AttachPlan::AttachNamed && decision.sessionID == 4 && decision.workspaceID == 80, "a live workspace is never recreated from a record of its name");
    // Returning by a remembered name does not guess between them: the live
    // one is not known to be the one the person left.
    const AttachDecision back = returning({ 3, "work" }, tree);
    CHECK(back.plan == AttachPlan::ChooseFromList && back.vanishedSessionID == 3);
}

TEST(aRestorableWorkspaceIsRecreatedByNameOnlyWhenTheNamePicksItAlone)
{
    const SessionTree one = treeOf({ restorable(60, "work"), live(80, "other", { tabOf({ 4 }) }) });
    const AttachDecision decision = named("work", one);
    CHECK(decision.plan == AttachPlan::AttachNamed);
    CHECK_MSG(decision.sessionID == 0 && decision.workspaceID == 60, "a workspace nothing runs in is named by its id, and lands on no session");
    const AttachDecision back = returning({ 3, "work" }, one);
    CHECK(back.plan == AttachPlan::AttachNamed && back.workspaceID == 60 && back.sessionID == 0 && back.vanishedSessionID == 3);
    CHECK_STR_EQ(back.workspaceName, "work");
    // A remembered session the host still lists is returned to by id, record
    // or no record of its old name.
    const SessionTree stillThere = treeOf({ restorable(60, "work"), live(80, "renamed", { tabOf({ 3 }) }) });
    const AttachDecision same = returning({ 3, "work" }, stillThere);
    CHECK(same.plan == AttachPlan::AttachByID && same.sessionID == 3 && same.workspaceID == 80 && same.vanishedSessionID == 0);
    CHECK_STR_EQ(same.workspaceName, "renamed");

    const SessionTree two = treeOf({ restorable(60, "work"), restorable(61, "work") });
    CHECK(named("work", two).plan == AttachPlan::ChooseFromList);
    CHECK(returning({ 3, "work" }, two).plan == AttachPlan::ChooseFromList);
    // A record under another name is not the one.
    const SessionTree another = treeOf({ restorable(60, "elsewhere") });
    CHECK(returning({ 3, "work" }, another).plan == AttachPlan::ChooseFromList);
    CHECK(named("work", another).plan == AttachPlan::CreateNamed);
}

TEST(aNameThatIsNowOnlyALabelNeitherAttachesNorCreates)
{
    TreeWorkspace renamed = live(80, "client", { tabOf({ 4 }) });
    renamed.label = "work";
    const SessionTree tree = treeOf({ renamed });
    const AttachDecision decision = named("work", tree);
    CHECK_MSG(decision.plan == AttachPlan::ChooseFromList, "a label is not a name: nothing attaches through it, and nothing is made beside it");
    CHECK(decision.sessionID == 0 && decision.workspaceID == 0);
    CHECK(decision.ask == AttachAsk::NameIsOnlyALabel && decision.askedName == "work" && decision.isNewSessionOffered);
    CHECK(pairsOf(decision.candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 80, 4 } }));
    CHECK(returning({ 3, "work" }, tree).plan == AttachPlan::ChooseFromList);
    // A workspace that bears the name outranks one only labelled it.
    const SessionTree both = treeOf({ renamed, live(81, "work", { tabOf({ 5 }) }) });
    const AttachDecision real = named("work", both);
    CHECK(real.plan == AttachPlan::AttachNamed && real.workspaceID == 81 && real.sessionID == 5);
    CHECK_MSG(real.ask == AttachAsk::None && real.candidates.empty() && real.askedName.empty(), "a plan that picked asks nothing");
    // Two that bear it are the question; the one only labelled it is not
    // among them.
    const AttachDecision two = named("work", treeOf({ renamed, live(81, "work", { tabOf({ 5 }) }), live(82, "work", { tabOf({ 6 }) }) }));
    CHECK(two.ask == AttachAsk::SeveralBearTheName && pairsOf(two.candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 81, 5 }, { 82, 6 } }));
    // A pane's, a tab's or a session's label is nobody's workspace name.
    TreeWorkspace paneLabelled = live(82, "x", { tabOf({ 6 }) });
    paneLabelled.windows[0].tabs[0].label = "work";
    paneLabelled.windows[0].tabs[0].panes[0].label = "work";
    paneLabelled.windows[0].tabs[0].panes[0].sessionLabel = "work";
    const AttachDecision made = named("work", treeOf({ paneLabelled }));
    CHECK(made.plan == AttachPlan::CreateNamed);
    CHECK_STR_EQ(made.workspaceName, "work");
}

TEST(aSessionInAWorkspaceWithNoNameIsReachedByItsIDAlone)
{
    const SessionTree tree = treeOf({ live(7, "", { tabOf({ 7 }) }), live(8, "", { tabOf({ 8 }) }) });
    // No name matches the nameless.
    const AttachDecision created = named("work", tree);
    CHECK(created.plan == AttachPlan::CreateNamed && created.sessionID == 0);
    const AttachDecision back = returning({ 8, "" }, tree);
    CHECK(back.plan == AttachPlan::AttachByID && back.sessionID == 8 && back.workspaceID == 8 && back.workspaceName.empty());
    // Once it is gone there is no name to return by, and the host still has
    // a session: the person picks.
    const AttachDecision gone = returning({ 9, "" }, tree);
    CHECK(gone.plan == AttachPlan::ChooseFromList && gone.vanishedSessionID == 9);
    // A remembered name of nothing matches no nameless workspace either.
    CHECK(returning({ 9, "" }, treeOf({ restorable(60, "") })).plan == AttachPlan::ChooseFromList);
}

TEST(aPaneNoSessionDrawsIntoIsNotASession)
{
    const SessionTree tree = treeOf({ live(80, "work", { tabOf({ 0, 4 }) }), restorable(60, "kept") });
    CHECK_MSG(RememberedSessions::decide(std::nullopt, "", &tree, 0).plan == AttachPlan::ChooseFromList, "choosing nothing chooses nothing");
    // A workspace the host could restore lists no session, whatever number a
    // record's pane carries.
    SessionTree numbered = tree;
    numbered.workspaces[1].windows[0].tabs[0].panes[0].sessionID = 12;
    const AttachDecision chosen = RememberedSessions::decide(std::nullopt, "", &numbered, 12);
    CHECK(chosen.plan == AttachPlan::ChooseFromList && chosen.sessionID == 0);
    const AttachDecision back = returning({ 12, "" }, numbered);
    CHECK(back.plan == AttachPlan::ChooseFromList && back.vanishedSessionID == 12);
    // Asked of two such records, each is picked by its name, not by a session.
    SessionTree twoKept = numbered;
    twoKept.workspaces.push_back(restorable(61, "kept"));
    CHECK(pairsOf(named("kept", twoKept).candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 60, 0 }, { 61, 0 } }));
    const AttachDecision kept = named("kept", numbered);
    CHECK_MSG(kept.plan == AttachPlan::AttachNamed && kept.workspaceID == 60 && kept.sessionID == 0, "a workspace the host would restore is landed on by no session");
}

TEST(aHostThatHasNotAnsweredIsNeverReadAsEmptyNorAsHavingLostASession)
{
    const SessionTree* unanswered = nullptr;
    const AttachDecision back = RememberedSessions::decide(Record { 3, "work" }, "", unanswered);
    CHECK(back.plan == AttachPlan::AttachByID && back.sessionID == 3 && back.vanishedSessionID == 0 && back.workspaceID == 0);
    CHECK_STR_EQ(back.workspaceName, "work");
    const AttachDecision named = RememberedSessions::decide(Record { 3, "old" }, "work", unanswered);
    CHECK(named.plan == AttachPlan::CreateNamed && named.vanishedSessionID == 0);
    CHECK_STR_EQ(named.workspaceName, "work");
    CHECK(RememberedSessions::decide(std::nullopt, "", unanswered).plan == AttachPlan::AwaitListing);
    CHECK(RememberedSessions::decide(Record { 0, "work" }, "", unanswered).plan == AttachPlan::AwaitListing);
    CHECK(RememberedSessions::decide(std::nullopt, "", unanswered, 5).plan == AttachPlan::AwaitListing);
}

TEST(theTreeRuleKeepsThePlansOrderAndWhatIsOfferedBesideThem)
{
    const SessionTree empty = treeOf({});
    const AttachDecision fresh = RememberedSessions::decide(std::nullopt, "", &empty);
    CHECK(fresh.plan == AttachPlan::CreateNew && !fresh.isNewSessionOffered);
    CHECK(RememberedSessions::decide(Record { 3, "work" }, "", &empty).plan == AttachPlan::CreateNew);
    CHECK(named("work", empty).plan == AttachPlan::CreateNamed && named("work", empty).isNewSessionOffered);
    // A host holding only what it could restore is not an empty host.
    const SessionTree recorded = treeOf({ restorable(60, "kept") });
    CHECK(RememberedSessions::decide(std::nullopt, "", &recorded).plan == AttachPlan::ChooseFromList);

    const SessionTree tree = treeOf({ live(80, "work", { tabOf({ 4, 5 }) }), live(81, "other", { tabOf({ 6 }) }) });
    // Chosen, then the name, then the remembered id.
    const AttachDecision chosen = RememberedSessions::decide(Record { 4, "work" }, "work", &tree, 6);
    CHECK(chosen.plan == AttachPlan::AttachByID && chosen.sessionID == 6 && chosen.workspaceID == 81 && chosen.isNewSessionOffered);
    CHECK_STR_EQ(chosen.workspaceName, "other");
    const AttachDecision byName = RememberedSessions::decide(Record { 6, "other" }, "work", &tree);
    CHECK(byName.plan == AttachPlan::AttachNamed && byName.workspaceID == 80 && byName.vanishedSessionID == 0);
    const AttachDecision byID = RememberedSessions::decide(Record { 5, "anything" }, "", &tree);
    CHECK(byID.plan == AttachPlan::AttachByID && byID.sessionID == 5 && byID.workspaceID == 80);
    // A chosen id the tree does not list falls through to the rule.
    const AttachDecision stale = RememberedSessions::decide(Record { 5, "work" }, "", &tree, 99);
    CHECK(stale.plan == AttachPlan::AttachByID && stale.sessionID == 5);
    // A vanished session is reported under every plan an answer reaches.
    CHECK(RememberedSessions::decide(Record { 9, "gone" }, "work", &tree).vanishedSessionID == 9);
    CHECK(RememberedSessions::decide(Record { 9, "gone" }, "", &tree, 6).vanishedSessionID == 9);
    const AttachDecision asked = RememberedSessions::decide(Record { 9, "gone" }, "", &tree);
    CHECK(asked.plan == AttachPlan::ChooseFromList && asked.vanishedSessionID == 9 && asked.isNewSessionOffered);
    CHECK(asked.ask == AttachAsk::WhichWorkspace && pairsOf(asked.candidates) == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 80, 4 }, { 81, 6 } }));
    // No other plan asks anything.
    for (const AttachDecision& picked : { chosen, byName, byID, fresh, RememberedSessions::decide(std::nullopt, "", static_cast<const SessionTree*>(nullptr)) })
        CHECK(picked.ask == AttachAsk::None && picked.candidates.empty());
}

// The pairing survives the crossing into the embedder's storage and back, name
// and id together: a round trip that dropped the name would leave a saved host
// attaching by id into a workspace it can no longer name.
TEST(aRememberedPairingRoundTripsThroughTheEmbeddersStore)
{
    FakeStore store;
    RememberedSessions sessions;
    sessions.setPersistence(&FakeStore::load, &FakeStore::store, &store);

    sessions.remember("laptop", 42, "notes");
    const std::optional<RememberedSessions::Record> recalled = sessions.recall("laptop");

    CHECK(recalled.has_value());
    CHECK_EQ(static_cast<int>(recalled->lastSessionID), 42);
    CHECK_STR_EQ(recalled->lastWorkspaceName, "notes");
}

// A host never remembered recalls nothing rather than a zeroed record, so the
// rule sees "no memory" and not "remembered session 0".
TEST(anUnknownHostRecallsNothing)
{
    FakeStore store;
    RememberedSessions sessions;
    sessions.setPersistence(&FakeStore::load, &FakeStore::store, &store);

    CHECK(!sessions.recall("laptop").has_value());
}

// Forgetting a host leaves the next connect deciding as a first one, whether
// the embedder erases the entry or keeps an empty one.
TEST(aForgottenHostRecallsNothing)
{
    FakeStore store;
    RememberedSessions sessions;
    sessions.setPersistence(&FakeStore::load, &FakeStore::store, &store);

    sessions.remember("laptop", 42, "notes");
    sessions.forget("laptop");

    CHECK(!sessions.recall("laptop").has_value());
}

// An embedder with nowhere to keep a record connects as though it had never
// been to the host: no store means no recall, and a remember that reached a
// null function would be a crash on the first successful connect.
TEST(anEmbedderWithNoStorageRemembersNothing)
{
    RememberedSessions sessions;

    sessions.remember("laptop", 42, "notes");

    CHECK(!sessions.recall("laptop").has_value());
}

TEST(aRememberedWorkspaceNameIsWholeAtItsBoundAndNeverTruncated)
{
    FakeStore store;
    RememberedSessions sessions;
    sessions.setPersistence(&FakeStore::load, &FakeStore::store, &store);
    const std::string name(Ubiquitty::Session::maxWorkspaceNameBytes, 'n');
    sessions.remember("laptop", 42, name);
    const auto remembered = sessions.recall("laptop");
    REQUIRE(remembered);
    CHECK_STR_EQ(remembered->lastWorkspaceName, name);
    sessions.remember("laptop", 43, name + "x");
    const auto unchanged = sessions.recall("laptop");
    REQUIRE(unchanged);
    CHECK_EQ(unchanged->lastSessionID, 42u);
    CHECK_STR_EQ(unchanged->lastWorkspaceName, name);
    RememberedSessions::Record oversized;
    oversized.lastSessionID = 43;
    oversized.lastWorkspaceName = name + "x";
    CHECK(RememberedSessions::encodeRecord(oversized).empty());
}

// A record whose version byte is not this build's is refused rather than read
// as this layout. The bytes that follow are a valid id and a valid name in the
// current spelling, so a decoder that ignored the version would answer a
// confident wrong pairing.
TEST(aRecordFromAnotherVersionIsRefused)
{
    RememberedSessions::Record record;
    record.lastSessionID = 42;
    record.lastWorkspaceName = "notes";
    std::string bytes = RememberedSessions::encodeRecord(record);
    CHECK(!bytes.empty());
    bytes[0] = static_cast<char>(Ubiquitty::Session::rememberedSessionRecordVersion + 1);

    CHECK(!RememberedSessions::decodeRecord(bytes).has_value());
}

// A record cut short of the name it claims decodes to nothing. The length is
// the one field a corrupted store can make larger than the bytes behind it, and
// a decoder that sized the name from it first would read past the end.
TEST(aRecordClaimingMoreNameThanItCarriesIsRefused)
{
    RememberedSessions::Record record;
    record.lastSessionID = 42;
    record.lastWorkspaceName = "notes";
    const std::string bytes = RememberedSessions::encodeRecord(record);

    CHECK(!RememberedSessions::decodeRecord(bytes.substr(0, bytes.size() - 2)).has_value());
}

// Empty bytes are not a record, which is what makes forgetting a host and
// never knowing it read the same.
TEST(emptyBytesAreNotARecord)
{
    CHECK(!RememberedSessions::decodeRecord(std::string()).has_value());
}

UBIQUITTY_TEST_MAIN()
