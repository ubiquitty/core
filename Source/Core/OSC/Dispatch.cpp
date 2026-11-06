// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC (Operating System Command) dispatch. This TU holds the dispatch surface: the leading-Ps parse, the
// per-OSC security gate, the sorted dispatch table, and the thin oscH_* wrappers. Handler bodies live in
// capability-slice TUs (OSCColor/OSCClipboard/OSCState/OSCNotify/OSCShellIntegration/OSC1337; see
// Internal.h). The per-byte hot path stays in VT.cpp, which accumulates the OSC string and calls
// dispatch on the terminator.

#include "Core/BuildVersion.h"
#include "Core/DragDrop/Client.h"
#include "Core/EffectGate.h"
#include "Core/OSC/Internal.h"
#include "Core/OSC/RichTextBlocks.h"
#include "Core/ReplyBuilder.h"
#include "Core/Unicode/ASCII.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Parser.h"
#include "PAL/Log.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::OSC {

// Labels contain display text. Dropping C0 and DEL preserves UTF-8 sequences
// and prevents control bytes from reaching presentation or title replies.
static bool labelHasFilteredByte(std::string_view src)
{
    for (const char character : src) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7F)
            return true;
    }
    return false;
}

static std::string filteredLabelUTF8(std::string_view src)
{
    std::string filtered;
    // The result is at most the source, and reached it a byte at a time
    // through the growth schedule — one reserve replaces that walk.
    filtered.reserve(src.size());
    for (const char character : src) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= 0x20 && byte != 0x7F)
            filtered.push_back(character);
    }
    return filtered;
}

// The per-OSC / per-DCS / per-APC sequence gate lives on the Terminal (Core/OSC/SequenceGate.h);
// dispatch consults it with a linear scan over a tiny (commonly empty) set.
namespace {
template<typename T> bool listContains(const T* list, int count, T value)
{
    for (int i = 0; i < count; ++i) {
        if (list[i] == value)
            return true;
    }
    return false;
}

// Shared gate decision: deny set wins; a non-empty allow set is exclusive.
template<typename T> bool gateAllows(T value, const T* allow, int allowCount, const T* deny, int denyCount)
{
    if (denyCount > 0 && listContains(deny, denyCount, value))
        return false;
    if (allowCount > 0 && !listContains(allow, allowCount, value))
        return false;
    return true;
}
} // namespace

// Say so when a gate list did not fit.
//
// These lists are a SECURITY control, and the two directions fail in opposite
// ways. A truncated ALLOW list fails closed -- fewer sequences pass than were
// asked for, which is safe and visible. A truncated DENY list fails OPEN: the
// codes past the cap are permitted, the control reports success, and it does
// less than it was told to. Nothing said so.
//
// stderr via PAL::logLine is the same channel Preferences uses to report a
// skipped key, in the same "-- ignored" shape, so an operator meets this where
// they already meet configuration complaints.
static void reportGateListTruncated(const char* which, int kept, int dropped)
{
    // Dropped RULES, not leftover text. Trailing separators cost nothing, and
    // neither does an unrecognised DCS name -- the parser skips those by
    // design, so reporting them would cry wolf on a list that lost nothing.
    // A warning that fires when nothing was lost is how an operator learns to
    // ignore the one that matters.
    if (dropped <= 0)
        return;
    Ubiquitty::PAL::logLine(stderr,
        "ubiquitty: {} sequence-gate list holds {} entries at most; {} more were ignored. "
        "A DENY list shortened this way ALLOWS what it names.",
        which, kept, dropped);
}

bool setOSCAllowList(Ubiquitty::Terminal& terminal, const int* codes, int count)
{
    if (count < 0)
        count = 0;
    const int dropped = count > oscListCapacity ? count - oscListCapacity : 0;
    if (count > oscListCapacity)
        count = oscListCapacity;
    for (int i = 0; i < count; ++i)
        terminal.sequenceGate.oscAllow[i] = codes[i];
    terminal.sequenceGate.oscAllowCount = count;
    // Truncating an ALLOW list fails closed, so this is a report and not a
    // refusal — but it is the same answer, because a caller that asked for a
    // policy it did not get should hear so whichever direction it lost in.
    reportGateListTruncated("OSC allow", count, dropped);
    return dropped == 0;
}

bool setOSCDenyList(Ubiquitty::Terminal& terminal, const int* codes, int count)
{
    if (count < 0)
        count = 0;
    // An over-capacity deny list loads to the cap and closes the dimension:
    // the dropped tail may name any code, so permitting everything past the
    // cap — or discarding the operator's allow list to spell deny-all — would
    // each honor one half of the config against the other. The overflow flag
    // denies the rest instead, and the ANSWER still tells the caller the list
    // as asked for did not fit.
    const int dropped = count > oscListCapacity ? count - oscListCapacity : 0;
    if (count > oscListCapacity)
        count = oscListCapacity;
    for (int i = 0; i < count; ++i)
        terminal.sequenceGate.oscDeny[i] = codes[i];
    terminal.sequenceGate.oscDenyCount = count;
    terminal.sequenceGate.hasOSCDenyOverflow = dropped > 0;
    reportGateListTruncated("OSC deny", count, dropped);
    return dropped == 0;
}

bool setDCSAllowList(Ubiquitty::Terminal& terminal, const DCSKind* kinds, int count)
{
    if (count < 0)
        count = 0;
    const int dropped = count > dcsListCapacity ? count - dcsListCapacity : 0;
    if (count > dcsListCapacity)
        count = dcsListCapacity;
    for (int i = 0; i < count; ++i)
        terminal.sequenceGate.dcsAllow[i] = kinds[i];
    terminal.sequenceGate.dcsAllowCount = count;
    reportGateListTruncated("DCS allow", count, dropped);
    return dropped == 0;
}

bool setDCSDenyList(Ubiquitty::Terminal& terminal, const DCSKind* kinds, int count)
{
    if (count < 0)
        count = 0;
    // See setOSCDenyList: loaded to the cap, reported, and the dimension
    // closed — abandoning the load would permit everything past it.
    const int dropped = count > dcsListCapacity ? count - dcsListCapacity : 0;
    if (count > dcsListCapacity)
        count = dcsListCapacity;
    for (int i = 0; i < count; ++i)
        terminal.sequenceGate.dcsDeny[i] = kinds[i];
    terminal.sequenceGate.dcsDenyCount = count;
    terminal.sequenceGate.hasDCSDenyOverflow = dropped > 0;
    reportGateListTruncated("DCS deny", count, dropped);
    return dropped == 0;
}

bool setAPCAllowList(Ubiquitty::Terminal& terminal, std::span<const uint8_t> introducers)
{
    const auto kept = introducers.first(std::min(introducers.size(), terminal.sequenceGate.apcAllow.size()));
    const int dropped = static_cast<int>(introducers.size() - kept.size());
    const int count = static_cast<int>(kept.size());
    std::ranges::copy(kept, terminal.sequenceGate.apcAllow.begin());
    terminal.sequenceGate.apcAllowCount = count;
    reportGateListTruncated("APC allow", count, dropped);
    return dropped == 0;
}

bool setAPCDenyList(Ubiquitty::Terminal& terminal, std::span<const uint8_t> introducers)
{
    // See setOSCDenyList: loaded to the cap, reported, and the dimension closed.
    const auto kept = introducers.first(std::min(introducers.size(), terminal.sequenceGate.apcDeny.size()));
    const int dropped = static_cast<int>(introducers.size() - kept.size());
    const int count = static_cast<int>(kept.size());
    std::ranges::copy(kept, terminal.sequenceGate.apcDeny.begin());
    terminal.sequenceGate.apcDenyCount = count;
    terminal.sequenceGate.hasAPCDenyOverflow = dropped > 0;
    reportGateListTruncated("APC deny", count, dropped);
    return dropped == 0;
}

bool oscAllowed(const Ubiquitty::Terminal& terminal, int ps)
{
    const SequenceGate& gate = terminal.sequenceGate;
    // A truncated deny closes the dimension, allow list included: an allowed
    // code may itself sit in the dropped tail, and permitting it would honor
    // the allow half against a deny half that never arrived whole.
    if (gate.hasOSCDenyOverflow)
        return false;
    return gateAllows(ps, gate.oscAllow.data(), gate.oscAllowCount, gate.oscDeny.data(), gate.oscDenyCount);
}

bool dcsAllowed(const Ubiquitty::Terminal& terminal, DCSKind kind)
{
    const SequenceGate& gate = terminal.sequenceGate;
    if (gate.hasDCSDenyOverflow)
        return false;
    return gateAllows(kind, gate.dcsAllow.data(), gate.dcsAllowCount, gate.dcsDeny.data(), gate.dcsDenyCount);
}

bool apcAllowed(const Ubiquitty::Terminal& terminal, uint8_t introducer)
{
    const SequenceGate& gate = terminal.sequenceGate;
    if (gate.hasAPCDenyOverflow)
        return false;
    return gateAllows(introducer, gate.apcAllow.data(), gate.apcAllowCount, gate.apcDeny.data(), gate.apcDenyCount);
}

int parseOSCList(std::string_view spec, int* out, int cap, int* droppedOut)
{
    int n = 0;
    int dropped = 0;
    std::size_t i = 0;
    const std::size_t len = spec.size();
    // Runs past `cap` rather than stopping at it: the entries beyond are not
    // stored, but they have to be COUNTED for the caller to be told anything
    // truthful about what its list lost.
    while (i < len) {
        // Skip separators (comma / whitespace).
        while (i < len && (spec[i] == ',' || spec[i] == ' ' || spec[i] == '\t' || spec[i] == '\n' || spec[i] == '\r'))
            ++i;
        if (i >= len)
            break;
        // Accumulate a decimal run; non-digit tokens are skipped entirely.
        bool haveDigit = false;
        int value = 0;
        while (i < len && spec[i] >= '0' && spec[i] <= '9') {
            value = Ubiquitty::decimalAppendSaturated(value, spec[i] - '0', 1 << 20);
            haveDigit = true;
            ++i;
        }
        if (haveDigit) {
            if (n < cap)
                out[n++] = value;
            else
                ++dropped;
        }
        // Drop any trailing non-separator junk up to the next separator.
        while (i < len && spec[i] != ',' && spec[i] != ' ' && spec[i] != '\t' && spec[i] != '\n' && spec[i] != '\r')
            ++i;
    }
    reportGateListTruncated("OSC", cap, dropped);
    if (droppedOut != nullptr)
        *droppedOut = dropped;
    return n;
}

int parseDCSList(std::string_view spec, DCSKind* out, int cap, int* droppedOut)
{
    auto eqCaseless = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z')
                ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z')
                cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb)
                return false;
        }
        return true;
    };
    int n = 0;
    int dropped = 0;
    std::size_t i = 0;
    const std::size_t len = spec.size();
    // Past `cap` as well, for the same reason -- and here only a RECOGNISED
    // name counts as a loss, since an unknown one was never going to become a
    // rule.
    while (i < len) {
        while (i < len && (spec[i] == ',' || spec[i] == ' ' || spec[i] == '\t' || spec[i] == '\n' || spec[i] == '\r'))
            ++i;
        if (i >= len)
            break;
        std::size_t start = i;
        while (i < len && spec[i] != ',' && spec[i] != ' ' && spec[i] != '\t' && spec[i] != '\n' && spec[i] != '\r')
            ++i;
        std::string_view token = spec.substr(start, i - start);
        bool known = true;
        DCSKind kind = DCSKind::Sixel;
        if (eqCaseless(token, "sixel"))
            kind = DCSKind::Sixel;
        else if (eqCaseless(token, "decudk"))
            kind = DCSKind::DECUDK;
        else if (eqCaseless(token, "decrqss"))
            kind = DCSKind::DECRQSS;
        else if (eqCaseless(token, "xtgettcap"))
            kind = DCSKind::XTGETTCAP;
        else if (eqCaseless(token, "tmux"))
            kind = DCSKind::Tmux;
        else if (eqCaseless(token, "decdld"))
            kind = DCSKind::DECDLD;
        else if (eqCaseless(token, "decrsts"))
            kind = DCSKind::DECRSTS;
        else if (eqCaseless(token, "sample"))
            kind = DCSKind::Sample;
        else
            known = false; // Unknown names are skipped, and cost no rule.
        if (known) {
            if (n < cap)
                out[n++] = kind;
            else
                ++dropped;
        }
    }
    reportGateListTruncated("DCS", cap, dropped);
    if (droppedOut != nullptr)
        *droppedOut = dropped;
    return n;
}

int parseAPCList(std::string_view spec, std::span<uint8_t> out, int* droppedOut)
{
    const int cap = static_cast<int>(out.size());
    const auto isSeparator = [](char c) { return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    int n = 0;
    int dropped = 0;
    std::size_t i = 0;
    const std::size_t len = spec.size();
    while (i < len) {
        while (i < len && isSeparator(spec[i]))
            ++i;
        if (i >= len)
            break;
        const std::size_t start = i;
        while (i < len && !isSeparator(spec[i]))
            ++i;
        // One byte per introducer, as the wire carries it. A longer token, or a
        // byte no APC starts with, is skipped and costs no rule.
        if (i - start != 1)
            continue;
        const auto introducer = static_cast<uint8_t>(spec[start]);
        if (!isAPCIntroducer(introducer))
            continue;
        if (n < cap)
            out[static_cast<std::size_t>(n++)] = introducer;
        else
            ++dropped;
    }
    reportGateListTruncated("APC", cap, dropped);
    if (droppedOut != nullptr)
        *droppedOut = dropped;
    return n;
}

// Parse "<digits>[;]" from the head of the OSC payload. Advances *pp
// past the consumed bytes. Returns:
// - false on no leading digit, or digits-then-garbage-non-';'
// (malformed). *outPs is left zero.
// - true on success, with *outPs holding the parsed number and *pp
// pointing at the post-';' payload (or at the trailing NUL when
// the wire form was bare-numeric).
// The cursor advances past the code and its separator, so it is taken by
// reference; the code comes back as the return value. A caller reading a code
// out of a refused parse is the shape the out-parameter allowed and this does not.
[[nodiscard]] static std::optional<int> oscParseLeadingPsCode(char*& cursor)
{
    char* p = cursor;
    int ps = 0;
    bool sawDigit = false;
    while (*p >= '0' && *p <= '9') {
        ps = Ubiquitty::decimalAppendSaturated(ps, *p - '0', 65535);
        p++;
        sawDigit = true;
    }
    // An omitted number is 0, as xterm reads it (misc.c, do_osc): `OSC ; Pt`
    // is OSC 0. With neither a number nor a separator there is no code at all.
    if (!sawDigit && *p != ';')
        return std::nullopt;
    if (*p == ';') {
        p++;
    } else if (*p != 0) {
        // The number is followed by garbage that isn't a separator —
        // malformed OSC. (Bare-numeric form is fine; the accumulator
        // NUL-terminates the payload, so *p == 0 here.)
        return std::nullopt;
    }
    cursor = p;
    return ps;
}

// ---- OSC dispatch table ---------------------------------------- *
//
// Sorted-by-Ps array of (code, handler) entries. Each oscH_* wrapper is a thin
// forwarder: it applies the per-feature security gate (allowTitle/Color/Font
// Operations) and calls the cold body in its capability slice. Keeping the
// wrappers adjacent to the table preserves the 1-to-1 dispatch surface (same
// pattern as csiHandlers / escapeHandlers / dcsHandlers in VT.cpp).
//
// The type is VT::Parser.h's, included above. A second `using OSCHandler` sat
// here saying the same thing, and this file did not include the header that
// declares it — so the two agreed only for as long as nobody edited one.
using VT::Parser::OSCHandler;

static int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Title mode 0 (CSI > 0 t) makes a label payload a hex string; an odd length or
// a non-hex digit leaves it alone rather than dropping the label.
static std::string decodeHexLabel(std::string_view src)
{
    if (src.empty() || (src.size() % 2) != 0)
        return std::string(src);
    std::string out;
    out.reserve(src.size() / 2);
    for (std::size_t i = 0; i < src.size(); i += 2) {
        const int high = hexDigitValue(src[i]);
        const int low = hexDigitValue(src[i + 1]);
        if (high < 0 || low < 0)
            return std::string(src);
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

// OSC 0 sets both labels, OSC 1 the shouldSetIcon name, OSC 2 the shouldSetWindow title.
// Which label an OSC 0/1/2 payload is for. A pair of flags spelled the three
// cases as `(true, false)` at the call, where the reader has to know which
// position is the icon.
enum class LabelTarget : uint8_t {
    IconName,
    WindowTitle,
    Both,
};

static void setLabels(Ubiquitty::Terminal& t, std::string_view p, LabelTarget target)
{
    // Both stages copied unconditionally: the payload into `decoded` even when
    // no hex decode was asked for, then every byte again into `filtered` even
    // when the filter had nothing to drop. A label arrives on the parser's
    // bytes and the setters take a view, so the ordinary label — no hex mode,
    // no control bytes — now reaches them without an allocation in between.
    std::string hexDecoded;
    std::string_view source = p;
    if (t.titleModes & 0x01u) {
        hexDecoded = decodeHexLabel(p);
        source = hexDecoded;
    }
    std::string filteredStorage;
    if (labelHasFilteredByte(source)) {
        filteredStorage = filteredLabelUTF8(source);
        source = filteredStorage;
    }
    if (target == LabelTarget::IconName || target == LabelTarget::Both)
        Ubiquitty::setIconNameUTF8String(t, source);
    if (target == LabelTarget::WindowTitle || target == LabelTarget::Both) {
        Ubiquitty::setTitleUTF8String(t, source);
        t.titleChanged = true;
    }
}
static void oscH_title(Ubiquitty::Terminal& t, std::string_view payload) { setLabels(t, payload, LabelTarget::Both); }
static void oscH_iconName(Ubiquitty::Terminal& t, std::string_view payload) { setLabels(t, payload, LabelTarget::IconName); }
static void oscH_windowTitle(Ubiquitty::Terminal& t, std::string_view payload) { setLabels(t, payload, LabelTarget::WindowTitle); }
static void oscH_indexedColor(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleIndexedColor(t, payload);
}
static void oscH_specialColor(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleSpecialColor(t, payload);
}
static void oscH_resetIndexedColor(Ubiquitty::Terminal& t, std::string_view payload)
{
    resetIndexedColor(t, payload);
}
static void oscH_resetSpecialColor(Ubiquitty::Terminal& t, std::string_view payload)
{
    resetSpecialColor(t, payload);
}
// OSC 6 is two unrelated sequences. iTerm2's tab colour always opens with
// `1;bg;`, which no file URL or path the current-file form carries does, so the
// prefix decides, and a malformed tab colour is never stored as a URL.
static void oscH_currentFileURL(Ubiquitty::Terminal& t, std::string_view payload)
{
    static constexpr std::string_view tabColorPrefix = "1;bg;";
    if (payload.starts_with(tabColorPrefix))
        handleTabColor(t, payload.substr(tabColorPrefix.size()));
    else
        handleCurrentFileURL(t, payload);
}
static void oscH_workingDirectory(Ubiquitty::Terminal& t, std::string_view payload) { handleWorkingDirectory(t, payload); }
static void oscH_hyperlink(Ubiquitty::Terminal& t, std::string_view payload) { handleHyperlink(t, payload); }
static void oscH_progressOrNotification(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC9(t, payload); }
static void oscH_tabStatus(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC21337(t, payload); }
// Parse a "W:H" aspect: two positive decimals split by a single ':', whole token
// consumed. Returns false (and leaves outputs untouched) on any deviation.
// The two sides of a ratio, which mean nothing apart.
struct AspectRatio {
    int width;
    int height;
};

[[nodiscard]] static std::optional<AspectRatio> parseFullscreenAspectRatio(std::string_view text)
{
    // The view's end is the terminator here: a value bounded by ';' inside the
    // payload has no NUL of its own.
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos)
        return std::nullopt;
    const std::string_view widthText = text.substr(0, colon);
    const std::string_view heightText = text.substr(colon + 1);

    // An empty side needs no test of its own: its digit loop runs zero times and
    // leaves the value at 0, which the positivity check below refuses. A guard
    // here would be one no input can reach.
    int width = 0, height = 0;
    for (const char digit : widthText) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        width = Ubiquitty::decimalAppendSaturated(width, digit - '0', 65535);
    }
    for (const char digit : heightText) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        height = Ubiquitty::decimalAppendSaturated(height, digit - '0', 65535);
    }
    if (width <= 0 || height <= 0)
        return std::nullopt;
    return AspectRatio { width, height };
}

// Parse exactly six hex digits into 0xRRGGBB. Nothing on any other length or a
// non-hex byte.
[[nodiscard]] static std::optional<uint32_t> parseFullscreenHexColor(std::string_view text)
{
    uint32_t color = 0;
    int digits = 0;
    for (const char c : text) {
        const int nibble = hexDigit(c);
        if (nibble < 0)
            return std::nullopt;
        ++digits;
        color = (color << 4) | static_cast<uint32_t>(nibble);
    }
    if (digits != 6)
        return std::nullopt;
    return color;
}

// OSC 7402 [ ; key=value ]* — the DECSET 7402 fullscreen-content display controller.
// One feature, one number: the mode sits in the 7400 block rather than beside VTE's
// 2500/2501, and the OSC takes the same number, so a reader is not asked to remember an
// arbitrary pair. 2501 is not an alias for it: no other implementation speaks that
// number, so there is nothing for an alias to keep working.
// Each OSC fully re-specifies the display state: named keys override, unnamed keys take
// their defaults (a bare OSC 7402 therefore resets to Contain + black-occlude). Keys
// (lowercase):
//   fit=contain|stretch|native|scale   how content maps onto the drawable (default contain)
//   ar=W:H                        target display aspect for Stretch (REQUIRED there),
//                                 positive ints, ratio within [1/8, 8]
//   scale=N                       source-px -> device-px integer multiplier for Scale
//                                 (REQUIRED there), positive int clamped to 64x
//   fill=RRGGBB | pass            letterbox fill: a 6-hex-digit color (default/empty =
//                                 000000 black) or "pass" (passthrough, terminal content
//                                 visible around the image)
// Unknown keys are ignored (forward-compat). A key with a malformed value, a token
// without '=', Stretch without a valid ar, or Scale without a valid scale makes the whole
// command malformed and leaves the current selection intact (attacker-controlled bytes: fail safe, never
// partially applied). Stored on Terminal (round-trips) and forwarded to the frontend,
// which applies it while edge-to-edge is active.
static void oscH_fullscreenContentDisplay(Ubiquitty::Terminal& t, std::string_view payload)
{
    Ubiquitty::FullscreenContentDisplay display; // defaults: Contain fit, black-occlude
    bool sawAspect = false;
    bool sawScale = false;

    // Each ';'-separated token is "key=value", bounded by measuring rather than
    // by writing a NUL over the separator: the payload is the parser's own
    // buffer, so a tokenizer that punched terminators into it would leave the
    // bytes a later reader sees rewritten.
    std::string_view rest = payload;
    while (!rest.empty()) {
        const std::size_t semicolon = rest.find(';');
        const std::string_view token = rest.substr(0, semicolon);
        rest = (semicolon == std::string_view::npos) ? std::string_view() : rest.substr(semicolon + 1);

        const std::size_t equals = token.find('=');
        if (equals == std::string_view::npos)
            return; // a token without '=' is malformed
        const std::string_view key = token.substr(0, equals);
        const std::string_view value = token.substr(equals + 1);

        if (key == "fit") {
            if (value == "contain")
                display.fit = FullscreenContentFit::Contain;
            else if (value == "stretch")
                display.fit = FullscreenContentFit::Stretch;
            else if (value == "native")
                display.fit = FullscreenContentFit::Native;
            else if (value == "scale")
                display.fit = FullscreenContentFit::Scale;
            else
                return; // unknown fit value
        } else if (key == "ar") {
            const std::optional<AspectRatio> ratio = parseFullscreenAspectRatio(value);
            if (!ratio)
                return;
            if (ratio->width > 8 * ratio->height || ratio->height > 8 * ratio->width)
                return; // ratio outside [1/8, 8] — absurd, ignore the whole command
            display.aspectWidth = static_cast<uint16_t>(ratio->width);
            display.aspectHeight = static_cast<uint16_t>(ratio->height);
            sawAspect = true;
        } else if (key == "fill") {
            if (value == "pass") {
                display.letterbox = FullscreenContentLetterbox::Passthrough;
                display.letterboxColor = 0;
            } else if (value.empty()) {
                display.letterbox = FullscreenContentLetterbox::Color; // empty ⇒ black
                display.letterboxColor = 0;
            } else {
                const std::optional<uint32_t> color = parseFullscreenHexColor(value);
                if (!color)
                    return;
                display.letterbox = FullscreenContentLetterbox::Color;
                display.letterboxColor = *color;
            }
        } else if (key == "scale") {
            int multiplier = 0;
            bool sawDigit = false;
            for (const char digit : value) {
                if (digit < '0' || digit > '9')
                    return; // scale must be a positive decimal integer
                multiplier = Ubiquitty::decimalAppendSaturated(multiplier, digit - '0', 64); // clamp at 64x (absurd beyond)
                sawDigit = true;
            }
            if (!sawDigit || multiplier <= 0)
                return; // scale requires a positive integer
            display.scaleMultiplier = static_cast<uint16_t>(multiplier);
            sawScale = true;
        }
        // Unknown key: ignore (forward-compat).
    }

    if (display.fit == FullscreenContentFit::Stretch && !sawAspect)
        return; // Stretch requires ar=W:H
    if (display.fit == FullscreenContentFit::Scale && !sawScale)
        return; // Scale requires scale=N

    t.fullscreenContentDisplay = display;
    // Same escape as the DECSET 7402 that turns the mode on, so the same gate.
    if (auto* c = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::WindowMutate))
        c->setFullscreenContentDisplay(display);
}
// OSC 110-119 take no data. xterm treats a reset that carries any as a
// malformed control and ignores it (misc.c:4157, "do_osc found unwanted
// data"), so "OSC 110 ; 1" leaves the foreground as it is.
static bool resetCarriesData(std::string_view payload) { return !payload.empty(); }
static void oscH_defaultForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 10);
}
static void oscH_defaultBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 11);
}
static void oscH_cursorColor(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 12);
}
static void oscH_clipboard(Ubiquitty::Terminal& t, std::string_view payload) { handleClipboard(t, payload); }
static void oscH_extendedClipboard(Ubiquitty::Terminal& t, std::string_view payload) { handleExtendedClipboard(t, payload); }
static void oscH_resetDefaultForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetDefaultForeground(t);
}
static void oscH_resetDefaultBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetDefaultBackground(t);
}
static void oscH_resetCursorColor(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetCursorColor(t);
}
static void oscH_shellIntegrationMark(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC133(t, payload); }
static void oscH_selectionBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 17);
}
static void oscH_selectionForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 19);
}
static void oscH_mousePointerShape(Ubiquitty::Terminal& t, std::string_view payload) { handleMousePointerShape(t, payload); }
static void oscH_resetSelectionBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetSelectionBackground(t);
}
static void oscH_resetSelectionForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetSelectionForeground(t);
}
static void oscH_mousePointerForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 13);
}
static void oscH_mousePointerBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 14);
}
static void oscH_tektronixForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 15);
}
static void oscH_tektronixBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 16);
}
static void oscH_tektronixCursor(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleDynamicColors(t, payload, 18);
}
static void oscH_resetMousePointerForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetMousePointerForeground(t);
}
static void oscH_resetMousePointerBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetMousePointerBackground(t);
}
static void oscH_resetTektronixForeground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetTektronixForeground(t);
}
static void oscH_resetTektronixBackground(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetTektronixBackground(t);
}
static void oscH_resetTektronixCursor(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (resetCarriesData(payload))
        return;
    resetTektronixCursor(t);
}
// ---- XTQALLOWED, XTQDISALLOWED, XTQALLOWABLE (OSC 60, 61, 62) ----------- *
//
// xterm's queries of its feature gates (ctlseqs, OSC Ps ; Pt ST; misc.c
// report_allowed_ops, charproc.c unparse_disallowed_ops and
// unparse_allowable_ops). The answers are read from the gates Core enforces, so
// a program learns what the terminal will in fact do: a category's flag and the
// capability policy at the current origin. allowPasteControls is not a
// category here: the flag of that name gates bracketed paste, not the controls
// xterm's resource admits into a paste.

namespace {

enum class AllowCategory : uint8_t {
    Color,
    Font,
    Mouse,
    TerminalCapability,
    Window,
};

struct AllowableOperation {
    std::string_view name;
    bool (*isAllowed)(Ubiquitty::Terminal&);
};

bool colorSetAllowed(Ubiquitty::Terminal& t) { return t.allowColorOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetColor); }
bool colorReadAllowed(Ubiquitty::Terminal& t) { return t.allowColorOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ColorRead); }
bool fontSetAllowed(Ubiquitty::Terminal& t) { return t.allowFontOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetFont); }
bool fontReadAllowed(Ubiquitty::Terminal& t) { return t.allowFontOperations; }
bool mouseAllowed(Ubiquitty::Terminal& t) { return t.allowMouseOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::InputModeReport); }
bool terminalCapabilityAllowed(Ubiquitty::Terminal& t) { return t.allowTerminalCapabilityOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::TerminfoRead); }
bool windowMutationAllowed(Ubiquitty::Terminal& t) { return t.allowWindowOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::WindowMutate); }
bool windowStateAllowed(Ubiquitty::Terminal& t) { return t.allowWindowOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::StatusReport); }
bool windowGeometryAllowed(Ubiquitty::Terminal& t) { return t.allowWindowOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::WindowGeometryRead); }
bool windowSizeAllowed(Ubiquitty::Terminal& t) { return t.allowWindowOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::TerminalSizeRead); }
bool windowTitleReadAllowed(Ubiquitty::Terminal& t) { return t.allowWindowOperations && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::WindowTitleRead); }
bool titleStackAllowed(Ubiquitty::Terminal& t) { return t.allowWindowOperations; }
// DECSLPP and DECCOLM are page functions, outside the window-operation gate.
bool pageAllowed(Ubiquitty::Terminal&) { return true; }
// Core sets no X properties and has no XTCHECKSUM.
bool neverAllowed(Ubiquitty::Terminal&) { return false; }
bool checksumAllowed(Ubiquitty::Terminal& t) { return Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ScreenContentRead); }
// A read the policy would ask about is not allowed: until the user answers, it is refused.
bool selectionReadAllowed(Ubiquitty::Terminal& t) { return t.isOSC52Allowed && t.osc52ClipboardReadPolicy == ClipboardReadPolicy::Allow && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ClipboardRead); }
bool selectionWriteAllowed(Ubiquitty::Terminal& t) { return t.isOSC52Allowed && t.osc52ClipboardWritePolicy != ClipboardWritePolicy::Deny && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ClipboardWrite); }

constexpr AllowableOperation colorOperations[] = { { "SetColor", colorSetAllowed }, { "GetColor", colorReadAllowed }, { "GetAnsiColor", colorReadAllowed } };
constexpr AllowableOperation fontOperations[] = { { "SetFont", fontSetAllowed }, { "GetFont", fontReadAllowed } };
constexpr AllowableOperation mouseOperations[] = { { "X10", mouseAllowed }, { "Locator", mouseAllowed }, { "VT200Click", mouseAllowed }, { "VT200Hilite", mouseAllowed }, { "AnyButton", mouseAllowed }, { "AnyEvent", mouseAllowed }, { "FocusEvent", mouseAllowed }, { "Extended", mouseAllowed }, { "SGR", mouseAllowed }, { "URXVT", mouseAllowed }, { "AlternateScroll", mouseAllowed } };
constexpr AllowableOperation terminalCapabilityOperations[] = { { "SetTcap", terminalCapabilityAllowed }, { "GetTcap", terminalCapabilityAllowed } };
// xterm's tblWindowOps, in its order, without StatusLine: Core's status line is
// not behind the window-operation gate.
constexpr AllowableOperation windowOperations[] = {
    { "RestoreWin", windowMutationAllowed },
    { "MinimizeWin", windowMutationAllowed },
    { "SetWinPosition", windowMutationAllowed },
    { "SetWinSizePixels", windowMutationAllowed },
    { "RaiseWin", windowMutationAllowed },
    { "LowerWin", windowMutationAllowed },
    { "RefreshWin", windowMutationAllowed },
    { "SetWinSizeChars", windowMutationAllowed },
    { "MaximizeWin", windowMutationAllowed },
    { "FullscreenWin", windowMutationAllowed },
    { "GetWinState", windowStateAllowed },
    { "GetWinPosition", windowGeometryAllowed },
    { "GetWinSizePixels", windowGeometryAllowed },
    { "GetWinSizeChars", windowSizeAllowed },
    { "GetScreenSizeChars", windowSizeAllowed },
    { "GetIconTitle", windowTitleReadAllowed },
    { "GetWinTitle", windowTitleReadAllowed },
    { "PushTitle", titleStackAllowed },
    { "PopTitle", titleStackAllowed },
    { "SetWinLines", pageAllowed },
    { "ColumnMode", pageAllowed },
    { "GetChecksum", checksumAllowed },
    { "SetChecksum", neverAllowed },
    { "GetSelection", selectionReadAllowed },
    { "SetSelection", selectionWriteAllowed },
    { "SetXprop", neverAllowed },
};
// The order xterm's disallowed report walks them in: its flag array is indexed
// by the operation's code (ptyx.h, ewSetWinLines onward), which puts SetXprop
// and the selection ahead of the checksums.
constexpr std::string_view windowDisallowedOrder[] = { "RestoreWin", "MinimizeWin", "SetWinPosition", "SetWinSizePixels", "RaiseWin", "LowerWin", "RefreshWin", "SetWinSizeChars", "MaximizeWin", "FullscreenWin", "GetWinState", "GetWinPosition", "GetWinSizePixels", "GetWinSizeChars", "GetScreenSizeChars", "GetIconTitle", "GetWinTitle", "PushTitle", "PopTitle", "SetWinLines", "ColumnMode", "SetXprop", "GetSelection", "SetSelection", "GetChecksum", "SetChecksum" };

std::span<const AllowableOperation> operationsOf(AllowCategory category)
{
    switch (category) {
    case AllowCategory::Color:
        return colorOperations;
    case AllowCategory::Font:
        return fontOperations;
    case AllowCategory::Mouse:
        return mouseOperations;
    case AllowCategory::TerminalCapability:
        return terminalCapabilityOperations;
    case AllowCategory::Window:
        return windowOperations;
    }
    return {};
}

// A category by its resource name, compared without regard to case, as xterm
// compares it; allowWinOps is its alias of allowWindowOps.
std::optional<AllowCategory> allowCategoryNamed(std::string_view name)
{
    static constexpr std::pair<std::string_view, AllowCategory> names[] = {
        { "allowColorOps", AllowCategory::Color },
        { "allowFontOps", AllowCategory::Font },
        { "allowMouseOps", AllowCategory::Mouse },
        { "allowTcapOps", AllowCategory::TerminalCapability },
        { "allowWinOps", AllowCategory::Window },
        { "allowWindowOps", AllowCategory::Window },
    };
    const auto equalIgnoringCase = [](std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b, [](char x, char y) { return Ubiquitty::Unicode::toASCIILower(x) == Ubiquitty::Unicode::toASCIILower(y); });
    };
    for (const auto& [candidate, category] : names) {
        if (equalIgnoringCase(candidate, name))
            return category;
    }
    return std::nullopt;
}

// The window category's list runs past both a ReplyBuilder's frame and the
// fixed reply buffer, so a reply that long takes the bulk channel. Its length
// is bounded by the fixed tables above.
void finishAllowReply(Ubiquitty::Terminal& t, std::string& reply)
{
    reply += Ubiquitty::oscReplyTerminator(t);
    const std::span<const char> bytes(reply.data(), reply.size());
    if (reply.size() <= static_cast<std::size_t>(Ubiquitty::Terminal::outputReplyPressureBytes))
        Ubiquitty::respond(t, Ubiquitty::Capability::None, bytes);
    else
        (void)Ubiquitty::respondBulk(t, Ubiquitty::Capability::None, bytes);
}

} // namespace

// OSC 60: the categories whose flag is on, in xterm's order. Data makes the
// request malformed, and it is not answered (misc.c, do_osc).
static void oscH_allowedOperations(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (!payload.empty())
        return;
    const std::pair<std::string_view, bool> categories[] = {
        { "allowColorOps", t.allowColorOperations },
        { "allowFontOps", t.allowFontOperations },
        { "allowMouseOps", t.allowMouseOperations },
        { "allowTcapOps", t.allowTerminalCapabilityOperations },
        { "allowTitleOps", t.allowTitleOperations },
        { "allowWindowOps", t.allowWindowOperations },
    };
    std::string reply = "\033]60";
    char delimiter = ';';
    for (const auto& [name, isAllowed] : categories) {
        if (!isAllowed)
            continue;
        reply += delimiter;
        reply += name;
        delimiter = ',';
    }
    finishAllowReply(t, reply);
}

// OSC 61 ; category: the category's operations that are refused now. OSC 62 ;
// category: every operation the category gates. A name that is no category,
// a second name included, gets the bare code. With no data there is no reply.
enum class CategoryReport : uint8_t {
    Disallowed,
    Allowable,
};

static void reportCategoryOperations(Ubiquitty::Terminal& t, std::string_view payload, int code, CategoryReport report)
{
    if (payload.empty())
        return;
    std::string reply = "\033]" + std::to_string(code);
    if (const std::optional<AllowCategory> category = allowCategoryNamed(payload)) {
        const std::span<const AllowableOperation> operations = operationsOf(*category);
        char delimiter = ';';
        const auto put = [&](const AllowableOperation& operation) {
            reply += delimiter;
            reply += operation.name;
            delimiter = ',';
        };
        if (report == CategoryReport::Allowable) {
            for (const AllowableOperation& operation : operations)
                put(operation);
        } else if (*category == AllowCategory::Window) {
            for (const std::string_view name : windowDisallowedOrder) {
                const auto found = std::ranges::find(operations, name, &AllowableOperation::name);
                if (found != operations.end() && !found->isAllowed(t))
                    put(*found);
            }
        } else {
            for (const AllowableOperation& operation : operations) {
                if (!operation.isAllowed(t))
                    put(operation);
            }
        }
    }
    finishAllowReply(t, reply);
}

static void oscH_disallowedOperations(Ubiquitty::Terminal& t, std::string_view payload) { reportCategoryOperations(t, payload, 61, CategoryReport::Disallowed); }
static void oscH_allowableOperations(Ubiquitty::Terminal& t, std::string_view payload) { reportCategoryOperations(t, payload, 62, CategoryReport::Allowable); }

// The App ID specification's OSC 176. "OSC 176 ; appID ST" sets "the desired
// App ID / Window Class of the running app", and an empty appID "unsets its
// app ID"; "OSC 176 ; ? ST" is answered "OSC 176 ; appID ST" with the current
// one, empty while none is set, so a program that saves it on start and sets
// it back on exit restores the unset state too. The reply ends in ST, the form
// the specification gives it. An ID is not a title, but it is window chrome a
// program reads back, so the reply takes a title report's gate, the window
// operations and the title-read capability, and stored text reaches it only
// through sanitizedReplyLabel. An ID longer than the 255 bytes kept for it is
// not taken; the specification expects App IDs to stay below the 255
// characters desktop application names are limited to.
static void oscH_applicationIdentifier(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (payload == "?") {
        if (!t.allowWindowOperations)
            return;
        const std::string reply = "\033]176;" + Ubiquitty::sanitizedReplyLabel(t.applicationIdentifier.view()) + "\033\\";
        Ubiquitty::respond(t, Ubiquitty::Capability::WindowTitleRead, reply.c_str());
        return;
    }
    if (payload.size() > Ubiquitty::Terminal::applicationIdentifierCapacity)
        return;
    // A refused allocation keeps the ID already set.
    (void)t.applicationIdentifier.tryAssign(payload);
}

// urxvt's version query, OSC 702 ; ?: rxvt(7) answers "rxvt-unicode, the
// resource name, the major and minor version numbers, e.g. ESC ] 702 ;
// rxvt-unicode ; urxvt ; 7 ; 4 ST". The first field names the protocol, so it
// is the constant; the resource name and the version are this terminal's, the
// major and minor numbers of the build version XTVERSION reports whole. The
// reply ends in ST, the form the manual gives it.
static void oscH_urxvtVersion(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (payload != "?")
        return;
    // A build version opens with `MAJOR.MINOR.`, the unversioned one included.
    const std::string_view version = Ubiquitty::buildVersion();
    const std::size_t majorEnd = version.find('.');
    const std::size_t minorEnd = majorEnd == std::string_view::npos ? std::string_view::npos : version.find('.', majorEnd + 1);
    if (minorEnd == std::string_view::npos)
        return;
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033]702;rxvt-unicode;ubiquitty;");
    reply.put(version.substr(0, majorEnd));
    reply.put(';');
    reply.put(version.substr(majorEnd + 1, minorEnd - majorEnd - 1));
    reply.put("\033\\");
    if (reply.isSendable())
        Ubiquitty::respond(t, Ubiquitty::Capability::DeviceAttributesRead, reply.span());
}

// urxvt's cell-size query, OSC 776 ; ?: "replies with ESC ] 776 ; cell-width ;
// cell-height ; font-ascent ST", in pixels, from the cell the frontend
// publishes for XTWINOPS 16 and the baseline its typography reports. Without a
// published cell the nominal 8x16 cell stands, as it does for CSI 16 t, with the
// ascent of the Unifont cell that size is: 14 of its 16 rows.
static void oscH_urxvtCellSize(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (payload != "?" || !windowGeometryAllowed(t))
        return;
    const int width = t.cellPixelWidth ? t.cellPixelWidth : 8;
    const int height = t.cellPixelHeight ? t.cellPixelHeight : 16;
    int ascent = (height * 14 + 8) / 16;
    if (const auto* facts = t.typography.facts.get(); facts && facts->baseline && facts->cellHeight)
        ascent = static_cast<int>((static_cast<long>(facts->baseline) * height + facts->cellHeight / 2) / facts->cellHeight);
    ReplyBuilder reply;
    reply.put("\033]776;");
    reply.putDecimal(width);
    reply.put(';');
    reply.putDecimal(height);
    reply.put(';');
    reply.putDecimal(std::clamp(ascent, 0, height));
    reply.put(Ubiquitty::oscReplyTerminator(t));
    Ubiquitty::respond(t, Ubiquitty::Capability::WindowGeometryRead, reply.span());
}

static void oscH_kittyNotification(Ubiquitty::Terminal& t, std::string_view payload) { handleKittyNotification(t, payload); }
static void oscH_textSize(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC66(t, payload); }
static void oscH_dragDrop(Ubiquitty::Terminal& t, std::string_view payload) { DragDrop::receive(t, payload); }
static void oscH_vsCodeShellIntegration(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC633(t, payload); }
static void oscH_urxvtModule(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC777(t, payload); }
static void oscH_iTerm2(Ubiquitty::Terminal& t, std::string_view payload) { handleOSC1337(t, payload); }
static void oscH_font(Ubiquitty::Terminal& t, std::string_view payload)
{
    handleFont(t, payload);
}
static void oscH_fontSize(Ubiquitty::Terminal& t, std::string_view payload) { handleFontSize(t, payload); }
static void oscH_fontAndWindowSize(Ubiquitty::Terminal& t, std::string_view payload) { handleFontAndWindowSize(t, payload); }

static void oscH_pushColors(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (payload.empty())
        VT::pushColors(t, 0);
}

static void oscH_popColors(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (payload.empty())
        VT::popColors(t, 0);
}

// The capability a code's whole effect hangs on, declared per row.
//
// A code the terminal must not act on at all is refused before its handler runs,
// so the obligation is stated once beside the code instead of repeated as the
// first two lines of every wrapper. Twenty-six wrappers opened with one of these
// tests, which is twenty-six chances to write the twenty-seventh without it.
//
// A gate that shapes a handler's behaviour rather than deciding whether it runs
// stays inside it: OSC 52 reads under one policy and writes under another, so no
// single answer here could stand for both.
enum class OSCGate : uint8_t {
    Unconditional,
    ColorOperations,
    FontOperations,
    TitleOperations,
};

struct OSCEntry {
    int ps;
    OSCHandler fn;
    OSCGate gate;
};

[[nodiscard]] static bool oscGateAllows(const Ubiquitty::Terminal& t, OSCGate gate)
{
    switch (gate) {
    case OSCGate::Unconditional:
        return true;
    case OSCGate::ColorOperations:
        return t.allowColorOperations;
    case OSCGate::FontOperations:
        return t.allowFontOperations;
    case OSCGate::TitleOperations:
        return t.allowTitleOperations;
    }
    return false;
}

// Sorted by Ps ascending — the lookup below stops as soon as it passes the code
// it wants, and the static_assert after the table is what keeps that true. So the
// order is the search's, not a grouping, and the standard each code comes from is
// spelled in the comments instead. That origin is the thing a reader needs: it
// says which document defines the payload, and codes that sit next to each other
// here routinely come from different ones.
//
//   xterm      0-2, 4-5, 10-19, 50, 52, 104-105, 110-119 (50 is rxvt's, adopted)
//   de facto   7 working directory, 8 hyperlinks (the OSC 8 gist)
//   iTerm2     9 notification (ConEmu's progress shares the code), 1337,
//              21337 session status
//   kitty      99 notification, 30001/30101 color stack
//   FinalTerm  133 prompt marks, as adopted by everything since
//   App ID     176 the App ID specification's window identifier
//   VS Code    633 shell integration
//   urxvt      702 version, 776 cell size, 777 module dispatch
//   mintty     7770 font size, 7777 font and window size
//   Ubiquitty  7402 fullscreen content display, 7404 rich text blocks
//
// 0/1/2 are title aliases: OSC 0 sets shouldSetIcon + title, 1 the shouldSetIcon
// only, 2 the title only. We don't model the shouldSetIcon distinction, so all
// three set the title.
static constexpr OSCEntry oscHandlers[] = {
    // Labels.
    { 0, oscH_title, OSCGate::TitleOperations },
    { 1, oscH_iconName, OSCGate::TitleOperations },
    { 2, oscH_windowTitle, OSCGate::TitleOperations },
    // Palette: an indexed or special slot by number.
    { 4, oscH_indexedColor, OSCGate::ColorOperations },
    { 5, oscH_specialColor, OSCGate::ColorOperations },
    // Session metadata, each stored on the Terminal for a consumer to read.
    { 6, oscH_currentFileURL, OSCGate::Unconditional },
    { 7, oscH_workingDirectory, OSCGate::Unconditional },
    { 8, oscH_hyperlink, OSCGate::Unconditional },
    { 9, oscH_progressOrNotification, OSCGate::Unconditional },
    // Dynamic colours, 10 upward. Each sets one named slot, and 110 upward reset
    // the matching one — the offset between a setter and its reset is 100.
    { 10, oscH_defaultForeground, OSCGate::ColorOperations },
    { 11, oscH_defaultBackground, OSCGate::ColorOperations },
    { 12, oscH_cursorColor, OSCGate::ColorOperations },
    { 13, oscH_mousePointerForeground, OSCGate::ColorOperations },
    { 14, oscH_mousePointerBackground, OSCGate::ColorOperations },
    { 15, oscH_tektronixForeground, OSCGate::ColorOperations },
    { 16, oscH_tektronixBackground, OSCGate::ColorOperations },
    { 17, oscH_selectionBackground, OSCGate::ColorOperations },
    { 18, oscH_tektronixCursor, OSCGate::ColorOperations },
    { 19, oscH_selectionForeground, OSCGate::ColorOperations },
    { 21, handleKittyColors, OSCGate::ColorOperations },
    // Window and device state.
    { 22, oscH_mousePointerShape, OSCGate::Unconditional },
    { 50, oscH_font, OSCGate::FontOperations },
    { 52, oscH_clipboard, OSCGate::Unconditional },
    // xterm's queries of its feature gates.
    { 60, oscH_allowedOperations, OSCGate::Unconditional },
    { 61, oscH_disallowedOperations, OSCGate::Unconditional },
    { 62, oscH_allowableOperations, OSCGate::Unconditional },
    { 66, oscH_textSize, OSCGate::Unconditional },
    { 72, oscH_dragDrop, OSCGate::Unconditional },
    { 99, oscH_kittyNotification, OSCGate::Unconditional },
    // Resets. 104/105 clear a palette slot; 110-119 mirror 10-19.
    { 104, oscH_resetIndexedColor, OSCGate::ColorOperations },
    { 105, oscH_resetSpecialColor, OSCGate::ColorOperations },
    { 110, oscH_resetDefaultForeground, OSCGate::ColorOperations },
    { 111, oscH_resetDefaultBackground, OSCGate::ColorOperations },
    { 112, oscH_resetCursorColor, OSCGate::ColorOperations },
    { 113, oscH_resetMousePointerForeground, OSCGate::ColorOperations },
    { 114, oscH_resetMousePointerBackground, OSCGate::ColorOperations },
    { 115, oscH_resetTektronixForeground, OSCGate::ColorOperations },
    { 116, oscH_resetTektronixBackground, OSCGate::ColorOperations },
    { 117, oscH_resetSelectionBackground, OSCGate::ColorOperations },
    { 118, oscH_resetTektronixCursor, OSCGate::ColorOperations },
    { 119, oscH_resetSelectionForeground, OSCGate::ColorOperations },
    // Shell integration, then per-emulator extensions.
    { 133, oscH_shellIntegrationMark, OSCGate::Unconditional },
    { 176, oscH_applicationIdentifier, OSCGate::Unconditional },
    { 633, oscH_vsCodeShellIntegration, OSCGate::Unconditional },
    { 666, handleOSC666, OSCGate::Unconditional },
    { 702, oscH_urxvtVersion, OSCGate::Unconditional },
    { 776, oscH_urxvtCellSize, OSCGate::Unconditional },
    { 777, oscH_urxvtModule, OSCGate::Unconditional },
    { 1337, oscH_iTerm2, OSCGate::Unconditional },
    { 3008, handleContext, OSCGate::Unconditional },
    // kitty extended clipboard; its gates are inside the handler, like 52's.
    { 5522, oscH_extendedClipboard, OSCGate::Unconditional },
    { 7402, oscH_fullscreenContentDisplay, OSCGate::Unconditional },
    // Its capabilities are checked inside: placing is RichTextRender, a query's reply TextMeasureRead.
    { 7404, RichTextBlocks::handle, OSCGate::Unconditional },
    { 7501, handleProgramStatus, OSCGate::Unconditional },
    { 7770, oscH_fontSize, OSCGate::FontOperations },
    { 7777, oscH_fontAndWindowSize, OSCGate::FontOperations },
    { 21337, oscH_tabStatus, OSCGate::Unconditional },
    { 30001, oscH_pushColors, OSCGate::ColorOperations },
    { 30101, oscH_popColors, OSCGate::ColorOperations },
};

template<size_t N> constexpr bool oscTableSorted(const OSCEntry (&a)[N], size_t i = 1) { return (i >= N) || (a[i - 1].ps < a[i].ps && oscTableSorted(a, i + 1)); }
static_assert(oscTableSorted(oscHandlers), "oscHandlers must be sorted by Ps");

// Linear scan with sorted-table early-out. The table is small enough
// that ~4 binary-search branch-mispredicts cost more than a tight
// cache-friendly loop, and the high-traffic codes (0/1/2 title, 7 currentDirectory,
// 9, 10/11/12 colours) cluster near the front so the common path hits
// in 1-3 iterations. The static_assert above keeps the sorted
// invariant intact — change it and the early-out breaks.
static const OSCEntry* oscLookup(int ps)
{
    for (const auto& oscHandler : oscHandlers) {
        if (oscHandler.ps == ps)
            return &oscHandler;
        if (oscHandler.ps > ps)
            break; // sorted — won't find it later
    }
    return nullptr;
}

// Exposed via VT::Parser::findOSC(ps); see VT/Parser.h.
OSCHandler findOSCHandler(int ps)
{
    const OSCEntry* e = oscLookup(ps);
    return e ? e->fn : nullptr;
}

void dispatchFixedPalette(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (!oscGateAllows(t, OSCGate::ColorOperations))
        return;
    if (payload == "R") {
        if (oscAllowed(t, 104) && Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetColor))
            resetIndexedColors(t);
        return;
    }
    if (payload.size() != 8 || payload.front() != 'P')
        return;
    for (const char digit : payload.substr(2)) {
        if (hexDigit(digit) < 0)
            return;
    }

    const int index = hexDigit(payload[1]);
    int code = 4;
    std::array<char, 11> parameters;
    std::size_t length = 0;
    if (index >= 0) {
        if (index >= 10)
            parameters[length++] = '1';
        parameters[length++] = static_cast<char>('0' + index % 10);
        parameters[length++] = ';';
    } else {
        switch (payload[1]) {
        case 'g':
            code = 10;
            break;
        case 'h':
            code = 11;
            break;
        case 'i':
            code = 5;
            parameters[length++] = '0';
            parameters[length++] = ';';
            break;
        case 'j':
            code = 17;
            break;
        case 'k':
            code = 19;
            break;
        case 'l':
            code = 12;
            break;
        default:
            return;
        }
    }
    parameters[length++] = '#';
    for (const char digit : payload.substr(2))
        parameters[length++] = digit;
    if (oscAllowed(t, code))
        oscLookup(code)->fn(t, std::string_view(parameters.data(), length));
}

// dtterm's letter forms, which xterm takes after any digits (xterm misc.c, do_osc):
// `l` sets the window title and `L` the icon name to everything after the
// letter, `;` included, and `I` names an XPM file for the window's icon
// picture, which no window here shows. The title forms are gated as the OSC 2
// and OSC 1 they stand for. Returns whether the payload was one of them.
static bool dispatchDttermForm(Ubiquitty::Terminal& t)
{
    const std::string_view payload(t.parser.osc.data, static_cast<std::size_t>(t.parser.osc.length));
    std::size_t letterIndex = 0;
    int mode = 0;
    while (letterIndex < payload.size() && payload[letterIndex] >= '0' && payload[letterIndex] <= '9') {
        mode = Ubiquitty::decimalAppendSaturated(mode, payload[letterIndex] - '0', 65536);
        ++letterIndex;
    }
    if (mode > 65535 || letterIndex >= payload.size())
        return false;
    const char letter = payload[letterIndex];
    if (letter != 'l' && letter != 'L' && letter != 'I')
        return false;
    if (letter == 'I')
        return true;
    const int equivalentCode = letter == 'l' ? 2 : 1;
    if (oscAllowed(t, equivalentCode) && oscGateAllows(t, OSCGate::TitleOperations))
        setLabels(t, payload.substr(letterIndex + 1), letter == 'l' ? LabelTarget::WindowTitle : LabelTarget::IconName);
    return true;
}

void dispatch(Ubiquitty::Terminal& t)
{
    if (t.parser.osc.length > 0 && t.parser.osc.data[0] == 'P') {
        dispatchFixedPalette(t, std::string_view(t.parser.osc.data, static_cast<std::size_t>(t.parser.osc.length)));
        return;
    }
    if (dispatchDttermForm(t))
        return;
    char* p = t.parser.osc.data;
    const std::optional<int> code = oscParseLeadingPsCode(p);
    if (!code)
        return;
    const int ps = *code;
    // The length the accumulator counted, less what the code and its separator
    // consumed. Handed to every handler so none measures the bytes again.
    const std::string_view payload(p, static_cast<std::size_t>(t.parser.osc.length - static_cast<int>(p - t.parser.osc.data)));
    // Per-OSC allowlist: a blocked code is parsed + consumed cleanly but
    // never dispatched — no hook, no response. Empty lists (the default) pass
    // everything, so this is inert until a user opts into [security] osc_*.
    if (!oscAllowed(t, ps))
        return;
    const OSCEntry* e = oscLookup(ps);
    if (e) {
        if (oscGateAllows(t, e->gate))
            e->fn(t, payload);
        return;
    }
    // Unknown OSC code: drop silently — an unhandled Ps must never reach the
    // screen as text. The parsed code still goes to a wired diagnostics client
    // so it can confirm large codes (OSC 1337, 7704, …) survive the parser intact.
    if (auto* c = t.diagnosticsClient.get())
        c->onUnknownOSCCode(ps, p);
}

} // namespace Ubiquitty::OSC
