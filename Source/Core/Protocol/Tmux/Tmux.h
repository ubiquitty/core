// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/Protocol/Tmux/PaneID.h"

#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// tmux Control Mode integration (`tmux -CC`). A pure byte-stream filter between the
// host transport and the VT100 model (depends only on Base/Types.h + the C++ standard
// library). On `%begin` framing, bytes are line-buffered and parsed as tmux's control
// protocol; only `%output` payloads reach the terminal model. Outbound keystrokes are
// wrapped as `send-keys -t %N -H xx xx ...` so they reach the active pane via the
// server. Single-pane focus: the most recently-active pane is the keystroke target.
//
// Protocol references: man 1 tmux (Control Mode); tmux/control.c, cmd-control-client.c

// Sized so a wide, heavily-styled `capture-pane -e` row fits without truncation;
// over-long lines flush truncated rather than dropped (a quality knob, not a
// correctness bound).

namespace Ubiquitty::Protocol::Tmux {

constexpr int maxTmuxLineLength = 8192;
// Saturation ceiling for decimal fields so a long digit run can't overflow the
// signed accumulator.
constexpr int maxTmuxValue = 1 << 24;
// Pane ids are server-global, monotonic and never reused, so a tmux server that
// has created this many panes over its life issues the next one above whatever
// ceiling a client picked. A ceiling below the scanners' meant the %output
// router clamped an id it could not hold and delivered those bytes to a
// different pane, while pane records accepted the same id happily — the two
// paths disagreeing about which panes exist. One ceiling, and it is the
// scanners'.
constexpr int maxTmuxPaneID = maxTmuxValue;
// Bytes of key data one send-keys command carries. Each costs three on the wire
// as " xx" and every command repeats its header, so a small chunk spends a large
// share of a paste on headers and makes the server parse a command line for
// every few bytes typed. At this size a command stays around a kilobyte, which
// is well inside what a control-mode line carries.
constexpr int sendKeysChunkBytes = 256;
static_assert(maxTmuxLineLength >= 256, "tmux %output frames can exceed 256B easily");
// The send-keys header formats a pane id, so its buffer has to hold one.
static_assert(maxTmuxPaneID < 100000000, "pane ID must fit in 8 decimal digits");
static_assert(sendKeysChunkBytes * 3 + 64 < maxTmuxLineLength,
    "one send-keys command must stay within the line length tmux parses");

// Window-layout bounds. A %layout-change carries a recursive cell tree; cap the pane
// count and nesting depth so a hostile server can't exhaust memory or blow the parse
// stack, and cap each dimension against integer overflow.
constexpr int tmuxMaxLayoutPanes = 256;
constexpr int tmuxMaxLayoutDepth = 16;
constexpr int tmuxMaxLayoutDimension = 65535;
// Session-model window cap: a hostile server can mint unlimited @N via %layout-change
// (each → Window + Pane + Terminal), and the registry gate is off the portable path,
// so the portable Session bounds itself.
constexpr int tmuxMaxSessionWindows = 256;

enum class TmuxState {
    Passthrough, // not in control mode — bytes flow straight to VT100
    Line, // accumulating a control protocol line
    Response // inside a %begin/%end command response we're skipping
};

// Sub-state for streaming a `%output %<id> ...` payload byte-by-byte. The full line
// can be tens of KB (a fresh attach dumps the entire pane state in one line), so it
// can't buffer into maxTmuxLineLength — recognize the header, then stream-decode.
enum class TmuxOutputSubState {
    None, // not currently streaming a %output line
    HeadPercent, // expecting '%' (start of pane id)
    HeadID, // parsing pane id digits
    HeadSpace, // expecting space between pane id and payload
    Body, // streaming payload, normal byte mode
    Escape, // saw '\\', awaiting escape body
    Octal1, // saw '\\d', awaiting 2nd octal digit
    Octal2, // saw '\\dd', awaiting 3rd octal digit
    ExtendedSkip, // %extended-output: skipping the "<age> [flags]" header to the ':' delimiter
    ExtendedColon // %extended-output: saw ':', consume the one separator space then stream the value
};

// Fires once on the PASSTHROUGH→LINE transition. The host uses it to send the
// commands tmux needs before it produces pane output, notably
// `refresh-client -C <columns>x<rows>`.
// The context is supplied alongside at setEnterControlCallback, for the reason
// TmuxEventClient carries one: a capturing callable here reaches Terminal.h.
using TmuxEnterControlHandler = void (*)(void* context);

// One pane's absolute position + size within its window, decoded from a tmux
// window-layout string. tmux gives absolute coordinates, so the frontend can
// place each pane's grid directly without walking the split tree itself.
struct TmuxPaneGeometry {
    Ubiquitty::Protocol::Tmux::PaneID paneID;
    int x;
    int y;
    int width;
    int height;
};

// Layout-change callbacks receive the raw tmux layout field as a borrowed view
// valid for the call; receivers parseLayout() it on demand.

// Host-installed window-event sink. A null handler means "not handled". The
// parser hands string payloads as borrowed views over the current parse line; a
// receiver that keeps text copies it into its own storage.
//
// Function pointer plus one context, the shape PaneRouter carries: Terminal
// holds a TmuxClient by value, so a capturing callable here would put
// <functional> in every translation unit that reaches Terminal.h. The
// static_assert below is what keeps it out, rather than a note asking.
//
// Each member defaults to null because a client is routinely declared and only
// partly assigned; without that the unassigned handlers would be indeterminate
// rather than "not handled".
struct TmuxEventClient {
    // Handed back to every handler below, so a receiver reaches its own state
    // without the callable owning any.
    void* context = nullptr;
    void (*windowAdd)(void* context, int windowID) = nullptr;
    void (*windowClose)(void* context, int windowID) = nullptr;
    void (*windowRenamed)(void* context, int windowID, std::string_view name) = nullptr;
    void (*sessionRenamed)(void* context, std::string_view name) = nullptr;
    void (*sessionChanged)(void* context) = nullptr;
    void (*exit)(void* context) = nullptr;
    void (*commandResponseLine)(void* context, std::string_view line) = nullptr;
    void (*commandResponseEnd)(void* context) = nullptr;
    void (*layoutChange)(void* context, int windowID, std::string_view layout) = nullptr;
    // "%pause %<pane>" — tmux stopped forwarding this pane's output. The host must
    // answer with continuePane, or the pane stays frozen forever.
    void (*paneDidPause)(void* context, Ubiquitty::Protocol::Tmux::PaneID paneID) = nullptr;
    // "%pane-mode-changed %<pane>" — the pane's mode/mouse toggled. tmux sends no new
    // value, so the host re-queries; without this a live `mouse on/off` leaves the
    // pane Terminal's DEC mouse flags stale.
    void (*paneDidChangeMode)(void* context, Ubiquitty::Protocol::Tmux::PaneID paneID) = nullptr;
};

static_assert(std::is_trivially_copyable_v<TmuxEventClient>);

struct TmuxClient {
    bool enabled; // preferences flag: integration master switch
    bool autoDetect; // preferences flag: switch to control on first %begin
    TmuxState state;
    char line[maxTmuxLineLength];
    int lineLength;
    Ubiquitty::Protocol::Tmux::PaneID activePaneID; // pane id used as send-keys target
    int responseDepth; // nesting count for %begin/%end pairs
    // DCS wrapping: some tmux variants wrap the whole control-mode session in a Device
    // Control String — `\eP1000p` ... payload ... `\e\\`.
    uint8_t dcsPrefixPosition;
    bool dcsEnvelopeActive;
    bool dcsSawEscape;
    // Whether this control session's end has been answered with `events.exit`. The
    // envelope's terminator and the stream's end each end the session too, and answer
    // it when no %exit did: a server that dies mid-line leaves its client's %exit inside
    // an unfinished %output, where it reads as pane output.
    bool hasSignalledExit;
    // Passthrough `%begin ` prefix matcher (start-of-line only). Matched bytes are
    // held back so they aren't double-delivered if the line is the introducer; on a
    // mismatch they flush verbatim and the byte reprocesses. Bounded to 7 bytes.
    uint8_t beginPrefixPosition;
    // True when the next passthrough byte is at column 0. Only then may a leading '%'
    // begin the `%begin ` match; otherwise `cat`ing a log that quotes "%begin " would
    // silently flip into control mode waiting for an `%end` that never comes.
    bool atLineStart;
    // %output streaming sub-state. Active only while state==LINE.
    TmuxOutputSubState outputSubState;
    Ubiquitty::Protocol::Tmux::PaneID outPaneAccumulator; // pane id accumulator while in HEAD_ID
    int outOctalAccumulator; // partial octal value while in OCT1/OCT2
    bool extendedOutput; // streaming a %extended-output line — skip its age/flags header before BODY
    TmuxEnterControlHandler enterControl;
    void* enterControlContext;
    TmuxEventClient events; // host window-event sink; a null handler means not handled
};

using TmuxDeliverCallback = void (*)(void* context, uint8_t byte);
using TmuxDeliverSpanCallback = void (*)(void* context, std::string_view bytes);

} // namespace Ubiquitty::Protocol::Tmux

namespace Ubiquitty::Protocol::Tmux {

/// How far tmux control-mode integration is engaged.
enum class IntegrationMode : uint8_t {
    Disabled,
    Enabled
};
/// Whether tmux control mode is entered on detection.
enum class AutoDetectMode : uint8_t {
    Manual,
    Automatic
};

// Reset to passthrough state. Safe to call repeatedly.
void init(TmuxClient* tc);

/// Installs the callback that fires when the client enters control mode.
///
/// The handler receives `context` on every call, so a receiver reaches its own
/// state without the callable holding any — which is what keeps `TmuxClient`
/// trivially copyable and `<functional>` out of every consumer of `Terminal.h`.
/// A null `handler` clears the installation; `context` is then unused.
///
/// - Parameters:
///   - tc: The client to install into. A null pointer is ignored.
///   - handler: Invoked once on the passthrough-to-line transition, or null to clear.
///   - context: Passed back to `handler` unchanged. Its lifetime must cover the client's.
void setEnterControlCallback(TmuxClient* tc, TmuxEnterControlHandler handler, void* context);

// Install the window-lifecycle event sink. Pass a default-constructed
// TmuxEventClient to clear: every handler is null and none is called.
void setEventClient(TmuxClient* tc, TmuxEventClient client);

// Parse a tmux window-layout string ("<checksum>,<cell-tree>") into a flat
// array of pane rects (absolute x/y/w/h). Returns the pane count
// (0..maxPanes) or -1 on a malformed/oversized layout. Bounded against a
// hostile server: caps nesting depth (tmuxMaxLayoutDepth), pane count
// (maxPanes), and each dimension (tmuxMaxLayoutDimension). Reusable for
// both the %layout-change event and a list-windows '#{window_layout}'
// command response.
[[nodiscard]] int parseLayout(std::string_view layout, std::span<TmuxPaneGeometry> outputBuffer);

// Whether the parser is currently in control mode (line-buffering and
// decoding tmux protocol).
[[nodiscard]] bool isActive(const TmuxClient* tc);

// Apply the preferences flags. Disabling drops back to passthrough.
void configure(TmuxClient* tc, IntegrationMode integration, AutoDetectMode autoDetectMode);

// Manually enter control mode (used when preferences forces it without
// waiting for %begin).
void enterControlMode(TmuxClient* tc);

/// Ends a control session the stream ended under.
///
/// A control client killed outright writes neither %exit nor the envelope's
/// terminator: its stream just stops, and the session it carried would stay in
/// control mode, its windows open over panes nothing writes to. The end of
/// the stream ends it instead, answering its exit as %exit would have.
///
/// - Parameter tc: The client. Null, or one in passthrough, is left alone.
void endOfStream(TmuxClient* tc);

// Process one byte from the host. In passthrough, the byte passes straight to
// deliver() (a `%begin` line flips to control mode when autoDetect is on); in control
// mode, bytes are line-buffered and only `%output` payload bytes are forwarded.
void feedInput(TmuxClient* tc, uint8_t c, void* context, TmuxDeliverCallback);
/// Processes a run of host bytes, forwarding unescaped stretches whole.
///
/// Equivalent to ``feedInput`` per byte, except that a run of %output payload
/// carrying no escape reaches the span callback in one call rather than one per
/// byte. That is the flood path, so the saving is the point.
///
/// - Parameters:
///   - tc: The client, or null to pass every byte straight through.
///   - bytes: The bytes as read from the host, at any boundary.
///   - context: Passed back to both callbacks untouched.
void feedInputBytes(TmuxClient* tc, std::string_view bytes, void* context, TmuxDeliverCallback, TmuxDeliverSpanCallback);

/// Wraps an outbound key chunk for the server.
///
/// In control mode the result is `send-keys -t %N -H xx ...\n` lines covering
/// every byte; in passthrough the bytes are copied verbatim.
///
/// - Parameters:
///   - tc: The client whose mode and active pane decide the wrapping.
///   - bytes: The key bytes to send.
///   - length: How many of them.
///   - output: Receives the wrapped form.
/// - Returns: The byte count written, or 0 when `output` is too small for it.
int wrapKey(const TmuxClient* tc, std::string_view bytes, std::span<char> output);

/// Wraps an outbound key chunk for a named pane.
///
/// ``wrapKey`` addresses `tc->activePaneID`, which follows the server's most
/// recent `%output`. A child window sends keystrokes to its pane regardless of
/// that, so it names the target itself.
///
/// - Parameters:
///   - tc: The client whose mode decides the wrapping.
///   - paneID: The pane the keys are for.
///   - bytes: The key bytes to send.
///   - length: How many of them.
///   - output: Receives the wrapped form.
/// - Returns: The byte count written, or 0 when `output` is too small for it.
int wrapKeyForPane(const TmuxClient* tc, PaneID paneID, std::string_view bytes, std::span<char> output);

// --- Control-mode response-row parsing (portable; no platform deps) ------
//
/// Consumes a run of decimal digits at `i`.
///
/// One of two tiny scanners the pane-record and capture-marker parsers share
/// across frontends. Neither reads past the view's end, which is what keeps a
/// hostile or truncated server line in bounds — the extent travels with the
/// bytes rather than beside them.
///
/// - Parameters:
///   - line: The line being scanned.
///   - i: The read position, advanced past the digits that were read.
///   - outValue: Receives the value, untouched when no digit was there.
/// - Returns: True when at least one digit was read.
bool scanUInt(std::string_view line, int& i, int& outValue);
/// Consumes `c` at `i`, advancing past it.
///
/// - Parameters:
///   - line: The line being parsed.
///   - length: Its length in bytes.
///   - i: The read position, advanced past `c` on a match.
///   - c: The character required at that position.
/// - Returns: True when `c` was there; `i` is unchanged otherwise.
bool expectCharacter(std::string_view line, int& i, char c);

/// Parses one pane-enumeration row from a list-panes response body.
///
/// The row reads `@<win> %<pane> <w> <h> <active> <layout> <session>
/// <window_name>`. `layout` and `session` are single space-free tokens;
/// `window_name` is the rest of the line and may contain spaces. Pure parsing —
/// it reaches nothing outside its arguments.
///
/// - Parameters:
///   - line: The row, without its terminator.
///   - windowID: Receives the window id.
///   - paneID: Receives the pane id.
///   - width: Receives the pane width in cells.
///   - height: Receives the pane height in cells.
///   - active: Receives the active flag, 1 when the field is absent.
///   - layout: Receives the layout token, possibly empty.
///   - session: Receives the session name, possibly empty.
///   - name: Receives the window name, possibly empty.
/// - Returns: False on a malformed prefix, leaving the outputs unspecified.
bool parsePaneRecord(std::string_view line, int& windowID, PaneID& paneID, int& width, int& height, int& active, std::string& layout, std::string& session, std::string& name);

} // namespace Ubiquitty::Protocol::Tmux
