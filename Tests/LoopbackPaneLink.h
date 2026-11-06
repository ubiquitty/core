// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/LoopbackPaneLink.h — the shadow-equality rule, applied across the wire.
//
// A host Terminal is fed PTY bytes exactly as the daemon feeds a session: in
// drain passes, with the coherence rule (Runtime::FrameCoherence) asked after
// each pass and again whenever the daemon's poll timeout would have woken it.
// Every frame the rule admits is published to the pane's Sync source
// (Protocol::Sync::PaneSource) and carried by the host end of a Sync
// connection to a client end driving a passive client Terminal, as wire bytes
// both ways: the client's Greet, Ack and Resync reach the host as the host's
// frames reach the client. After every frame the client's screen and history
// are compared with the host's; the first disagreement names the frame, the
// row and the cell.
//
// What this mirrors from the daemon and what it does not:
//
// - Mirrored: pass grouping (a pass ends at a would-block or where
//   TerminalRuntime::drainPassReadsAgain says its budget is spent, never at a
//   short read, as Daemon::servicePTY reads), the rule and its wake cadence,
//   the attach checkpoint, and the connection's own choice of checkpoint or
//   update — not mirrored but shared, since both drive HostConnection.
// - Not mirrored: sockets, datagrams, pacing (the window is wide and the
//   client acknowledges at once), more than one client, the foreground probe,
//   and the title and bell frames that follow a screen frame. A client here
//   never falls behind, so a frame the oracle passes is a statement about the
//   encoding and the gate, not about pacing.
//
// Time is the replay's, not the machine's: the rule, the 2026 expiry and the
// cursor blink phase all read the replay clock, so a recorded stream produces
// the same frames on every run. The zero-timeout poll the rule asks for finds
// bytes exactly when the next recorded read is already readable.
//
// The loopback is also a `Session::PaneLink`, the same contract the in-process
// and remote links keep (Tests/Core/Session/PaneLinkConformance.h): input a
// frontend hands it goes up the client end as a remote link's does, its view
// lends the client's replica, and its observer is told of the frames the
// client applies. What the host's end writes to the pane's program waits in
// `programInput` for a test to answer as the program.

#include "Core/Attributes/Hyperlinks.h"
#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Modes/SyncOutput.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/PublishCensus.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/RemoteInput.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/PaneLink.h"
#include "Core/Session/PaneLinkEvents.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TextSizeTable.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace UbiquittyTest {

/// One read the daemon would make off a PTY.
struct PTYRead {
    /// When the bytes became readable, on the replay clock.
    std::uint64_t atMicroseconds = 0;
    /// The bytes one `read` returns.
    std::string_view bytes;
    /// Whether the PTY had nothing more to give after this read. False means
    /// the next read was already pending, so the daemon keeps draining.
    bool isFollowedByWouldBlock = true;
};

/// A link with a bandwidth and a delay, for measuring pacing rather than the
/// encoding: frames leave the host one at a time as the link drains, and
/// arrive one serialization time and one delay later.
struct LinkShape {
    /// The link's rate host to client; the client's frames are small enough
    /// that only the delay is charged for them.
    std::uint64_t bytesPerSecond = 0;
    std::uint64_t oneWayDelayMicroseconds = 0;
};

/// How the loopback client attached.
struct LoopbackOptions {
    int rows = 24;
    int columns = 80;
    /// A shaped link between the ends. Without one, every frame is carried
    /// whole and acknowledged before the next, and the client is compared with
    /// the host after every frame; with one, the client trails the host, and is
    /// compared once the link has drained.
    std::optional<LinkShape> link {};
    /// The host's constants on a shaped link; the instant link's own otherwise.
    std::optional<Ubiquitty::Protocol::Sync::HostPolicy> hostPolicy {};
    /// How long the client holds an acknowledgement on a shaped link.
    std::uint64_t ackDelayMicroseconds = 0;
    /// The refresh interval the client reports on a shaped link.
    std::uint32_t refreshIntervalMicroseconds = 0;
    /// A read before which the host's grid loses a row, as a client resizing
    /// its window would make it, so a checkpoint has to travel while the
    /// program keeps writing.
    std::optional<std::size_t> resizeBeforeRead {};
    /// The constants the coherence rule runs on.
    Ubiquitty::Runtime::CoherenceTuning coherence {};
    /// Whether frames wait for the coherence rule. Off, a frame is published
    /// after every drain pass, which is what lets a test see the shapes the
    /// rule exists to hold.
    bool isCoherenceGated = true;
};

/// What one publish sent.
struct PublishedFrame {
    std::uint64_t generation = 0;
    std::uint64_t atMicroseconds = 0;
    /// Byte offset in the replayed stream the host had parsed up to.
    std::size_t streamOffset = 0;
    /// Whether the frame went as a checkpoint rather than an update.
    bool isCheckpoint = false;
    /// The host's stream bytes the frame cost, envelopes included.
    std::size_t wireBytes = 0;
    /// The census's account: class, redraw bracket and hold time.
    Ubiquitty::Protocol::Sync::CensusVerdict census;
};

[[nodiscard]] inline std::string cellDifference(const Ubiquitty::Terminal& hostTerminal, const Ubiquitty::Cell& host, const Ubiquitty::Terminal& clientTerminal, const Ubiquitty::Cell& client)
{
    char message[256];
    if (std::memcmp(&host, &client, sizeof(Ubiquitty::Cell)) != 0) {
        const auto* a = reinterpret_cast<const unsigned char*>(&host); // Cell viewed as bytes, to name the first that differs
        const auto* b = reinterpret_cast<const unsigned char*>(&client); // Cell viewed as bytes, to name the first that differs
        std::size_t offset = 0;
        while (offset < sizeof(Ubiquitty::Cell) && a[offset] == b[offset])
            ++offset;
        std::snprintf(message, sizeof(message), "host U+%04X, client U+%04X (cell bytes differ from offset %zu)",
            static_cast<unsigned>(host.codePoint), static_cast<unsigned>(client.codePoint), offset);
        return message;
    }
    if (host.clusterID() != 0) {
        std::array<std::uint32_t, Ubiquitty::clusterPoolMaximumCodePoints> hostCluster {};
        std::array<std::uint32_t, Ubiquitty::clusterPoolMaximumCodePoints> clientCluster {};
        const int hostLength = Ubiquitty::cellClusterCodePoints(&hostTerminal, &host, hostCluster.data(), static_cast<int>(hostCluster.size()));
        const int clientLength = Ubiquitty::cellClusterCodePoints(&clientTerminal, &client, clientCluster.data(), static_cast<int>(clientCluster.size()));
        if (hostLength != clientLength || !std::equal(hostCluster.begin(), hostCluster.begin() + hostLength, clientCluster.begin())) {
            std::snprintf(message, sizeof(message), "cluster %u: host resolves %d code points, client %d, from U+%04X",
                static_cast<unsigned>(host.clusterID()), hostLength, clientLength, static_cast<unsigned>(host.codePoint));
            return message;
        }
    }
    if (host.linkID() != 0) {
        const char* hostURI = Ubiquitty::Attributes::Hyperlinks::uri(&hostTerminal, host.linkID());
        const char* clientURI = Ubiquitty::Attributes::Hyperlinks::uri(&clientTerminal, client.linkID());
        if (std::string_view(hostURI ? hostURI : "") != std::string_view(clientURI ? clientURI : "")) {
            std::snprintf(message, sizeof(message), "link %u: host URI \"%.80s\", client \"%.80s\"",
                static_cast<unsigned>(host.linkID()), hostURI ? hostURI : "", clientURI ? clientURI : "");
            return message;
        }
    }
    return {};
}

/// Compares the screen-wide state a frame carries beside its cells.
///
/// Each is something the client draws or acts on: reverse video, the cursor
/// shape, which screen is up, the input modes a key or a click is encoded
/// under, the palette and the OSC 66 spec table the cells' ids resolve
/// through. The title travels in its own frame, which the loopback does not
/// send, so it is not compared.
///
/// - Parameters:
///   - host: The host terminal.
///   - client: The client terminal.
/// - Returns: The first difference, named, or empty.
[[nodiscard]] inline std::string stateDifference(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client)
{
    char message[160];
    if (host.modes.reverseVideo != client.modes.reverseVideo) {
        std::snprintf(message, sizeof(message), "reverse video: host %d, client %d", host.modes.reverseVideo ? 1 : 0, client.modes.reverseVideo ? 1 : 0);
        return message;
    }
    if (host.cursorStyle != client.cursorStyle) {
        std::snprintf(message, sizeof(message), "cursor style: host %d, client %d", static_cast<int>(host.cursorStyle), static_cast<int>(client.cursorStyle));
        return message;
    }
    if (host.screens.isUsingAlternateScreen != client.screens.isUsingAlternateScreen) {
        std::snprintf(message, sizeof(message), "alternate screen: host %d, client %d", host.screens.isUsingAlternateScreen ? 1 : 0, client.screens.isUsingAlternateScreen ? 1 : 0);
        return message;
    }
    // Every rich text block, its placement and its document; the serial a
    // later placement takes is the host's alone.
    for (const Ubiquitty::ScreenMode screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        const auto hostBlocks = host.richTextBlocks.blocks(screen);
        const auto clientBlocks = client.richTextBlocks.blocks(screen);
        if (hostBlocks.size() != clientBlocks.size()) {
            std::snprintf(message, sizeof(message), "rich text blocks on screen %d: host %zu, client %zu", static_cast<int>(screen), hostBlocks.size(), clientBlocks.size());
            return message;
        }
        for (std::size_t index = 0; index < hostBlocks.size(); ++index) {
            if (!(hostBlocks[index] == clientBlocks[index])) {
                std::snprintf(message, sizeof(message), "rich text block %zu on screen %d (serial %u) differs", index, static_cast<int>(screen), hostBlocks[index].serial);
                return message;
            }
        }
    }
    struct InputMode {
        const char* name;
        bool Ubiquitty::Terminal::Modes::* mode;
    };
    static constexpr InputMode inputModes[] = {
        { "mouse button tracking", &Ubiquitty::Terminal::Modes::mouseButtonTracking },
        { "mouse cell motion", &Ubiquitty::Terminal::Modes::mouseCellMotion },
        { "mouse any event", &Ubiquitty::Terminal::Modes::mouseAnyEvent },
        { "mouse SGR", &Ubiquitty::Terminal::Modes::mouseSGR },
        { "mouse SGR pixels", &Ubiquitty::Terminal::Modes::mouseSGRPixels },
        { "mouse UTF8 coordinates", &Ubiquitty::Terminal::Modes::mouseUTF8Coordinates },
        { "mouse URXVT", &Ubiquitty::Terminal::Modes::mouseURXVT },
        { "mouse X10", &Ubiquitty::Terminal::Modes::mouseX10 },
        { "application cursor keys", &Ubiquitty::Terminal::Modes::appCursorKeys },
        { "application keypad", &Ubiquitty::Terminal::Modes::appKeypad },
        { "bracketed paste", &Ubiquitty::Terminal::Modes::bracketedPaste },
    };
    for (const InputMode& inputMode : inputModes) {
        if (host.modes.*inputMode.mode != client.modes.*inputMode.mode) {
            std::snprintf(message, sizeof(message), "%s: host %d, client %d", inputMode.name, host.modes.*inputMode.mode ? 1 : 0, client.modes.*inputMode.mode ? 1 : 0);
            return message;
        }
    }
    if (host.modes.alternateScrollMode.asRequested() != client.modes.alternateScrollMode.asRequested()) {
        std::snprintf(message, sizeof(message), "alternate scroll: host %d, client %d", host.modes.alternateScrollMode.asRequested() ? 1 : 0, client.modes.alternateScrollMode.asRequested() ? 1 : 0);
        return message;
    }
    if (!(host.palette == client.palette))
        return "palette: the client's differs from the host's";
    if (host.textSizeTable.liveMask() != client.textSizeTable.liveMask()) {
        std::snprintf(message, sizeof(message), "text size table: host live mask 0x%04X, client 0x%04X", host.textSizeTable.liveMask(), client.textSizeTable.liveMask());
        return message;
    }
    for (int slot = 0; slot < Ubiquitty::textSizeTableCapacity; ++slot) {
        if (((host.textSizeTable.liveMask() >> slot) & 1) == 0)
            continue;
        if (!(host.textSizeTable.specAtSlot(slot) == client.textSizeTable.specAtSlot(slot))) {
            std::snprintf(message, sizeof(message), "text size table slot %d: the client's spec differs from the host's", slot);
            return message;
        }
    }
    return {};
}

/// One member of `Terminal` a record frame carries into a replica, with how
/// the oracle compares it. A member the host keeps past the fact it names (an
/// indicator's colour once the indicator is cleared, a notification's words
/// once it is seen) is compared only while the fact holds.
struct RecordedMember {
    std::string_view name;
    bool (*isSame)(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client);
};

/// The user variables a terminal holds, in slot order, without the slots.
///
/// - Parameter terminal: The terminal.
/// - Returns: Each variable's name and value.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> userVariablesOf(const Ubiquitty::Terminal& terminal)
{
    std::vector<std::pair<std::string, std::string>> variables;
    for (const Ubiquitty::Terminal::UserVariable& variable : terminal.userVariables) {
        if (variable.used)
            variables.emplace_back(variable.name, variable.value);
    }
    return variables;
}

/// Every member the checkpoint ledger classes `Recorded`, each compared.
/// `test_sync_checkpoint_ledger` holds the two lists to each other.
inline constexpr std::array recordedMembers {
    RecordedMember { "progressState", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.progressState == client.progressState; } },
    RecordedMember { "progressValue", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.progressValue == client.progressValue; } },
    RecordedMember { "tabStatusSet", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return (host.tabStatusSet != 0) == (client.tabStatusSet != 0); } },
    RecordedMember { "tabStatusR", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.tabStatusSet == 0 || host.tabStatusR == client.tabStatusR; } },
    RecordedMember { "tabStatusG", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.tabStatusSet == 0 || host.tabStatusG == client.tabStatusG; } },
    RecordedMember { "tabStatusB", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.tabStatusSet == 0 || host.tabStatusB == client.tabStatusB; } },
    RecordedMember { "tabStatusText", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return Ubiquitty::tabStatusText(&host) == Ubiquitty::tabStatusText(&client); } },
    RecordedMember { "tabStatusTextLength", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return Ubiquitty::tabStatusText(&host).size() == Ubiquitty::tabStatusText(&client).size(); } },
    RecordedMember { "tabColorSet", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return (host.tabColorSet != 0) == (client.tabColorSet != 0); } },
    RecordedMember { "tabColorRed", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.tabColorSet == 0 || host.tabColorRed == client.tabColorRed; } },
    RecordedMember { "tabColorGreen", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.tabColorSet == 0 || host.tabColorGreen == client.tabColorGreen; } },
    RecordedMember { "tabColorBlue", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.tabColorSet == 0 || host.tabColorBlue == client.tabColorBlue; } },
    RecordedMember { "badgeText", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.badgeText == client.badgeText; } },
    RecordedMember { "hasUnseenBell", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.hasUnseenBell == client.hasUnseenBell; } },
    RecordedMember { "hasUnseenNotification", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.hasUnseenNotification == client.hasUnseenNotification; } },
    RecordedMember { "unseenNotificationTitle", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return !host.hasUnseenNotification || host.unseenNotificationTitle == client.unseenNotificationTitle; } },
    RecordedMember { "unseenNotificationBody", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return !host.hasUnseenNotification || host.unseenNotificationBody == client.unseenNotificationBody; } },
    RecordedMember { "userVariables", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return userVariablesOf(host) == userVariablesOf(client); } },
    RecordedMember { "currentFileURL", [](const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client) { return host.currentFileURL == client.currentFileURL; } },
};

/// Compares what a pane's records carry into a client's replica with the
/// host's terminal.
///
/// - Parameters:
///   - host: The terminal the program drives.
///   - client: The replica a pane's records were applied to.
/// - Returns: The first member that differs, named, or empty.
[[nodiscard]] inline std::string recordedDisagreement(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client)
{
    for (const RecordedMember& member : recordedMembers) {
        if (!member.isSame(host, client))
            return "recorded member " + std::string(member.name) + ": the client's differs from the host's";
    }
    return {};
}

/// Compares a client's screen with its host's.
///
/// Whole cells are compared — code point, pen, combining marks, link and
/// cluster ids — because a frame that reconstructs the text and drops the
/// colour is still wrong. A cell's link and cluster ids are compared through
/// what they resolve to on each side as well, since equal ids into a pool or
/// link table the client decoded wrongly still draw the wrong text or open
/// the wrong URI.
///
/// - Parameters:
///   - host: The terminal the program drives.
///   - client: The passive terminal a wire rebuilt from it.
/// - Returns: The first disagreement, naming the row and column, or empty.
[[nodiscard]] inline std::string firstDisagreement(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal& client)
{
    char message[256];
    if (host.rows != client.rows || host.columns != client.columns) {
        std::snprintf(message, sizeof(message), "geometry: host %dx%d, client %dx%d", host.rows, host.columns, client.rows, client.columns);
        return message;
    }
    for (int r = 0; r < host.rows; ++r) {
        for (int c = 0; c < host.columns; ++c) {
            const Ubiquitty::Cell* a = Ubiquitty::Screen::visibleCell(&host, 0, r, c);
            const Ubiquitty::Cell* b = Ubiquitty::Screen::visibleCell(&client, 0, r, c);
            if (!a || !b) {
                std::snprintf(message, sizeof(message), "grid row %d column %d: host %s, client %s", r, c, a ? "cell" : "null", b ? "cell" : "null");
                return message;
            }
            if (std::string difference = cellDifference(host, *a, client, *b); !difference.empty())
                return "grid row " + std::to_string(r) + " column " + std::to_string(c) + ": " + difference;
        }
        const std::uint8_t hostAttribute = host.screens.active->lineAttribute[r];
        const std::uint8_t clientAttribute = client.screens.active->lineAttribute[r];
        if (hostAttribute != clientAttribute) {
            std::snprintf(message, sizeof(message), "grid row %d line attribute: host %u, client %u", r, hostAttribute, clientAttribute);
            return message;
        }
        const bool isHostWrapped = host.screens.active->lineWrapped[r];
        const bool isClientWrapped = client.screens.active->lineWrapped[r];
        if (isHostWrapped != isClientWrapped) {
            std::snprintf(message, sizeof(message), "grid row %d soft wrap: host %d, client %d", r, isHostWrapped ? 1 : 0, isClientWrapped ? 1 : 0);
            return message;
        }
        const std::uint8_t hostMark = Ubiquitty::Screen::rowMark(&host, Ubiquitty::Screen::scrollbackCount(&host) + r);
        const std::uint8_t clientMark = Ubiquitty::Screen::rowMark(&client, Ubiquitty::Screen::scrollbackCount(&client) + r);
        if (hostMark != clientMark) {
            std::snprintf(message, sizeof(message), "grid row %d mark: host 0x%02X, client 0x%02X", r, hostMark, clientMark);
            return message;
        }
    }
    const Ubiquitty::Cursor::RenderState hostCursor = Ubiquitty::Cursor::resolve(&host, 0);
    const Ubiquitty::Cursor::RenderState clientCursor = Ubiquitty::Cursor::resolve(&client, 0);
    if (hostCursor.visible != clientCursor.visible || hostCursor.row != clientCursor.row || hostCursor.column != clientCursor.column) {
        std::snprintf(message, sizeof(message), "cursor: host %s at row %d column %d, client %s at row %d column %d",
            hostCursor.visible ? "shown" : "hidden", hostCursor.row, hostCursor.column,
            clientCursor.visible ? "shown" : "hidden", clientCursor.row, clientCursor.column);
        return message;
    }
    if (std::string difference = stateDifference(host, client); !difference.empty())
        return difference;

    return {};
}

/// Compares a client's screen with its host's, for a client that may hold no
/// pane yet.
///
/// A client the host never bound a pane for disagrees with every screen; it
/// is reported as such rather than dereferenced.
///
/// - Parameters:
///   - host: The terminal the program drives.
///   - client: The client's replica, or null when it holds none.
/// - Returns: The first disagreement, or empty.
[[nodiscard]] inline std::string firstDisagreement(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal* client)
{
    return client ? firstDisagreement(host, *client) : std::string("the client holds no pane");
}

/// Compares a client's history cache with its host's history (Docs/sync.md
/// §6.6): the history travels on its own plane, as logical lines the client
/// wraps at its own width, so lines are compared by number and `lineHash`,
/// not row by row.
///
/// Every line the cache holds that the host still holds must hash as the
/// host's line did; the cache's head must be the host's; every host line
/// from `depth` below the head, or the host's oldest when that is nearer, up
/// to the head must be held; and the cache's open line must be the host's,
/// the rows a line straddling the seam has left on it so far.
///
/// A line is what it was when it left the screen (Docs/sync.md §6.6): a link
/// the host's ring retires after that names nothing on the host, and the
/// client keeps the target the line was sent with. A caller that recorded each
/// line's hash as it left supplies `emittedHash`; without one the host's line
/// is hashed as it stands.
///
/// - Parameters:
///   - host: The terminal the program drives.
///   - cache: The client's cache of the pane's history.
///   - depth: How far below the head the client must hold every line.
///   - emittedHash: The hash a line had when it left the screen, if known.
/// - Returns: The first disagreement, naming the line, or empty.
[[nodiscard]] inline std::string historyDisagreement(const Ubiquitty::Terminal& host, const Ubiquitty::Protocol::Sync::HistoryCache& cache, std::uint64_t depth,
    const std::function<std::optional<std::uint64_t>(std::uint64_t)>& emittedHash = {})
{
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(host);
    if (cache.key().epoch != address.epoch)
        return "history epoch: host " + std::to_string(address.epoch) + ", client " + std::to_string(cache.key().epoch);
    if (cache.head() != address.head)
        return "history head: host " + std::to_string(address.head) + ", client " + std::to_string(cache.head());
    for (const Ubiquitty::Protocol::Sync::LineRange& range : cache.cachedRanges()) {
        for (std::uint64_t number = std::max(range.from, address.oldest); number < std::min(range.to, address.head); ++number) {
            const std::optional<std::uint64_t> recorded = emittedHash ? emittedHash(number) : std::nullopt;
            const std::optional<std::uint64_t> hostHash = recorded ? recorded : Ubiquitty::Protocol::Sync::logicalLineContentHash(host, number);
            if (hostHash && *hostHash != cache.line(number)->contentHash())
                return "history line " + std::to_string(number) + ": the client's hash differs from the host's";
        }
    }
    const std::uint64_t from = std::max(address.oldest, address.head > depth ? address.head - depth : 0);
    for (std::uint64_t number = from; number < address.head; ++number) {
        if (!cache.line(number) && Ubiquitty::Protocol::Sync::logicalLineHash(host, number))
            return "history line " + std::to_string(number) + ": the host holds it, the client does not";
    }
    const std::optional<Ubiquitty::Protocol::Sync::OpenLine> hostOpen = Ubiquitty::Protocol::Sync::openLineHash(host);
    const Ubiquitty::Protocol::Sync::DecodedLine* clientOpen = cache.openLine();
    if (hostOpen.has_value() != (clientOpen != nullptr))
        return std::string("open line ") + std::to_string(address.head) + ": the " + (hostOpen ? "host" : "client") + " holds one, the " + (hostOpen ? "client" : "host") + " does not";
    if (hostOpen && hostOpen->contentHash != clientOpen->contentHash())
        return "open line " + std::to_string(address.head) + ": the client's hash differs from the host's";
    return {};
}

/// Compares a client's scrollback with what its history cache projects: the
/// client wraps its history at its own width, so its scrollback must be the
/// cache's newest run wrapped there, row for row, however the projection was
/// built up.
///
/// - Parameters:
///   - client: The client's terminal for the pane.
///   - cache: The client's cache of the pane's history.
/// - Returns: The first disagreement, naming the row, or empty.
[[nodiscard]] inline std::string projectionDisagreement(const Ubiquitty::Terminal& client, const Ubiquitty::Protocol::Sync::HistoryCache& cache)
{
    auto reference = std::make_unique<Ubiquitty::Terminal>();
    reference->scrollbackLimit = client.scrollbackLimit;
    reference->drawsSizedText = client.drawsSizedText;
    reference->textSizeTable = client.textSizeTable;
    Ubiquitty::resize(*reference, client.rows, client.columns);
    Ubiquitty::Protocol::Sync::HistoryCache projected = cache;
    projected.invalidateProjection();
    projected.project(*reference, client.columns);
    const int expected = Ubiquitty::Screen::scrollbackCount(reference.get());
    const int held = Ubiquitty::Screen::scrollbackCount(&client);
    if (expected != held)
        return "history projection: the cache wraps to " + std::to_string(expected) + " rows, the client holds " + std::to_string(held);
    for (int index = 0; index < held; ++index) {
        const Ubiquitty::Screen::ScrollLine* a = Ubiquitty::Screen::scrollbackLine(reference.get(), index);
        const Ubiquitty::Screen::ScrollLine* b = Ubiquitty::Screen::scrollbackLine(&client, index);
        if (!a || !b || a->length != b->length || a->wrapped != b->wrapped || a->lineAttribute != b->lineAttribute || a->mark != b->mark)
            return "history projection row " + std::to_string(index) + ": the client's row is not the cache's";
        // Text, marks and pens; a link or cluster id resolves through the
        // client's mirrored tables, which the reference does not hold.
        for (int column = 0; column < a->length; ++column) {
            const Ubiquitty::Cell& expected = a->cells[column];
            const Ubiquitty::Cell& found = b->cells[column];
            if (expected.codePoint != found.codePoint || expected.combiningLength() != found.combiningLength()
                || std::memcmp(&expected.pen, &found.pen, sizeof(Ubiquitty::Pen)) != 0)
                return "history projection row " + std::to_string(index) + " column " + std::to_string(column) + ": the client's cell is not the cache's";
        }
    }
    return {};
}

/// The input the loopback's host end writes to its pane's program, kept for a
/// test to answer as the program would.
class LoopbackProgramInput final : public Ubiquitty::Protocol::Sync::InputWriter {
public:
    /// Takes one event as written: raw input is kept as its bytes, and an
    /// event the host encodes under the program's modes is taken and kept as
    /// none.
    ///
    /// - Parameter intent: The event.
    /// - Returns: `Written`.
    [[nodiscard]] Ubiquitty::Protocol::Sync::WriteStatus write(Ubiquitty::Protocol::Sync::ID128, const Ubiquitty::Protocol::Sync::InputIntent& intent) override
    {
        if (const auto* raw = std::get_if<Ubiquitty::Protocol::Sync::RawInput>(&intent))
            m_bytes.append(raw->bytes.begin(), raw->bytes.end());
        return Ubiquitty::Protocol::Sync::WriteStatus::Written;
    }

    /// Every byte written, in order.
    [[nodiscard]] const std::string& bytes() const { return m_bytes; }

private:
    std::string m_bytes;
};

/// The daemon's publish path for one pane and one attached client, in memory.
class LoopbackPaneLink final : public Ubiquitty::Session::PaneLink {
public:
    explicit LoopbackPaneLink(const LoopbackOptions& options = {})
        : m_options(options)
        , m_host(std::make_unique<Ubiquitty::Terminal>())
        , m_source(std::make_unique<Ubiquitty::Protocol::Sync::PaneSource>(*m_host, Ubiquitty::Protocol::Sync::ID128 { 1, 1 }))
        , m_hostEnd(options.hostPolicy.value_or(hostPolicy()), Ubiquitty::Protocol::Sync::HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .principal = "loopback", .mayDrive = true, .verifiedKind = Ubiquitty::Protocol::Sync::ClientKind::Human, .selection = { .target = Ubiquitty::Protocol::Sync::AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "loopback.sock", .buildFlags = 0, .hostPlatform = "test" } })
        , m_clientEnd(clientPolicy(options), clientIdentity(options))
    {
        m_source->setInputWriter(m_programInput);
        Ubiquitty::resize(*m_host, options.rows, options.columns);
        attach();
    }

    LoopbackPaneLink(const LoopbackPaneLink&) = delete;
    LoopbackPaneLink& operator=(const LoopbackPaneLink&) = delete;

    /// Runs after the client applies each frame and before the comparison. A
    /// test corrupts the client here to prove the oracle sees it.
    std::function<void(Ubiquitty::Terminal& client)> afterClientApply;

    /// Runs after every published frame has been compared.
    std::function<void(const PublishedFrame& frame, const LoopbackPaneLink& link)> onFrame;

    /// Replays recorded reads, asks the rule as the daemon would, and drains
    /// whatever is still due once the stream ends.
    ///
    /// - Parameter reads: The reads, in order, on the replay clock.
    void replay(std::span<const PTYRead> reads)
    {
        std::size_t index = 0;
        while (index < reads.size()) {
            // One drain pass: reads keep coming while the PTY stays readable,
            // until a would-block or the pass capacity ends it.
            const std::uint64_t passStart = reads[index].atMicroseconds;
            m_nextReadMicroseconds = passStart;
            serviceTimersUntil(passStart);
            std::size_t passBytes = 0;
            bool isSaturated = false;
            std::uint64_t passEnd = passStart;
            for (;;) {
                const PTYRead& read = reads[index];
                passEnd = read.atMicroseconds > passEnd ? read.atMicroseconds : passEnd;
                if (m_options.resizeBeforeRead == index) {
                    Ubiquitty::resize(*m_host, m_host->rows - 1, m_host->columns);
                    m_source->noteTerminalChanged();
                    m_isDue = true;
                }
                feedHost(read.bytes);
                passBytes += read.bytes.size();
                ++index;
                if (read.isFollowedByWouldBlock || index >= reads.size())
                    break;
                if (!Ubiquitty::Runtime::TerminalRuntime::drainPassReadsAgain(static_cast<long long>(passBytes))) {
                    isSaturated = true;
                    break;
                }
            }
            m_lastPassWasSaturated = isSaturated;
            advanceLink(passEnd);
            m_nowMicroseconds = passEnd;
            m_nextReadMicroseconds = index < reads.size() ? reads[index].atMicroseconds : noNextRead;
            m_coherence.noteDrainPass(*m_host, hostMicroseconds(),
                isSaturated ? Ubiquitty::Runtime::PassEnd::Capacity : Ubiquitty::Runtime::PassEnd::WouldBlock);
            m_census.noteDirty(m_nowMicroseconds);
            if (!m_options.isCoherenceGated) {
                publish(Ubiquitty::Runtime::FrameClass::Settled);
                continue;
            }
            askToPublish();
        }
        m_nextReadMicroseconds = noNextRead;
        finish();
    }

    /// Asks the rule until nothing is due, advancing the replay clock by the
    /// daemon's own wake interval.
    void finish()
    {
        // A 2026 block that never closes expires on its own budget and anything
        // else on the deadline, so this terminates; the bound only stops a rule
        // defect from hanging the suite.
        for (int wake = 0; wake < 100000 && m_isDue; ++wake) {
            const std::uint64_t wakeAt = m_nowMicroseconds + nextWakeDelayMicroseconds();
            advanceLink(wakeAt);
            m_nowMicroseconds = wakeAt;
            askToPublish();
        }
        drainLink();
    }

    /// Feeds the host bytes as one read and marks the pane due.
    ///
    /// - Parameter bytes: One read's worth; a sequence may span reads.
    void feedHost(std::string_view bytes)
    {
        Ubiquitty::processBytes(m_host.get(), bytes);
        m_source->noteTerminalChanged();
        m_streamOffset += bytes.size();
        // Replies go back to the program in the daemon; here nothing reads them,
        // and a full reply buffer would only drop later ones.
        std::array<char, 4096> discard;
        while (Ubiquitty::drainOutput(m_host.get(), { discard.data(), discard.size() }) > 0) {
        }
        if (!bytes.empty())
            m_isDue = true;
    }

    /// Asks the coherence rule at the current replay time and publishes when
    /// it admits.
    ///
    /// - Returns: Whether a frame was published.
    bool askToPublish()
    {
        if (!m_isDue)
            return false;
        const Ubiquitty::Runtime::ReadReadiness readiness = m_nextReadMicroseconds <= m_nowMicroseconds
            ? Ubiquitty::Runtime::ReadReadiness::Pending
            : Ubiquitty::Runtime::ReadReadiness::Quiet;
        const Ubiquitty::Runtime::FrameAdmission admission = m_coherence.admit(*m_host, hostMicroseconds(), Ubiquitty::Ticks(ticks()), readiness);
        if (!admission.isAdmitted)
            return false;
        publish(admission.frameClass);
        return true;
    }

    /// Publishes one frame to the client, whatever the rule would say.
    ///
    /// - Parameter frameClass: The class the frame is counted in.
    void publish(Ubiquitty::Runtime::FrameClass frameClass)
    {
        const Ubiquitty::Protocol::Sync::CensusVerdict verdict = m_census.notePublish(*m_host, m_nowMicroseconds, frameClass);
        const std::uint64_t changedAt = m_coherence.dirtySinceMicroseconds().value_or(hostMicroseconds());
        m_coherence.noteFrameTaken(*m_host, hostMicroseconds());
        setRecords();
        m_source->publish(Ubiquitty::Runtime::wireFrameClass(frameClass), changedAt);
        recordEmittedLines();
        const std::uint64_t checkpointsBefore = m_hostEnd.counters().checkpointsSent;
        PublishedFrame frame;
        frame.generation = m_source->generation();
        frame.atMicroseconds = m_nowMicroseconds;
        frame.streamOffset = m_streamOffset;
        frame.census = verdict;
        if (m_options.link) {
            m_publishedAt.push_back(m_nowMicroseconds);
            m_reachedAt.push_back(0);
            m_roundTripAtPublish.push_back(m_hostEnd.pacer().smoothedRoundTripMicroseconds().value_or(0));
            const std::uint64_t bytesBefore = m_linkBytes;
            serviceLink(m_nowMicroseconds);
            frame.wireBytes = static_cast<std::size_t>(m_linkBytes - bytesBefore);
            frame.isCheckpoint = m_hostEnd.counters().checkpointsSent != checkpointsBefore;
            m_isDue = false;
            m_frames.push_back(frame);
            if (onFrame)
                onFrame(frame, *this);
            return;
        }
        frame.wireBytes = pump();
        frame.isCheckpoint = m_hostEnd.counters().checkpointsSent != checkpointsBefore;
        m_isDue = false;

        Ubiquitty::Terminal* client = m_clientEnd.paneTerminal(m_handle);
        if (afterClientApply && client)
            afterClientApply(*client);
        if (m_failure.empty()) {
            std::string disagreement = firstDisagreement();
            if (disagreement.empty() && m_clientEnd.paneGeneration(m_handle) != m_source->generation())
                disagreement = "the client holds generation " + std::to_string(m_clientEnd.paneGeneration(m_handle)) + ", not the one published";
            if (disagreement.empty() && m_clientEnd.counters().resyncsSent > 0)
                disagreement = "the client asked for a resync: a frame it was sent did not apply";
            if (!disagreement.empty()) {
                char prefix[160];
                std::snprintf(prefix, sizeof(prefix), "frame %zu (generation %llu, %s, stream offset %zu): ",
                    m_frames.size(), static_cast<unsigned long long>(frame.generation), frame.isCheckpoint ? "checkpoint" : "update", m_streamOffset);
                m_failure = prefix + disagreement;
            }
        }
        m_frames.push_back(frame);
        if (onFrame)
            onFrame(frame, *this);
    }

    /// The first frame after which the client disagreed with the host, or empty.
    [[nodiscard]] const std::string& failure() const { return m_failure; }

    /// Every frame published, in order.
    [[nodiscard]] const std::vector<PublishedFrame>& frames() const { return m_frames; }

    /// The census of every frame published, by class and hold time.
    [[nodiscard]] const Ubiquitty::Protocol::Sync::PublishCensus& census() const { return m_census; }

    [[nodiscard]] Ubiquitty::Terminal& host() { return *m_host; }
    [[nodiscard]] const Ubiquitty::Terminal& host() const { return *m_host; }

    /// The client's replica of the pane.
    [[nodiscard]] const Ubiquitty::Terminal& client() const { return *m_clientEnd.paneTerminal(m_handle); }

    /// The pane's handle on the connection.
    [[nodiscard]] std::uint32_t handle() const { return m_handle; }

    /// The host's end of the connection, for its counters.
    [[nodiscard]] const Ubiquitty::Protocol::Sync::HostConnection& hostEnd() const { return m_hostEnd; }

    /// The replay clock.
    [[nodiscard]] std::uint64_t nowMicroseconds() const { return m_nowMicroseconds; }

    /// On a shaped link, how long each published frame waited before the
    /// client held it or a newer one, in publish order; a frame the client
    /// never reached is absent.
    [[nodiscard]] std::vector<std::uint64_t> stalenessMicroseconds() const
    {
        std::vector<std::uint64_t> waits;
        for (std::size_t index = 0; index < m_publishedAt.size(); ++index) {
            if (m_reachedAt[index] != 0)
                waits.push_back(m_reachedAt[index] - m_publishedAt[index]);
        }
        return waits;
    }

    /// On a shaped link, each frame that reached the client while the host
    /// held a smoothed round trip: how long it waited, and that round trip.
    /// The ratio is what a stall bound of a multiple of the round trip must
    /// clear for an answer that travels with the frames.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> stalenessAgainstRoundTrip() const
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        for (std::size_t index = 0; index < m_publishedAt.size(); ++index) {
            if (m_reachedAt[index] != 0 && m_roundTripAtPublish[index] != 0)
                pairs.emplace_back(m_reachedAt[index] - m_publishedAt[index], m_roundTripAtPublish[index]);
        }
        return pairs;
    }

    /// On a shaped link, the bytes the host sent the client.
    [[nodiscard]] std::uint64_t linkBytes() const { return m_linkBytes; }

    /// The client end of the connection, for its counters.
    [[nodiscard]] const Ubiquitty::Protocol::Sync::ClientConnection& clientEnd() const { return m_clientEnd; }

    /// Publishes the client's typography table and carries it to the host,
    /// as a frontend drawing the pane would.
    ///
    /// - Parameter facts: The table.
    void publishClientTypography(const Ubiquitty::RichText::TypographyFacts& facts)
    {
        m_clientEnd.publishTypography(m_handle, facts);
        (void)pump();
    }

    /// Whether the last drain pass ended at the pass capacity rather than at a
    /// would-block.
    [[nodiscard]] bool lastPassWasSaturated() const { return m_lastPassWasSaturated; }

    /// Every byte the host's end wrote to the pane's program, in order.
    [[nodiscard]] const std::string& programInput() const { return m_programInput.bytes(); }

    /// Hands input up the client end, as a remote link does, and carries it
    /// to the host's end.
    ///
    /// - Parameter input: Borrowed semantic input; raw input sends its local
    ///   encoding, in events of at most 16 KiB.
    /// - Returns: Whether the client end took every event; false for raw
    ///   input with no bytes, which is no event.
    bool send(const Ubiquitty::Session::PaneInput& input) override
    {
        using Ubiquitty::Session::PaneInput;
        if (std::holds_alternative<PaneInput::Raw>(input.intent) && input.localEncoding.empty())
            return false;
        bool isTaken = true;
        if (const auto* key = std::get_if<PaneInput::Key>(&input.intent)) {
            const std::array<Ubiquitty::Protocol::Sync::KeyEvent, 1> events { Ubiquitty::Runtime::keyEventOf(key->press) };
            isTaken = m_clientEnd.sendKeys(m_handle, events).has_value();
        } else if (const auto* paste = std::get_if<PaneInput::Paste>(&input.intent)) {
            isTaken = m_clientEnd.sendPaste(m_handle, paste->text).has_value();
        } else if (const auto* pointer = std::get_if<PaneInput::Pointer>(&input.intent)) {
            const std::array<Ubiquitty::Protocol::Sync::PointerEvent, 1> events { pointer->event };
            isTaken = m_clientEnd.sendPointer(m_handle, events).has_value();
        } else {
            constexpr std::size_t maxRawEventBytes = 16 * 1024;
            for (std::span<const char> bytes = input.localEncoding; isTaken && !bytes.empty();) {
                const std::span<const char> piece = bytes.first(std::min(bytes.size(), maxRawEventBytes));
                isTaken = m_clientEnd.sendRaw(m_handle, { reinterpret_cast<const std::uint8_t*>(piece.data()), piece.size() }).has_value();
                bytes = bytes.subspan(piece.size());
            }
        }
        (void)pump();
        return isTaken;
    }

    /// Lends the client's replica of the pane, its history and its facts.
    ///
    /// - Returns: The view; its pointers expire at the next carry.
    [[nodiscard]] Ubiquitty::Session::PaneView view() override
    {
        return { .terminal = m_clientEnd.paneTerminal(m_handle), .history = m_clientEnd.historyCache(m_handle), .facts = m_clientEnd.paneFacts(m_handle) };
    }

    /// Tells the host how the client holds the pane, and carries it.
    ///
    /// - Parameter state: The view.
    void setView(Ubiquitty::Protocol::Sync::ViewState state) override
    {
        m_view = state;
        m_clientEnd.setView(m_handle, state);
        (void)pump();
    }

    /// Tells the host whether the client's view holds the keyboard.
    ///
    /// - Parameter focus: The focus.
    void setFocus(Ubiquitty::Modes::FocusState focus) override
    {
        (void)m_clientEnd.sendFocus(m_handle, focus);
        (void)pump();
    }

    /// Tells the host the grid the client shows the pane at; the host's
    /// terminal keeps its size, as no daemon applies it here.
    ///
    /// - Parameter viewport: The grid and the drawable's pixels.
    void setViewport(const Ubiquitty::Session::PaneViewport& viewport) override
    {
        const auto extent = [](int value) { return static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFF)); };
        (void)m_clientEnd.sendViewport(m_handle, { .rows = extent(viewport.rows), .columns = extent(viewport.columns), .cellPixelWidth = 0, .cellPixelHeight = 0, .textAreaPixelWidth = 0, .textAreaPixelHeight = 0, .drawablePixelWidth = extent(viewport.widthInPixels), .drawablePixelHeight = extent(viewport.heightInPixels) });
        (void)pump();
    }

    /// Keeps no pixel metrics: the loopback's client measures no cells.
    void setPixelGeometry(const Ubiquitty::PixelGeometry&) override { }

    /// Keeps no window size: the loopback's client has no window.
    void setWindowSize(int, int, int, int) override { }

    /// Asks the host for history lines, and carries the want.
    ///
    /// - Parameters:
    ///   - priority: The want's priority.
    ///   - ranges: The lines, sorted and disjoint.
    /// - Returns: Whether the client end admitted the want.
    [[nodiscard]] bool wantHistory(Ubiquitty::Protocol::Sync::HistoryPriority priority, std::span<const Ubiquitty::Protocol::Sync::LineRange> ranges) override
    {
        const bool isAdmitted = m_clientEnd.wantHistory(m_handle, priority, ranges);
        if (isAdmitted)
            (void)pump();
        return isAdmitted;
    }

    /// Asks for nothing: the loopback's client is backfilled to the whole
    /// history already.
    void setViewOffset(int) override { }

    /// The client end's phase: Live while attached, Ended once closed.
    ///
    /// - Returns: The state; the loopback never stalls.
    [[nodiscard]] Ubiquitty::Session::LinkState linkState(std::uint64_t) const override
    {
        Ubiquitty::Session::LinkState state;
        if (m_clientEnd.state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached)
            state.phase = Ubiquitty::Session::LinkPhase::Live;
        else if (m_clientEnd.state() == Ubiquitty::Protocol::Sync::ConnectionState::Closed)
            state.phase = Ubiquitty::Session::LinkPhase::Ended;
        return state;
    }

    /// The host's monotonic clock, on the replay's time.
    ///
    /// - Returns: Microseconds.
    [[nodiscard]] std::uint64_t clockMicroseconds() const override { return hostMicroseconds(); }

    /// Lends the link an observer, which is told the state again.
    ///
    /// - Parameter observer: The observer; it outlives the link or is replaced.
    void observe(Ubiquitty::Session::PaneLinkObserver& observer) override
    {
        if (m_events.observe(observer))
            m_notifiedFactsRevision.reset();
    }

    /// Tells the observer the link's state, the newest frame the client
    /// applied while the view is Live, and a change of the facts while the
    /// view is not Dormant.
    ///
    /// - Parameter now: The clock.
    void pollEvents(std::uint64_t now) override
    {
        if (!m_events.hasObserver())
            return;
        m_events.noteState(linkState(now));
        if (m_view != Ubiquitty::Protocol::Sync::ViewState::Dormant) {
            const std::optional<Ubiquitty::Protocol::Sync::FrameClass> frameClass = m_clientEnd.paneFrameClass(m_handle);
            if (frameClass && m_view == Ubiquitty::Protocol::Sync::ViewState::Live)
                m_events.noteFrame({ .frameClass = *frameClass, .generation = m_clientEnd.paneGeneration(m_handle), .lagMicroseconds = m_clientEnd.paneLagMicroseconds(m_handle) });
            const Ubiquitty::Protocol::Sync::PaneFacts* facts = m_clientEnd.paneFacts(m_handle);
            if (facts && m_notifiedFactsRevision != facts->revision) {
                m_notifiedFactsRevision = facts->revision;
                m_events.noteFacts();
            }
        }
        m_events.deliver();
    }

    /// Compares the client's screen and history with the host's; see
    /// `UbiquittyTest::firstDisagreement`.
    ///
    /// - Returns: The first disagreement, naming the row and column, or empty.
    [[nodiscard]] std::string firstDisagreement() const
    {
        const Ubiquitty::Terminal* client = m_clientEnd.paneTerminal(m_handle);
        const Ubiquitty::Protocol::Sync::HistoryCache* cache = m_clientEnd.historyCache(m_handle);
        if (!client || !cache)
            return "the client has no pane bound";
        if (std::string screen = UbiquittyTest::firstDisagreement(*m_host, *client); !screen.empty())
            return screen;
        if (std::string recorded = UbiquittyTest::recordedDisagreement(*m_host, *client); !recorded.empty())
            return recorded;
        if (std::string projection = UbiquittyTest::projectionDisagreement(*client, *cache); !projection.empty())
            return projection;
        // The backfill runs to the whole history: the oracle compares every
        // line the host holds, as it compared every row before history had a
        // plane of its own.
        return UbiquittyTest::historyDisagreement(*m_host, *cache, std::numeric_limits<std::uint64_t>::max(), [&](std::uint64_t number) -> std::optional<std::uint64_t> {
            const auto found = m_emittedHashes.find(number);
            return found == m_emittedHashes.end() ? std::nullopt : std::optional<std::uint64_t>(found->second);
        });
    }

private:
    // The daemon's monotonic clock reads far from zero; the replay clock starts
    // there too, so nothing measured from zero is mistaken for a real instant.
    static constexpr std::uint64_t clockOriginMilliseconds = 1'000'000;
    static constexpr std::uint64_t noNextRead = UINT64_MAX;
    /// Each history line's hash as it left the host's screen, in the current
    /// epoch.
    std::unordered_map<std::uint64_t, std::uint64_t> m_emittedHashes;
    std::uint32_t m_emittedEpoch = 0;
    std::uint64_t m_emittedHead = 0;
    // Pacing is not under test here: a window that admits any frame the
    // corpus can produce, and an acknowledgement sent the moment it is owed.
    static constexpr std::size_t maxCheckpointBytes = std::size_t { 64 } << 20;

    /// The host's constants: a window no corpus frame fills, and no probe.
    ///
    /// - Returns: The policy.
    static Ubiquitty::Protocol::Sync::HostPolicy hostPolicy()
    {
        return {
            .pacing = { .windowFloorBytes = maxCheckpointBytes, .probeIdleMicroseconds = UINT64_MAX / 2, .roundTripStaleMicroseconds = UINT64_MAX / 2, .rateFilterMicroseconds = 1'000'000, .datagramLossFloorMicroseconds = 1'000'000 },
            .lanes = { .bulkSharePermille = 500 },
            .datagramMaxBytes = 0,
            .maxCheckpointBytes = maxCheckpointBytes,
            .limits = Ubiquitty::Protocol::Sync::provisionalLimits,
            .history = {
                .backfillChunkLines = 256,
                .maxHistoryTransferBytes = Ubiquitty::Protocol::Sync::maxHistoryTransferBytes,
                .maxQueuedBulkBytes = maxCheckpointBytes,
            },
        };
    }

    /// The client's constants: an acknowledgement the moment one is owed,
    /// unless a shaped link asks for a delay.
    ///
    /// - Parameter options: The link's options.
    /// - Returns: The policy.
    static Ubiquitty::Protocol::Sync::ClientPolicy clientPolicy(const LoopbackOptions& options)
    {
        // A cache deeper than any corpus history, so none is evicted, and
        // every line backfilled, so the oracle can compare the whole history
        // after every frame.
        return { .maxCheckpointBytes = maxCheckpointBytes, .maxBufferedUpdateBytes = maxCheckpointBytes, .minAckIntervalMicroseconds = options.ackDelayMicroseconds, .historyCacheLines = 100'000, .backfillDepthLines = 100'000 };
    }

    /// What the client tells the host: the pane's geometry and its refresh cap.
    ///
    /// - Parameter options: The link's options.
    /// - Returns: The identity.
    static Ubiquitty::Protocol::Sync::ClientIdentity clientIdentity(const LoopbackOptions& options)
    {
        Ubiquitty::Protocol::Sync::ClientIdentity identity;
        identity.clientInstance = { 7, 8 };
        identity.facts.viewportRows = static_cast<std::uint16_t>(options.rows);
        identity.facts.viewportColumns = static_cast<std::uint16_t>(options.columns);
        // No rate cap unless a shaped link asks for one: every frame the rule
        // takes goes out.
        identity.facts.refreshIntervalMicroseconds = options.refreshIntervalMicroseconds;
        return identity;
    }

    [[nodiscard]] std::uint64_t nowMilliseconds() const { return clockOriginMilliseconds + m_nowMicroseconds / 1000; }

    // The host's monotonic microseconds, as the daemon hands them to the rule.
    [[nodiscard]] std::uint64_t hostMicroseconds() const { return clockOriginMilliseconds * 1000 + m_nowMicroseconds; }

    // The daemon's 60 Hz tick: milliseconds * 60 / 1000.
    [[nodiscard]] std::uint32_t ticks() const { return static_cast<std::uint32_t>(nowMilliseconds() * 60 / 1000); }

    [[nodiscard]] std::uint64_t nextWakeDelayMicroseconds() const
    {
        const std::optional<std::uint64_t> microseconds = m_coherence.microsecondsUntilAsk(*m_host, hostMicroseconds(), Ubiquitty::Ticks(ticks()));
        // The daemon's poll takes whole milliseconds and rounds up; a zero
        // answer means "ask now", and the next turn asks with the clock moved
        // on by at least one millisecond.
        const std::uint64_t milliseconds = microseconds ? (*microseconds + 999) / 1000 : 1;
        return (milliseconds == 0 ? 1 : milliseconds) * 1000;
    }

    /// Runs the poll timeouts the daemon would have slept on before the next
    /// read, asking the rule at each.
    ///
    /// - Parameter untilMicroseconds: When the next read arrives.
    void serviceTimersUntil(std::uint64_t untilMicroseconds)
    {
        while (m_isDue) {
            const std::uint64_t wake = m_nowMicroseconds + nextWakeDelayMicroseconds();
            if (wake >= untilMicroseconds)
                break;
            advanceLink(wake);
            m_nowMicroseconds = wake;
            askToPublish();
        }
        advanceLink(untilMicroseconds);
        if (untilMicroseconds > m_nowMicroseconds)
            m_nowMicroseconds = untilMicroseconds;
    }

    /// Moves bytes both ways until neither end has more to say.
    ///
    /// - Returns: The host's stream bytes moved.
    std::size_t pump()
    {
        std::size_t hostBytes = 0;
        for (;;) {
            const std::uint64_t now = hostMicroseconds();
            const std::vector<std::uint8_t> upstream = m_clientEnd.takeStreamOutput();
            if (!upstream.empty())
                m_hostEnd.receiveStream(upstream, now);
            m_hostEnd.service(now);
            std::vector<std::uint8_t> downstream;
            while (m_hostEnd.popStreamFrame(downstream, now)) {
            }
            if (!downstream.empty())
                m_clientEnd.receiveStream(downstream, now);
            m_clientEnd.tick(now);
            hostBytes += downstream.size();
            if (upstream.empty() && downstream.empty())
                return hostBytes;
        }
    }

    /// One transmission in flight on a shaped link.
    struct InFlight {
        std::uint64_t arrivesAt = 0;
        std::vector<std::uint8_t> bytes;
    };

    /// On a shaped link, services the host at a replay time and starts the next
    /// frame onto the wire when the wire is idle.
    ///
    /// - Parameter at: The replay time.
    void serviceLink(std::uint64_t at)
    {
        const std::uint64_t host = clockOriginMilliseconds * 1000 + at;
        m_hostEnd.service(host);
        if (m_wireFreeAt > at)
            return;
        std::vector<std::uint8_t> frame;
        if (!m_hostEnd.popStreamFrame(frame, host))
            return;
        const LinkShape& shape = *m_options.link;
        const std::uint64_t serialization = frame.size() * 1'000'000 / shape.bytesPerSecond;
        m_wireFreeAt = at + serialization;
        m_linkBytes += frame.size();
        m_downstream.push_back({ .arrivesAt = m_wireFreeAt + shape.oneWayDelayMicroseconds, .bytes = std::move(frame) });
    }

    /// The soonest shaped-link event after `after`, or `noNextRead` when none.
    [[nodiscard]] std::uint64_t nextLinkEvent(std::uint64_t after) const
    {
        std::uint64_t next = noNextRead;
        if (!m_downstream.empty())
            next = std::min(next, m_downstream.front().arrivesAt);
        if (!m_upstream.empty())
            next = std::min(next, m_upstream.front().arrivesAt);
        if (!m_ackTicks.empty())
            next = std::min(next, m_ackTicks.front());
        // A busy wire frees for the next frame; an idle wire with frames the
        // window holds back waits for the acknowledgement that opens it.
        if (m_hostEnd.hasQueuedStreamFrames() && m_wireFreeAt > after)
            next = std::min(next, m_wireFreeAt);
        if (const std::optional<std::uint64_t> due = m_hostEnd.nextServiceMicroseconds(clockOriginMilliseconds * 1000 + after)) {
            const std::uint64_t replay = *due > clockOriginMilliseconds * 1000 ? *due - clockOriginMilliseconds * 1000 : 0;
            next = std::min(next, std::max(replay, after + 1));
        }
        return next;
    }

    /// Runs every shaped-link event up to a replay time, in order.
    ///
    /// - Parameter until: The replay time to stop at.
    void advanceLink(std::uint64_t until)
    {
        if (!m_options.link)
            return;
        std::uint64_t at = m_linkClock;
        for (int events = 0; events < 10'000'000; ++events) {
            const std::uint64_t next = nextLinkEvent(at);
            if (next > until)
                break;
            at = next;
            runLinkEvent(at);
        }
        m_linkClock = std::max(m_linkClock, until);
    }

    /// Runs everything due at one replay time on a shaped link.
    ///
    /// - Parameter at: The replay time.
    void runLinkEvent(std::uint64_t at)
    {
        const LinkShape& shape = *m_options.link;
        const std::uint64_t host = clockOriginMilliseconds * 1000 + at;
        while (!m_downstream.empty() && m_downstream.front().arrivesAt <= at) {
            m_clientEnd.receiveStream(m_downstream.front().bytes, host);
            m_downstream.pop_front();
            m_ackTicks.push_back(at + m_options.ackDelayMicroseconds);
            const std::uint64_t held = m_clientEnd.paneGeneration(m_handle);
            for (std::size_t index = 0; index < m_publishedAt.size() && m_firstGeneration + index <= held; ++index) {
                if (m_reachedAt[index] == 0)
                    m_reachedAt[index] = at;
            }
        }
        while (!m_ackTicks.empty() && m_ackTicks.front() <= at)
            m_ackTicks.pop_front();
        m_clientEnd.tick(host);
        std::vector<std::uint8_t> up = m_clientEnd.takeStreamOutput();
        if (!up.empty())
            m_upstream.push_back({ .arrivesAt = at + shape.oneWayDelayMicroseconds, .bytes = std::move(up) });
        while (!m_upstream.empty() && m_upstream.front().arrivesAt <= at) {
            m_hostEnd.receiveStream(m_upstream.front().bytes, host);
            m_upstream.pop_front();
        }
        serviceLink(at);
    }

    /// Runs the shaped link until nothing is in flight, then compares the
    /// client with the host.
    void drainLink()
    {
        if (!m_options.link)
            return;
        std::uint64_t at = m_linkClock;
        for (int events = 0; events < 10'000'000; ++events) {
            const std::uint64_t next = nextLinkEvent(at);
            const bool isQuiet = m_downstream.empty() && m_upstream.empty() && !m_hostEnd.hasQueuedStreamFrames()
                && m_clientEnd.paneGeneration(m_handle) == m_source->generation();
            if (next == noNextRead || isQuiet)
                break;
            at = next;
            runLinkEvent(at);
        }
        m_linkClock = at;
        if (m_failure.empty()) {
            std::string disagreement = firstDisagreement();
            if (disagreement.empty() && m_clientEnd.paneGeneration(m_handle) != m_source->generation())
                disagreement = "the client holds generation " + std::to_string(m_clientEnd.paneGeneration(m_handle)) + ", not the newest";
            if (!disagreement.empty())
                m_failure = "once the link drained: " + disagreement;
        }
    }

    /// Sets the pane's records from the host's terminal, as a host does at
    /// each frame it takes: what a record carries is as current as the picture.
    void setRecords()
    {
        m_source->setFacts(Ubiquitty::Protocol::Sync::paneFactsOf(*m_host, {}));
        m_source->setStatus(Ubiquitty::Protocol::Sync::statusRecordsOf(*m_host));
    }

    /// Records the hash of each line the host's history gained since the last
    /// publish, as the line was when it left the screen. A line a pull-back
    /// withdrew and a later shrink pushed out again is recorded anew.
    void recordEmittedLines()
    {
        const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(*m_host);
        if (address.epoch != m_emittedEpoch) {
            m_emittedHashes.clear();
            m_emittedEpoch = address.epoch;
            m_emittedHead = address.oldest;
        }
        for (std::uint64_t number = std::min(m_emittedHead, address.head); number < address.head; ++number) {
            if (const std::optional<std::uint64_t> hash = Ubiquitty::Protocol::Sync::logicalLineContentHash(*m_host, number))
                m_emittedHashes[number] = *hash;
        }
        m_emittedHead = address.head;
    }

    /// Greets the host, binds the pane and delivers the attach checkpoint.
    ///
    /// The checkpoint is the client's sync point; every publish after it
    /// goes as an update unless only a checkpoint can say it.
    void attach()
    {
        m_clientEnd.start();
        (void)pump();
        setRecords();
        m_source->publish(Ubiquitty::Protocol::Sync::FrameClass::Structural, m_nowMicroseconds);
        recordEmittedLines();
        m_handle = m_hostEnd.bindPane(*m_source);
        // The frame the checkpoint carries. The host takes it again, under
        // the next generation, for the update that tells the client the echo
        // context, which may still be on its way when the pump returns.
        const std::uint64_t attached = m_source->generation();
        (void)pump();
        if (m_clientEnd.paneGeneration(m_handle) < attached)
            m_failure = "the attach checkpoint did not apply";
        m_firstGeneration = m_source->generation() + 1;
    }

    LoopbackOptions m_options;
    // Ahead of the source that holds it, so it outlives the source.
    LoopbackProgramInput m_programInput;
    std::unique_ptr<Ubiquitty::Terminal> m_host;
    std::unique_ptr<Ubiquitty::Protocol::Sync::PaneSource> m_source;
    Ubiquitty::Protocol::Sync::HostConnection m_hostEnd;
    Ubiquitty::Protocol::Sync::ClientConnection m_clientEnd;
    std::uint32_t m_handle = 0;
    Ubiquitty::Protocol::Sync::PublishCensus m_census;
    Ubiquitty::Runtime::FrameCoherence m_coherence { m_options.coherence };
    std::uint64_t m_nowMicroseconds = 0;
    std::uint64_t m_nextReadMicroseconds = noNextRead;
    std::size_t m_streamOffset = 0;
    bool m_isDue = false;
    bool m_lastPassWasSaturated = false;
    std::vector<PublishedFrame> m_frames;
    std::string m_failure;
    std::deque<InFlight> m_downstream;
    std::deque<InFlight> m_upstream;
    std::deque<std::uint64_t> m_ackTicks;
    std::uint64_t m_wireFreeAt = 0;
    std::uint64_t m_linkClock = 0;
    std::uint64_t m_linkBytes = 0;
    std::uint64_t m_firstGeneration = 0;
    std::vector<std::uint64_t> m_publishedAt;
    std::vector<std::uint64_t> m_reachedAt;
    std::vector<std::uint64_t> m_roundTripAtPublish;
    Ubiquitty::Session::PaneLinkEvents m_events;
    Ubiquitty::Protocol::Sync::ViewState m_view = Ubiquitty::Protocol::Sync::ViewState::Live;
    std::optional<std::uint32_t> m_notifiedFactsRevision;
};

} // namespace UbiquittyTest
