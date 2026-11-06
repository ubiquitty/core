// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// HostListing.h — the tree a client holds of the host it lists.
//
// A client that calls `host.sessions.subscribe` is sent every workspace's
// structure and the two records of every pane (Docs/sync.md §7.3, §7.7).
// They arrive apart and in any order, on a connection that may also be
// attached to a workspace of its own. This puts them together: it reads the
// host's answer to the subscription, decodes each structure once per
// revision, finds a pane's records wherever the connection holds them (on its
// listing, or on the binding of a pane the client has open) and assembles
// the `SessionTree` every outline is built from.
//
// The host states its own clock once, in the answer. Ages are read against
// that clock, so the listing keeps it running by the reader's: the host's
// clock now is what it said then plus what has passed here since.
//
// Pure but for what it is handed: it owns no connection and reads no clock.
// A directory errand and an attached session both keep one beside their
// connection.
//

#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/SessionRows.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {

namespace HostListingDetail {

/// The id the subscription is called with, so its answer is told from the
/// answer to any other call on the connection.
inline constexpr std::string_view callID = "ubiquitty.host-listing";

/// The largest whole number a JSON number carries exactly.
inline constexpr double maxExactNumber = 9007199254740992.0;

/// Reads a JSON value as a whole number that is none below zero.
///
/// - Parameters:
///   - value: The value, or null for a member that is absent.
///   - maximum: The largest number taken.
/// - Returns: The number, or nothing for a value that is absent, no number,
///   negative, fractional or past `maximum`.
[[nodiscard]] inline std::optional<std::uint64_t> wholeNumber(const Codec::JSON::Value* value, double maximum = maxExactNumber)
{
    if (!value || !value->isNumber())
        return std::nullopt;
    const double number = value->asNumber();
    // Below zero and not-a-number are refused before the conversion below,
    // which is defined for neither.
    if (!(number >= 0.0) || number > maximum || number != static_cast<double>(static_cast<std::uint64_t>(number)))
        return std::nullopt;
    return static_cast<std::uint64_t>(number);
}

} // namespace HostListingDetail

/// The tree a client assembles of a host it subscribed to.
class HostListing {
public:
    /// Makes one subscription answer owner.
    /// - Parameter responseID: The unique JSON-RPC id on this connection.
    explicit HostListing(std::string responseID = std::string(HostListingDetail::callID))
        : m_responseID(std::move(responseID))
    {
    }

    /// What a look at the connection found.
    enum class Change : std::uint8_t {
        /// Nothing the tree is built from moved.
        None,
        /// The tree was assembled again.
        Assembled,
        /// A structure the host sent does not decode.
        Malformed,
    };

    /// How the host answered the subscription.
    enum class Answer : std::uint8_t {
        /// The text answers some other call.
        NotThisCall,
        /// The host lists itself; the tree follows.
        Listing,
        /// The host refused, or answered without stating its clock.
        Refused,
    };

    /// Why the subscription produced no listing.
    enum class Failure : std::uint8_t {
        None,
        Refused,
        Forbidden,
        Unsupported,
        Malformed,
    };

    /// The call that subscribes a connection to its host's listing.
    ///
    /// - Parameter responseID: The unique response identity.
    /// - Returns: A JSON-RPC 2.0 request.
    [[nodiscard]] static std::string subscribeRequest(std::string_view responseID = HostListingDetail::callID)
    {
        std::string request = R"({"jsonrpc":"2.0","id":)";
        // Callers use generated ASCII identities; JSON escaping also keeps
        // this helper safe for a caller-supplied identity.
        const std::string quoted = Codec::JSON::serialize(Codec::JSON::Value::string(std::string(responseID)));
        request.append(quoted);
        request.append(R"(,"method":"host.sessions.subscribe","params":{}})");
        return request;
    }

    /// Ends this connection's listing without changing its pane bindings.
    /// - Parameter responseID: The unsubscribe call's response identity.
    /// - Returns: A JSON-RPC 2.0 request.
    [[nodiscard]] static std::string unsubscribeRequest(std::string_view responseID)
    {
        std::string request = R"({"jsonrpc":"2.0","id":)";
        request.append(Codec::JSON::serialize(Codec::JSON::Value::string(std::string(responseID))));
        request.append(R"(,"method":"host.sessions.unsubscribe","params":{}})");
        return request;
    }

    /// Reads an answer to a call on the connection.
    ///
    /// - Parameters:
    ///   - response: A JSON-RPC 2.0 response.
    ///   - readerNowUnixMilliseconds: The reader's wall clock, which the
    ///     host's is kept running by from here.
    /// - Returns: Whether the text answered the subscription, and how.
    [[nodiscard]] Answer noteAnswer(std::string_view response, std::uint64_t readerNowUnixMilliseconds)
    {
        using namespace HostListingDetail;
        // The answer is a peer's bytes, read as the JSON-RPC response it has
        // to be: a member is one at its own depth of a parsed object.
        // Text that is not JSON parses to a value with no member.
        bool isJSON = false; // bool-ok: the parser's out flag
        const Codec::JSON::Value answer = Codec::JSON::parse(std::string(response), isJSON);
        const Codec::JSON::Value* id = answer.find("id");
        if (!isJSON || !id || !id->isString() || id->asString() != m_responseID)
            return Answer::NotThisCall;
        const auto refuse = [this](Failure failure, std::string_view detail) {
            m_answer = Answer::Refused;
            m_failure = failure;
            m_failureDetail = detail;
            m_isWhole = false;
            return Answer::Refused;
        };
        if (answer.stringOr("jsonrpc") != "2.0")
            return refuse(Failure::Malformed, "the host returned an invalid session listing response");
        const Codec::JSON::Value* result = answer.find("result");
        if (const auto* error = answer.find("error")) {
            const auto* code = error->find("code");
            const auto* message = error->find("message");
            if (result || !error->isObject() || !code || !code->isNumber() || !message || !message->isString()
                || !(code->asNumber() >= std::numeric_limits<std::int32_t>::min()) || code->asNumber() > std::numeric_limits<std::int32_t>::max()
                || code->asNumber() != static_cast<std::int32_t>(code->asNumber()))
                return refuse(Failure::Malformed, "the host returned an invalid session listing response");
            const auto errorCode = static_cast<Protocol::Control::ErrorCode>(static_cast<std::int32_t>(code->asNumber()));
            if (errorCode == Protocol::Control::ErrorCode::MethodNotFound)
                return refuse(Failure::Unsupported, "the host does not support session listing; update ubiquitty-host");
            return refuse(errorCode == Protocol::Control::ErrorCode::Forbidden ? Failure::Forbidden : Failure::Refused,
                message->asString().empty() ? "the host refused the session listing request" : message->asString());
        }
        const std::optional<std::uint64_t> stated = result ? wholeNumber(result->find("hostNowUnixMilliseconds")) : std::nullopt;
        // The workspaces to expect, when the answer names any: each an
        // identity, or the answer is not one this method writes.
        std::vector<std::uint32_t> expected;
        bool isEveryWorkspaceAnIdentity = false; // bool-ok: a local verdict
        if (const Codec::JSON::Value* workspaces = stated ? result->find("workspaces") : nullptr) {
            isEveryWorkspaceAnIdentity = workspaces->isArray() && workspaces->items().size() <= Protocol::Sync::maxHeldStructures;
            if (!isEveryWorkspaceAnIdentity)
                return refuse(Failure::Malformed, "the host returned an invalid session listing response");
            for (const Codec::JSON::Value& workspace : workspaces->items()) {
                // Zero is never minted, so it names no workspace.
                const std::optional<std::uint64_t> workspaceID = wholeNumber(&workspace, 4294967295.0);
                isEveryWorkspaceAnIdentity = isEveryWorkspaceAnIdentity && workspaceID && *workspaceID != 0
                    && std::ranges::find(expected, *workspaceID) == expected.end();
                if (workspaceID)
                    expected.push_back(static_cast<std::uint32_t>(*workspaceID));
            }
        }
        std::vector<std::uint64_t> revisions;
        const auto* structureRevisions = result ? result->find("structureRevisions") : nullptr;
        if (stated && isEveryWorkspaceAnIdentity && (!structureRevisions || !result->find("paneRevisions")))
            return refuse(Failure::Malformed, "the host's session listing lacks required revision metadata; update ubiquitty-host");
        if (structureRevisions && structureRevisions->isArray() && structureRevisions->items().size() <= Protocol::Sync::maxHeldStructures) {
            for (const auto& revision : structureRevisions->items()) {
                if (!revision.isString() || revision.asString().empty() || revision.asString().front() == '0')
                    break;
                const std::string& text = revision.asString();
                std::uint64_t value = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
                    break;
                revisions.push_back(value);
            }
        }
        std::map<std::uint32_t, std::uint32_t> factsRevisions;
        const auto* panes = result ? result->find("paneRevisions") : nullptr;
        bool arePaneRevisionsValid = panes && panes->isArray() && panes->items().size() <= Protocol::Sync::maxPanesPerConnection;
        if (arePaneRevisionsValid) {
            for (const auto& pane : panes->items()) {
                const auto session = wholeNumber(pane.find("session"), 4294967295.0);
                const auto revision = wholeNumber(pane.find("revision"), 4294967295.0);
                if (!session || !*session || !revision || !*revision
                    || !factsRevisions.emplace(static_cast<std::uint32_t>(*session), static_cast<std::uint32_t>(*revision)).second) {
                    arePaneRevisionsValid = false;
                    break;
                }
            }
        }
        if (!stated || !isEveryWorkspaceAnIdentity || !structureRevisions || !structureRevisions->isArray()
            || revisions.size() != expected.size() || revisions.size() != structureRevisions->items().size() || !arePaneRevisionsValid) {
            return refuse(Failure::Malformed, "the host returned an invalid session listing response");
        }
        m_answer = Answer::Listing;
        m_hostInstances = readHostInstances(result->find("instances"));
        m_failure = Failure::None;
        m_failureDetail.clear();
        m_hostNowAtAnswer = *stated;
        m_readerNowAtAnswer = readerNowUnixMilliseconds;
        m_expected = std::move(expected);
        m_expectedRevisions = std::move(revisions);
        m_expectedFacts = std::move(factsRevisions);
        m_isWhole = false;
        m_isStale = true;
        return Answer::Listing;
    }

    /// Whether the host answered that it lists itself.
    [[nodiscard]] bool isListing() const { return m_answer == Answer::Listing; }

    /// Whether the host refused the subscription.
    [[nodiscard]] bool isRefused() const { return m_answer == Answer::Refused; }

    /// The failure classification, without inventing an authentication verdict.
    [[nodiscard]] Failure failure() const { return m_failure; }

    /// The failure's description, excluding structured RPC error data.
    [[nodiscard]] const std::string& failureDetail() const { return m_failureDetail; }

    /// Assembles the tree again when anything it is built from moved.
    ///
    /// - Parameters:
    ///   - connection: The connection the host lists itself on.
    ///   - listingRevision: A number that moves whenever a structure, a
    ///     listing or a listed pane's records changed on the connection.
    ///   - readerNowUnixMilliseconds: The reader's wall clock.
    /// - Returns: What happened; `None` before the host answered.
    [[nodiscard]] Change refresh(const Protocol::Sync::ClientConnection& connection, std::uint64_t listingRevision, std::uint64_t readerNowUnixMilliseconds)
    {
        if (!isListing() || (!m_isStale && listingRevision == m_listingRevision))
            return Change::None;
        const auto& held = connection.workspaceStructures();
        std::erase_if(m_structures, [&](const auto& kept) { return !held.contains(kept.first); });
        for (const auto& [workspaceID, structure] : held) {
            const auto kept = m_structures.find(workspaceID);
            if (kept != m_structures.end() && kept->second.revision == structure.revision)
                continue;
            std::optional<TreeWorkspace> decoded = decodeWorkspaceStructure(structure.record);
            if (!decoded || workspaceID == 0)
                return Change::Malformed;
            decoded->revision = structure.revision;
            m_structures.insert_or_assign(workspaceID, std::move(*decoded));
        }
        std::vector<TreeWorkspace> structures;
        structures.reserve(m_structures.size());
        for (const auto& [workspaceID, structure] : m_structures) {
            if (m_hasCompleted || std::ranges::find(m_expected, workspaceID) != m_expected.end())
                structures.push_back(structure);
        }
        // A bound pane's status set is handed out by value; the tree reads
        // through pointers, so each is kept until the tree is assembled.
        std::list<Protocol::Sync::StatusRecords> boundStatus;
        bool isEveryPaneDescribed = true; // bool-ok: a local verdict
        m_tree = assembleSessionTree(std::span<const TreeWorkspace>(structures), ++m_treeRevision, hostNowUnixMilliseconds(readerNowUnixMilliseconds),
            [&](Protocol::Sync::ID128 paneID) {
                HeldPaneRecords records;
                if (paneID == Protocol::Sync::ID128 {})
                    return records;
                records.facts = connection.listedPaneFacts(paneID);
                records.status = connection.listedPaneStatus(paneID);
                if (const std::uint32_t handle = records.facts ? 0 : connection.handleForPane(paneID)) {
                    records.facts = connection.paneFacts(handle);
                    if (std::optional<Protocol::Sync::StatusRecords> status = connection.paneStatus(handle))
                        records.status = &boundStatus.emplace_back(std::move(*status));
                }
                isEveryPaneDescribed = isEveryPaneDescribed && records.facts;
                return records;
            });
        m_tree.hostInstances = m_hostInstances;
        m_isWhole = isEveryPaneDescribed;
        for (std::size_t index = 0; index < m_expected.size(); ++index) {
            const auto found = m_structures.find(m_expected[index]);
            m_isWhole = m_isWhole && found != m_structures.end() && found->second.revision >= m_expectedRevisions[index];
        }
        for (const auto& [session, revision] : m_expectedFacts) {
            bool isCurrent = false;
            for (const auto& workspace : m_tree.workspaces) {
                for (const auto& window : workspace.windows) {
                    for (const auto& tab : window.tabs) {
                        for (const auto& pane : tab.panes)
                            isCurrent = isCurrent || (pane.sessionID == session && pane.facts.revision >= revision);
                    }
                }
            }
            m_isWhole = m_isWhole && isCurrent;
        }
        m_listingRevision = listingRevision;
        m_hasCompleted = m_hasCompleted || m_isWhole;
        m_isStale = false;
        return Change::Assembled;
    }

    /// Whether the tree holds everything the host said it would send: every
    /// workspace its answer named, and the facts of every pane that runs.
    [[nodiscard]] bool isWhole() const { return m_isWhole; }

    /// The tree as last assembled; empty before the first.
    [[nodiscard]] const SessionTree& tree() const { return m_tree; }

    /// The host's clock now, kept running by the reader's since the answer.
    ///
    /// - Parameter readerNowUnixMilliseconds: The reader's wall clock.
    /// - Returns: The host's wall clock; what it stated when the reader's
    ///   has gone backwards since.
    [[nodiscard]] std::uint64_t hostNowUnixMilliseconds(std::uint64_t readerNowUnixMilliseconds) const
    {
        return m_hostNowAtAnswer + (readerNowUnixMilliseconds > m_readerNowAtAnswer ? readerNowUnixMilliseconds - m_readerNowAtAnswer : 0);
    }

private:
    std::string m_responseID;
    std::optional<Answer> m_answer;
    Failure m_failure { Failure::None };
    std::string m_failureDetail;
    std::uint64_t m_hostNowAtAnswer = 0;
    std::uint64_t m_readerNowAtAnswer = 0;
    /// The workspaces the answer said the host holds.
    std::vector<std::uint32_t> m_expected;
    std::vector<std::uint64_t> m_expectedRevisions;
    std::map<std::uint32_t, std::uint32_t> m_expectedFacts;
    /// Each structure the connection holds, decoded at the revision it has.
    std::map<std::uint32_t, TreeWorkspace> m_structures;
    std::uint64_t m_listingRevision = 0;
    std::uint64_t m_treeRevision = 0;
    bool m_isStale = false; // the answer arrived since the tree was assembled
    bool m_isWhole = false;
    bool m_hasCompleted = false;
    SessionTree m_tree;
    std::vector<HostInstanceInfo> m_hostInstances;
};

} // namespace Ubiquitty::Session
