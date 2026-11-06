// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC 5522 — kitty's extended clipboard. MIME-typed reads and writes over the
// tty, which is what lets a program on the far side of an ssh connection reach
// the clipboard of the terminal the user is sitting at.
//
// Wire shape: OSC 5522 ; <metadata> [; <payload>] ST, metadata a
// colon-separated list of key=value pairs, payload base64. A read is answered
// as a stream of packets — `status=OK`, one `status=DATA` per chunk of no
// more than 4096 pre-encoding bytes, `status=DONE` — and a write arrives the same
// way: type=write opens a session, type=wdata packets carry chunks, and a
// wdata with no MIME commits it.
//
// The protocol shares OSC 52's policy: the master toggle, the read policy
// (listing the available types is answered without a prompt; reading data is
// gated), the write policy and the origin-aware capability gate. One policy
// for both sequences, or a user who denied clipboard reads would find the
// denial applied to the spelling of the request rather than to the request.
//
// The status replies fit the fixed output buffer; the data stream does not —
// a clipboard runs to megabytes where that buffer holds 256 bytes — so DATA
// packets queue through the bulk reply channel as one transaction: composed
// whole, or taken back whole and answered with EBUSY.
//
// Types cross the ClipboardClient seam. A typed client names what it serves
// and answers per-MIME payloads; the untyped one — every client that
// overrides none of the typed methods — serves text/plain plus the last
// committed write's aliases, through the seam's adapting defaults. Which
// kind is behind the seam is the client's own answer, resolved when the
// reply is framed, because an asynchronous client cannot say what it serves
// before its capture.

#include "Core/Codec/Base64.h"
#include "Core/OSC/Internal.h"
#include "Core/VT/Constants.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace Ubiquitty::OSC {

namespace {

constexpr std::string_view textPlain = "text/plain";
// Spec ceiling for one chunk's pre-encoding bytes; replies chunk at it.
constexpr int chunkRawBytes = 4096;

static_assert(std::tuple_size_v<decltype(Terminal::extendedClipboardWriteItems)> == Ubiquitty::VT::maxExtendedClipboardWriteItems,
    "the write session holds one buffer per allowed item");
static_assert(std::tuple_size_v<decltype(Terminal::extendedClipboardWriteItemMimes)> == Ubiquitty::VT::maxExtendedClipboardWriteItems,
    "every item buffer has a MIME slot");
static_assert(std::is_same_v<decltype(Terminal::extendedClipboardWriteMime),
                  decltype(Terminal::extendedClipboardWriteItemMimes)::value_type>,
    "a name the session carrier holds always fits an item's slot");

// Caseless ASCII equality, for MIME comparisons — MIME types match
// case-insensitively and remote programs spell them either way.
[[nodiscard]] bool equalsCaseless(std::string_view a, std::string_view b)
{
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
}

// Whether a MIME names text/plain — the seam's own predicate, so this handler
// and the seam's adapting defaults agree on which requests the untyped
// client's one type answers.
[[nodiscard]] bool isTextMime(std::string_view mime) { return Ubiquitty::mimeIsTextPlain(mime); }

// A response id byte, per the spec's multiplexer contract: anything else is
// stripped rather than echoed, so an id cannot smuggle metadata separators
// back into the response stream.
[[nodiscard]] bool isIDByte(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '+' || c == '.';
}

[[nodiscard]] bool isValidEncodedMetadata(std::string_view encoded)
{
    if (encoded.size() % 4)
        return false;
    std::array<char, 768> decoded;
    while (!encoded.empty()) {
        const std::string_view block = encoded.substr(0, 1024);
        encoded.remove_prefix(block.size());
        if (!Codec::base64Decode(block, decoded) || (!encoded.empty() && block.back() == '='))
            return false;
    }
    return true;
}

// The request's parsed metadata. Views borrow the OSC accumulator and are
// valid for the handler call; the id is copied because deferred reads echo it
// after the accumulator is gone.
struct RequestMetadata {
    std::string_view type;
    std::string_view mimeB64;
    bool isPrimary = false;
    bool hasInvalidEncodedMetadata = false;
    std::array<char, 65> id {};
};

void parseMetadata(std::string_view metadata, RequestMetadata& out)
{
    std::size_t idLength = 0;
    while (!metadata.empty()) {
        const std::size_t colon = metadata.find(':');
        const std::string_view pair = metadata.substr(0, colon);
        metadata = colon == std::string_view::npos ? std::string_view {} : metadata.substr(colon + 1);
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = pair.substr(0, equals);
        const std::string_view value = pair.substr(equals + 1);
        if (key == "type") {
            out.type = value;
        } else if (key == "mime") {
            out.mimeB64 = value;
        } else if (key == "loc") {
            out.isPrimary = value == "primary";
        } else if (key == "id") {
            for (char c : value) {
                if (idLength + 1 >= out.id.size())
                    break;
                if (isIDByte(c))
                    out.id[idLength++] = c;
            }
        } else if (key == "name" || key == "pw") {
            out.hasInvalidEncodedMetadata |= !isValidEncodedMetadata(value);
        }
        // Password metadata does not bypass the clipboard consent policy.
    }
}

// Decodes a base64 metadata value into a bounded buffer. A value that does not
// decode, or does not fit, answers `std::nullopt` — the caller's EINVAL. An
// empty value decodes to an empty span.
[[nodiscard]] std::optional<std::span<char>> decodeBounded(std::string_view b64, std::span<char> out)
{
    if (b64.size() % 4 != 0)
        return std::nullopt;
    return Codec::base64Decode(b64, out);
}

// Frames one response packet into `out`:
//   ESC ] 5522 ; type=<t>:status=<s>[:mime=<m64>][:id=<id>] [; payload] ST
// ending with BEL instead when the request did.
// The id travels in every packet because a multiplexer routes each one alone.
[[nodiscard]] int frameStatusPacket(std::span<char> out, std::string_view terminator, std::string_view type, std::string_view status,
    std::string_view mimeB64, const char* id, std::string_view payloadB64)
{
    int n = std::snprintf(out.data(), out.size(), "\033]5522;type=%.*s:status=%.*s",
        static_cast<int>(type.size()), type.data(), static_cast<int>(status.size()), status.data());
    if (n <= 0 || std::cmp_greater_equal(n, out.size()))
        return -1;
    auto appendPiece = [&](std::string_view prefix, std::string_view piece) {
        if (piece.empty())
            return true;
        const int wrote = std::snprintf(out.data() + n, out.size() - static_cast<std::size_t>(n), "%.*s%.*s",
            static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(piece.size()), piece.data());
        if (wrote <= 0 || std::cmp_greater_equal(n + wrote, out.size()))
            return false;
        n += wrote;
        return true;
    };
    if (!appendPiece(":mime=", mimeB64))
        return -1;
    if (!appendPiece(":id=", id ? std::string_view { id } : std::string_view {}))
        return -1;
    if (!appendPiece(";", payloadB64))
        return -1;
    if (std::cmp_greater_equal(n + static_cast<int>(terminator.size()), out.size()))
        return -1;
    for (const char byte : terminator)
        out[static_cast<std::size_t>(n++)] = byte;
    return n;
}

// A status-only reply — the acks and the errors. Small by construction, so it
// rides the fixed buffer and survives even when the bulk channel is full.
void sendStatus(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, std::string_view type,
    std::string_view status, const char* id)
{
    std::array<char, 128> packet;
    const int n = frameStatusPacket(packet, Ubiquitty::oscReplyTerminator(t), type, status, {}, id, {});
    if (n <= 0)
        return;
    Ubiquitty::respond(t, capability, std::span<const char> { packet.data(), static_cast<std::size_t>(n) });
}

// Whether `mime` appears in the space-separated `aliases` list, caselessly.
[[nodiscard]] bool aliasListContains(std::string_view aliases, std::string_view mime)
{
    while (!aliases.empty()) {
        const std::size_t space = aliases.find(' ');
        const std::string_view alias = aliases.substr(0, space);
        aliases = space == std::string_view::npos ? std::string_view {} : aliases.substr(space + 1);
        if (!alias.empty() && equalsCaseless(alias, mime))
            return true;
    }
    return false;
}

// The served alias list, as a view over the terminal's buffer.
[[nodiscard]] std::string_view servedAliases(const Ubiquitty::Terminal& t)
{
    return { t.extendedClipboardServedAliases.data, static_cast<std::size_t>(t.extendedClipboardServedAliases.length) };
}

// Whether the untyped client serves `mime`: text/plain always, plus whatever
// names the last committed write aliased to its text. The aliases describe
// that payload; a copy made outside this protocol replaces the text without
// touching the claim, so the claim dies with the next commit. A typed client
// never consults this — it stores alias entries outright and answers for
// itself.
[[nodiscard]] bool servesMime(const Ubiquitty::Terminal& t, std::string_view mime)
{
    return isTextMime(mime) || aliasListContains(servedAliases(t), mime);
}

// Base64 of a MIME string, bounded: MIME names are short and one that is not
// has no business in a response.
[[nodiscard]] std::optional<std::span<char>> encodeMime(std::string_view mime, std::span<char> out)
{
    return Codec::base64Encode(std::span<const char> { mime.data(), mime.size() }, out);
}

// A read reply under composition: the transaction against the bulk channel.
// Groups are emitted between construction and end(); a refused append poisons
// the composition, and end() takes back whatever was queued and answers
// `EBUSY` through the fixed buffer — the read section's error set is closed
// at ENOSYS/EPERM/EBUSY, and a full reply channel is exactly the temporary
// conflict EBUSY names — so a refusal can never strand a stream without its
// `DONE`.
struct ReadStream {
    Ubiquitty::Terminal& t;
    const char* id;
    int bulkStart;
    bool isComposed;

    ReadStream(Ubiquitty::Terminal& terminal, const char* responseID)
        : t(terminal)
        , id(responseID)
        , bulkStart(terminal.bulkOutput.length)
    {
        std::array<char, 128> packet;
        const int n = frameStatusPacket(packet, Ubiquitty::oscReplyTerminator(t), "read", "OK", {}, id, {});
        isComposed = n > 0 && respondBulk(t, Ubiquitty::Capability::ClipboardRead, { packet.data(), static_cast<std::size_t>(n) });
    }

    // One name's group of `DATA` chunks. Each chunk is encoded and framed
    // alone, padding included: a consumer decodes packet by packet, and a
    // chunk that borrowed bytes from its neighbour would not decode at all.
    void emitGroup(std::string_view mime, std::span<const char> raw)
    {
        if (!isComposed)
            return;
        std::array<char, 192> mimeB64 {};
        const auto encodedMime = encodeMime(mime, std::span { mimeB64 }.first(mimeB64.size() - 1));
        if (!encodedMime) {
            isComposed = false;
            return;
        }
        std::array<char, 8192> packet;
        // Sized so a full chunk's encoding always fits; a refusal would poison
        // the composition like any other.
        std::array<char, chunkRawBytes / 3 * 4 + 8> chunkB64 {};
        static_assert(Codec::base64EncodedLength(chunkRawBytes) < std::tuple_size_v<decltype(chunkB64)>,
            "a full chunk encodes into the block");
        std::size_t offset = 0;
        while (isComposed && offset < raw.size()) {
            const std::size_t take = std::min<std::size_t>(chunkRawBytes, raw.size() - offset);
            const auto encodedChunk = Codec::base64Encode(raw.subspan(offset, take), std::span { chunkB64 }.first(chunkB64.size() - 1));
            if (!encodedChunk) {
                isComposed = false;
                break;
            }
            const int n = frameStatusPacket(packet, Ubiquitty::oscReplyTerminator(t), "read", "DATA",
                { encodedMime->data(), encodedMime->size() }, id,
                { encodedChunk->data(), encodedChunk->size() });
            isComposed = n > 0 && respondBulk(t, Ubiquitty::Capability::ClipboardRead, { packet.data(), static_cast<std::size_t>(n) });
            offset += take;
        }
    }

    void end()
    {
        if (isComposed) {
            std::array<char, 128> packet;
            const int n = frameStatusPacket(packet, Ubiquitty::oscReplyTerminator(t), "read", "DONE", {}, id, {});
            isComposed = n > 0 && respondBulk(t, Ubiquitty::Capability::ClipboardRead, { packet.data(), static_cast<std::size_t>(n) });
        }
        if (!isComposed) {
            // Truncation, not reset: a composition can only fail against
            // content earlier streams queued (its own first packet is far
            // under the channel cap), so the rollback never leaves the buffer
            // empty — and the drain that empties it releases the capacity.
            t.bulkOutput.length = bulkStart;
            sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EBUSY", id);
        }
    }
};

// Answers the type-listing request: the types this terminal can serve, under
// the spec's "." pseudo-MIME. Served without a prompt — the list discloses
// what the terminal can serve, not what the clipboard holds — which is the
// one read the spec exempts from consent. A typed client names its own types;
// the untyped one serves text/plain plus the served aliases; no client at all
// still answers the type Core could serve, so a probing program learns the
// protocol is spoken.
void sendTypeList(Ubiquitty::Terminal& t, Ubiquitty::ClipboardBuffer buffer, const char* id)
{
    std::array<char, 512> types {};
    int typesLength = -1;
    if (auto* clip = t.clipboardClient.get())
        typesLength = clip->readClipboardTypes(buffer, std::span { types }.first(types.size() - 1));
    std::array<char, Ubiquitty::VT::maxExtendedClipboardAliasBytes + 32> list {};
    int length = 0;
    if (typesLength >= 0) {
        length = typesLength;
        std::memcpy(list.data(), types.data(), static_cast<std::size_t>(typesLength));
    } else {
        length = static_cast<int>(textPlain.size());
        std::memcpy(list.data(), textPlain.data(), textPlain.size());
        const std::string_view aliases = servedAliases(t);
        if (!aliases.empty() && length + 1 + static_cast<int>(aliases.size()) < static_cast<int>(list.size())) {
            list[static_cast<std::size_t>(length++)] = ' ';
            std::memcpy(list.data() + length, aliases.data(), aliases.size());
            length += static_cast<int>(aliases.size());
        }
    }
    ReadStream stream(t, id);
    stream.emitGroup(".", { list.data(), static_cast<std::size_t>(length) });
    stream.end();
}

// Reads the requested names and streams what the clipboard serves of them.
// On a typed client the payloads differ per name, so each served name earns
// its own group; the untyped client's one payload answers under whichever of
// its names was asked — that is what an alias is. Each group echoes the name
// the requester routes by. Every payload read is bounded at
// maxExtendedClipboardReadBytes: the reply channel carries every byte back
// across the transport, and an unbounded read hands a remote the whole of
// whatever a user last copied, however large.
void readAndAnswer(Ubiquitty::Terminal& t, Ubiquitty::ClipboardBuffer buffer, std::string_view mimes, const char* id)
{
    auto* clip = t.clipboardClient.get();
    if (!clip) {
        // ENOSYS: with nothing to serve it, the requested type is unavailable
        // — the read set's own word for it, where EIO is only the write's.
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "ENOSYS", id);
        return;
    }
    const ClipboardReadBuffer rawBuffer;
    if (!rawBuffer) {
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EBUSY", id);
        return;
    }
    char* raw = rawBuffer.data();
    const std::span<char> rawSpan = rawBuffer.span();
    // Which client kind is behind the seam, answered by the client itself —
    // asked here, after any capture, because an asynchronous client answers
    // from what it staged.
    std::array<char, 512> types {};
    const bool isTyped = clip->readClipboardTypes(buffer, std::span { types }.first(types.size() - 1)) >= 0;
    // The untyped client holds one payload however many names answer for it,
    // so it is read once and served under each.
    int untypedLength = 0;
    bool hasReadUntyped = false;

    ReadStream stream(t, id);
    // The walk continues past a poisoned composition — emitGroup owns the
    // swallowing, and end() turns the poison into the one EBUSY — so exactly
    // one place decides what a refused stream looks like.
    std::string_view remaining = mimes;
    while (!remaining.empty()) {
        const std::size_t space = remaining.find(' ');
        // The carry is canonical — single-spaced, no blanks — so the split
        // yields a name every iteration.
        const std::string_view mime = remaining.substr(0, space);
        remaining = space == std::string_view::npos ? std::string_view {} : remaining.substr(space + 1);
        int n = -1;
        if (isTyped) {
            n = clip->readClipboardItem(buffer, mime, rawSpan);
        } else if (servesMime(t, mime)) {
            if (!hasReadUntyped) {
                untypedLength = clip->readClipboard(buffer, rawSpan);
                hasReadUntyped = true;
            }
            n = untypedLength;
        }
        if (n > 0)
            stream.emitGroup(mime, { raw, static_cast<std::size_t>(n) });
        // n == 0 is a served-but-empty payload and n < 0 an unserved name:
        // neither earns a group, and a complete stream with no `DATA` is the
        // spec's spelling for both — the requester can proceed past it, which
        // is why OSC 52 answers its empty reply for the same shape.
    }
    stream.end();
    // `read` owns the buffer and releases it here.
}

void resetWriteSession(Ubiquitty::Terminal& t)
{
    for (auto& item : t.extendedClipboardWriteItems)
        item.reset();
    for (auto& mime : t.extendedClipboardWriteItemMimes)
        mime[0] = 0;
    t.extendedClipboardWriteItemCount = 0;
    t.extendedClipboardWriteTotalBytes = 0;
    t.extendedClipboardSessionAliases.reset();
    t.isExtendedClipboardWriteActive = false;
    t.isExtendedClipboardWritePrimary = false;
    t.didExtendedClipboardWriteOverflow = false;
    t.extendedClipboardWriteID[0] = 0;
    t.extendedClipboardWriteMime[0] = 0;
    t.extendedClipboardBase64Tail = {};
    t.extendedClipboardBase64TailLength = 0;
    t.isExtendedClipboardBase64Complete = false;
}

void rejectWrite(Ubiquitty::Terminal& t)
{
    const std::array<char, 65> id = t.extendedClipboardWriteID;
    resetWriteSession(t);
    sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "EINVAL", id.data());
}

// The parsed read request: the names it asked for, deduplicated, and whether
// it asked for the type list. Nothing here resolves what is served — an
// asynchronous typed client cannot answer that before its capture, so
// serving is the answer path's question — and the names ride space-separated
// because the deferred read's carry holds them that way. An empty request
// reads as text/plain — the type a caller that names nothing means.
struct ReadRequest {
    bool wantsList = false;
    bool isValid = true;
    std::array<char, 256> requested {};
    int requestedLength = 0;
};

[[nodiscard]] ReadRequest parseReadRequest(std::string_view payloadB64)
{
    ReadRequest request;
    auto addName = [&request](std::string_view mime) {
        if (aliasListContains({ request.requested.data(), static_cast<std::size_t>(request.requestedLength) }, mime))
            return;
        const int needed = static_cast<int>(mime.size()) + (request.requestedLength > 0 ? 1 : 0);
        if (request.requestedLength + needed + 1 > static_cast<int>(request.requested.size()))
            return; // past the bound the pending-read carry can hold
        if (request.requestedLength > 0)
            request.requested[static_cast<std::size_t>(request.requestedLength++)] = ' ';
        std::memcpy(request.requested.data() + request.requestedLength, mime.data(), mime.size());
        request.requestedLength += static_cast<int>(mime.size());
    };
    if (payloadB64.empty()) {
        addName(textPlain);
        return request;
    }
    std::array<char, 512> decoded {};
    const auto names = decodeBounded(payloadB64, std::span { decoded }.first(decoded.size() - 1));
    if (!names) {
        request.isValid = false;
        return request;
    }
    std::string_view list { names->data(), names->size() };
    while (!list.empty()) {
        const std::size_t space = list.find(' ');
        const std::string_view mime = list.substr(0, space);
        list = space == std::string_view::npos ? std::string_view {} : list.substr(space + 1);
        if (mime.empty())
            continue;
        if (mime == ".")
            request.wantsList = true;
        else
            addName(mime);
    }
    if (request.requestedLength == 0 && !request.wantsList)
        addName(textPlain);
    return request;
}

void handleRead(Ubiquitty::Terminal& t, const RequestMetadata& request, std::string_view payloadB64)
{
    const ReadRequest asked = parseReadRequest(payloadB64);
    if (!asked.isValid) {
        // Dropped without a reply, as OSC 52 drops an invalid selector: the
        // read section's error set is closed at ENOSYS/EPERM/EBUSY, none of
        // which this is, and a requester that sent undecodable base64 was not
        // following the protocol whose answer it would be waiting on.
        return;
    }
    // loc=primary is served rather than answered `ENOSYS`: every frontend
    // backs ClipboardBuffer::Primary with a real store — OSC 52's `p`
    // selector routes to the same one — so the type is available here, on
    // macOS included, where the store is the app's rather than a windowing
    // system's.
    const Ubiquitty::ClipboardBuffer buffer = request.isPrimary ? Ubiquitty::ClipboardBuffer::Primary : Ubiquitty::ClipboardBuffer::Clipboard;
    auto* clip = t.clipboardClient.get();

    // Deny denies everything, list included: a type list answers which MIME
    // aliases are served and that a clipboard is live, which is exactly the
    // oracle a deny policy exists to withhold. The spec's consent exemption
    // for listing survives below for Allow/Prompt; an explicit Deny is not
    // the absence of consent, it is its refusal.
    if (t.osc52ClipboardReadPolicy == ClipboardReadPolicy::Deny) {
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EPERM", request.id.data());
        return;
    }

    if (asked.wantsList) {
        // Listing first, whatever else was asked: the spec exempts it from
        // consent, and a lister who also asked for data gets the data leg
        // under the gates below on a second request. A clientless or
        // synchronous listing answers here; an asynchronous client lists
        // from its capture, so even the promptless list defers.
        if (!clip || !clip->requiresAsynchronousClipboardRead()) {
            sendTypeList(t, buffer, request.id.data());
            return;
        }
    } else {
        // Deny is answered above, so only Allow/Prompt reach this switch.
        switch (t.osc52ClipboardReadPolicy) {
        case ClipboardReadPolicy::Deny:
            sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EPERM", request.id.data());
            return;
        case ClipboardReadPolicy::Allow:
        case ClipboardReadPolicy::Prompt:
            break;
        }
        if (!clip) {
            sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "ENOSYS", request.id.data());
            return;
        }
    }

    if (t.isOSC52ClipboardReadInFlight) {
        // The one-outstanding-read invariant is shared with OSC 52, so a
        // stream cannot stack prompts by alternating protocols. EBUSY rather
        // than OSC 52's empty reply, because this protocol has a word for it.
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EBUSY", request.id.data());
        return;
    }

    const bool needsDeferral = asked.wantsList
        || t.osc52ClipboardReadPolicy == ClipboardReadPolicy::Prompt
        || clip->requiresAsynchronousClipboardRead();
    if (!needsDeferral) {
        readAndAnswer(t, buffer, { asked.requested.data(), static_cast<std::size_t>(asked.requestedLength) }, request.id.data());
        return;
    }
    // Deferred: the completion needs what the reply must echo, captured now
    // because the accumulator these views borrow is gone when it runs. The
    // selection-set spelling handed to the frontend is OSC 52's, so a prompt
    // that names the buffer keeps working unchanged. The list leg defers with
    // NotRequired — it is consent-exempt and only waits for the capture — so
    // no dialog is raised for it.
    const ClipboardReadConfirmation confirmation = !asked.wantsList && t.osc52ClipboardReadPolicy == ClipboardReadPolicy::Prompt
        ? ClipboardReadConfirmation::Required
        : ClipboardReadConfirmation::NotRequired;
    // The pending state is written before the frontend is asked, because a
    // frontend may answer from inside the ask: a completion that runs before
    // these fields are set would frame the answer as the other protocol's.
    // On a synchronous completion the fields are already consumed and cleared
    // by the time the call returns, and the declined path below un-sets what
    // the completion never saw.
    t.isOSC52ClipboardReadInFlight = true;
    t.isClipboardReadRequestBellTerminated = t.parser.isOSCRequestBellTerminated;
    t.isPendingClipboardReadExtended = true;
    t.isExtendedClipboardReadPrimary = request.isPrimary;
    t.isExtendedClipboardReadListRequested = asked.wantsList;
    std::memcpy(t.extendedClipboardReadID.data(), request.id.data(), t.extendedClipboardReadID.size());
    t.extendedClipboardReadMimes = asked.requested;
    if (!clip->requestClipboardRead(buffer, request.isPrimary ? "p" : "c", confirmation)) {
        t.isOSC52ClipboardReadInFlight = false;
        t.isPendingClipboardReadExtended = false;
        t.isExtendedClipboardReadListRequested = false;
        // A frontend with nowhere to ask has denied the consent the policy
        // required: EPERM, the same answer a refused prompt gives.
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EPERM", request.id.data());
        return;
    }
}

void handleWrite(Ubiquitty::Terminal& t, const RequestMetadata& request)
{
    // Opening a transfer retains bounded bytes; capability consent belongs
    // to its commit so a one-use grant authorizes the clipboard mutation.
    if (t.osc52ClipboardWritePolicy == ClipboardWritePolicy::Deny) {
        resetWriteSession(t);
        sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "EPERM", request.id.data());
        return;
    }
    resetWriteSession(t);
    if (request.hasInvalidEncodedMetadata) {
        sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "EINVAL", request.id.data());
        return;
    }
    t.isExtendedClipboardWriteActive = true;
    t.isExtendedClipboardWritePrimary = request.isPrimary;
    std::memcpy(t.extendedClipboardWriteID.data(), request.id.data(), t.extendedClipboardWriteID.size());
}

// The accumulation buffer for `mime`, existing or newly claimed. Null past
// the item cap: those chunks are dropped, and a typed list then shows what
// is held, so a probe reads the truth. The name always fits the slot — it
// arrives through the session carrier, and the static_assert above keeps the
// two the same size.
[[nodiscard]] Ubiquitty::ByteBuffer* writeItemFor(Ubiquitty::Terminal& t, std::string_view mime)
{
    for (int i = 0; i < t.extendedClipboardWriteItemCount; ++i) {
        if (equalsCaseless(t.extendedClipboardWriteItemMimes[static_cast<std::size_t>(i)].data(), mime))
            return &t.extendedClipboardWriteItems[static_cast<std::size_t>(i)];
    }
    if (t.extendedClipboardWriteItemCount >= Ubiquitty::VT::maxExtendedClipboardWriteItems)
        return nullptr;
    auto& slot = t.extendedClipboardWriteItemMimes[static_cast<std::size_t>(t.extendedClipboardWriteItemCount)];
    std::memcpy(slot.data(), mime.data(), mime.size());
    slot[mime.size()] = 0;
    return &t.extendedClipboardWriteItems[static_cast<std::size_t>(t.extendedClipboardWriteItemCount++)];
}

void commitWriteSession(Ubiquitty::Terminal& t)
{
    if (t.extendedClipboardBase64TailLength != 0) {
        rejectWrite(t);
        return;
    }
    const std::array<char, 65> id = t.extendedClipboardWriteID;
    if (t.osc52ClipboardWritePolicy == ClipboardWritePolicy::Deny
        || !Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ClipboardWrite)) {
        resetWriteSession(t);
        sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "EPERM", id.data());
        return;
    }
    auto* clip = t.clipboardClient.get();
    if (t.didExtendedClipboardWriteOverflow || !clip) {
        resetWriteSession(t);
        sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "EIO", id.data());
        return;
    }
    const Ubiquitty::ClipboardBuffer buffer = t.isExtendedClipboardWritePrimary ? Ubiquitty::ClipboardBuffer::Primary : Ubiquitty::ClipboardBuffer::Clipboard;

    // The typed set: the accumulated items, then one entry per alias carrying
    // the text item's bytes — an alias names the same data, and a typed store
    // holds it as an entry of its own. The array covers the
    // item cap plus every alias the 512-byte list can name at its densest, so
    // no declaration is dropped here; the untyped default picks the first
    // text item, which is exactly what that client stored before types.
    std::array<Ubiquitty::ClipboardItem,
        Ubiquitty::VT::maxExtendedClipboardWriteItems + Ubiquitty::VT::maxExtendedClipboardAliasBytes / 2>
        items;
    std::size_t itemCount = 0;
    std::span<const char> textBytes {};
    for (int i = 0; i < t.extendedClipboardWriteItemCount; ++i) {
        const auto& payload = t.extendedClipboardWriteItems[static_cast<std::size_t>(i)];
        const std::string_view mime { t.extendedClipboardWriteItemMimes[static_cast<std::size_t>(i)].data() };
        const std::span<const char> bytes { payload.data, static_cast<std::size_t>(payload.length) };
        items[itemCount++] = { mime, bytes };
        if (isTextMime(mime))
            textBytes = bytes;
    }
    const std::string_view aliases { t.extendedClipboardSessionAliases.data,
        static_cast<std::size_t>(t.extendedClipboardSessionAliases.length) };
    std::string_view aliasWalk = aliases;
    while (!aliasWalk.empty() && itemCount < items.size()) {
        // The list is canonical — handleWriteAlias builds it single-spaced
        // with no blanks — so the split yields a name every iteration.
        const std::size_t space = aliasWalk.find(' ');
        const std::string_view alias = aliasWalk.substr(0, space);
        aliasWalk = space == std::string_view::npos ? std::string_view {} : aliasWalk.substr(space + 1);
        // A name that already has its own item keeps its own bytes: the item
        // arrived before the declaration, and overriding it would replace
        // real data with a copy of the text.
        bool hasOwnItem = false;
        for (int i = 0; i < t.extendedClipboardWriteItemCount; ++i)
            hasOwnItem = hasOwnItem || equalsCaseless(t.extendedClipboardWriteItemMimes[static_cast<std::size_t>(i)].data(), alias);
        if (!hasOwnItem)
            items[itemCount++] = { alias, textBytes };
    }
    clip->writeClipboardItems(buffer, std::span<const Ubiquitty::ClipboardItem> { items.data(), itemCount });

    // The served aliases back the untyped client's read fallback: the
    // session's declarations replace the previous ones, and a session that
    // declared none clears them — the old names described text this write
    // replaced.
    t.extendedClipboardServedAliases.reset();
    if (!aliases.empty())
        (void)t.extendedClipboardServedAliases.append(aliases, Ubiquitty::VT::maxExtendedClipboardAliasBytes);
    resetWriteSession(t);
    sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "DONE", id.data());
}

// Records a session's alias declarations: names that serve the same data as
// the target MIME. Only a text target can be aliased — the text is the one
// payload every client kind can serve the alias from — and a target outside
// it consumes the packet without recording, which the list reply then shows.
// Names past the bound are dropped the same way: the list names what is
// served, so a probe reads the truth either way.
void handleWriteAlias(Ubiquitty::Terminal& t, const RequestMetadata& request, std::string_view payloadB64)
{
    if (!t.isExtendedClipboardWriteActive)
        return;
    std::array<char, 128> target {};
    const auto decodedTarget = decodeBounded(request.mimeB64, std::span { target }.first(target.size() - 1));
    std::array<char, Ubiquitty::VT::maxExtendedClipboardAliasBytes> names {};
    const auto decodedNames = decodeBounded(payloadB64, std::span { names }.first(names.size() - 1));
    if (!decodedTarget || !decodedNames) {
        const std::array<char, 65> id = t.extendedClipboardWriteID;
        resetWriteSession(t);
        sendStatus(t, Ubiquitty::Capability::ClipboardWrite, "write", "EINVAL", id.data());
        return;
    }
    if (!isTextMime({ decodedTarget->data(), decodedTarget->size() }))
        return;
    std::string_view list { decodedNames->data(), decodedNames->size() };
    while (!list.empty()) {
        const std::size_t space = list.find(' ');
        const std::string_view alias = list.substr(0, space);
        list = space == std::string_view::npos ? std::string_view {} : list.substr(space + 1);
        // "." is the list request's own name, and text/plain is served
        // already; recording either would let the echo collide with them.
        if (alias.empty() || alias == "." || isTextMime(alias))
            continue;
        const std::string_view held { t.extendedClipboardSessionAliases.data,
            static_cast<std::size_t>(t.extendedClipboardSessionAliases.length) };
        if (aliasListContains(held, alias))
            continue;
        if (t.extendedClipboardSessionAliases.length > 0
            && !t.extendedClipboardSessionAliases.append(" ", Ubiquitty::VT::maxExtendedClipboardAliasBytes))
            break;
        if (!t.extendedClipboardSessionAliases.append(alias, Ubiquitty::VT::maxExtendedClipboardAliasBytes))
            break;
    }
}

void handleWriteData(Ubiquitty::Terminal& t, const RequestMetadata& request, std::string_view payloadB64)
{
    if (!t.isExtendedClipboardWriteActive)
        return;
    // A wdata with no MIME is the commit; the id it carries is ignored in
    // favour of the session's, because the session opened the response.
    if (request.mimeB64.empty() && payloadB64.empty()) {
        commitWriteSession(t);
        return;
    }
    if (t.didExtendedClipboardWriteOverflow)
        return;
    if (request.mimeB64.empty()) {
        rejectWrite(t);
        return;
    }
    if (!request.mimeB64.empty()) {
        std::array<char, 128> mime {};
        const auto decodedMime = decodeBounded(request.mimeB64, std::span { mime }.first(mime.size() - 1));
        if (!decodedMime || decodedMime->empty()) {
            rejectWrite(t);
            return;
        }
        mime[decodedMime->size()] = 0;
        if (!equalsCaseless(t.extendedClipboardWriteMime.data(), { decodedMime->data(), decodedMime->size() })) {
            if (t.extendedClipboardBase64TailLength != 0) {
                rejectWrite(t);
                return;
            }
            t.isExtendedClipboardBase64Complete = false;
        }
        std::memcpy(t.extendedClipboardWriteMime.data(), mime.data(), t.extendedClipboardWriteMime.size());
    }
    if (payloadB64.empty())
        return; // a MIME announcement with no bytes yet
    if (t.isExtendedClipboardBase64Complete || !std::ranges::all_of(payloadB64, [](char byte) {
            return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
                || byte == '+' || byte == '/' || byte == '=';
        })) {
        rejectWrite(t);
        return;
    }
    // The poison swallows: an overflowed session decodes nothing further, or
    // an attacker withholding the terminator would drive repeated fill-and-
    // reset cycles through a session whose commit is already lost. Poisoning
    // also frees the items — bytes held for a commit that answers EIO are
    // bytes held for nothing.
    auto poisonSession = [&t] {
        t.didExtendedClipboardWriteOverflow = true;
        for (auto& item : t.extendedClipboardWriteItems)
            item.reset();
        for (auto& mime : t.extendedClipboardWriteItemMimes)
            mime[0] = 0;
        t.extendedClipboardWriteItemCount = 0;
        t.extendedClipboardWriteTotalBytes = 0;
        t.extendedClipboardBase64TailLength = 0;
        t.isExtendedClipboardBase64Complete = false;
    };
    // An aliased name is the target's data under another spelling, so its
    // bytes accumulate in the target's item — always the text, the one target
    // a walias records — and a writer that sends its payload only under the
    // aliased name loses nothing to the naming.
    const std::string_view writeMime { t.extendedClipboardWriteMime.data() };
    const std::string_view sessionAliases { t.extendedClipboardSessionAliases.data,
        static_cast<std::size_t>(t.extendedClipboardSessionAliases.length) };
    Ubiquitty::ByteBuffer* item = writeItemFor(t, aliasListContains(sessionAliases, writeMime) ? textPlain : writeMime);
    if (!item)
        return; // past the item cap: dropped, and a typed list shows what is held
    // Only an unfinished quartet crosses packets; decoded data retains its byte cap.
    std::array<char, chunkRawBytes * 2> decoded;
    constexpr std::size_t sliceB64Chars = sizeof decoded / 3 * 4;
    static_assert(sliceB64Chars % 4 == 0 && Codec::base64EncodedLength(sizeof decoded / 3 * 3) <= sliceB64Chars,
        "a full slice decodes into the block");
    std::string_view remainingB64 = payloadB64;
    while (!remainingB64.empty()) {
        std::string_view encoded;
        if (t.extendedClipboardBase64TailLength || remainingB64.size() < 4) {
            const std::size_t take = std::min(remainingB64.size(), t.extendedClipboardBase64Tail.size() - t.extendedClipboardBase64TailLength);
            std::memcpy(t.extendedClipboardBase64Tail.data() + t.extendedClipboardBase64TailLength, remainingB64.data(), take);
            t.extendedClipboardBase64TailLength += static_cast<std::uint8_t>(take);
            remainingB64.remove_prefix(take);
            if (t.extendedClipboardBase64TailLength < 4)
                return;
            encoded = { t.extendedClipboardBase64Tail.data(), t.extendedClipboardBase64Tail.size() };
            t.extendedClipboardBase64TailLength = 0;
        } else {
            const std::size_t take = std::min(remainingB64.size() / 4 * 4, sliceB64Chars);
            encoded = remainingB64.substr(0, take);
            remainingB64.remove_prefix(take);
        }
        const auto slice = decodeBounded(encoded, decoded);
        if (!slice) {
            rejectWrite(t);
            return;
        }
        if (encoded.back() == '=') {
            t.isExtendedClipboardBase64Complete = true;
            if (!remainingB64.empty()) {
                rejectWrite(t);
                return;
            }
        }
        // The byte cap spans the session's items together: one total a remote
        // can grow, not one per name it can invent. A slice is no longer than
        // the block, so its size fits the int total.
        const int length = static_cast<int>(slice->size());
        if (t.extendedClipboardWriteTotalBytes + length > Ubiquitty::VT::maxExtendedClipboardWriteBytes
            || !item->append({ slice->data(), slice->size() }, Ubiquitty::VT::maxExtendedClipboardWriteBytes)) {
            poisonSession();
            return;
        }
        t.extendedClipboardWriteTotalBytes += length;
    }
}

} // namespace

std::string clipboardPasteNotification(std::string_view types, ClipboardBuffer buffer)
{
    if (types.size() > chunkRawBytes)
        return "\033]5522;type=read:status=EBUSY\033\\";
    std::array<char, chunkRawBytes / 3 * 4 + 8> encoded {};
    const auto characters = Codec::base64Encode(types, encoded);
    if (!characters)
        return "\033]5522;type=read:status=EBUSY\033\\";
    std::string reply = "\033]5522;type=read:status=OK";
    if (buffer == ClipboardBuffer::Primary)
        reply += ":loc=primary";
    reply += "\033\\\033]5522;type=read:status=DATA:mime=Lg==;";
    reply.append(characters->data(), characters->size());
    reply += "\033\\\033]5522;type=read:status=DONE\033\\";
    return reply;
}

void handleExtendedClipboard(Ubiquitty::Terminal& t, std::string_view p)
{
    // The OSC 52 master toggle governs both spellings of the clipboard escape.
    if (!t.isOSC52Allowed)
        return;

    const std::size_t semicolon = p.find(';');
    const std::string_view metadata = p.substr(0, semicolon);
    const std::string_view payload = semicolon == std::string_view::npos ? std::string_view {} : p.substr(semicolon + 1);

    RequestMetadata request;
    parseMetadata(metadata, request);

    if (request.type == "read" && !request.hasInvalidEncodedMetadata)
        handleRead(t, request, payload);
    else if (request.type == "write")
        handleWrite(t, request);
    else if ((request.type == "wdata" || request.type == "walias") && request.hasInvalidEncodedMetadata) {
        if (t.isExtendedClipboardWriteActive)
            rejectWrite(t);
    } else if (request.type == "wdata")
        handleWriteData(t, request, payload);
    else if (request.type == "walias")
        handleWriteAlias(t, request, payload);
    // An unknown type is ignored rather than answered EINVAL: only read and
    // write carry response contracts, and a newer client's newer subcommand
    // deserves forward silence, not an error minted for it.
}

// The 5522 half of the deferred-read completion. Reads what the frontend
// staged and streams it back in this protocol's framing; the OSC 52 half
// lives beside handleClipboard.
void completeExtendedClipboardRead(Ubiquitty::Terminal& t, ClipboardReadDecision decision)
{
    t.isPendingClipboardReadExtended = false;
    const std::array<char, 65> id = t.extendedClipboardReadID;
    t.extendedClipboardReadID[0] = 0;
    const bool wasListRequested = t.isExtendedClipboardReadListRequested;
    t.isExtendedClipboardReadListRequested = false;
    const std::array<char, 256> mimes = t.extendedClipboardReadMimes;
    t.extendedClipboardReadMimes[0] = 0;
    const Ubiquitty::ClipboardBuffer buffer = t.isExtendedClipboardReadPrimary ? Ubiquitty::ClipboardBuffer::Primary : Ubiquitty::ClipboardBuffer::Clipboard;
    if (!t.isOSC52Allowed)
        return;
    if (t.osc52ClipboardReadPolicy == ClipboardReadPolicy::Deny) {
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EPERM", id.data());
        return;
    }
    if (wasListRequested) {
        // The list is consent-exempt, so the decision is not consulted: its
        // deferral existed for the capture, not for a prompt.
        sendTypeList(t, buffer, id.data());
        return;
    }
    if (decision != ClipboardReadDecision::Approved) {
        sendStatus(t, Ubiquitty::Capability::ClipboardRead, "read", "EPERM", id.data());
        return;
    }
    readAndAnswer(t, buffer, { mimes.data(), std::strlen(mimes.data()) }, id.data());
}

} // namespace Ubiquitty::OSC
