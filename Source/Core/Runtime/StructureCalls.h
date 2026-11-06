// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// StructureCalls.h — the changes a reader of a host's tree asks of it.
//
// A frontend that draws a host's outline lets a person label a row, tag it,
// name a workspace or a tab, move a tab and close a window. Each is one call
// the host serves (Docs/sync.md §7.3). This is the client's half, once: a
// typed request is checked against the tree the reader holds, turned into
// the call, and its answer turned into a result a frontend shows. No
// frontend writes a method's name or a parameter's.
//
// It owns no connection. Whoever holds one hands `admit` a request and sends
// what comes back, then hands `noteAnswer` what the host said.
//
// A request is refused here, with a sentence, when the reader's tree is not
// one the host is known to hold (none, or one a watch left behind), when it
// names something the tree does not hold, when a change to the same thing is
// still waiting for the host, and when more are waiting than a reader may
// have out. What the host refuses arrives in the host's own words.
//

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Runtime/DestructiveConfirmation.h"
#include "Core/Session/Identity.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

/// Which change is asked. The values are explicit because the Swift and web
/// mirrors pin them.
enum class StructureOperation : std::uint8_t {
    /// Set or clear the label of a workspace, window, tab, pane or session.
    SetLabel = 1,
    /// Replace the tags of one of those.
    SetTags = 2,
    /// Give a workspace its name, or take it away.
    RenameWorkspace = 3,
    /// Give a tab its name, or take it away.
    RenameTab = 4,
    /// Put a tab at a place among the tabs of a window of its workspace.
    MoveTab = 5,
    /// Close a window and end the sessions in its panes.
    CloseWindow = 6,
};

/// How a request ended. The values are explicit for the same reason.
enum class StructureOutcome : std::uint8_t {
    /// The host made the change; the tree's next revision shows it.
    Served = 1,
    /// The host refused, for the reason in its own words.
    RefusedByHost = 2,
    /// Refused here, before anything was sent.
    RefusedHere = 3,
    /// Sent, and the host's answer never arrived.
    Unanswered = 4,
};

/// The tags of a request, built a name at a time by a caller whose language
/// hands no list across.
struct StructureTags {
    std::vector<std::string> names;

    /// Adds one tag.
    ///
    /// - Parameter name: The tag.
    void append(const std::string& name) { names.push_back(name); }
};

/// One change, typed.
struct StructureRequest {
    StructureOperation operation = StructureOperation::SetLabel;
    /// What is changed: the row's identity, or the identity of what the row
    /// folded into itself when that is what is meant.
    Session::Identity::ObjectID target;
    /// The label or the name; empty clears it.
    std::string text;
    /// The tags, for `SetTags`; none clears them.
    std::vector<std::string> tags;
    /// The window a moved tab is to stand in.
    Session::Identity::ObjectID window;
    /// The place it takes there, from zero.
    std::uint32_t index = 0;
    /// For `CloseWindow`, which ends sessions: the token its question
    /// returned (`StructureCalls::confirmations`). Read by no other change.
    std::uint64_t confirmationToken = 0;
};

/// How one request ended.
struct StructureResult {
    /// The number `admit` gave the request; never zero for a real one.
    std::uint64_t requestID = 0;
    StructureOperation operation = StructureOperation::SetLabel;
    Session::Identity::ObjectID target;
    StructureOutcome outcome = StructureOutcome::Unanswered;
    /// Why it was not served, as a sentence a person reads: the host's own
    /// for `RefusedByHost`, this file's otherwise. Empty when served.
    std::string reason;

    friend bool operator==(const StructureResult&, const StructureResult&) = default;
};

/// The requests a reader has out, and what turns one into a call and an
/// answer into a result.
class StructureCalls {
public:
    /// The most requests waiting for the host at once.
    static constexpr std::size_t maxInFlight = 16;

    /// What `admit` made of a request.
    struct Admission {
        /// The request's number.
        std::uint64_t requestID = 0;
        /// The JSON-RPC call to send; empty when the request was refused
        /// here.
        std::string call;
        /// How it ended, when it was refused here.
        std::optional<StructureResult> refusal;
    };

    /// Checks a request against the reader's tree and makes its call.
    ///
    /// - Parameters:
    ///   - request: The change.
    ///   - heldFresh: The host's tree as the reader holds it, when it is one
    ///     a listing answered or a connected watch follows; null otherwise.
    /// - Returns: The call to send under the request's number, or the
    ///   refusal.
    [[nodiscard]] Admission admit(const StructureRequest& request, const Session::SessionTree* heldFresh)
    {
        using Session::Identity::format;
        using Session::Identity::ObjectKind;
        Admission admission;
        admission.requestID = ++m_lastRequestID;
        const auto refuse = [&](std::string reason) {
            admission.refusal = StructureResult { .requestID = admission.requestID, .operation = request.operation, .target = request.target, .outcome = StructureOutcome::RefusedHere, .reason = std::move(reason) };
            return admission;
        };
        const ObjectKind kind = request.target.kind;
        const bool isOfALabelledLevel = kind == ObjectKind::Workspace || kind == ObjectKind::Window || kind == ObjectKind::Tab || kind == ObjectKind::Pane || kind == ObjectKind::Session; // bool-ok: a local verdict
        switch (request.operation) {
        case StructureOperation::SetLabel:
        case StructureOperation::SetTags:
            if (!isOfALabelledLevel)
                return refuse("A label or a tag is put on a workspace, a window, a tab, a pane or a session.");
            break;
        case StructureOperation::RenameWorkspace:
            if (kind != ObjectKind::Workspace)
                return refuse("Only a workspace is named this way, and " + format(request.target) + " is not one.");
            break;
        case StructureOperation::RenameTab:
        case StructureOperation::MoveTab:
            if (kind != ObjectKind::Tab)
                return refuse("Only a tab is named or moved this way, and " + format(request.target) + " is not one.");
            break;
        case StructureOperation::CloseWindow:
            if (kind != ObjectKind::Window)
                return refuse("Only a window is closed this way, and " + format(request.target) + " is not one.");
            break;
        }
        if (request.operation == StructureOperation::MoveTab && request.window.kind != ObjectKind::Window)
            return refuse("A tab is moved to a window, and " + format(request.window) + " is not one.");
        if ((request.operation == StructureOperation::RenameWorkspace || request.operation == StructureOperation::RenameTab) && request.text.size() > Session::maxTreeNameBytes) {
            const std::string level = request.operation == StructureOperation::RenameWorkspace ? "workspace" : "tab";
            return refuse("A " + level + " name holds at most " + std::to_string(Session::maxTreeNameBytes) + " bytes, and this one holds " + std::to_string(request.text.size()) + ".");
        }
        if (!heldFresh)
            return refuse("This host’s list is out of date. Refresh it, then try again.");
        const Standing standing = standingOf(*heldFresh, request.target);
        if (standing == Standing::Absent)
            return refuse(format(request.target) + " is no longer on this host’s list.");
        if (standing == Standing::Recorded)
            return refuse("Nothing runs in " + format(request.target) + " yet. Open it before changing it.");
        if (request.operation == StructureOperation::MoveTab && standingOf(*heldFresh, request.window) != Standing::Live)
            return refuse(format(request.window) + " is no longer on this host’s list.");
        if (std::ranges::any_of(m_inFlight, [&](const InFlight& waiting) { return waiting.target == request.target; }))
            return refuse("A change to " + format(request.target) + " is still waiting for this host’s answer.");
        if (m_inFlight.size() >= maxInFlight)
            return refuse("Too many changes are waiting for this host’s answer.");
        std::uint64_t windowRevision = 0;
        if (request.operation == StructureOperation::CloseWindow) {
            for (const auto& workspace : heldFresh->workspaces) {
                if (std::ranges::any_of(workspace.windows, [&](const auto& window) { return std::to_underlying(window.id) == request.target.number; })) {
                    windowRevision = workspace.revision;
                    break;
                }
            }
            if (!windowRevision)
                return refuse("This window has no current structure revision. Refresh it, then try again.");
        }
        // Last of the checks, so that a request refused for any other reason
        // keeps its token for the next try.
        if (request.operation == StructureOperation::CloseWindow) {
            DestructiveConfirmations::Redemption redemption = m_confirmations.redeem(request.confirmationToken, DestructiveRequest::CloseWindow, request.target, heldFresh);
            if (!redemption.refusal.empty())
                return refuse(std::move(redemption.refusal));
        }

        using Codec::JSON::Value;
        Value parameters = Value::object();
        std::string_view method;
        const auto number = [](Session::Identity::ObjectID id) { return Value::number(static_cast<double>(id.number)); };
        switch (request.operation) {
        case StructureOperation::SetLabel:
            method = "label.set";
            parameters.set("object", Value::string(format(request.target)));
            parameters.set("label", Value::string(request.text));
            break;
        case StructureOperation::SetTags: {
            method = "tags.set";
            Value tags = Value::array();
            for (const std::string& tag : request.tags)
                tags.push(Value::string(tag));
            parameters.set("object", Value::string(format(request.target)));
            parameters.set("tags", std::move(tags));
            break;
        }
        case StructureOperation::RenameWorkspace:
            method = "workspace.rename";
            parameters.set("workspace", number(request.target));
            parameters.set("name", Value::string(request.text));
            break;
        case StructureOperation::RenameTab:
            method = "tab.rename";
            parameters.set("tab", number(request.target));
            parameters.set("name", Value::string(request.text));
            break;
        case StructureOperation::MoveTab:
            method = "tab.move";
            parameters.set("tab", number(request.target));
            parameters.set("window", number(request.window));
            parameters.set("index", Value::number(request.index));
            break;
        case StructureOperation::CloseWindow:
            method = "window.close";
            parameters.set("window", number(request.target));
            parameters.set("expectedRevision", Value::string(std::to_string(windowRevision)));
            break;
        }
        Value call = Value::object();
        call.set("jsonrpc", Value::string("2.0"));
        call.set("id", Value::string(callID(admission.requestID)));
        call.set("method", Value::string(std::string(method)));
        call.set("params", std::move(parameters));
        std::string encoded = Codec::JSON::serialize(call);
        if (std::optional<std::string> refusal = Protocol::Sync::callSizeRefusal(encoded.size()))
            return refuse(std::move(*refusal));
        admission.call = std::move(encoded);
        m_inFlight.push_back({ .requestID = admission.requestID, .operation = request.operation, .target = request.target });
        return admission;
    }

    /// Reads what the host answered a call with.
    ///
    /// - Parameter response: A JSON-RPC 2.0 response.
    /// - Returns: How the request it answers ended; nothing for an answer to
    ///   no request this holds.
    [[nodiscard]] std::optional<StructureResult> noteAnswer(std::string_view response)
    {
        bool isJSON = false; // bool-ok: the parser's out flag
        const Codec::JSON::Value answer = Codec::JSON::parse(std::string(response), isJSON);
        const Codec::JSON::Value* id = isJSON ? answer.find("id") : nullptr;
        if (!id || !id->isString())
            return std::nullopt;
        const auto waiting = std::ranges::find_if(m_inFlight, [&](const InFlight& each) { return callID(each.requestID) == id->asString(); });
        if (waiting == m_inFlight.end())
            return std::nullopt;
        StructureResult result { .requestID = waiting->requestID, .operation = waiting->operation, .target = waiting->target, .outcome = StructureOutcome::Served, .reason = {} };
        m_inFlight.erase(waiting);
        if (const Codec::JSON::Value* error = answer.find("error")) {
            result.outcome = StructureOutcome::RefusedByHost;
            result.reason = error->stringOr("message");
            if (result.reason.empty())
                result.reason = "The host refused and gave no reason.";
        } else if (!answer.find("result")) {
            result.outcome = StructureOutcome::Unanswered;
            result.reason = "Ubiquitty couldn’t read the host’s answer.";
        }
        return result;
    }

    /// Ends a request whose answer will not come.
    ///
    /// - Parameters:
    ///   - requestID: The request.
    ///   - reason: Why, as a sentence; a stock one when empty.
    /// - Returns: How it ended; nothing for a request this does not hold.
    [[nodiscard]] std::optional<StructureResult> fail(std::uint64_t requestID, std::string reason)
    {
        const auto waiting = std::ranges::find(m_inFlight, requestID, &InFlight::requestID);
        if (waiting == m_inFlight.end())
            return std::nullopt;
        StructureResult result { .requestID = waiting->requestID, .operation = waiting->operation, .target = waiting->target, .outcome = StructureOutcome::Unanswered, .reason = reason.empty() ? std::string("The host did not answer.") : std::move(reason) };
        m_inFlight.erase(waiting);
        return result;
    }

    /// Ends a request that was admitted and could not be sent, as one
    /// refused here.
    ///
    /// - Parameters:
    ///   - requestID: The request.
    ///   - reason: Why, as a sentence.
    /// - Returns: How it ended; nothing for a request this does not hold.
    [[nodiscard]] std::optional<StructureResult> withdraw(std::uint64_t requestID, std::string reason)
    {
        std::optional<StructureResult> result = fail(requestID, std::move(reason));
        if (result)
            result->outcome = StructureOutcome::RefusedHere;
        return result;
    }

    /// Ends every request that is out, as unanswered: the connection they
    /// were sent on is gone.
    ///
    /// - Parameter reason: Why, as a sentence.
    /// - Returns: How each ended, oldest first.
    [[nodiscard]] std::vector<StructureResult> failAll(const std::string& reason)
    {
        std::vector<StructureResult> results;
        while (!m_inFlight.empty()) {
            if (std::optional<StructureResult> result = fail(m_inFlight.front().requestID, reason))
                results.push_back(std::move(*result));
        }
        return results;
    }

    /// How many requests are waiting for the host.
    [[nodiscard]] std::size_t inFlightCount() const { return m_inFlight.size(); }

    /// The questions asked before a request that ends sessions: `admit`
    /// spends a `CloseWindow`'s token here, and the reader's terminate
    /// spends its own.
    ///
    /// - Returns: The questions this reader has asked and not acted on.
    [[nodiscard]] DestructiveConfirmations& confirmations() { return m_confirmations; }

    /// The id a request's call carries, which tells its answer from any
    /// other's on the connection.
    ///
    /// - Parameter requestID: The request.
    /// - Returns: The JSON-RPC id.
    [[nodiscard]] static std::string callID(std::uint64_t requestID) { return "ubiquitty.structure." + std::to_string(requestID); }

private:
    /// One request the host has not answered.
    struct InFlight {
        std::uint64_t requestID = 0;
        StructureOperation operation = StructureOperation::SetLabel;
        Session::Identity::ObjectID target;
    };

    /// Where an identity stands in a tree.
    enum class Standing : std::uint8_t {
        /// The tree does not hold it.
        Absent,
        /// It is a workspace nothing runs in.
        Recorded,
        /// It runs.
        Live,
    };

    /// Finds where an identity stands in a tree.
    ///
    /// - Parameters:
    ///   - tree: The tree.
    ///   - id: The identity.
    /// - Returns: Whether it runs, is a workspace nothing runs in, or is not
    ///   there.
    [[nodiscard]] static Standing standingOf(const Session::SessionTree& tree, Session::Identity::ObjectID id)
    {
        using Session::Identity::ObjectKind;
        if (id.number == 0)
            return Standing::Absent;
        for (const Session::TreeWorkspace& workspace : tree.workspaces) {
            if (id.kind == ObjectKind::Workspace) {
                if (workspace.id == id.number)
                    return workspace.isRestorable() ? Standing::Recorded : Standing::Live;
                continue;
            }
            if (workspace.isRestorable())
                continue;
            for (const Session::TreeWindow& window : workspace.windows) {
                if (id.kind == ObjectKind::Window && std::to_underlying(window.id) == id.number)
                    return Standing::Live;
                for (const Session::TreeTab& tab : window.tabs) {
                    if (id.kind == ObjectKind::Tab && std::to_underlying(tab.id) == id.number)
                        return Standing::Live;
                    for (const Session::TreePane& pane : tab.panes) {
                        if (id.kind == ObjectKind::Pane && std::to_underlying(pane.id) == id.number)
                            return Standing::Live;
                        if (id.kind == ObjectKind::Session && pane.sessionID == id.number)
                            return Standing::Live;
                    }
                }
            }
        }
        return Standing::Absent;
    }

    std::vector<InFlight> m_inFlight;
    std::uint64_t m_lastRequestID = 0;
    DestructiveConfirmations m_confirmations;
};

} // namespace Ubiquitty::Runtime
