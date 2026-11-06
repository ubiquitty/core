// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC 52 — Manipulate Selection Data. Holds the RFC 4648 base64 codec, the per-Terminal clipboard
// security policy install, single-shot and chunked transfer, and the deferred read-completion path.

#include "Core/Codec/Base64.h"
#include "Core/OSC/Internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>

namespace Ubiquitty::OSC {

void applyOSC52Policy(Ubiquitty::Terminal& t, const OSC52Policy& policy)
{
    t.osc52ClipboardReadPolicy = policy.read;
    t.osc52ClipboardWritePolicy = policy.write;
}

ClipboardReadPolicy clipboardReadPolicyFromString(std::string_view s)
{
    // Case-insensitive; unrecognized falls through to Prompt (the safe default).
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
    if (eqCaseless(s, "allow"))
        return ClipboardReadPolicy::Allow;
    if (eqCaseless(s, "deny"))
        return ClipboardReadPolicy::Deny;
    return ClipboardReadPolicy::Prompt;
}

ClipboardWritePolicy clipboardWritePolicyFromString(std::string_view s)
{
    // Only "deny" (case-insensitive) restricts; everything else is Allow.
    if (s.size() == 4) {
        auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lc(s[0]) == 'd' && lc(s[1]) == 'e' && lc(s[2]) == 'n' && lc(s[3]) == 'y')
            return ClipboardWritePolicy::Deny;
    }
    return ClipboardWritePolicy::Allow;
}

// Programmatic clipboard WRITE allowed when the toggle is on and write policy is not Deny. Shared by
// ITerm2.cpp's Copy= channel.
bool osc52WriteAllowed(Ubiquitty::Terminal& t) { return t.isOSC52Allowed && t.osc52ClipboardWritePolicy != ClipboardWritePolicy::Deny; }

static void sendEmptyClipboardReadReply(Ubiquitty::Terminal& t, std::string_view selectionSet);

// The selection-set specifier, NUL-terminated into a fixed buffer for snprintf.
//
// A view carries its own length and cannot be null, so the only case to guard is
// one longer than the buffer.
// The selection set, NUL-terminated and truncated to what `out` holds. A set
// longer than the buffer is a set no reply can name, and the truncation keeps
// the framing well-formed rather than the name accurate — a requester reading a
// reply it did not ask for is better served than one reading none.
static void copySelectionSet(std::string_view selectionSet, std::span<char> out)
{
    if (out.empty())
        return;
    const std::size_t length = std::min(selectionSet.size(), out.size() - 1);
    if (length > 0)
        std::memcpy(out.data(), selectionSet.data(), length);
    out[length] = 0;
}

// OSC 52 selector helpers (defined with the rest of the OSC 52 machinery below;
// forward-declared here because the read-completion + write-routing paths above
// the definitions reference them).
static bool isOSC52Selector(char c);
static Ubiquitty::ClipboardBuffer osc52BufferForSelector(char c);

// Read the clipboard via the client, base64-encode it, and send the framed
// OSC 52 read reply: ESC ] 52 ; <selection-set> ; <base64> ST. Shared by the Allow path and Prompt
// approval. An encode/framing failure falls back to the empty reply (a dropped reply would hang the
// requester); a refused read (no clipboard) stays silent.
ClipboardReadBuffer::ClipboardReadBuffer()
    : m_bytes(new (std::nothrow) char[Ubiquitty::VT::maxExtendedClipboardReadBytes])
{
}

std::span<char> ClipboardReadBuffer::span() const
{
    if (!m_bytes)
        return {};
    return { m_bytes.get(), static_cast<std::size_t>(Ubiquitty::VT::maxExtendedClipboardReadBytes) };
}

// "\033]52;" plus the longest selection set the copy below can produce, plus its
// separator: what the framing costs before a single encoded byte.
constexpr std::size_t maxSelectionSetFraming = 5 + 15 + 1;

// The framed reply for a selection, or an empty owner when it cannot be built.
//
// Separate from the sending because every way of failing to build one has the
// same answer — an empty reply, so the requester learns nothing rather than
// waiting for ever — and a caller that spells that answer beside each failure
// says it five times.
struct ComposedReply {
    std::unique_ptr<char[]> bytes;
    std::size_t length = 0;

    [[nodiscard]] explicit operator bool() const { return bytes != nullptr; }
    [[nodiscard]] std::span<const char> span() const { return { bytes.get(), length }; }
};

static ComposedReply composeClipboardReadReply(std::span<const char> selection, std::string_view selectionSet, std::string_view terminator)
{
    ComposedReply reply;

    std::array<char, 16> selectionSetBuffer {};
    copySelectionSet(selectionSet, selectionSetBuffer);

    // Sized exactly: a fixed prefix, the encoding the codec itself measures,
    // and a two-byte terminator.
    const std::size_t encodedBytes = Codec::base64EncodedLength(selection.size());
    reply.bytes.reset(new (std::nothrow) char[maxSelectionSetFraming + encodedBytes + 2]);
    if (!reply.bytes)
        return {};

    const int headerLength = std::snprintf(reply.bytes.get(), maxSelectionSetFraming + 1, "\033]52;%s;", selectionSetBuffer.data());
    if (headerLength <= 0)
        return {};
    const auto encoded = Codec::base64Encode(selection,
        std::span<char> { reply.bytes.get() + headerLength, encodedBytes });
    if (!encoded)
        return {};

    reply.length = static_cast<std::size_t>(headerLength) + encoded->size();
    for (const char byte : terminator)
        reply.bytes[reply.length++] = byte;
    return reply;
}

static void sendClipboardReadReply(Ubiquitty::Terminal& t, Ubiquitty::ClipboardBuffer buffer, std::string_view selectionSet)
{
    // The whole selection, base64-encoded, which is what a read query asks for.
    // The fixed reply buffer holds 256 bytes and `respond` drops what will not
    // fit, so a reply sized to it carries the first 168 bytes of the clipboard
    // and says nothing about the rest — a remote editor yanking over a link
    // receives a shorter string than the one on the clipboard, with no way to
    // tell. The bulk channel exists for this and OSC 5522 already uses it; both
    // drain through the one flush loop.
    auto* clip = t.clipboardClient.get();
    if (!clip)
        return;
    const ClipboardReadBuffer selection;
    const int selectionLength = selection ? clip->readClipboard(buffer, selection.span()) : 0;
    if (selectionLength < 0)
        return; // refused / no clipboard — no reply, by contract

    // A clipboard past the bulk ceiling is refused rather than clipped: a reply
    // the requester can parse but that holds less than it asked for is worse
    // than one that plainly holds nothing. Every other way of failing to build
    // or queue the reply answers the same way, so they answer in one place.
    const ComposedReply reply = selection
        ? composeClipboardReadReply({ selection.data(), static_cast<std::size_t>(selectionLength) }, selectionSet, Ubiquitty::oscReplyTerminator(t))
        : ComposedReply {};
    if (!reply || !Ubiquitty::respondBulk(t, Ubiquitty::Capability::ClipboardRead, reply.span()))
        sendEmptyClipboardReadReply(t, selectionSet);
}

// OSC 52 read reply with empty data — the Deny path and the overflow fallback. Well-formed so the
// requester won't hang, but learns nothing.
static void sendEmptyClipboardReadReply(Ubiquitty::Terminal& t, std::string_view selectionSet)
{
    std::array<char, 16> selectionSetBuffer {};
    copySelectionSet(selectionSet, selectionSetBuffer);

    char resp[32];
    const std::string_view terminator = Ubiquitty::oscReplyTerminator(t);
    int n = std::snprintf(resp, sizeof(resp), "\033]52;%s;%.*s", selectionSetBuffer.data(), static_cast<int>(terminator.size()), terminator.data());
    if (n <= 0 || n >= static_cast<int>(sizeof(resp)))
        return;
    Ubiquitty::respond(t, Ubiquitty::Capability::ClipboardRead, resp);
}

// Hand one read query to the frontend, or answer it here if one is already outstanding.
//
// The frontend answers asynchronously: it allocates a request and, under Prompt, raises a dialog, while
// the stream that asked keeps running. So the number of these a stream can have in flight is the number
// of allocations and dialogs it can force, and a query in a loop outruns any user. One at a time; the
// rest are answered empty, exactly as Deny answers — well-formed, carrying nothing, and costing the
// frontend nothing. The cap sits here rather than in each frontend because it is a property of the
// protocol, and because a frontend that forgets it has no symptom until someone tries the loop.
//
// A frontend that declines the request (no window to ask in) gets the same empty reply rather than
// silence: the query was already parsed and its sender is entitled to an answer it can proceed past.
static void beginDeferredClipboardRead(Ubiquitty::Terminal& t, Ubiquitty::ClipboardClient* clip, Ubiquitty::ClipboardBuffer buffer, std::string_view selectionSet, ClipboardReadConfirmation confirmation)
{
    if (t.isOSC52ClipboardReadInFlight) {
        sendEmptyClipboardReadReply(t, selectionSet);
        return;
    }
    // Armed ahead of the request, because the frontend is allowed to answer inside
    // it: clientRunOnMain runs inline when the ingest call is already on main, so
    // requestClipboardRead can complete the whole read — and clear this flag —
    // before it returns. Setting the flag afterwards would then re-arm it with no
    // request outstanding, and since only completeClipboardReadQuery clears it,
    // every later read is answered empty and the surface never prompts again.
    t.isOSC52ClipboardReadInFlight = true;
    t.isClipboardReadRequestBellTerminated = t.parser.isOSCRequestBellTerminated;
    if (!clip->requestClipboardRead(buffer, selectionSet, confirmation)) {
        t.isOSC52ClipboardReadInFlight = false; // nothing outstanding to wait on
        sendEmptyClipboardReadReply(t, selectionSet);
        return;
    }
}

void completeClipboardReadQuery(Ubiquitty::Terminal& t, std::string_view selectionSet, ClipboardReadDecision decision)
{
    // No query outstanding: a stray or duplicate completion. Answering one
    // would emit an unsolicited reply the stream never asked for, and a
    // completion racing teardown must not write into state no query owns —
    // the flag is the only record of what was asked. Drop it.
    if (!t.isOSC52ClipboardReadInFlight)
        return;
    // The frontend is done with the outstanding query however it answered, so the next one may go out.
    // Cleared before the reply is framed: respond() reaches the PTY, and the reply is what a caller
    // waits on before asking again.
    t.isOSC52ClipboardReadInFlight = false;
    // The answer ends the way the request it answers ended (xterm do_osc),
    // however long after that request's dispatch it is framed.
    struct RequestTerminatorScope {
        Ubiquitty::Terminal& terminal;
        bool wasBellTerminated;
        ~RequestTerminatorScope() { terminal.parser.isOSCRequestBellTerminated = wasBellTerminated; }
    } requestTerminatorScope { t, t.parser.isOSCRequestBellTerminated };
    t.parser.isOSCRequestBellTerminated = t.isClipboardReadRequestBellTerminated;
    if (t.isPendingClipboardReadExtended) {
        completeExtendedClipboardRead(t, decision);
        return;
    }
    // One completion call serves both clipboard protocols — the frontends
    // answer a prompt without knowing which spelling asked — so the framing
    // routes on which one is outstanding.

    if (!t.isOSC52Allowed)
        return;
    if (decision == ClipboardReadDecision::Approved && t.osc52ClipboardReadPolicy != ClipboardReadPolicy::Deny) {
        // Read the first selector in the set (frontend already staged its bytes). Empty/illegal → Clipboard.
        Ubiquitty::ClipboardBuffer buffer = Ubiquitty::ClipboardBuffer::Clipboard;
        if (!selectionSet.empty() && isOSC52Selector(selectionSet[0]))
            buffer = osc52BufferForSelector(selectionSet[0]);
        sendClipboardReadReply(t, buffer, selectionSet);
    } else {
        sendEmptyClipboardReadReply(t, selectionSet);
    }
}

// OSC 52 chunked (multi-part) clipboard write. A SET whose base64 ends in '!' is a chunk: its base64 (sans
// marker) appends to the pending buffer and the terminal waits. The first SET to the same selection without
// '!' terminates: it appends, then the whole buffer is decoded once and applied. '!' is not in the base64
// alphabet, so single-shot SET is provably untouched.
// Bounded by maxOSC52ChunkSize. On overflow the transfer is poisoned: accumulated bytes dropped and
// remaining chunks swallowed, so it can neither grow unbounded nor partially apply. An interleaved SET to a
// different selection, a read query, or a hard reset also resets the pending state.

static void osc52ChunkReset(Ubiquitty::Terminal& t)
{
    t.osc52Chunk.length = 0;
    t.osc52ChunkActive = false;
    t.osc52ChunkOverflow = false;
    t.osc52ChunkSet[0] = 0;
}

// Append `n` base64 bytes to the pending chunk buffer, growing up to maxOSC52ChunkSize. False on overflow
// or alloc failure: the buffer is poisoned so the rest of the stream is swallowed — nothing applies.
static bool osc52ChunkAppend(Ubiquitty::Terminal& t, std::string_view data)
{
    // The storage question — may this grow, and by how much — is ByteBuffer's.
    // What remains here is the OSC 52 answer to "no": poison the transfer and
    // drop what was held, so the rest of the stream is swallowed rather than
    // applied as a truncated selection. ByteBuffer leaves a refused buffer
    // intact, which is right for a caller that means to retry and wrong for
    // this one.
    if (!t.osc52Chunk.append(data, Ubiquitty::VT::maxOSC52ChunkSize)) {
        t.osc52ChunkOverflow = true;
        t.osc52Chunk.length = 0;
        return false;
    }
    return true;
}

// Write `data` to every distinct buffer named in the validated selection set (duplicates collapse to one
// write per buffer). No recognized selector (impossible after validation) → Clipboard, so it never vanishes.
static void osc52WriteToSet(Ubiquitty::Terminal& terminal, const char* set, std::span<const char> data)
{
    bool written[static_cast<int>(Ubiquitty::ClipboardBuffer::Count)] = {};
    std::array<Ubiquitty::ClipboardBuffer, static_cast<std::size_t>(Ubiquitty::ClipboardBuffer::Count)> buffers {};
    std::size_t count = 0;
    for (const char* s = set; *s; ++s) {
        if (!isOSC52Selector(*s))
            continue;
        Ubiquitty::ClipboardBuffer buffer = osc52BufferForSelector(*s);
        int index = static_cast<int>(buffer);
        if (written[index])
            continue;
        written[index] = true;
        buffers[count++] = buffer;
    }
    if (!count)
        buffers[count++] = Ubiquitty::ClipboardBuffer::Clipboard;
    Ubiquitty::requestClipboardWrite(terminal, std::span(buffers).first(count), data);
}

// Decode the accumulated chunk buffer and hand it to each buffer in the transfer's saved selection set.
// Uses a transient heap buffer (chunked transfers can far exceed the single-shot stack buffer). Resets the
// pending state regardless of outcome.
static void osc52ChunkApply(Ubiquitty::Terminal& t)
{
    int b64Length = t.osc52Chunk.length;
    if (b64Length <= 0) {
        osc52WriteToSet(t, t.osc52ChunkSet.data(), {}); // empty transfer clears the buffers
        osc52ChunkReset(t);
        return;
    }
    const std::string_view encoded(t.osc52Chunk.data, static_cast<std::size_t>(b64Length));
    const std::size_t decodedCapacity = Codec::base64DecodedCapacity(encoded);
    // Room for no byte: the transfer decodes to nothing or not at all, and
    // either clears the selection, with no buffer to decode into.
    if (!decodedCapacity) {
        osc52WriteToSet(t, t.osc52ChunkSet.data(), {});
        osc52ChunkReset(t);
        return;
    }
    const std::unique_ptr<char[]> decodedBytes(new (std::nothrow) char[decodedCapacity]);
    if (!decodedBytes) {
        osc52ChunkReset(t);
        return;
    }
    const auto decoded = Codec::base64Decode(encoded, { decodedBytes.get(), decodedCapacity });
    // A transfer that is not base64 clears the selection, as a single SET does.
    osc52WriteToSet(t, t.osc52ChunkSet.data(), decoded.value_or(std::span<char> {}));
    osc52ChunkReset(t);
}

// A character of the standard base64 alphabet, its padding included.
static bool isBase64Character(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

// Legal OSC 52 selector: c=CLIPBOARD, p=PRIMARY, q=SECONDARY, s=select, 0-7=cut buffers. Any other byte
// invalidates the request.
static bool isOSC52Selector(char c) { return c == 'c' || c == 'p' || c == 'q' || c == 's' || (c >= '0' && c <= '7'); }

// Map a validated selector to its clipboard buffer (default Clipboard, defensive — never fires).
static Ubiquitty::ClipboardBuffer osc52BufferForSelector(char c)
{
    using B = Ubiquitty::ClipboardBuffer;
    switch (c) {
    case 'c':
        return B::Clipboard;
    case 'p':
        return B::Primary;
    case 'q':
        return B::Secondary;
    case 's':
        return B::Select;
    default:
        if (c >= '0' && c <= '7')
            return static_cast<B>(static_cast<uint8_t>(B::Cut0) + (c - '0'));
        return B::Clipboard;
    }
}

// Handle the OSC 52 payload. `p` points past the leading "52;". The
// payload format is "<selection-set>;<data-or-?>".
//
// Selector routing: each legal Pc selector (c=CLIPBOARD, p=PRIMARY,
// q=SECONDARY, s=select, 0-7=cut buffers; empty default "s0") names a
// DISTINCT buffer (osc52BufferForSelector → ClipboardBuffer). A SET writes
// every distinct buffer in the list (so "pc" writes both Primary and
// Clipboard once each); a query reads the buffer of the FIRST selector. The
// frontend keeps each buffer as its own store — only Clipboard bridges to the
// system pasteboard. All routing stays behind the isOSC52Allowed master toggle
// and the write/read policy gates below — there is no unguarded selector
// path, and the read policy guards every buffer (a remote can't read
// Primary/cut buffers unguarded). A Pc containing any byte outside the legal
// set makes the whole request invalid: it is ignored, never silently routed to
// a default buffer.
void handleClipboard(Ubiquitty::Terminal& t, std::string_view p)
{
    if (!t.isOSC52Allowed)
        return;

    // Trim leading whitespace from the selection set.
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
        p.remove_prefix(1);

    // Walk to the next ';' — that's the selection-set string. We validate
    // it (below) and echo it back verbatim, but all legal selectors route
    // to the one system pasteboard.
    const std::size_t semicolon = p.find(';');
    if (semicolon == std::string_view::npos)
        return; // malformed — no data field at all
    const std::string_view selectionSet = p.substr(0, semicolon);
    const std::string_view data = p.substr(semicolon + 1);

    // Snapshot the selection-set bytes so the response can echo them. It stays a
    // bounded C string: osc52ChunkSet holds one across controls and the two are
    // compared to decide whether a fragment continues the transfer in flight.
    std::array<char, 16> selectionSetBuffer {};
    copySelectionSet(selectionSet, selectionSetBuffer);
    int selectionSetLength = static_cast<int>(std::strlen(selectionSetBuffer.data()));
    // Empty selection-set means the default "s0"; echo "s0" back for a well-formed response.
    if (selectionSetLength == 0) {
        selectionSetBuffer[0] = 's';
        selectionSetBuffer[1] = '0';
        selectionSetBuffer[2] = 0;
        selectionSetLength = 2;
    }

    // Validate the selector list: a single illegal byte invalidates the request — drop it (no write, no
    // reply) and abort any in-flight chunked transfer, so an unknown selector never routes to the clipboard.
    for (int i = 0; i < selectionSetLength; ++i) {
        if (!isOSC52Selector(selectionSetBuffer[i])) {
            osc52ChunkReset(t);
            return;
        }
    }

    // Query vs. write. The data field is the rest of the OSC accumulator, and a
    // clipboard write carries its whole base64 body there — measuring it again
    // would cost a scan of the paste to read one byte.
    const int dataLength = static_cast<int>(data.size());

    if (data == "?") {
        // Read query — gated by the read policy. Interleaving into a pending chunked write aborts it.
        osc52ChunkReset(t);
        auto* clip = t.clipboardClient.get();
        if (!clip)
            return;
        // A query reads one buffer: the first selector (validated above, so always legal).
        Ubiquitty::ClipboardBuffer readBuffer = osc52BufferForSelector(selectionSetBuffer[0]);
        switch (t.osc52ClipboardReadPolicy) {
        case ClipboardReadPolicy::Deny:
            sendEmptyClipboardReadReply(t, std::string_view(selectionSetBuffer.data(), static_cast<std::size_t>(selectionSetLength))); // empty so requester doesn't hang
            return;
        case ClipboardReadPolicy::Allow:
            // Read+reply now, unless the client requires async capture (macOS captures NSPasteboard on main),
            // in which case defer with NotRequired so the frontend auto-approves.
            if (clip->requiresAsynchronousClipboardRead())
                beginDeferredClipboardRead(t, clip, readBuffer, std::string_view(selectionSetBuffer.data(), static_cast<std::size_t>(selectionSetLength)), ClipboardReadConfirmation::NotRequired);
            else
                sendClipboardReadReply(t, readBuffer, std::string_view(selectionSetBuffer.data(), static_cast<std::size_t>(selectionSetLength)));
            return;
        case ClipboardReadPolicy::Prompt:
            beginDeferredClipboardRead(t, clip, readBuffer, std::string_view(selectionSetBuffer.data(), static_cast<std::size_t>(selectionSetLength)), ClipboardReadConfirmation::Required);
            return;
        }
        return;
    }

    // Write path (empty data clears). Deny blocks a remote from staging paste-injection via the clipboard.
    if (t.osc52ClipboardWritePolicy == ClipboardWritePolicy::Deny) {
        osc52ChunkReset(t); // also drop any in-flight chunked transfer
        return;
    }
    // Capability consent belongs to the complete decoded write, not each
    // transport fragment. Its typed request can be retained without reparsing.
    auto* clip = t.clipboardClient.get();
    if (!clip) {
        // No sink — still consume pending chunk state so a later client-bearing Terminal doesn't inherit stale bytes.
        osc52ChunkReset(t);
        return;
    }

    // A data field ending in '!' after a base64 character is a continuation fragment (strip and
    // accumulate). '!' is outside the base64 alphabet, so single-shot SET stays on the fast path below.
    // A field that is no fragment of base64, a lone '!' among them, is data ctlseqs clears the selection
    // for: "If the second parameter is neither a base64 string nor ?, then the selection is cleared."
    const bool isContinuation = dataLength >= 2 && data.back() == '!' && isBase64Character(data[data.size() - 2]);

    if (isContinuation || t.osc52ChunkActive) {
        // A SET to a different selection aborts the in-flight transfer and starts fresh. strcmp safe: both
        // are NUL-terminated bounded snapshots.
        if (t.osc52ChunkActive && std::strcmp(t.osc52ChunkSet.data(), selectionSetBuffer.data()) != 0)
            osc52ChunkReset(t);

        if (!t.osc52ChunkActive) {
            // First fragment: snapshot the selection-set so later fragments / the terminator must match.
            t.osc52ChunkActive = true;
            std::memcpy(t.osc52ChunkSet.data(), selectionSetBuffer.data(), static_cast<size_t>(selectionSetLength) + 1);
        }

        // Poisoned (a prior fragment overflowed): swallow this one; a terminator ends it with no apply.
        if (t.osc52ChunkOverflow) {
            if (!isContinuation)
                osc52ChunkReset(t);
            return;
        }

        int fragLength = isContinuation ? (dataLength - 1) : dataLength;
        if (!osc52ChunkAppend(t, data.substr(0, static_cast<std::size_t>(fragLength)))) {
            // Overflow/OOM mid-stream: a continuation stays poisoned; a terminator ends it now.
            if (!isContinuation)
                osc52ChunkReset(t);
            return;
        }

        if (isContinuation)
            return; // more to come; wait for the terminating (unmarked) SET

        osc52ChunkApply(t); // terminator: decode + apply the whole buffer, then reset
        return;
    }

    // Single-shot SET. Small payloads decode off the stack; larger ones go through a transient heap buffer.
    // Data that is not base64 clears the selection: "If the second parameter is neither a base64 string
    // nor ?, then the selection is cleared" (xterm ctlseqs).
    char stackDecoded[256];
    if (dataLength <= static_cast<int>(sizeof(stackDecoded)) * 4 / 3) {
        const auto decoded = Codec::base64Decode(data, stackDecoded);
        osc52WriteToSet(t, selectionSetBuffer.data(), decoded.value_or(std::span<char> {}));
        return;
    }
    if (dataLength > Ubiquitty::VT::maxOSC52ChunkSize)
        return; // implausibly large single-shot — drop (same ceiling as chunked)
    // Past the stack buffer's reach, so there is room for at least one byte.
    const std::size_t decodedCapacity = Codec::base64DecodedCapacity(data);
    const std::unique_ptr<char[]> heapDecoded(new (std::nothrow) char[decodedCapacity]);
    if (!heapDecoded)
        return;
    const auto decoded = Codec::base64Decode(data, { heapDecoded.get(), decodedCapacity });
    osc52WriteToSet(t, selectionSetBuffer.data(), decoded.value_or(std::span<char> {}));
}

} // namespace Ubiquitty::OSC
