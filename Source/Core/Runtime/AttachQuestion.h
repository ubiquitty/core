// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// AttachQuestion.h — what a person is asked when connecting to a host does
// not say where to land.
//
// `Session::RememberedSessions::decide` answers `ChooseFromList` with which
// question it is and the workspaces it is about. The words of the question,
// and what each workspace is called, are put here once, from the outline a
// listing of the host draws, so every frontend asks the same thing of the
// same rows and words nothing itself. A frontend presents it.
//

#include "Core/App/SessionOutline.h"
#include "Core/Session/RememberedSessions.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Runtime {

/// One workspace a person may pick, as the outline calls it.
struct AttachQuestionCandidate {
    /// The workspace, as the host's tree numbers it.
    std::uint32_t workspaceID = 0;
    /// The session picking it attaches to; 0 for a workspace the host would
    /// restore, which is attached to by its name.
    std::uint32_t sessionID = 0;
    /// What picking it opens: the value its row in the outline carries, and
    /// the one an attach takes. No frontend finds a session or a name for a
    /// candidate itself.
    Session::AttachTarget open;
    /// What the workspace's row in the outline is called; empty for a
    /// workspace the outline holds no row for.
    std::string title;
    /// Whether anything runs in it.
    Session::TreeWorkspaceState state = Session::TreeWorkspaceState::Live;

    friend bool operator==(const AttachQuestionCandidate&, const AttachQuestionCandidate&) = default;
};

/// The question, in words, and what may be picked.
struct AttachQuestion {
    /// Which question it is; `None` when nothing is asked.
    Session::AttachAsk ask = Session::AttachAsk::None;
    /// The sentence a person reads; empty when nothing is asked.
    std::string text;
    /// The same question in the room a sidebar or a bottom bar has: one
    /// short line. Empty when nothing is asked.
    std::string shortText;
    /// What may be picked, in the tree's order.
    std::vector<AttachQuestionCandidate> candidates;
    /// Whether starting a new workspace is offered beside the candidates.
    bool isNewSessionOffered = false; // bool-ok: names the one state it reports

    friend bool operator==(const AttachQuestion&, const AttachQuestion&) = default;
};

/// Words the question a decision asks.
///
/// - Parameters:
///   - decision: What `RememberedSessions::decide` answered on a tree.
///   - outline: The outline of that tree, whose workspace rows name the
///     candidates.
/// - Returns: The question; one that asks nothing for any plan but
///   `ChooseFromList` and for a decision made on rows.
[[nodiscard]] inline AttachQuestion attachQuestionOf(const Session::AttachDecision& decision, const App::SessionOutline& outline)
{
    AttachQuestion question;
    if (decision.plan != Session::AttachPlan::ChooseFromList || decision.ask == Session::AttachAsk::None)
        return question;
    question.ask = decision.ask;
    question.isNewSessionOffered = decision.isNewSessionOffered;
    for (const Session::AttachCandidate& candidate : decision.candidates) {
        AttachQuestionCandidate named { .workspaceID = candidate.workspaceID, .sessionID = candidate.sessionID, .open = candidate.open, .title = {}, .state = Session::TreeWorkspaceState::Live };
        for (const App::OutlineRow& row : outline.rows) {
            if (row.level == App::ListedLevel::Workspace && row.id.number == candidate.workspaceID) {
                named.title = row.title;
                named.state = row.workspaceState;
                break;
            }
        }
        question.candidates.push_back(std::move(named));
    }
    const std::size_t count = question.candidates.size();
    const std::string quoted = "\"" + decision.askedName + "\"";
    switch (decision.ask) {
    case Session::AttachAsk::None:
        break;
    case Session::AttachAsk::WhichWorkspace:
        question.text = count == 1 ? "This host holds one workspace, and nothing says you were in it. Attach to it?" : "This host holds " + std::to_string(count) + " workspaces, and nothing says which you were in. Which one?";
        question.shortText = count == 1 ? "One workspace here. Attach to it?" : std::to_string(count) + " workspaces here. Which one?";
        break;
    case Session::AttachAsk::SeveralBearTheName:
        question.text = std::to_string(count) + " workspaces on this host are named " + quoted + ". Which one do you mean?";
        question.shortText = std::to_string(count) + " are named " + quoted + ". Which one?";
        break;
    case Session::AttachAsk::NameIsOnlyALabel:
        question.text = "No workspace on this host is named " + quoted + ", but "
            + (count == 1 ? std::string("one is labelled ") + quoted + ". Attach to it, or start a new workspace named " : std::to_string(count) + " are labelled " + quoted + ". Attach to one of them, or start a new workspace named ")
            + quoted + "?";
        question.shortText = count == 1 ? "None is named " + quoted + "; one is labelled it. Attach, or start new?" : "None is named " + quoted + "; " + std::to_string(count) + " are labelled it. Which one, or new?";
        break;
    }
    return question;
}

} // namespace Ubiquitty::Runtime
