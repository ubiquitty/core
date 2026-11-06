// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SessionListWords.h — the words a list of a host's sessions is drawn with.
//
// A Connect window, a sidebar, a page and a phone each list a host's
// sessions, and each shows the same states: a host that is being asked, one
// that answered nothing, one that could not be reached, a session that would
// not end. Which state a host is in is decided here from the facts a
// directory reports, and so is every word shown for it: the title a content
// area takes, the short sentence a sidebar has room for, the label a reader
// that cannot see is given, the button that is the default and the sentence
// beside it. A frontend draws them and words none.
//
// A person is shown a workspace as a "session": the thing they open, name
// and return to. The session behind one pane is "the pane's session".
//
// Three kinds of words:
//
// - A phrase that never varies is a `SessionListPhrase`.
// - A sentence built from a name, a count or an age is a function.
// - A state with several wordings is a struct of them, decided once.
//
// A function a frontend calls with a name takes it as a string, not a view:
// a caller across a language seam hands over a value, and a view of it would
// outlive it.
//
// Pure: no clock, no preferences and no tree are read from here.
//

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/AttachTarget.h"
#include "Core/Session/Identity.h"
#include "Core/Session/RememberedSessions.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/SessionRows.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Runtime {

/// A phrase a session list shows that never varies. The values are explicit
/// because the Swift and web mirrors pin them.
enum class SessionListPhrase : std::uint8_t {
    // Buttons.
    Connect UBIQUITTY_SWIFT_NAME(connect) = 0,
    Open UBIQUITTY_SWIFT_NAME(open) = 1,
    NewSession UBIQUITTY_SWIFT_NAME(newSession) = 2,
    ShowWindow UBIQUITTY_SWIFT_NAME(showWindow) = 3,
    TryAgain UBIQUITTY_SWIFT_NAME(tryAgain) = 4,
    AskAgain UBIQUITTY_SWIFT_NAME(askAgain) = 5,
    Cancel UBIQUITTY_SWIFT_NAME(cancel) = 6,
    Save UBIQUITTY_SWIFT_NAME(save) = 7,
    Create UBIQUITTY_SWIFT_NAME(create) = 8,
    Attach UBIQUITTY_SWIFT_NAME(attach) = 9,
    // A row's menu and the toolbar.
    CopySessionLink UBIQUITTY_SWIFT_NAME(copySessionLink) = 10,
    Label UBIQUITTY_SWIFT_NAME(label) = 11,
    Tags UBIQUITTY_SWIFT_NAME(tags) = 12,
    RenameSession UBIQUITTY_SWIFT_NAME(renameSession) = 13,
    RenameTab UBIQUITTY_SWIFT_NAME(renameTab) = 14,
    MoveTabEarlier UBIQUITTY_SWIFT_NAME(moveTabEarlier) = 15,
    MoveTabLater UBIQUITTY_SWIFT_NAME(moveTabLater) = 16,
    MoveTabToWindow UBIQUITTY_SWIFT_NAME(moveTabToWindow) = 17,
    CloseWindow UBIQUITTY_SWIFT_NAME(closeWindow) = 18,
    Terminate UBIQUITTY_SWIFT_NAME(terminate) = 19,
    TerminateSession UBIQUITTY_SWIFT_NAME(terminateSession) = 20,
    NewNamedSession UBIQUITTY_SWIFT_NAME(newNamedSession) = 21,
    ExpandAll UBIQUITTY_SWIFT_NAME(expandAll) = 22,
    // The sheets a label, tags and a name are edited in.
    NoLabel UBIQUITTY_SWIFT_NAME(noLabel) = 23,
    TagsSeparatedByCommas UBIQUITTY_SWIFT_NAME(tagsSeparatedByCommas) = 24,
    SessionName UBIQUITTY_SWIFT_NAME(sessionName) = 25,
    TabName UBIQUITTY_SWIFT_NAME(tabName) = 26,
    NoName UBIQUITTY_SWIFT_NAME(noName) = 27,
    // Markers beside a row, and what a host is.
    LastUsed UBIQUITTY_SWIFT_NAME(lastUsed) = 28,
    AlwaysOpens UBIQUITTY_SWIFT_NAME(alwaysOpens) = 29,
    Ended UBIQUITTY_SWIFT_NAME(ended) = 30,
    LastSession UBIQUITTY_SWIFT_NAME(lastSession) = 31,
    ThisMachine UBIQUITTY_SWIFT_NAME(thisMachine) = 32,
    UbiquittyHost UBIQUITTY_SWIFT_NAME(ubiquittyHost) = 33,
    // What a mark is, for a reader that cannot see it.
    ShownInAWindowHere UBIQUITTY_SWIFT_NAME(shownInAWindowHere) = 34,
    AWindowShowsThisHost UBIQUITTY_SWIFT_NAME(aWindowShowsThisHost) = 35,
    TabColour UBIQUITTY_SWIFT_NAME(tabColour) = 36,
    Badges UBIQUITTY_SWIFT_NAME(badges) = 37,
    Bell UBIQUITTY_SWIFT_NAME(bell) = 38,
    Notification UBIQUITTY_SWIFT_NAME(notification) = 39,
    Indicator UBIQUITTY_SWIFT_NAME(indicator) = 40,
    LastActive UBIQUITTY_SWIFT_NAME(lastActive) = 41,
    Here UBIQUITTY_SWIFT_NAME(here) = 42,
    HoldsHere UBIQUITTY_SWIFT_NAME(holdsHere) = 43,
    ContinueOnAnotherDevice UBIQUITTY_SWIFT_NAME(continueOnAnotherDevice) = 44,
};

/// How many phrases there are.
inline constexpr std::size_t sessionListPhraseCount = 45;

/// A phrase's words.
///
/// - Parameter phrase: Which.
/// - Returns: The words, as they are shown; empty for a number that names
///   no phrase.
[[nodiscard]] constexpr std::string_view sessionListPhraseWords(SessionListPhrase phrase)
{
    switch (phrase) {
    case SessionListPhrase::Connect:
        return "Connect";
    case SessionListPhrase::Open:
        return "Open";
    case SessionListPhrase::NewSession:
        return "New Workspace";
    case SessionListPhrase::ShowWindow:
        return "Show Window";
    case SessionListPhrase::TryAgain:
        return "Try Again";
    case SessionListPhrase::AskAgain:
        return "Ask Again";
    case SessionListPhrase::Cancel:
        return "Cancel";
    case SessionListPhrase::Save:
        return "Save";
    case SessionListPhrase::Create:
        return "Create";
    case SessionListPhrase::Attach:
        return "Attach";
    case SessionListPhrase::CopySessionLink:
        return "Copy Session Link";
    case SessionListPhrase::Label:
        return "Label";
    case SessionListPhrase::Tags:
        return "Tags";
    case SessionListPhrase::RenameSession:
        return "Rename Workspace…";
    case SessionListPhrase::RenameTab:
        return "Rename Tab…";
    case SessionListPhrase::MoveTabEarlier:
        return "Move Tab Earlier";
    case SessionListPhrase::MoveTabLater:
        return "Move Tab Later";
    case SessionListPhrase::MoveTabToWindow:
        return "Move Tab to Window";
    case SessionListPhrase::CloseWindow:
        return "Close Window…";
    case SessionListPhrase::Terminate:
        return "Terminate…";
    case SessionListPhrase::TerminateSession:
        return "Terminate Session";
    case SessionListPhrase::NewNamedSession:
        return "New Named Workspace…";
    case SessionListPhrase::ExpandAll:
        return "Expand All";
    case SessionListPhrase::NoLabel:
        return "No label";
    case SessionListPhrase::TagsSeparatedByCommas:
        return "Separated by commas";
    case SessionListPhrase::SessionName:
        return "Workspace Name";
    case SessionListPhrase::TabName:
        return "Tab Name";
    case SessionListPhrase::NoName:
        return "No name";
    case SessionListPhrase::LastUsed:
        return "Last used";
    case SessionListPhrase::AlwaysOpens:
        return "Always opens";
    case SessionListPhrase::Ended:
        return "ended";
    case SessionListPhrase::LastSession:
        return "Last workspace";
    case SessionListPhrase::ThisMachine:
        return "This Mac";
    case SessionListPhrase::UbiquittyHost:
        return "Ubiquitty Host";
    case SessionListPhrase::ShownInAWindowHere:
        return "Shown in a window here";
    case SessionListPhrase::AWindowShowsThisHost:
        return "A window shows this host";
    case SessionListPhrase::TabColour:
        return "Tab color";
    case SessionListPhrase::Badges:
        return "Badges";
    case SessionListPhrase::Bell:
        return "Bell";
    case SessionListPhrase::Notification:
        return "Notification";
    case SessionListPhrase::Indicator:
        return "Indicator";
    case SessionListPhrase::LastActive:
        return "Last active";
    case SessionListPhrase::Here:
        return "This window";
    case SessionListPhrase::HoldsHere:
        return "Holds this window";
    case SessionListPhrase::ContinueOnAnotherDevice:
        return "Continue on Another Device…";
    }
    return {};
}

/// A phrase's words, for a caller across a language seam.
///
/// - Parameter phrase: A `SessionListPhrase`'s number.
/// - Returns: The words; empty for a number that names no phrase.
[[nodiscard]] inline std::string sessionListPhrase(std::uint8_t phrase)
{
    return phrase < sessionListPhraseCount ? std::string(sessionListPhraseWords(static_cast<SessionListPhrase>(phrase))) : std::string();
}

/// The name a phrase is keyed by where phrases are handed over as one
/// object: the page reads `{key: words}`.
///
/// - Parameter phrase: Which.
/// - Returns: The key, in lower camel case; empty for a number that names
///   no phrase.
[[nodiscard]] constexpr std::string_view sessionListPhraseKey(SessionListPhrase phrase)
{
    constexpr std::string_view keys[sessionListPhraseCount] = {
        "connect", "open", "newSession", "showWindow", "tryAgain", "askAgain", "cancel", "save", "create", "attach", "copySessionLink",
        "label", "tags", "renameSession", "renameTab", "moveTabEarlier", "moveTabLater", "moveTabToWindow", "closeWindow", "terminate", "terminateSession", "newNamedSession",
        "expandAll", "noLabel", "tagsSeparatedByCommas", "sessionName", "tabName", "noName", "lastUsed", "alwaysOpens", "ended", "lastSession", "thisMachine",
        "ubiquittyHost", "shownInAWindowHere", "aWindowShowsThisHost", "tabColour", "badges", "bell", "notification", "indicator", "lastActive", "here", "holdsHere", "continueOnAnotherDevice"
    };
    const std::size_t index = std::to_underlying(phrase);
    return index < sessionListPhraseCount ? keys[index] : std::string_view();
}

namespace SessionListWordsDetail {

/// A name in the quotation marks a sentence puts around it.
///
/// - Parameter name: The name.
/// - Returns: The name between a left and a right double quotation mark.
[[nodiscard]] inline std::string quoted(std::string_view name)
{
    std::string text = "“";
    text.append(name);
    text.append("”");
    return text;
}

/// A count with its noun, singular for one.
///
/// - Parameters:
///   - count: How many.
///   - singular: The noun for one.
///   - plural: The noun for any other count.
/// - Returns: "1 session", "3 sessions".
[[nodiscard]] inline std::string counted(std::uint64_t count, std::string_view singular, std::string_view plural)
{
    std::string text = std::to_string(count);
    text.push_back(' ');
    text.append(count == 1 ? singular : plural);
    return text;
}

/// How long ago, as a list says it.
///
/// - Parameter seconds: Whole seconds.
/// - Returns: "just now" under five seconds, then "12s ago", "3m ago",
///   "2h ago" or "5d ago".
[[nodiscard]] inline std::string ago(std::uint64_t seconds)
{
    if (seconds < 5)
        return "just now";
    if (seconds < 60)
        return std::to_string(seconds) + "s ago";
    if (seconds < 3600)
        return std::to_string(seconds / 60) + "m ago";
    if (seconds < 86'400)
        return std::to_string(seconds / 3600) + "h ago";
    return std::to_string(seconds / 86'400) + "d ago";
}

} // namespace SessionListWordsDetail

// ---- Sentences built from a name, a count or an age -------------------------

/// How many sessions a host holds, as a list's summary says it.
///
/// - Parameter count: How many workspaces the host's tree holds.
/// - Returns: "no sessions", "1 session", "3 sessions".
[[nodiscard]] inline std::string sessionCountWords(std::uint64_t count)
{
    return count == 0 ? std::string("no workspaces") : SessionListWordsDetail::counted(count, "workspace", "workspaces");
}

/// The menu item that edits one level's label or tags, by what a person
/// calls that level.
///
/// - Parameter kind: The level.
/// - Returns: "Session…" for a workspace, "Pane’s Session…" for the session
///   behind a pane, and so on.
[[nodiscard]] constexpr std::string_view levelMenuTitle(Session::Identity::ObjectKind kind)
{
    switch (kind) {
    case Session::Identity::ObjectKind::Workspace:
        return "Workspace…";
    case Session::Identity::ObjectKind::Session:
        return "Pane’s Session…";
    case Session::Identity::ObjectKind::Window:
        return "Window…";
    case Session::Identity::ObjectKind::Tab:
        return "Tab…";
    case Session::Identity::ObjectKind::Pane:
        return "Pane…";
    case Session::Identity::ObjectKind::Client:
        return "Client…";
    }
    return {};
}

/// ``levelMenuTitle`` for a caller across a language seam.
///
/// - Parameter kind: A `Session::Identity::ObjectKind`'s number.
/// - Returns: The title; empty for a number that names no kind.
[[nodiscard]] inline std::string levelMenuTitleWords(std::uint8_t kind)
{
    return kind < Session::Identity::objectKindCount ? std::string(levelMenuTitle(static_cast<Session::Identity::ObjectKind>(kind))) : std::string();
}

/// What the control that closes an open row is, for a reader that cannot
/// see it.
///
/// - Parameter title: What the row is called.
/// - Returns: "Collapse build".
[[nodiscard]] inline std::string collapseControlWords(const std::string& title) { return "Collapse " + std::string(title); }

/// What the control that opens a closed row is, for a reader that cannot
/// see it.
///
/// - Parameter title: What the row is called.
/// - Returns: "Expand build".
[[nodiscard]] inline std::string expandControlWords(const std::string& title) { return "Expand " + std::string(title); }

/// A label chip, for a reader that cannot see it.
///
/// - Parameter label: The label.
/// - Returns: "Label build".
[[nodiscard]] inline std::string labelChipWords(const std::string& label) { return "Label " + std::string(label); }

/// A tag chip, for a reader that cannot see it.
///
/// - Parameter name: The tag.
/// - Returns: "Tag production".
[[nodiscard]] inline std::string tagChipWords(const std::string& name) { return "Tag " + std::string(name); }

/// A pane's grid.
///
/// - Parameters:
///   - columns: Its width in cells.
///   - rows: Its height in cells.
/// - Returns: "132×43"; empty when either is not positive.
[[nodiscard]] inline std::string gridWords(int columns, int rows)
{
    return columns > 0 && rows > 0 ? std::to_string(columns) + "×" + std::to_string(rows) : std::string();
}

/// When anything under a row last wrote.
///
/// - Parameter seconds: How long ago, by the host's clock.
/// - Returns: "Last output 3m ago".
[[nodiscard]] inline std::string lastOutputWords(std::uint64_t seconds) { return "Last output " + SessionListWordsDetail::ago(seconds); }

/// When anything under a row was last typed into.
///
/// - Parameter seconds: How long ago, by the host's clock.
/// - Returns: "Last input 3m ago".
[[nodiscard]] inline std::string lastInputWords(std::uint64_t seconds) { return "Last input " + SessionListWordsDetail::ago(seconds); }

/// When anything under a row last moved either way.
///
/// - Parameter seconds: How long ago, by the host's clock.
/// - Returns: "Last active 3m ago".
[[nodiscard]] inline std::string lastActiveWords(std::uint64_t seconds) { return "Last active " + SessionListWordsDetail::ago(seconds); }

/// The compact age at the end of a row, without repeating what is aged.
///
/// - Parameter seconds: How long ago, by the host's clock.
/// - Returns: "just now", "12s ago", "3m ago", "2h ago" or "5d ago".
[[nodiscard]] inline std::string compactAgeWords(std::uint64_t seconds) { return SessionListWordsDetail::ago(seconds); }

/// The process and lifetime details of one pane's session. Dates remain
/// instants so each frontend can format them in the person's locale.
struct HostSessionDetails {
    std::uint32_t sessionID = 0;
    std::uint64_t createdUnixSeconds = 0;
    std::string title;
    std::string directory;
    std::string placement;
    std::string creationLabel;
    std::string unknownCreationTime;
    std::string origin;
    std::string sharing;
    std::string lifetime;
};

/// The details under one live outline level, from the same tree as its row.
/// Restorable workspaces contain no running processes to describe.
///
/// - Parameters:
///   - tree: The host's tree.
///   - target: The workspace, window, tab, pane or session selected.
/// - Returns: One detail per session below it, in session-id order.
[[nodiscard]] inline std::vector<HostSessionDetails> hostSessionDetails(const Session::SessionTree& tree, Session::Identity::ObjectID target)
{
    using Session::Identity::ObjectKind;
    std::vector<HostSessionDetails> details;
    for (const Session::SessionRow& row : Session::sessionRows(tree)) {
        if (row.isRestorable())
            continue;
        std::uint32_t number = 0;
        switch (target.kind) {
        case ObjectKind::Workspace:
            number = row.workspaceID;
            break;
        case ObjectKind::Window:
            number = row.windowID;
            break;
        case ObjectKind::Tab:
            number = row.tabID;
            break;
        case ObjectKind::Pane:
            number = row.paneID;
            break;
        case ObjectKind::Session:
            number = row.sessionID;
            break;
        case ObjectKind::Client:
            break;
        }
        if (number == 0 || number != target.number)
            continue;
        HostSessionDetails detail;
        detail.sessionID = row.sessionID;
        detail.createdUnixSeconds = row.createdUnixSeconds;
        detail.title = row.foregroundProcess.empty() ? "Session " + std::to_string(row.sessionID) : row.foregroundProcess;
        detail.directory = row.currentDirectory;
        detail.placement = (row.workspaceName.empty() ? std::string() : row.workspaceName + " · ")
            + "Window " + std::to_string(row.windowID) + " · Tab " + std::to_string(row.tabID) + " · Pane " + std::to_string(row.paneID);
        detail.creationLabel = row.isAdopted ? "Hosted since" : "Started";
        detail.unknownCreationTime = "Not recorded";
        detail.origin = row.isAdopted ? "Adopted" : "Host process";
        detail.sharing = row.isShared() ? "Multiple clients" : "No concurrent clients";
        detail.lifetime = row.endsWhenDetached() ? "Ends after the last client leaves" : "Survives client disconnect";
        details.push_back(std::move(detail));
    }
    return details;
}

/// Why a session name is refused before anything is asked.
///
/// - Returns: The sentence, with the most bytes a name holds.
[[nodiscard]] inline std::string sessionNameTooLongSentence()
{
    return "A workspace name holds at most " + std::to_string(Protocol::Sync::maxWorkspaceNameLength) + " bytes.";
}

/// What naming a new session a name the host already holds does.
///
/// - Parameter name: The name.
/// - Returns: The sentence.
[[nodiscard]] inline std::string sessionNameTakenSentence(const std::string& name)
{
    return SessionListWordsDetail::quoted(name) + " is already on the host; this opens it rather than starting a second one.";
}

/// The button that opens the session a new one would have been named as.
///
/// - Parameter name: The name.
/// - Returns: "Open Existing “ci”".
[[nodiscard]] inline std::string openExistingTitle(const std::string& name) { return "Open Existing " + SessionListWordsDetail::quoted(name); }

// ---- Notices ------------------------------------------------------------------

/// A notice: what happened, and what it means for the person.
struct SessionListNotice {
    std::string title;
    std::string message;
    /// Whether asking again is the action beside it: the people on a session
    /// declined, or did not answer.
    bool doesOfferAskingAgain = false; // bool-ok: names the one condition it reports
};

/// What a session is called in a notice when its row's name was not kept.
///
/// - Parameter sessionID: The session.
/// - Returns: "session 12".
[[nodiscard]] inline std::string unnamedSessionWords(std::uint32_t sessionID) { return "session " + std::to_string(sessionID); }

/// The notice for how a terminate settled.
///
/// - Parameters:
///   - outcome: A `Session::TerminateOutcome`'s number; any other number is
///     an answer this build does not know.
///   - name: What the session is called; empty for none kept.
///   - sessionID: The session, which names it when `name` is empty.
/// - Returns: The notice; empty for a session that was terminated, which
///   the list shows by losing its row.
[[nodiscard]] inline SessionListNotice terminateOutcomeNotice(std::uint8_t outcome, const std::string& name, std::uint32_t sessionID)
{
    const std::string called = name.empty() ? unnamedSessionWords(sessionID) : name;
    SessionListNotice notice;
    switch (outcome) {
    case std::to_underlying(Session::TerminateOutcome::Terminated):
        return notice;
    case std::to_underlying(Session::TerminateOutcome::Denied):
        notice.title = "A person on " + called + " declined.";
        notice.message = "The workspace is still running. Whoever is attached to it answered the host’s question with no.";
        notice.doesOfferAskingAgain = true;
        return notice;
    case std::to_underlying(Session::TerminateOutcome::TimedOut):
        notice.title = "Nobody answered for " + called + " in time.";
        notice.message = "The host asked whoever is attached and its window closed unanswered. The workspace is still running.";
        notice.doesOfferAskingAgain = true;
        return notice;
    case std::to_underlying(Session::TerminateOutcome::Failed):
        // Failed is a request that never left this machine: the host neither
        // took it nor refused it.
        notice.title = "The request to end " + called + " didn’t reach the host.";
        notice.message = "Nothing on the host was touched, and the workspace is still running. Check the connection and try again.";
        return notice;
    case std::to_underlying(Session::TerminateOutcome::Refused):
        notice.title = "The host refused to terminate " + called + ".";
        notice.message = "The host declined the request itself, not a person, and Ubiquitty won’t retry it. The workspace is still running.";
        return notice;
    default:
        notice.title = "The host sent an answer about " + called + " that this version of Ubiquitty doesn’t understand.";
        notice.message = "Nothing was assumed about the workspace. Refresh the list to see what the host is holding now.";
        return notice;
    }
}

/// The notice for a connect that was waiting for a listing that failed.
///
/// - Parameters:
///   - hostName: What the host is called.
///   - reason: Why the listing failed, standing alone; empty for none.
/// - Returns: The notice.
[[nodiscard]] inline SessionListNotice listingFailedWhileConnectingNotice(const std::string& hostName, const std::string& reason)
{
    SessionListNotice notice;
    notice.title = "Ubiquitty couldn’t ask " + std::string(hostName) + " which workspaces it’s holding.";
    if (!reason.empty())
        notice.message = std::string(reason) + "\n\n";
    notice.message += "Nothing was opened, so no workspace was started on the host. Check the connection and connect again.";
    return notice;
}

/// The notice for a connect that produced no window on this machine.
///
/// - Parameter hostName: What the host is called.
/// - Returns: The notice.
[[nodiscard]] inline SessionListNotice windowNotOpenedNotice(const std::string& hostName)
{
    SessionListNotice notice;
    notice.title = "Ubiquitty couldn’t open a window on " + std::string(hostName) + ".";
    notice.message = "Nothing on the host was touched.";
    return notice;
}

// ---- What a host's section shows ----------------------------------------------

/// How far a listing of a host has got. Numbered as
/// `HostDirectoryClient::ListingPhase`.
enum class HostListingProgress : std::uint8_t {
    NotAsked UBIQUITTY_SWIFT_NAME(notAsked) = 0,
    Asking UBIQUITTY_SWIFT_NAME(asking) = 1,
    Listed UBIQUITTY_SWIFT_NAME(listed) = 2,
    Failed UBIQUITTY_SWIFT_NAME(failed) = 3,
};

/// Where a host is.
enum class HostKind : std::uint8_t {
    /// Another machine, reached over a carrier.
    Remote UBIQUITTY_SWIFT_NAME(remote) = 0,
    /// This machine's own host.
    ThisMachine UBIQUITTY_SWIFT_NAME(thisMachine) = 1,
};

/// Why a host could not be dialled at all, which a frontend learns before
/// any errand starts.
enum class HostTransportRefusal : std::uint8_t {
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// The host's address is not one ssh can be handed.
    InvalidAddress UBIQUITTY_SWIFT_NAME(invalidAddress) = 1,
    /// This copy of the app carries no host to run on this machine.
    MissingLocalHostBinary UBIQUITTY_SWIFT_NAME(missingLocalHostBinary) = 2,
};

/// What a host's section shows. The values are explicit because the Swift
/// and web mirrors pin them.
enum class HostSectionState : std::uint8_t {
    /// Its sessions.
    Sessions UBIQUITTY_SWIFT_NAME(sessions) = 0,
    /// Nothing yet: the host has not answered, and nothing earlier is held.
    Asking UBIQUITTY_SWIFT_NAME(asking) = 1,
    /// The host answered and holds nothing.
    Empty UBIQUITTY_SWIFT_NAME(empty) = 2,
    Unreachable UBIQUITTY_SWIFT_NAME(unreachable) = 3,
    SignInRequired UBIQUITTY_SWIFT_NAME(signInRequired) = 4,
    NotInstalled UBIQUITTY_SWIFT_NAME(notInstalled) = 5,
    NeedsToken UBIQUITTY_SWIFT_NAME(needsToken) = 6,
    TokenInvalid UBIQUITTY_SWIFT_NAME(tokenInvalid) = 7,
    TokenExpired UBIQUITTY_SWIFT_NAME(tokenExpired) = 8,
    TokenRevoked UBIQUITTY_SWIFT_NAME(tokenRevoked) = 9,
    TokenScope UBIQUITTY_SWIFT_NAME(tokenScope) = 10,
    OlderHost UBIQUITTY_SWIFT_NAME(olderHost) = 11,
    OlderClient UBIQUITTY_SWIFT_NAME(olderClient) = 12,
    Refused UBIQUITTY_SWIFT_NAME(refused) = 13,
    Malformed UBIQUITTY_SWIFT_NAME(malformed) = 14,
    InvalidAddress UBIQUITTY_SWIFT_NAME(invalidAddress) = 15,
    HostBinaryMissing UBIQUITTY_SWIFT_NAME(hostBinaryMissing) = 16,
    /// The SSH server ended the connection after too many authentication
    /// attempts.
    AuthenticationAttemptsExhausted UBIQUITTY_SWIFT_NAME(authenticationAttemptsExhausted) = 17,
    /// The host closed the connection before its `ubiquitty-host` answered.
    EndedBeforeAnswer UBIQUITTY_SWIFT_NAME(endedBeforeAnswer) = 18,
};

/// What a directory reports about one host, as the facts its section's
/// state and words are decided from.
struct HostSectionFacts {
    /// Whether the host is this machine.
    bool isThisMachine = false; // bool-ok: names the one condition it reports
    HostTransportRefusal transportRefusal = HostTransportRefusal::None;
    HostListingProgress progress = HostListingProgress::NotAsked;
    /// Why the last listing failed; `None` when it did not.
    Session::ListingFailure failure = Session::ListingFailure::None;
    /// Why the host refused, for a listing that failed `Refused`.
    Protocol::Sync::ByeReason deniedReason = Protocol::Sync::ByeReason::TokenMissing;
    /// Which build is the older one, for a version refusal.
    Protocol::Sync::OlderSide olderSide = Protocol::Sync::OlderSide::Unknown;
    /// The platform of a host with nothing installed; empty when unknown.
    std::string remotePlatform;
    /// How many workspaces the tree the reader holds lists.
    std::uint32_t workspaceCount = 0;
    /// Whether a tree is held that a watch no longer follows.
    bool isStale = false; // bool-ok: names the one state it reports
    /// Whether the host ever answered, and how long ago it last did.
    bool hasAnswered = false; // bool-ok: names the one condition it reports
    std::uint64_t secondsSinceAnswered = 0;
};

/// Decides what a host's section shows.
///
/// A host that cannot be dialled says so first. A failed listing replaces
/// the sessions an earlier one left, unless the failure is only that the
/// host could not be reached: those stay on screen, stale. With nothing to
/// show, the section says how far the asking has got.
///
/// - Parameter facts: What the directory reports.
/// - Returns: The state.
[[nodiscard]] constexpr HostSectionState hostSectionState(const HostSectionFacts& facts)
{
    using Protocol::Sync::ByeReason;
    using Session::ListingFailure;
    switch (facts.transportRefusal) {
    case HostTransportRefusal::InvalidAddress:
        return HostSectionState::InvalidAddress;
    case HostTransportRefusal::MissingLocalHostBinary:
        return HostSectionState::HostBinaryMissing;
    case HostTransportRefusal::None:
        break;
    }
    if (facts.failure != ListingFailure::None && (facts.workspaceCount == 0 || facts.failure != ListingFailure::Unreachable)) {
        switch (facts.failure) {
        case ListingFailure::None:
            break;
        case ListingFailure::Unreachable:
            return HostSectionState::Unreachable;
        case ListingFailure::SignInRequired:
            return HostSectionState::SignInRequired;
        case ListingFailure::AuthenticationAttemptsExhausted:
            return HostSectionState::AuthenticationAttemptsExhausted;
        // A stopped daemon closes the pinned errand before any host answers;
        // the frontend asks the host's current daemon next.
        case ListingFailure::EndedBeforeAnswer:
        case ListingFailure::InstanceGone:
            return HostSectionState::EndedBeforeAnswer;
        case ListingFailure::NotInstalled:
            return HostSectionState::NotInstalled;
        case ListingFailure::Malformed:
        case ListingFailure::InstanceUnreadable:
            return HostSectionState::Malformed;
        case ListingFailure::Refused:
            switch (facts.deniedReason) {
            case ByeReason::TokenMissing:
                return HostSectionState::NeedsToken;
            case ByeReason::TokenInvalid:
                return HostSectionState::TokenInvalid;
            case ByeReason::TokenExpired:
                return HostSectionState::TokenExpired;
            case ByeReason::TokenRevoked:
                return HostSectionState::TokenRevoked;
            case ByeReason::OutsideScope:
                return HostSectionState::TokenScope;
            case ByeReason::VersionMismatch:
                return facts.olderSide == Protocol::Sync::OlderSide::Host  ? HostSectionState::OlderHost
                    : facts.olderSide == Protocol::Sync::OlderSide::Client ? HostSectionState::OlderClient
                                                                           : HostSectionState::Refused;
            case ByeReason::DuplicateHandshake:
            case ByeReason::NoConsent:
            case ByeReason::RemoteHostRefused:
            case ByeReason::SessionGone:
            case ByeReason::HostFull:
            // Not refusals; a listing that failed as refused with one of
            // these says no more than that.
            case ByeReason::Normal:
            case ByeReason::Malformed:
            case ByeReason::Oversize:
            case ByeReason::UnknownKind:
            case ByeReason::UnboundHandle:
            case ByeReason::TransferRefused:
            case ByeReason::Superseded:
            case ByeReason::ResumeInProgress:
            case ByeReason::Suspending:
            case ByeReason::HandingOver:
            case ByeReason::OutputBacklog:
            case ByeReason::Silent:
                return HostSectionState::Refused;
            }
            return HostSectionState::Refused;
        }
    }
    if (facts.workspaceCount == 0) {
        switch (facts.progress) {
        case HostListingProgress::NotAsked:
        case HostListingProgress::Asking:
            return HostSectionState::Asking;
        case HostListingProgress::Listed:
            return HostSectionState::Empty;
        case HostListingProgress::Failed:
            return HostSectionState::Unreachable;
        }
    }
    return HostSectionState::Sessions;
}

/// Every wording of a host's section.
struct HostSectionWords {
    HostSectionState state = HostSectionState::Sessions;
    /// The title a content area takes, with the host's name in it; empty
    /// while the section shows sessions, and for a version refusal, which
    /// `Session::versionRefusalTitle` words from the two builds.
    std::string title;
    /// The sentence a sidebar has room for, with no host name: the section
    /// is under it. Empty while the section shows sessions a watch follows.
    std::string shortSentence;
    /// What the section's mark is, for a reader that cannot see it; empty
    /// while it shows sessions.
    std::string markWords;
    /// The facts line under a window's title: what the host is, then its
    /// state, joined by a middle dot.
    std::string subtitle;
};

/// Words a host's section.
///
/// - Parameters:
///   - facts: What the directory reports.
///   - hostName: What the host is called.
/// - Returns: The state and every wording of it.
[[nodiscard]] inline HostSectionWords hostSectionWords(const HostSectionFacts& facts, const std::string& hostName)
{
    using SessionListWordsDetail::ago;
    const std::string& host = hostName;
    HostSectionWords words;
    words.state = hostSectionState(facts);
    switch (words.state) {
    case HostSectionState::Sessions:
        // Still on screen after the host went quiet: say how old they are.
        if (facts.isStale || facts.failure == Session::ListingFailure::Unreachable)
            words.shortSentence = facts.hasAnswered ? "Not reached. Last answered " + ago(facts.secondsSinceAnswered) + "." : std::string("Not reached.");
        break;
    case HostSectionState::Asking:
        words.title = "Asking " + host + "…";
        words.shortSentence = "Asking…";
        words.markWords = "Asking";
        break;
    case HostSectionState::Empty:
        words.title = "No Workspaces on " + host;
        words.shortSentence = "No workspaces.";
        break;
    case HostSectionState::Unreachable:
        words.title = "Can’t Reach " + host;
        words.shortSentence = facts.hasAnswered ? "Can’t reach this host. Last answered " + ago(facts.secondsSinceAnswered) + "." : std::string("Can’t reach this host.");
        words.markWords = "Can’t reach this host";
        break;
    case HostSectionState::SignInRequired:
        words.title = host + " Needs a Password or Key";
        words.shortSentence = "Needs a password or key.";
        words.markWords = "Can’t sign in";
        break;
    case HostSectionState::AuthenticationAttemptsExhausted:
        words.title = "Too Many Authentication Attempts";
        words.shortSentence = "Too many authentication attempts.";
        words.markWords = "Can’t sign in";
        break;
    case HostSectionState::EndedBeforeAnswer:
        words.title = host + " Closed the Connection";
        words.shortSentence = "Closed the connection before answering.";
        words.markWords = "Closed the connection";
        break;
    case HostSectionState::NotInstalled:
        words.title = "Ubiquitty Isn’t Installed on " + host;
        words.shortSentence = "Ubiquitty isn’t installed here.";
        words.markWords = "Not installed";
        break;
    case HostSectionState::NeedsToken:
    case HostSectionState::TokenInvalid:
        words.title = host + " Requires an Access Token";
        words.shortSentence = "Needs an access token.";
        words.markWords = "Needs an access token";
        break;
    case HostSectionState::TokenExpired:
        words.title = host + "’s Access Token Has Expired";
        words.shortSentence = "The access token has expired.";
        words.markWords = "Needs an access token";
        break;
    case HostSectionState::TokenRevoked:
        words.title = host + "’s Access Token Was Revoked";
        words.shortSentence = "The access token was revoked.";
        words.markWords = "Needs an access token";
        break;
    case HostSectionState::TokenScope:
        words.title = "This Token Can’t Open Workspaces on " + host;
        words.shortSentence = "This token can’t open workspaces.";
        words.markWords = "Needs an access token";
        break;
    case HostSectionState::OlderHost:
        words.shortSentence = "Runs an older host.";
        words.markWords = "Runs an older host";
        break;
    case HostSectionState::OlderClient:
        words.shortSentence = "Runs a newer host than this app.";
        words.markWords = "Can’t reach this host";
        break;
    case HostSectionState::Refused:
        words.title = host + " Refused the Connection";
        words.shortSentence = "Refused the connection.";
        words.markWords = "Can’t reach this host";
        break;
    case HostSectionState::Malformed:
        words.title = host + " Sent an Unexpected Reply";
        words.shortSentence = "Sent a reply Ubiquitty doesn’t understand.";
        words.markWords = "Can’t reach this host";
        break;
    case HostSectionState::InvalidAddress:
        words.title = host + "’s Address Isn’t Valid";
        words.shortSentence = "The address isn’t valid.";
        words.markWords = "Can’t reach this host";
        break;
    case HostSectionState::HostBinaryMissing:
        words.title = "Ubiquitty’s Host Is Missing";
        words.shortSentence = "This copy of Ubiquitty doesn’t include ubiquitty-host, so it can’t start a host here.";
        words.markWords = "Can’t reach this host";
        break;
    }

    // The facts line: what the host is, then how the listing stands.
    words.subtitle = std::string(sessionListPhraseWords(facts.isThisMachine ? SessionListPhrase::ThisMachine : SessionListPhrase::UbiquittyHost));
    const auto part = [&words](std::string_view said) {
        if (!said.empty())
            words.subtitle.append(" · ").append(said);
    };
    switch (facts.progress) {
    case HostListingProgress::NotAsked:
        break;
    case HostListingProgress::Asking:
        part("Asking…");
        break;
    case HostListingProgress::Listed:
        part(sessionCountWords(facts.workspaceCount));
        break;
    case HostListingProgress::Failed:
        if (facts.failure == Session::ListingFailure::NotInstalled) {
            part("not installed");
            part(facts.remotePlatform);
        } else if (facts.failure == Session::ListingFailure::Refused) {
            // A refusal is the host's own answer, so it was reached: the line
            // says what it answered.
            switch (words.state) {
            case HostSectionState::OlderHost:
                part("runs an older host");
                break;
            case HostSectionState::OlderClient:
                part("runs a newer host");
                break;
            case HostSectionState::NeedsToken:
            case HostSectionState::TokenInvalid:
            case HostSectionState::TokenExpired:
            case HostSectionState::TokenRevoked:
            case HostSectionState::TokenScope:
                part("needs an access token");
                break;
            case HostSectionState::Sessions:
            case HostSectionState::Asking:
            case HostSectionState::Empty:
            case HostSectionState::Unreachable:
            case HostSectionState::SignInRequired:
            case HostSectionState::NotInstalled:
            case HostSectionState::Refused:
            case HostSectionState::Malformed:
            case HostSectionState::InvalidAddress:
            case HostSectionState::HostBinaryMissing:
            case HostSectionState::AuthenticationAttemptsExhausted:
            case HostSectionState::EndedBeforeAnswer:
                part("refused the connection");
                break;
            }
        } else if (facts.hasAnswered) {
            part("last answered " + ago(facts.secondsSinceAnswered));
        } else {
            part("not reached");
        }
        break;
    }
    return words;
}

// ---- The default button of a connect --------------------------------------------

/// What the default button of a connect does.
enum class ConnectActionEffect : std::uint8_t {
    /// Nothing: there is nothing to do yet, or what is selected cannot be
    /// opened.
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// Nothing: a connect is already waiting for the host.
    Waiting UBIQUITTY_SWIFT_NAME(waiting) = 1,
    /// Shows the window this app already has open on it.
    Reveal UBIQUITTY_SWIFT_NAME(reveal) = 2,
    /// Connects by `ConnectAction::plan`.
    Connect UBIQUITTY_SWIFT_NAME(connect) = 3,
};

/// The default button of a connect: its title, the sentence beside it and
/// what it does.
struct ConnectAction {
    std::string title;
    std::string sentence;
    ConnectActionEffect effect = ConnectActionEffect::None;
    /// How it connects, for `Connect`.
    Session::AttachPlan plan = Session::AttachPlan::AwaitListing;
    /// Whether the button can be pressed.
    bool isEnabled = false; // bool-ok: names the one condition it reports
};

/// What a Connect window holds when its default button is titled.
struct ConnectActionFacts {
    /// What the selected host is called; empty with no host selected.
    std::string hostName;
    /// Whether a connect is waiting for the host's answer.
    bool isConnectPending = false; // bool-ok: names the one state it reports
    /// Whether the connect rule has decided, and what.
    bool isDecided = false; // bool-ok: names the one condition it reports
    Session::AttachPlan plan = Session::AttachPlan::AwaitListing;
    std::uint32_t decidedSessionID = 0;
    /// The workspace the plan lands in; zero for none.
    std::uint32_t decidedWorkspaceID = 0;
    std::string decidedWorkspaceName;
    /// What the outline calls the workspace the plan lands in; empty when
    /// the outline has no row for it.
    std::string landingRowTitle;
    /// Whether this app already shows the plan's target in a window: its
    /// row opens by revealing, or the frontend's window registry matches
    /// the remembered target before a listing has supplied a row.
    bool isLandingShownHere = false; // bool-ok: names the one condition it reports
    /// The question the plan asks, for `ChooseFromList`; empty when none.
    std::string questionText;
    /// Whether a row is selected, and what opening it does.
    bool isRowSelected = false; // bool-ok: names the one condition it reports
    Session::AttachTarget selectedOpen;
    /// How many people are attached under the selected row.
    std::uint32_t selectedAttachedPeople = 0;
};

/// Titles the default button of a connect and says what it does.
///
/// The first of these that holds: no host or no decision, nothing; a
/// connect waiting, waiting; a selected row that opens no session, what
/// that row does; a workspace this app already shows, its window; else the
/// connect rule's plan.
///
/// - Parameter facts: What the window holds.
/// - Returns: The button.
[[nodiscard]] inline ConnectAction connectAction(const ConnectActionFacts& facts)
{
    using Session::AttachOpening;
    using Session::AttachPlan;
    const std::string& host = facts.hostName;
    const auto phrase = [](SessionListPhrase which) { return std::string(sessionListPhraseWords(which)); };
    const auto action = [](std::string title, std::string sentence, ConnectActionEffect effect, AttachPlan plan) {
        ConnectAction made;
        made.title = std::move(title);
        made.sentence = std::move(sentence);
        made.effect = effect;
        made.plan = plan;
        made.isEnabled = effect == ConnectActionEffect::Reveal || effect == ConnectActionEffect::Connect;
        return made;
    };
    if (host.empty() || !facts.isDecided)
        return action(phrase(SessionListPhrase::Connect), {}, ConnectActionEffect::None, AttachPlan::AwaitListing);
    if (facts.isConnectPending)
        return action("Waiting for " + host + "…", "Connects once " + host + " says which workspaces it holds. Press Esc to stop waiting.", ConnectActionEffect::Waiting, AttachPlan::AwaitListing);
    const auto reveal = [&](const std::string& name) {
        return action(phrase(SessionListPhrase::ShowWindow), "Brings forward the window showing " + (name.empty() ? std::string("the workspace") : SessionListWordsDetail::quoted(name)) + ".", ConnectActionEffect::Reveal, AttachPlan::AwaitListing);
    };
    if (facts.isRowSelected) {
        switch (facts.selectedOpen.opening) {
        case AttachOpening::ByRestoring:
            return action("Open " + SessionListWordsDetail::quoted(facts.selectedOpen.workspaceName), "Opens " + SessionListWordsDetail::quoted(facts.selectedOpen.workspaceName) + ".", ConnectActionEffect::Connect, AttachPlan::AttachNamed);
        case AttachOpening::Cannot:
            return action(phrase(SessionListPhrase::Open), std::string(Session::attachObstacleSentence(facts.selectedOpen.obstacle)), ConnectActionEffect::None, AttachPlan::AwaitListing);
        case AttachOpening::ByRevealing:
            return reveal(facts.selectedOpen.workspaceName);
        case AttachOpening::BySession:
            break;
        }
    }
    if (facts.isLandingShownHere && (facts.plan == AttachPlan::AttachByID || facts.plan == AttachPlan::AttachNamed))
        return reveal(facts.decidedWorkspaceName);
    switch (facts.plan) {
    case AttachPlan::AttachByID: {
        // The workspace is called what its row in the outline is called.
        const std::string& name = facts.decidedWorkspaceName.empty() ? facts.landingRowTitle : facts.decidedWorkspaceName;
        if (facts.isRowSelected && facts.selectedOpen.sessionID != 0 && facts.selectedOpen.sessionID == facts.decidedSessionID) {
            std::string sentence = name.empty() ? std::string("Opens this workspace.") : "Opens " + SessionListWordsDetail::quoted(name) + ".";
            if (facts.selectedAttachedPeople == 1)
                sentence += " 1 person is attached.";
            else if (facts.selectedAttachedPeople > 1)
                sentence += " " + std::to_string(facts.selectedAttachedPeople) + " people are attached.";
            return action(name.empty() ? std::string("Open Workspace") : "Open " + SessionListWordsDetail::quoted(name), std::move(sentence), ConnectActionEffect::Connect, AttachPlan::AttachByID);
        }
        return action(name.empty() ? std::string("Reopen Workspace") : "Reopen " + SessionListWordsDetail::quoted(name),
            name.empty() ? "Reopens the workspace you last used on " + host + "." : "Reopens " + SessionListWordsDetail::quoted(name) + ", the workspace you last used on " + host + ".", ConnectActionEffect::Connect, AttachPlan::AttachByID);
    }
    case AttachPlan::AttachNamed:
        return action("Open " + SessionListWordsDetail::quoted(facts.decidedWorkspaceName), "Opens " + SessionListWordsDetail::quoted(facts.decidedWorkspaceName) + ", the workspace " + host + " always opens.", ConnectActionEffect::Connect, AttachPlan::AttachNamed);
    case AttachPlan::CreateNamed:
        return action("Create " + SessionListWordsDetail::quoted(facts.decidedWorkspaceName), "Starts a workspace named " + SessionListWordsDetail::quoted(facts.decidedWorkspaceName) + " on " + host + ".", ConnectActionEffect::Connect, AttachPlan::CreateNamed);
    case AttachPlan::CreateNew:
        return action(phrase(SessionListPhrase::NewSession), "A workspace started here keeps running after you close its window.", ConnectActionEffect::Connect, AttachPlan::CreateNew);
    case AttachPlan::ChooseFromList: {
        // The sentence is the question, and the button puts it in front of
        // the person with what may be picked.
        ConnectAction asked = action(phrase(SessionListPhrase::Open), facts.questionText, ConnectActionEffect::Connect, AttachPlan::ChooseFromList);
        asked.isEnabled = !facts.questionText.empty();
        return asked;
    }
    case AttachPlan::AwaitListing:
        return action(phrase(SessionListPhrase::Connect), "Connects once " + host + " says which workspaces it holds.", ConnectActionEffect::Connect, AttachPlan::AwaitListing);
    }
    return action(phrase(SessionListPhrase::Connect), {}, ConnectActionEffect::None, AttachPlan::AwaitListing);
}

/// The marker a connect's plan puts beside a workspace's row.
enum class ConnectRowMarker : std::uint8_t {
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// The session this host was last used on, which the default button
    /// reopens.
    LastUsed UBIQUITTY_SWIFT_NAME(lastUsed) = 1,
    /// The session this host always opens, by its saved name.
    AlwaysOpens UBIQUITTY_SWIFT_NAME(alwaysOpens) = 2,
};

/// Decides the marker beside a row.
///
/// A selection replaces the remembered session as what the default button
/// opens, so "last used" is said only with no row selected.
///
/// - Parameters:
///   - facts: What the window holds.
///   - rowWorkspaceID: The workspace a workspace's row is; zero on a row of
///     another level.
/// - Returns: The marker; `sessionListPhraseWords` has its words under
///   `LastUsed` and `AlwaysOpens`.
[[nodiscard]] inline ConnectRowMarker connectRowMarker(const ConnectActionFacts& facts, std::uint32_t rowWorkspaceID)
{
    if (!facts.isDecided || facts.decidedWorkspaceID == 0 || rowWorkspaceID != facts.decidedWorkspaceID)
        return ConnectRowMarker::None;
    if (facts.plan == Session::AttachPlan::AttachByID && !facts.isRowSelected)
        return ConnectRowMarker::LastUsed;
    if (facts.plan == Session::AttachPlan::AttachNamed)
        return ConnectRowMarker::AlwaysOpens;
    return ConnectRowMarker::None;
}

} // namespace Ubiquitty::Runtime
