// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Session/RememberedSessions.h — which session a host was last seen
// on, and the one rule that turns that memory, a saved host's predefined
// workspace name and the host's own answer into a connect.
//
// The memory and the rule live together because they are one decision: a
// frontend that kept the pairing itself would also have to spell the rule
// itself, and each frontend spelling it produces a different answer to the same
// three inputs — one reattaching where another opens a second shell on the same
// host.
//
// Portable: no platform types and no storage of its own. Where a record is kept
// is the embedder's — a preferences file, a keychain item, a browser store —
// so persistence crosses as a load and a store function, the shape every other
// Core seam a platform fills takes.
//
// Exception/rtti-free (WebKit style); no AppKit / ObjC.

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/AttachTarget.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

/// What connecting to a host should do about its sessions.
///
/// The one definition of the plans, read by every frontend through
/// ``Runtime::HostDirectory``; a kit whose own module cannot re-export this
/// type re-declares the cases and pins the same numbers. The values are
/// explicit for that reason — a re-declaration that trusted the order would
/// drift the first time an enumerator moved, and every wrong value is still in
/// range, so the mistake reads as a connect that lands on the wrong session.
enum class AttachPlan : uint8_t {
    /// Re-home to the remembered session, which the host still lists or has
    /// not answered about. The attach is attach-only, so a session that ended
    /// is refused rather than replaced by a fresh one.
    AttachByID UBIQUITTY_SWIFT_NAME(attachByID) = 0,
    /// Create a session and name it, because nothing listed bears the name.
    CreateNamed UBIQUITTY_SWIFT_NAME(createNamed) = 1,
    /// Attach to the listed session bearing the name.
    AttachNamed UBIQUITTY_SWIFT_NAME(attachNamed) = 2,
    /// Ask the human, because the host has sessions and nothing picks one.
    ChooseFromList UBIQUITTY_SWIFT_NAME(chooseFromList) = 3,
    /// Create a fresh unnamed session, because the host has answered that it
    /// holds none.
    CreateNew UBIQUITTY_SWIFT_NAME(createNew) = 4,
    /// Connect nothing yet, because the host has not answered and nothing
    /// remembered names a session to return to. The caller decides again
    /// once the listing lands.
    AwaitListing UBIQUITTY_SWIFT_NAME(awaitListing) = 5,
};

/// What the person is asked when a plan is ``AttachPlan::ChooseFromList``.
///
/// Which question it is, never its words: the words are
/// ``Runtime::attachQuestionOf``'s, so every frontend asks the same thing.
/// The values are explicit for the reason ``AttachPlan``'s are.
enum class AttachAsk : uint8_t {
    /// Nothing is asked: the plan is not ``AttachPlan::ChooseFromList``.
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// The host holds workspaces and nothing picks one.
    WhichWorkspace UBIQUITTY_SWIFT_NAME(whichWorkspace) = 1,
    /// More than one workspace bears the name asked for.
    SeveralBearTheName UBIQUITTY_SWIFT_NAME(severalBearTheName) = 2,
    /// No workspace bears the name asked for, and at least one is labelled
    /// it.
    NameIsOnlyALabel UBIQUITTY_SWIFT_NAME(nameIsOnlyALabel) = 3,
};

/// One workspace the person may be asked to pick.
struct AttachCandidate {
    /// The workspace, as the host's tree numbers it.
    uint32_t workspaceID {};
    /// The session picking it attaches to: its landing session, or 0 for a
    /// workspace the host would restore, which is attached to by its name.
    uint32_t sessionID {};
    /// What picking it opens, whole: the session and the pane to show, or
    /// the name it is restored by, or why it cannot be opened. The value a
    /// row of the outline carries for the same workspace.
    AttachTarget open;

    friend bool operator==(const AttachCandidate&, const AttachCandidate&) = default;
};

/// The connect one ``RememberedSessions::decide`` describes.
struct AttachDecision {
    /// What to do.
    AttachPlan plan {};
    /// The session the plan lands on: the one to re-home to for
    /// ``AttachPlan::AttachByID``, and the listed session bearing the name for
    /// ``AttachPlan::AttachNamed``; 0 otherwise.
    ///
    /// Only the by-id plan attaches by this id. The named plan attaches by its
    /// name, and carries the id so a frontend can mark the row the name
    /// resolves to without matching names itself.
    uint32_t sessionID {};
    /// The workspace the plan lands in, as the host's tree numbers it: the one
    /// holding the session for ``AttachPlan::AttachByID`` once the host has
    /// answered, and the one bearing the name for ``AttachPlan::AttachNamed``,
    /// which is all a workspace the host would restore has to be marked by;
    /// 0 otherwise.
    uint32_t workspaceID {};
    /// The name to attach under or create under, for the named plans; empty
    /// otherwise.
    std::string workspaceName;
    /// The remembered session when the host answered and no longer lists it;
    /// 0 before the host answers, or while it still lists the session.
    ///
    /// A fact about the record rather than a plan: a frontend shows it as the
    /// session that ended, and never compares ids itself to find it.
    uint32_t vanishedSessionID {};
    /// Whether an unnamed new session is offered beside the plan as a second,
    /// different action.
    ///
    /// False exactly when the plan is that new session: a frontend that drew
    /// New Session beside it would show two buttons doing one thing. Decided
    /// here so no frontend composes its buttons by restating the plan.
    bool isNewSessionOffered {};
    /// Which question the person is asked, for
    /// ``AttachPlan::ChooseFromList``.
    AttachAsk ask {};
    /// The name that was asked for and picked nothing alone; empty unless
    /// ``ask`` is about a name.
    std::string askedName;
    /// The workspaces the question is about, in the tree's order: the ones
    /// bearing the name, the ones labelled it, or every workspace the host
    /// holds.
    std::vector<AttachCandidate> candidates;
};

/// The version stamp a serialised record opens with.
///
/// A record written by a build that spelled the fields differently is refused
/// on load rather than read as this layout, so a store whose meaning changed
/// costs the pairing and never a wrong session.
constexpr uint8_t rememberedSessionRecordVersion = 1;

// The name bound lives with the wire that carries names:
// ``Protocol::Sync::maxWorkspaceNameLength`` (4096). A local duplicate would
// drift the first time one side moved. Workspace names longer than the wire
// bound are floored to it on a UTF-8 boundary when encoding, so a stored
// record is always one the protocol could carry back to a host.

/// What a host was last attached on, remembered across restarts.
///
/// The key is the embedder's name for the host — whatever it already uses to
/// tell one saved connection from another. Nothing here parses it or gives it
/// meaning; two hosts the embedder calls the same thing are one host to this.
class RememberedSessions {
public:
    /// Reads a host's stored bytes.
    ///
    /// - Parameters:
    ///   - context: The embedder's, as ``setPersistence`` was given it.
    ///   - hostKey: The host being recalled.
    ///   - bytesOut: Receives the stored bytes.
    /// - Returns: Whether anything is stored for the host.
    using Load = bool (*)(void* context, const std::string& hostKey, std::string& bytesOut);

    /// Writes a host's bytes, replacing whatever was there.
    ///
    /// - Parameters:
    ///   - context: The embedder's, as ``setPersistence`` was given it.
    ///   - hostKey: The host being remembered.
    ///   - bytes: The record, or empty to forget the host.
    using Store = void (*)(void* context, const std::string& hostKey, const std::string& bytes);

    /// What one host was last attached on.
    struct Record {
        /// The daemon session id, or 0 when the pairing names only a name.
        uint32_t lastSessionID {};
        /// The workspace the session was attached under, empty for an unnamed
        /// one.
        std::string lastWorkspaceName;
    };

    /// Installs the embedder's storage.
    ///
    /// Until this is called every recall answers nothing and every remember is
    /// dropped, which is right for an embedder with nowhere to keep a record:
    /// it connects as though it had never been to the host.
    ///
    /// - Parameters:
    ///   - load: Reads a host's bytes.
    ///   - store: Writes them.
    ///   - context: Handed to both.
    void setPersistence(Load load, Store store, void* context);

    /// What the host was last attached on.
    ///
    /// - Parameter hostKey: The embedder's name for the host.
    /// - Returns: The record, or nothing when the host is unknown, the store
    ///   holds no bytes for it, or the bytes are not a record this build reads.
    [[nodiscard]] std::optional<Record> recall(const std::string& hostKey) const;

    /// Pairs a host with the session it is attached on.
    ///
    /// Replaces the host's record rather than merging into it: a connect knows
    /// both halves of the pairing, and a merge would leave a name from an
    /// earlier connect beside an id from this one.
    ///
    /// - Parameters:
    ///   - hostKey: The embedder's name for the host.
    ///   - sessionID: The session now attached, as the host pinned it.
    ///   - workspaceName: The workspace it is attached under, empty for an
    ///     unnamed session.
    void remember(const std::string& hostKey, uint32_t sessionID, const std::string& workspaceName);

    /// Drops a host's pairing, so the next connect decides as a first one.
    ///
    /// - Parameter hostKey: The embedder's name for the host.
    void forget(const std::string& hostKey);

    /// The one connect rule, so every frontend answers the same inputs the
    /// same way, over the host's tree.
    ///
    /// A session is listed when a pane of a live workspace names it. A
    /// workspace bears a name when its name is that name; a label is never a
    /// name, and nothing attaches through one. Before the host answers, a
    /// remembered id is attached to directly, and the caller makes that attach
    /// attach-only, so a session that ended is refused by the daemon rather
    /// than replaced. With nothing chosen, named or remembered, a host that has
    /// workspaces is a question for the person, a host that answered none is
    /// a new session, and a host that has not answered is waited for — an
    /// unanswered listing is never read as an empty one, because that reading
    /// opens a fresh shell beside the session the person came back for.
    ///
    /// A chosen session the tree lists outranks everything. A predefined name
    /// attaches to the one live workspace bearing it, landing on the session
    /// of its host-focused pane, or to the one workspace the host would
    /// restore under it when none is live; it creates under the name when
    /// nothing bears it and nothing is labelled it. It asks the person when
    /// the name picks more than one workspace, since a HELLO by name would
    /// land on whichever the host found first, and when no workspace bears
    /// the name but one is labelled it, since that may be the one they mean.
    /// A remembered id is attached to while the tree lists it, or before the
    /// host answers; once it is gone, a remembered name returns only to the
    /// one workspace the host would restore under it. A live workspace of
    /// that name is not known to be the one the person left, so it is theirs
    /// to pick.
    ///
    /// - Parameters:
    ///   - remembered: What this host was last attached on, if anything.
    ///   - predefinedWorkspaceName: The name the saved host carries, empty when
    ///     it names none.
    ///   - listed: The host's tree, held fresh; null when the host has not
    ///     answered, the ask failed, or what is held has gone stale, none of
    ///     which is ever read as an empty host or as a session having ended.
    ///   - chosenSessionID: The listed session the human selected, or 0 for
    ///     none.
    /// - Returns: The connect to make.
    [[nodiscard]] static AttachDecision decide(const std::optional<Record>& remembered,
        const std::string& predefinedWorkspaceName,
        const SessionTree* listed,
        uint32_t chosenSessionID = 0);

    /// Writes a record as the bytes the store keeps.
    ///
    /// - Parameter record: The pairing.
    /// - Returns: The bytes.
    [[nodiscard]] static std::string encodeRecord(const Record& record);

    /// Reads bytes the store kept.
    ///
    /// - Parameter bytes: What the store answered.
    /// - Returns: The pairing, or nothing when the bytes are short, carry a
    ///   version this build does not read, or claim a name longer than they
    ///   hold.
    [[nodiscard]] static std::optional<Record> decodeRecord(const std::string& bytes);

private:
    Load m_load = nullptr;
    Store m_store = nullptr;
    void* m_context = nullptr;
    /// Guards the hook triple above. The hooks are plain fields rather than an
    /// owned lease because this object is also a value type tests hold
    /// directly; the process-global publisher (``Runtime::HostDirectory``)
    /// carries the lease, and this lock covers the hook-once write against a
    /// recall racing the first install. Never held across an embedder call:
    /// every member snapshots the triple, unlocks, and only then invokes.
    mutable std::mutex m_mutex;
};

} // namespace Ubiquitty::Session
