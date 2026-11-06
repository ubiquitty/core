// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// DestructiveConfirmation.h — the question asked before a request ends
// sessions on a host, and the token that proves it was asked.
//
// Closing a window and terminating a session or a workspace end programs
// somebody may be watching. What the question says is read from the host's
// tree here, so every frontend asks the same one: what is named, how many
// sessions end, what runs in them, and what the two buttons are called.
//
// Asking returns a token. A request that ends sessions is sent only with the
// token its own question returned, once, and only while it would still end
// what the question said. No frontend sends one it did not ask about, and
// none sends one whose answer was given about a tree that has since changed.
//

#include "Base/SwiftInterop.h"
#include "Core/Session/Identity.h"
#include "Core/Session/SessionTree.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

/// A request that ends sessions. The values are explicit because the Swift
/// and web mirrors pin them.
enum class DestructiveRequest : std::uint8_t {
    /// Close a window and end the sessions in its panes.
    CloseWindow UBIQUITTY_SWIFT_NAME(closeWindow) = 1,
    /// End one session: the target is the session, or the pane it draws
    /// into.
    TerminateSession UBIQUITTY_SWIFT_NAME(terminateSession) = 2,
    /// End every session of a workspace, and so the workspace.
    TerminateWorkspace UBIQUITTY_SWIFT_NAME(terminateWorkspace) = 3,
};

/// What a button of the question does. The values are explicit because the
/// Swift and web mirrors pin them.
enum class ConfirmationButtonRole : std::uint8_t {
    /// Ends nothing. The default: the one Return picks, drawn as a default
    /// button is.
    SafeDefault UBIQUITTY_SWIFT_NAME(safeDefault) = 0,
    /// Goes through with it. Drawn as a destructive button is, and never
    /// the default.
    Destructive UBIQUITTY_SWIFT_NAME(destructive) = 1,
};

/// The question, worded.
struct DestructiveConfirmation {
    /// What a request is sent with; zero when there is nothing to ask, and
    /// `whyNot` says why.
    std::uint64_t token = 0;
    DestructiveRequest request = DestructiveRequest::CloseWindow;
    Session::Identity::ObjectID target;
    /// The question.
    std::string title;
    /// What ends: how many sessions, what runs in them, who is attached.
    std::string body;
    /// The button that ends nothing. A frontend makes it the default.
    std::string safeButton;
    /// The button that goes through with it.
    std::string destructiveButton;
    /// The sessions that end, in the tree's order.
    std::vector<std::uint32_t> sessionIDs;
    /// Why there is nothing to ask, as a sentence; empty when there is.
    std::string whyNot;

    /// How many sessions end.
    [[nodiscard]] std::uint32_t sessionCount() const { return static_cast<std::uint32_t>(sessionIDs.size()); }

    /// How many buttons the question has, in the order a frontend adds
    /// them: the safe default first, the destructive one second.
    static constexpr std::size_t buttonCount = 2;
    /// One button's title.
    ///
    /// - Parameter index: Which, below ``buttonCount``.
    /// - Returns: The title; empty past the last.
    [[nodiscard]] std::string buttonTitle(std::size_t index) const { return index == 0 ? safeButton : index == 1 ? destructiveButton
                                                                                                                 : std::string(); }
    /// What one button does, so that the two are never drawn alike.
    ///
    /// - Parameter index: Which, below ``buttonCount``.
    /// - Returns: The role; the safe one past the last.
    [[nodiscard]] static constexpr ConfirmationButtonRole buttonRole(std::size_t index) { return index == 1 ? ConfirmationButtonRole::Destructive : ConfirmationButtonRole::SafeDefault; }
};

/// The most sessions one request may end. A tree is a host's word, and a
/// question that lists more than this is not one a person can answer.
inline constexpr std::size_t maxSessionsEndedAtOnce = 256;

namespace DestructiveConfirmationDetail {

/// What a request would end, found in a tree.
struct Found {
    /// What the question calls it; empty when it has no name of its own.
    std::string name;
    /// The panes a session draws into, in the tree's order.
    std::vector<const Session::TreePane*> panes;
    /// The people attached to it.
    std::uint32_t people = 0;
    /// Whether the tree holds it at all.
    bool isHeld = false; // bool-ok: names the one state it reports
    /// Whether it is, or is part of, a workspace nothing runs in.
    bool isRecorded = false; // bool-ok: names the one state it reports
};

/// Adds the panes of a tab that a session draws into.
///
/// - Parameters:
///   - tab: The tab.
///   - found: Where they are added.
inline void addPanes(const Session::TreeTab& tab, Found& found)
{
    for (const Session::TreePane& pane : tab.panes) {
        if (pane.sessionID != 0)
            found.panes.push_back(&pane);
    }
}

/// Adds the panes of a window that a session draws into.
///
/// - Parameters:
///   - window: The window.
///   - found: Where they are added.
inline void addPanes(const Session::TreeWindow& window, Found& found)
{
    for (const Session::TreeTab& tab : window.tabs)
        addPanes(tab, found);
}

/// The first of some names that is not empty.
///
/// - Parameters:
///   - first: The preferred name.
///   - second: The next.
///   - third: The last.
/// - Returns: The name; empty when all are.
[[nodiscard]] inline std::string firstNamed(std::string_view first, std::string_view second, std::string_view third = {})
{
    return std::string(!first.empty() ? first : !second.empty() ? second
                                                                : third);
}

/// Finds what a request would end.
///
/// - Parameters:
///   - tree: The host's tree.
///   - request: The request.
///   - target: What it names.
/// - Returns: What was found; `isHeld` false when the tree holds no such
///   thing of the kind the request takes.
[[nodiscard]] inline Found find(const Session::SessionTree& tree, DestructiveRequest request, Session::Identity::ObjectID target)
{
    using Session::Identity::ObjectKind;
    Found found;
    if (target.number == 0)
        return found;
    for (const Session::TreeWorkspace& workspace : tree.workspaces) {
        if (request == DestructiveRequest::TerminateWorkspace) {
            if (target.kind != ObjectKind::Workspace || workspace.id != target.number)
                continue;
            found.isHeld = true;
            found.isRecorded = workspace.isRestorable();
            found.name = firstNamed(workspace.name, workspace.label, workspace.defaultName());
            found.people = workspace.attendance.people;
            for (const Session::TreeWindow& window : workspace.windows)
                addPanes(window, found);
            return found;
        }
        if (workspace.isRestorable())
            continue;
        for (const Session::TreeWindow& window : workspace.windows) {
            if (request == DestructiveRequest::CloseWindow) {
                if (target.kind != ObjectKind::Window || std::to_underlying(window.id) != target.number)
                    continue;
                found.isHeld = true;
                found.name = firstNamed(window.label, window.hostActiveTab < window.tabs.size() ? std::string_view(window.tabs[window.hostActiveTab].name) : std::string_view());
                found.people = window.attendance.people;
                addPanes(window, found);
                return found;
            }
            for (const Session::TreeTab& tab : window.tabs) {
                for (const Session::TreePane& pane : tab.panes) {
                    const bool isNamed = (target.kind == ObjectKind::Pane && std::to_underlying(pane.id) == target.number) || (target.kind == ObjectKind::Session && pane.sessionID == target.number); // bool-ok: a local verdict
                    if (!isNamed)
                        continue;
                    found.isHeld = true;
                    found.name = firstNamed(pane.sessionLabel, pane.label, pane.program);
                    found.people = pane.attendance.people;
                    if (pane.sessionID != 0)
                        found.panes.push_back(&pane);
                    return found;
                }
            }
        }
    }
    return found;
}

/// The programs some panes run, each once with how many run it, in the
/// order the tree first shows them.
///
/// Commas throughout and no conjunction, as a close's own question lists
/// them (`Session::closeConfirmationBody`): a reader is counting.
///
/// - Parameter panes: The panes.
/// - Returns: The list, "vim, cargo, zsh (2)"; empty when no pane says what
///   it runs.
[[nodiscard]] inline std::string programList(const std::vector<const Session::TreePane*>& panes)
{
    /// The most programs named before the rest are counted.
    constexpr std::size_t maxNamed = 6;
    std::vector<std::pair<std::string_view, std::size_t>> programs;
    std::size_t unnamed = 0;
    for (const Session::TreePane* pane : panes) {
        if (pane->program.empty()) {
            ++unnamed;
            continue;
        }
        const auto known = std::ranges::find(programs, std::string_view(pane->program), &std::pair<std::string_view, std::size_t>::first);
        if (known != programs.end())
            ++known->second;
        else
            programs.emplace_back(pane->program, 1);
    }
    if (programs.empty())
        return {};
    std::string list;
    std::size_t others = unnamed;
    for (std::size_t index = 0; index < programs.size(); ++index) {
        if (index >= maxNamed) {
            others += programs[index].second;
            continue;
        }
        if (index != 0)
            list.append(", ");
        list.append(programs[index].first);
        if (programs[index].second > 1)
            list.append(" (" + std::to_string(programs[index].second) + ")");
    }
    if (others != 0)
        list.append(", " + std::to_string(others) + " more");
    return list;
}

/// The noun a request's target is called by.
///
/// - Parameter request: The request.
/// - Returns: "window", "session" or "workspace".
[[nodiscard]] constexpr std::string_view nounOf(DestructiveRequest request)
{
    switch (request) {
    case DestructiveRequest::CloseWindow:
        return "window";
    case DestructiveRequest::TerminateSession:
        return "session";
    case DestructiveRequest::TerminateWorkspace:
        return "workspace";
    }
    return "session";
}

} // namespace DestructiveConfirmationDetail

/// Words the question a request that ends sessions is asked with.
///
/// The token is left zero: `DestructiveConfirmations::ask` gives one.
///
/// - Parameters:
///   - tree: The host's tree.
///   - request: The request.
///   - target: What it names: a window, a session or its pane, a workspace.
///   - hostName: What the host is called in the question; empty leaves it
///     out.
/// - Returns: The question, or `whyNot` when there is nothing to ask.
[[nodiscard]] inline DestructiveConfirmation wordDestructiveRequest(const Session::SessionTree& tree, DestructiveRequest request, Session::Identity::ObjectID target, std::string_view hostName)
{
    using namespace DestructiveConfirmationDetail;
    DestructiveConfirmation confirmation;
    confirmation.request = request;
    confirmation.target = target;
    const std::string_view noun = nounOf(request);
    const Found found = find(tree, request, target);
    if (!found.isHeld) {
        confirmation.whyNot = Session::Identity::format(target) + " is no longer on this host’s list.";
        return confirmation;
    }
    const bool isTerminate = request != DestructiveRequest::CloseWindow; // bool-ok: a local verdict
    if (found.isRecorded || (isTerminate && found.panes.empty())) {
        confirmation.whyNot = "Nothing runs in this " + std::string(noun) + ", so there is nothing to terminate.";
        return confirmation;
    }
    if (found.panes.size() > maxSessionsEndedAtOnce) {
        confirmation.whyNot = "This " + std::string(noun) + " holds more sessions than one request ends.";
        return confirmation;
    }
    for (const Session::TreePane* pane : found.panes)
        confirmation.sessionIDs.push_back(pane->sessionID);

    confirmation.title = isTerminate ? "Terminate " : "Close ";
    confirmation.title.append(found.name.empty() ? "this " + std::string(noun) : found.name);
    if (!hostName.empty())
        confirmation.title.append(" on ").append(hostName);
    confirmation.title.push_back('?');

    const std::size_t count = found.panes.size();
    const std::string sessions = count == 1 ? std::string("session") : std::to_string(count) + " sessions";
    const std::string_view inThem = count == 1 ? "it" : "them";
    switch (request) {
    case DestructiveRequest::CloseWindow:
        if (count == 0)
            confirmation.body = "The host closes the window. Nothing runs in it.";
        else
            confirmation.body = "The host closes the window and ends its " + sessions + " and everything running in " + std::string(inThem) + ".";
        break;
    case DestructiveRequest::TerminateSession:
        confirmation.body = "The host ends the session and everything running in it.";
        break;
    case DestructiveRequest::TerminateWorkspace:
        confirmation.body = "The host ends its " + sessions + " and everything running in " + std::string(inThem) + ", and the workspace with " + std::string(inThem) + ".";
        break;
    }
    if (const std::string programs = programList(found.panes); !programs.empty())
        confirmation.body.append(" Running: " + programs + ".");
    if (isTerminate)
        confirmation.body.append(count == 1 ? " Anyone attached to it is asked first and can decline." : " Anyone attached to one is asked first and can decline.");
    else if (found.people == 1)
        confirmation.body.append(" One person is attached to it.");
    else if (found.people > 1)
        confirmation.body.append(" " + std::to_string(found.people) + " people are attached to it.");

    // Named for what it keeps, not "Cancel": a toolkit that gives that word
    // its own key leaves the safe button without the default one.
    confirmation.safeButton = "Keep ";
    confirmation.safeButton.push_back(static_cast<char>(noun.front() - 'a' + 'A'));
    confirmation.safeButton.append(noun.substr(1));
    confirmation.destructiveButton = isTerminate ? "Terminate" : "Close Window";
    return confirmation;
}

/// What became of a terminate sent with a token.
struct TerminateAdmission {
    /// How many sessions the host was asked to end; zero when none was.
    std::uint32_t sessionCount = 0;
    /// Why nothing was sent, as a sentence; empty when it was.
    std::string whyNot;
};

/// The questions a reader has asked and not yet acted on.
class DestructiveConfirmations {
public:
    /// The most questions held at once; asking another forgets the oldest.
    static constexpr std::size_t maxHeld = 8;

    /// What `redeem` made of a token.
    struct Redemption {
        /// Why the request is not sent, as a sentence; empty when it is.
        std::string refusal;
        /// The sessions it ends, when it is sent.
        std::vector<std::uint32_t> sessionIDs;
    };

    /// Words a question and gives it its token.
    ///
    /// - Parameters:
    ///   - heldFresh: The host's tree as the reader holds it, when it is one
    ///     a listing answered or a connected watch follows; null otherwise.
    ///   - request: The request.
    ///   - target: What it names.
    ///   - hostName: What the host is called in the question.
    /// - Returns: The question with its token, or `whyNot`.
    [[nodiscard]] DestructiveConfirmation ask(const Session::SessionTree* heldFresh, DestructiveRequest request, Session::Identity::ObjectID target, std::string_view hostName)
    {
        if (!heldFresh) {
            DestructiveConfirmation confirmation;
            confirmation.request = request;
            confirmation.target = target;
            confirmation.whyNot = "This host’s list is out of date. Refresh it, then try again.";
            return confirmation;
        }
        DestructiveConfirmation confirmation = wordDestructiveRequest(*heldFresh, request, target, hostName);
        if (!confirmation.whyNot.empty())
            return confirmation;
        confirmation.token = ++m_lastToken;
        if (m_held.size() >= maxHeld)
            m_held.erase(m_held.begin());
        m_held.push_back({ .token = confirmation.token, .request = request, .target = target, .sessionIDs = confirmation.sessionIDs });
        return confirmation;
    }

    /// Spends a token on the request its question was about.
    ///
    /// A token is good once, for the request and the target it was given
    /// for, and only while the request would end exactly the sessions the
    /// question said.
    ///
    /// - Parameters:
    ///   - token: What `ask` returned.
    ///   - request: The request about to be sent.
    ///   - target: What it names.
    ///   - heldFresh: The host's tree as the reader holds it now; null when
    ///     it holds none that is current.
    /// - Returns: The sessions the request ends, or why it is not sent.
    [[nodiscard]] Redemption redeem(std::uint64_t token, DestructiveRequest request, Session::Identity::ObjectID target, const Session::SessionTree* heldFresh)
    {
        Redemption redemption;
        // No question is given the token zero, so zero finds none.
        const auto held = std::ranges::find(m_held, token, &Held::token);
        if (held == m_held.end() || held->request != request || !(held->target == target)) {
            redemption.refusal = "This was not confirmed. Nothing is ended without asking first.";
            return redemption;
        }
        const Held asked = std::move(*held);
        m_held.erase(held);
        if (!heldFresh) {
            redemption.refusal = "This host’s list is out of date. Refresh it, then try again.";
            return redemption;
        }
        const DestructiveConfirmation now = wordDestructiveRequest(*heldFresh, request, target, {});
        if (!now.whyNot.empty()) {
            redemption.refusal = now.whyNot;
            return redemption;
        }
        if (now.sessionIDs != asked.sessionIDs) {
            redemption.refusal = "What this would end changed after you were asked. Look again.";
            return redemption;
        }
        redemption.sessionIDs = now.sessionIDs;
        return redemption;
    }

    /// How many questions are held.
    [[nodiscard]] std::size_t heldCount() const { return m_held.size(); }

private:
    /// One question asked and not yet acted on.
    struct Held {
        std::uint64_t token = 0;
        DestructiveRequest request = DestructiveRequest::CloseWindow;
        Session::Identity::ObjectID target;
        /// The sessions the question said would end.
        std::vector<std::uint32_t> sessionIDs;
    };

    std::vector<Held> m_held;
    std::uint64_t m_lastToken = 0;
};

} // namespace Ubiquitty::Runtime
