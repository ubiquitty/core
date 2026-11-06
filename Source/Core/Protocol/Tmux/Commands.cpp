// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Commands.cpp — see Commands.h.

#include "Core/Protocol/Tmux/Commands.h"

#include "Core/Protocol/Tmux/Tmux.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

namespace Ubiquitty::Protocol::Tmux {

std::string sessionReferenceQuery()
{
    return "display-message -p \"" + std::string(sessionReferenceReplyPrefix)
        + "#{pid} #{start_time} #{session_id} #{session_created} #{socket_path}\"";
}

static bool validSessionReference(const SessionReference& reference)
{
    return reference.serverProcessID && reference.serverStartTime && reference.sessionCreated
        && !reference.socketPath.empty() && reference.socketPath.front() == '/' && reference.socketPath.size() <= 4096
        && std::ranges::none_of(reference.socketPath, [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; });
}

std::optional<SessionReference> parseSessionReference(std::string_view line)
{
    if (!line.starts_with(sessionReferenceReplyPrefix) || line.size() > 4224) {
        return std::nullopt;
    }
    line.remove_prefix(sessionReferenceReplyPrefix.size());
    const auto takeNumber = [&line](std::uint64_t& value, bool session) {
        if (session) {
            if (line.empty() || line.front() != '$') {
                return false;
            }
            line.remove_prefix(1);
        }
        const auto separator = line.find(' ');
        if (separator == std::string_view::npos || !separator) {
            return false;
        }
        const auto token = line.substr(0, separator);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
            return false;
        }
        line.remove_prefix(separator + 1);
        return true;
    };
    SessionReference reference;
    if (!takeNumber(reference.serverProcessID, false) || !takeNumber(reference.serverStartTime, false)
        || !takeNumber(reference.sessionID, true) || !takeNumber(reference.sessionCreated, false)) {
        return std::nullopt;
    }
    reference.socketPath = line;
    if (!validSessionReference(reference)) {
        return std::nullopt;
    }
    return reference;
}

std::vector<std::string> restoreSessionArguments(std::string_view tmuxPath, const SessionReference& reference)
{
    if (tmuxPath.empty() || tmuxPath.find('\0') != std::string_view::npos || !validSessionReference(reference)) {
        return {};
    }
    const auto target = "$" + std::to_string(reference.sessionID);
    const auto condition = "#{&&:#{&&:#{==:#{pid}," + std::to_string(reference.serverProcessID)
        + "},#{==:#{start_time}," + std::to_string(reference.serverStartTime)
        + "}},#{==:#{session_created}," + std::to_string(reference.sessionCreated) + "}}";
    return { std::string(tmuxPath), "-S", reference.socketPath, "-CC", "if-shell", "-F", "-t", target + ":", condition,
        "attach-session -t " + target, "display-message -p 'Saved tmux session is unavailable'" };
}

std::string listPanesForSession() { return std::string("list-panes -s -F ") + std::string(paneListFormat); }

std::string listPanesForWindow(int windowID) { return "list-panes -t @" + std::to_string(windowID) + " -F " + std::string(paneListFormat); }

std::string refreshClientSize(int columns, int rows) { return "refresh-client -C " + std::to_string(columns) + "x" + std::to_string(rows); }

std::string paneModeQuery(PaneID paneID)
{
    // `#{?flag,1,0}` forces one digit per field so an unknown var can't shift
    // the parsed positions. The field order here is the contract decodePaneModeReport
    // decodes: 1000 1002 1003 1006 1005 DECCKM.
    return "display-message -p -t %" + std::to_string(paneID) + " \"" + std::string(paneModeReplyPrefix)
        + "#{pane_id} "
          "#{?mouse_standard_flag,1,0}#{?mouse_button_flag,1,0}#{?mouse_all_flag,1,0}"
          "#{?mouse_sgr_flag,1,0}#{?mouse_utf8_flag,1,0}#{?keypad_cursor_flag,1,0}\"";
}

std::string clipboardPullMarker(PaneID paneID)
{
    // The reply — `UBCLIPBEGIN %<pane>` — self-identifies in the response router;
    // the show-buffer block that follows is the buffer content.
    return "display-message -p -t %" + std::to_string(paneID) + " \"" + std::string(clipboardPullPrefix) + "#{pane_id}\"";
}

std::string clipboardPullShowBuffer()
{
    // `-b` unset ⇒ the most-recently-created buffer, which a copy-mode copy just
    // wrote. Raw content, no format, delivered as the command-response body.
    return "show-buffer";
}

bool shouldSyncClipboardPull(std::string_view pulled, std::string_view lastSynced, bool writeAllowed)
{
    if (!writeAllowed)
        return false;
    if (pulled.empty())
        return false; // never clear the host clipboard from an empty/absent buffer
    if (pulled == lastSynced)
        return false; // unchanged since our last sync — not a fresh copy
    return true;
}

int parseClipboardPullMarkerPane(std::string_view afterPrefix)
{
    if (afterPrefix.empty() || afterPrefix.front() != '%')
        return -1;
    long pane = 0;
    bool sawDigit = false;
    for (std::size_t i = 1; i < afterPrefix.size(); ++i) {
        const char c = afterPrefix[i];
        if (c < '0' || c > '9')
            break; // stop at the first non-digit (trailing content ignored)
        pane = pane * 10 + (c - '0');
        if (pane > 100000000) // far beyond any real pane id — bound the untrusted parse
            return -1;
        sawDigit = true;
    }
    return sawDigit ? static_cast<int>(pane) : -1;
}

bool clipboardPullShouldWrite(PaneAttribution attribution, ClipboardWriteGate gate)
{
    if (attribution != PaneAttribution::Resolved)
        return false; // fail closed: the server-controlled pane id didn't resolve
    return gate == ClipboardWriteGate::Allowed; // the gate decides the untrusted-origin write
}

CaptureForm captureFormForVersion(std::string_view version)
{
    // A development build names the release it leads to.
    if (version.starts_with("next-"))
        version.remove_prefix(5);
    const char* const end = version.data() + version.size();
    int major = 0;
    const auto [afterMajor, majorError] = std::from_chars(version.data(), end, major);
    if (majorError != std::errc {} || afterMajor == end || *afterMajor != '.')
        return CaptureForm::Text;
    int minor = 0;
    const auto [afterMinor, minorError] = std::from_chars(afterMajor + 1, end, minor);
    if (minorError != std::errc {})
        return CaptureForm::Text;
    return major > 3 || (major == 3 && minor >= 7) ? CaptureForm::FlaggedRows : CaptureForm::Text;
}

std::string serverVersionQuery() { return "display-message -p \"" + std::string(serverVersionReplyPrefix) + "#{version}\""; }

std::string captureBeginMarker(PaneID paneID, int retryCount, CaptureScope scope, CaptureForm form)
{
    // The marker carries the pane id, cursor (restored after the content lands),
    // the retry count, the scope (1 = FullHistory, 0 = CurrentScreen) and the
    // form (1 = FlaggedRows, 0 = Text); a field left off reads as 0. The
    // following capture-pane block is the content.
    return "display-message -p -t %" + std::to_string(paneID) + " \"" + std::string(captureBeginPrefix) + std::to_string(paneID) + " #{cursor_x} #{cursor_y} "
        + std::to_string(retryCount) + " " + (scope == CaptureScope::FullHistory ? "1" : "0") + " " + (form == CaptureForm::FlaggedRows ? "1" : "0") + "\"";
}

static std::string captureRowsFlags(CaptureForm form) { return form == CaptureForm::FlaggedRows ? " -e -p -N -T -F" : " -e -p"; }

std::string capturePane(PaneID paneID, CaptureForm form) { return "capture-pane -t %" + std::to_string(paneID) + captureRowsFlags(form); }

std::string capturePaneWithHistory(PaneID paneID, CaptureForm form)
{
    // `-S -`/`-E -`: literal dashes ("start of history"/"end of visible pane"),
    // resolved server-side — not a `#{...}` format string, which breaks over -CC.
    return "capture-pane -t %" + std::to_string(paneID) + captureRowsFlags(form) + " -S - -E -";
}

std::string splitWindow(PaneID paneID, SplitDirection direction)
{
    // `-h` = side-by-side (column), `-v` = stacked (row), matching tmux's own
    // split-window flags. The new pane lands when the server echoes its
    // %layout-change; this command never builds it locally.
    const char* flag = (direction == SplitDirection::SideBySide) ? "-h" : "-v";
    return std::string("split-window ") + flag + " -t %" + std::to_string(paneID);
}

std::string movePane(PaneID source, PaneID target, Panes::Orientation orientation, Panes::InsertSide side, int percentage, Panes::InsertTarget insertTarget)
{
    if (source < 0 || target < 0 || source == target || percentage < 1 || percentage > 99) {
        return {};
    }
    // Selection queues a resize; yielding the command queue applies it before the split without starting a shell.
    return "select-window -t %" + std::to_string(target) + " ; run-shell -d 0 ; move-pane -d " + (orientation == Panes::Orientation::Vertical ? "-h" : "-v")
        + (insertTarget == Panes::InsertTarget::Root ? " -f" : "")
        + (side == Panes::InsertSide::First ? " -b" : "") + " -l " + std::to_string(percentage)
        + "% -s %" + std::to_string(source) + " -t %" + std::to_string(target);
}

std::string moveLayout(const Panes::Tree& tree, PaneID target, Panes::Orientation orientation, Panes::InsertSide side, Panes::InsertTarget insertTarget)
{
    if (target < 0 || tree.holdsPane(target)) {
        return {};
    }
    const auto firstPane = [&](int index) {
        while (!tree.isLeaf(index)) {
            index = tree.node(index).first;
        }
        return tree.node(index).paneID;
    };
    std::string command = movePane(firstPane(tree.root()), target, orientation, side, 50, insertTarget);
    if (command.empty()) {
        return {};
    }
    const auto appendSplits = [&](auto&& append, int index) -> void {
        if (tree.isLeaf(index)) {
            return;
        }
        const auto node = tree.node(index);
        command += " ; " + movePane(firstPane(node.second), firstPane(node.first), node.orientation, Panes::InsertSide::Second, static_cast<int>((1.0f - node.ratio) * 100.0f + 0.5f));
        append(append, node.first);
        append(append, node.second);
    };
    appendSplits(appendSplits, tree.root());
    command += " ; " + selectPane(tree.focusedPaneID());
    return command;
}

std::string breakPane(PaneID pane)
{
    return pane >= 0 ? "break-pane -s %" + std::to_string(pane) : std::string {};
}

std::string killPane(PaneID paneID) { return "kill-pane -t %" + std::to_string(paneID); }

std::string killWindow(int windowID) { return "kill-window -t @" + std::to_string(windowID); }

std::string selectPane(PaneID paneID) { return "select-pane -t %" + std::to_string(paneID); }

std::string zoomPane(PaneID paneID) { return "resize-pane -Z -t %" + std::to_string(paneID); }

std::string newWindow() { return "new-window"; }

std::string renameWindow(PaneID paneID, std::string_view name)
{
    // Double-quote for tmux's command parser; inside double quotes only
    // backslash and the quote itself need escaping.
    std::string quoted;
    quoted.reserve(name.size() + 2);
    quoted.push_back('"');
    for (char c : name) {
        if (c == '\\' || c == '"')
            quoted.push_back('\\');
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return "rename-window -t %" + std::to_string(paneID) + " " + quoted;
}

std::string detachClient() { return "detach-client"; }

std::string killSession() { return "kill-session"; }

std::string continuePane(PaneID paneID)
{
    // The literal '%' is a plain argument to -A (tmux's arg grammar, not its
    // format engine), so no quoting or #{...} substitution issues arise here.
    return "refresh-client -A %" + std::to_string(paneID) + ":continue";
}

std::string enableAggressiveResize() { return "set-option aggressive-resize on"; }

std::string decodePaneModeReport(std::string_view body, PaneID& paneID)
{
    paneID = 0;
    if (body.empty())
        return {};
    int i = 0;
    if (!expectCharacter(body, i, '%') || !scanUInt(body, i, paneID))
        return {};
    if (!expectCharacter(body, i, ' '))
        return {};
    // Fixed-width bit string, in order: 1000 1002 1003 1006 1005 DECCKM. Each is
    // a single '1'/'0' (the query uses #{?flag,1,0}, so an unknown var yields '0').
    auto bit = [&](int index) -> bool { return std::cmp_less(i + index, body.size()) && body[i + index] == '1'; };
    std::string sequence;
    if (bit(0))
        sequence += "\x1b[?1000h"; // standard mouse tracking
    if (bit(1))
        sequence += "\x1b[?1002h"; // button-event tracking
    if (bit(2))
        sequence += "\x1b[?1003h"; // any-event tracking
    if (bit(3))
        sequence += "\x1b[?1006h"; // SGR extended coordinates
    if (bit(4))
        sequence += "\x1b[?1005h"; // UTF-8 extended coordinates
    if (bit(5))
        sequence += "\x1b[?1h"; // DECCKM application cursor keys
    return sequence;
}

} // namespace Ubiquitty::Protocol::Tmux
