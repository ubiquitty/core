// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_kitty_graphics_refusals — the codes a client is told when we say no.
//
// A kitty client reads the status in the reply and acts on it: retry smaller,
// fall back to another transport, report to the user. So the code is a contract
// as much as the accepted path is, and it is the half that ships untested —
// every case here is written against a scene that succeeds, and refusal is what
// happens to the scenes nobody writes down.
//
// `Graphics.cpp` has NINE distinct refusal strings that no succeeding scene
// reaches — `ENOSPC:no image slot`, `ENOMEM:dispatch`, `ENOMEM:b64`,
// `EINVAL:bad payload`, `EINVAL:missing image geometry`,
// `EINVAL:image too large`, `ENOSPC:too many frames`,
// `ENOTSUP:frame format unsupported`, `EINVAL:chunk too large`.
//
// This file takes what a scene can reach without injecting an allocation failure
// or pushing 64 MiB through the parser. Three of the nine need a mechanism
// rather than a case: `ENOMEM:dispatch` and `ENOMEM:b64` want an allocation or a
// queue submit that fails, and `EINVAL:chunk too large` wants more than
// `maxAPCCapacity` — 64 MiB — accumulated through the parser, which is a cost
// this tier should not carry for one line. Inventing a mechanism for the sake of
// a percentage would buy a number instead of a check.
//
// One of the nine cannot be reached at all. `ENOTSUP:frame format unsupported`
// (`kittyAddFrame`) tests a STORED image's format, and every path that stores
// one bounds it to 24 or 32: the direct transmit refuses anything else in
// `kittyStoreDirectImage`, and f=100 is decoded before storage so the format
// recorded is the decoder's. Measured rather than argued — transmitting f=7 and
// f=0 answers `EINVAL:missing image geometry`, f=32 stores 32, and no input
// reached a stored format outside the pair:
//
//     f=24   EINVAL:invalid payload             stored -
//     f=32   OK                                 stored 32
//     f=7    EINVAL:missing image geometry      stored -
//     f=100  EINVAL:png decode failed           stored -
//     f=0    EINVAL:missing image geometry      stored -
//
// So it is defence behind a guard that already holds, and a case for it would
// have to reach past the protocol to construct the image. It is named here
// rather than chased, because the next reader counting uncovered refusals will
// find it too.
//
// `ENOSPC:no image slot` is reachable. It is guarded by `if (!image)` after
// `Image::allocateSlot` at both sites. The allocator evicts the oldest image
// nothing is drawing, and returns null when every stored image is on screen
// (`allocateSlot` in TerminalImage.cpp) — so the guard fires and the string
// ships. The two cases at the end of this file hold both halves: a ninth image
// displaces the oldest undrawn slot, and is refused when every slot is on
// screen and none is free to take.
//
// Each case asserts the CODE, not merely that something was refused. A handler
// that answered `EINVAL:` to everything would satisfy "the image was rejected"
// and tell a client nothing it could act on, and that is the failure a
// count-shaped assertion cannot see.
//
// Both codes below are spelled at TWO sites: once in the load path and once in
// `kittyQuery`, which is the one `a=q` reaches. A mutation of the first
// occurrence leaves these cases green and reads exactly like a case that cannot
// fail — mutate by line, and confirm the string reached the binary before
// concluding anything about the assertion.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using UbiquittyTest::drain;

using UbiquittyTest::Kitty::apc;
using UbiquittyTest::Kitty::base64;
using UbiquittyTest::Kitty::feed;
using UbiquittyTest::Kitty::makePNG;
using UbiquittyTest::Kitty::makeRGBA;

/// The status the terminal replied with, or an empty string when it said
/// nothing.
///
/// The reply is `ESC _ G <status> ESC \`, and only the status is interesting
/// here — a case that compared the whole frame would fail on an id the handler
/// is free to echo.
///
/// - Parameter t: The terminal to drain.
/// - Returns: The status text between the introducer and the terminator.
std::string replyStatus(Ubiquitty::Terminal& t)
{
    char outputBuffer[512];
    const int written = drain(t, outputBuffer);
    const std::string all(outputBuffer, static_cast<std::size_t>(written));
    const std::size_t start = all.find("\033_G");
    if (start == std::string::npos)
        return "";
    const std::size_t end = all.find("\033\\", start);
    const std::size_t from = start + 3;
    return all.substr(from, (end == std::string::npos ? all.size() : end) - from);
}

/// Everything after the id echo, which is what the client reads as the outcome.
///
/// - Parameter status: A reply status, possibly carrying `i=<id>,` first.
/// - Returns: The status with any leading key list removed.
std::string outcome(const std::string& status)
{
    const std::size_t semicolon = status.find(';');
    return semicolon == std::string::npos ? status : status.substr(semicolon + 1);
}

/// The stored image carrying an id, or null.
///
/// - Parameters:
///   - t: The terminal.
///   - imageID: The id to look for.
/// - Returns: The slot, or null when no live slot carries it.
const Ubiquitty::Image::TerminalImage* storedWithID(const Ubiquitty::Terminal& t, std::uint32_t imageID)
{
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i) {
        if (t.images.stored[i].used && t.images.stored[i].imageID == imageID)
            return &t.images.stored[i];
    }
    return nullptr;
}

} // namespace

// A query that names no geometry cannot be answered, and says which part is
// missing rather than refusing generically.
//
// f=24 declares raw 24-bit pixels, for which the sender supplies the dimensions;
// s=0 is the omission a client makes when it forgets them.
TEST(aRawQueryWithoutGeometryIsRefusedForItsGeometry)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, apc("a=q,f=24,s=0,v=1,i=1", "AAAA"));

    const std::string answer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(answer.c_str(), "EINVAL:missing image geometry");
}

// Geometry that would decode past the image bound is refused for its size, and
// not for the geometry above: both guards read the same three keys, and only the
// order between them decides which answer a client gets.
//
// 64 MiB at 4 bytes a pixel over a 100-pixel width is 167,772 rows, so 200,000
// clears it while staying a number a real client could send.
TEST(aRawQueryPastTheImageBoundIsRefusedForItsSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, apc("a=q,f=32,s=100,v=200000,i=2", "AAAA"));

    const std::string answer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(answer.c_str(), "EINVAL:image too large");
}

// The control, and the case that makes the two above about their codes rather
// than about refusal.
//
// The same action with geometry inside the bound is answered OK, so a handler
// that refused every query would fail here — which is what stops "it said
// EINVAL" from being the whole assertion.
TEST(aRawQueryWithinTheBoundIsAccepted)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    const std::string pixels(2 * 2 * 4, 'A'); // 2x2 at f=32, base64 of the right length
    feed(t, apc("a=q,f=32,s=2,v=2,i=3", pixels));

    const std::string answer = outcome(replyStatus(t));
    CHECK_MSG(answer.rfind("EINVAL:missing image geometry", 0) != 0,
        "geometry inside the bound is not a geometry refusal");
    CHECK_MSG(answer.rfind("EINVAL:image too large", 0) != 0,
        "geometry inside the bound is not a size refusal");
}

// A raw transmit refused at any stage leaves the image already stored under its
// id as it was: refused by the decode, by the decoded size, or by the
// compression it names.
//
// The buffer a payload decodes into is the one the image adopts, so every
// refusal after the decode is a place a half-installed image could come from.
// The payloads include the shapes whose decoded capacity is zero ("=", "Q"),
// which are refused before anything is allocated, on both the shared envelope
// and the compressed direct route.
TEST(rawPayloadRefusalsPreserveAnExistingImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::array<std::uint8_t, 5> pixels { 0x12, 0x34, 0x56, 0x78, 0x9A };
    const auto exactPixels = std::span(pixels).first(4);
    const std::string exactPayload = base64(exactPixels);
    feed(t, apc("f=32,s=1,v=1,i=41", exactPayload));
    const std::string accepted = outcome(replyStatus(t));
    CHECK_CSTR_EQ(accepted.c_str(), "OK");
    const auto* original = storedWithID(t, 41);
    REQUIRE(original != nullptr);
    const std::uint32_t generation = original->contentGeneration;

    struct Refusal {
        const char* compressionKeys;
        std::string payload;
        const char* status;
    };
    const std::array<Refusal, 15> refusals { {
        { "", base64(std::span(pixels).first(3)), "EINVAL:payload size mismatch" },
        { "", base64(pixels), "EINVAL:payload too large" },
        { "", "", "EINVAL:invalid payload" },
        { "", "!!!!", "EINVAL:invalid payload" },
        { "", "=", "EINVAL:invalid payload" },
        { "", "Q", "EINVAL:invalid payload" },
        { "", "QQ=", "EINVAL:invalid payload" },
        { "", "QQ A", "EINVAL:invalid payload" },
        { "o=x,", exactPayload, "ENOTSUP:unsupported compression" },
        { "o=x,", base64(pixels), "ENOTSUP:unsupported compression" },
        { "o=x,", "!!!!", "EINVAL:invalid payload" },
        { "o=x,", "=", "EINVAL:invalid payload" },
        { "o=z,", "=", "EINVAL:bad payload" },
        { "o=z,", "Q", "EINVAL:bad payload" },
        { "o=z,", "QQ A", "EINVAL:bad payload" },
    } };
    for (const auto& refusal : refusals) {
        feed(t, apc(std::string(refusal.compressionKeys) + "f=32,s=1,v=1,i=41", refusal.payload));
        const std::string answer = outcome(replyStatus(t));
        CHECK_CSTR_EQ(answer.c_str(), refusal.status);
        const auto* retained = storedWithID(t, 41);
        REQUIRE(retained != nullptr);
        CHECK_EQ(retained->contentGeneration, generation);
        REQUIRE(retained->pixels != nullptr);
        REQUIRE(retained->pixelLength == exactPixels.size());
        CHECK(std::memcmp(retained->pixels, exactPixels.data(), exactPixels.size()) == 0);
    }

    feed(t, apc("a=q,f=32,s=1,v=1,i=41", exactPayload));
    const std::string queryAnswer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(queryAnswer.c_str(), "OK");
}

// The frame bound is a bound, and the refusal says which resource ran out.
//
// A client animating an image adds frames until it stops being told OK, so this
// is the reply that ends a loop rather than an error nobody planned for.
TEST(aFramePastTheBoundIsRefusedForSpace)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> pixels = makeRGBA(2, 2);
    feed(t, apc("a=T,f=32,s=2,v=2,i=41", base64(pixels)));
    Ubiquitty::drainAsyncDecodes(t);
    (void)replyStatus(t);

    // Filled to the bound first: each of these has to be accepted, or the
    // refusal below is about the wrong guard.
    for (int frame = 1; frame < Ubiquitty::Image::maxTerminalImageFrameCount; ++frame) {
        feed(t, apc("a=f,i=41", base64(pixels)));
        const std::string accepted = outcome(replyStatus(t));
        CHECK_MSG(accepted.rfind("ENOSPC", 0) != 0, "a frame inside the bound is not refused for space");
    }

    feed(t, apc("a=f,i=41", base64(pixels)));
    const std::string answer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(answer.c_str(), "ENOSPC:too many frames");
}

// The image table is a fixed eight, and a ninth transmit needs a slot. What
// happens then turns on whether anything is drawing the eight already stored:
// the allocator gives up the oldest image nothing is looking at, and refuses
// only when every slot is on screen. These two cases hold that fork — the same
// ninth transmit, one fill where a slot is free to take and one where none is.
//
// `a=t` transmits without a placement (`TransmitDisplay` in Graphics.cpp), so
// the eight it stores are all undrawn and the oldest is evictable; `a=T`
// installs a placement, so the eight it stores are all on screen and none is.
// Only the fill's case differs between the two, which is what isolates the
// eviction rule from everything else the transmit does.
TEST(aNinthImageEvictsTheOldestImageNothingIsDrawing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    // a=t transmits without displaying, so every stored image is a candidate
    // for eviction — none is held on screen by a placement.
    const std::vector<std::uint8_t> png = makePNG(2, 2);
    for (int id = 1; id <= Ubiquitty::Image::maxTerminalImageCount; ++id) {
        feed(t, apc("a=t,f=100,i=" + std::to_string(id), base64(png)));
        Ubiquitty::drainAsyncDecodes(t);
        (void)replyStatus(t);
    }
    CHECK_MSG(storedWithID(t, 1) != nullptr, "the first image is stored before the table fills");

    feed(t, apc("a=t,f=100,i=99", base64(png)));
    Ubiquitty::drainAsyncDecodes(t);
    const std::string answer = outcome(replyStatus(t));

    CHECK_MSG(answer.rfind("ENOSPC", 0) != 0, "a slot was free to take, so the ninth is not refused");
    CHECK_MSG(storedWithID(t, 99) != nullptr, "the ninth image takes a slot");
    CHECK_MSG(storedWithID(t, 1) == nullptr, "and the oldest undrawn image is the one evicted");
}

// The other half of the fork: with every stored image on screen the allocator
// has no undrawn victim, and the ninth is refused rather than displacing a
// picture a reader is looking at. This is the `ENOSPC:no image slot` that was
// unreachable while a full store evicted stored[0] unconditionally — the same
// string spelled at two sites, so mutate by line to confirm which one shipped.
TEST(aNinthImageIsRefusedWhenEveryImageIsOnScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    // a=T transmits and displays, so every stored image carries a placement and
    // none is evictable.
    const std::vector<std::uint8_t> png = makePNG(2, 2);
    for (int id = 1; id <= Ubiquitty::Image::maxTerminalImageCount; ++id) {
        feed(t, apc("a=T,f=100,i=" + std::to_string(id), base64(png)));
        Ubiquitty::drainAsyncDecodes(t);
        (void)replyStatus(t);
    }
    CHECK_MSG(storedWithID(t, 1) != nullptr, "the first image is stored before the table fills");

    feed(t, apc("a=T,f=100,i=99", base64(png)));
    Ubiquitty::drainAsyncDecodes(t);
    const std::string answer = outcome(replyStatus(t));

    CHECK_CSTR_EQ(answer.c_str(), "ENOSPC:no image slot");
    CHECK_MSG(storedWithID(t, 99) == nullptr, "the refused image is not stored");
    CHECK_MSG(storedWithID(t, 1) != nullptr, "and no image on screen is displaced");
}

// A compressed transmit whose envelope will not decode is refused for the
// payload, before anything tries to inflate it.
//
// o=z with the direct transport is the one route that decodes base64 itself
// rather than through the shared envelope, so this code is reachable only with
// the compression key set — an uncompressed transmit with the same bytes takes
// another path and answers differently.
TEST(aCompressedTransmitWithAnUndecodableEnvelopeIsRefusedForItsPayload)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    // Outside the base64 alphabet, and long enough that the capacity it sizes
    // is non-zero — an empty payload is refused earlier, for having no content.
    feed(t, apc("a=T,f=32,s=2,v=2,o=z,i=50", "@@@@@@@@"));
    const std::string answer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(answer.c_str(), "EINVAL:bad payload");
}

// --- The transport and policy refusals, pinned as whole phrases -------------
//
// Measured before these were written: 28 distinct refusal strings in
// Graphics.cpp, 13 of them asserted nowhere in Tests/ as either the string or
// its message half. test_kitty_graphics_transports.cpp reaches several of the
// same branches and asserts the CODE — `refusal.find("ENOTSUP")` — which a
// handler answering `ENOTSUP:` to everything would satisfy. These pin the
// phrase, which is what a client acts on.
//
// Still unpinned after this file, each for a stated reason:
//   ENOSPC:frame exceeds the image size limit — needs image bytes plus one
//     frame past Image::maxDecodedImageBytes (64 MiB), and every frame is
//     exactly width*height*bpp, so reaching it pushes tens of megabytes
//     through the parser: the same cost bound that leaves EINVAL:chunk too
//     large to a mechanism.
//   ENOMEM:dispatch, ENOMEM:b64 — an allocation or queue submit that fails.
//   ENOTSUP:frame format unsupported, ENOSPC:no image slot — dead behind
//     guards that hold, measured in this file's header.

namespace {

/// A reader whose reads fail, which is a different double from one that
/// serves: `readFile` returning a negative is the transport breaking mid-use,
/// where returning zero bytes is a transport that worked on an empty object.
/// The EBADF refusals are reachable only through the first.
class FailingFileReader final : public Ubiquitty::FileReadClient {
public:
    int readFile(const char*, bool, unsigned char*, int) override { return -1; } // bool-ok: FileReadClient's signature
    int readSharedMemory(const char*, bool, unsigned char*, int) override { return -1; } // bool-ok: FileReadClient's signature
    [[nodiscard]] bool supportsSharedMemoryTransport() const override { return true; }
};

/// A reader that serves, so a gate upstream of the read is the only thing
/// left to refuse.
class ServingFileReader final : public Ubiquitty::FileReadClient {
public:
    int readFile(const char*, bool, unsigned char* out, int capacity) override // bool-ok: FileReadClient's signature
    {
        const int length = capacity < 16 ? capacity : 16;
        for (int i = 0; i < length; ++i)
            out[i] = static_cast<unsigned char>(i);
        return length;
    }
    int readSharedMemory(const char* name, bool shouldConsume, unsigned char* out, int capacity) override // bool-ok: FileReadClient's signature
    {
        return readFile(name, shouldConsume, out, capacity);
    }
    [[nodiscard]] bool supportsSharedMemoryTransport() const override { return true; }
};

std::string base64Text(const std::string& text)
{
    return base64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Ubiquitty::Terminal& freshTerminal(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    return t;
}

} // namespace

// Acting on an image nothing stored names the image as the problem, not the
// action: a client holding a stale id re-transmits, where a generic EINVAL
// would send it hunting through its own keys.
TEST(aDisplayForAnImageNothingStoredSaysNotFound)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);
    feed(t, apc("a=p,i=99", ""));
    const std::string reply = outcome(replyStatus(t));
    CHECK_CSTR_EQ(reply.c_str(), "ENOENT:image not found");
}

// The two animation entry points that need an id refuse a command without
// one, and with no id to answer to, the refusal goes unanswered (kitty
// graphics protocol: the terminal replies when the client sends an id).
TEST(aFrameWithoutAnImageIDIsRefusedUnanswered)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);
    feed(t, apc("a=f,f=32,s=2,v=2", "AAAA"));
    CHECK(replyStatus(t).empty());
}

TEST(anAnimationControlWithoutAnImageIDIsRefusedUnanswered)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);
    feed(t, apc("a=a,s=3", ""));
    CHECK(replyStatus(t).empty());
}

// An action byte outside the protocol is refused as unsupported rather than
// invalid: the sender may be speaking a newer kitty than this terminal, and
// ENOTSUP is the code that says `not here` rather than `not ever`.
TEST(anUnknownActionIsRefusedAsUnsupported)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);
    feed(t, apc("a=z,i=1", ""));
    const std::string reply = outcome(replyStatus(t));
    CHECK_CSTR_EQ(reply.c_str(), "ENOTSUP:unsupported action");
}

// The transport refusals are three distinct phrases, and the distinction is
// what a client falls back on: an unknown `t=` letter can never work, a named
// transport with no reader may work in another terminal, and each names which.
TEST(theThreeUnsupportedTransportsAreToldApart)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);
    CHECK(!t.fileReadClient);

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=x", base64Text("/name")));
    const std::string unknown = outcome(replyStatus(t));
    CHECK_CSTR_EQ(unknown.c_str(), "ENOTSUP:unsupported transport");

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=f", base64Text("/name")));
    const std::string file = outcome(replyStatus(t));
    CHECK_CSTR_EQ(file.c_str(), "ENOTSUP:file transport unavailable");

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=s", base64Text("/name")));
    const std::string shared = outcome(replyStatus(t));
    CHECK_CSTR_EQ(shared.c_str(), "ENOTSUP:shared memory unavailable");
}

// A reader that fails mid-read is EBADF, per transport: the client's name was
// admitted and the read broke, which retries differently from a name that was
// never going to open.
TEST(aReaderThatFailsMidReadSaysWhichTransportBroke)
{
    FailingFileReader reader;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=f", base64Text("/anywhere")));
    const std::string file = outcome(replyStatus(t));
    CHECK_CSTR_EQ(file.c_str(), "EBADF:file transport failed");

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=s", base64Text("/anywhere")));
    const std::string shared = outcome(replyStatus(t));
    CHECK_CSTR_EQ(shared.c_str(), "EBADF:shared memory read failed");
}

// An empty shared-memory payload never reaches the name validation — there is
// no name in it to validate — and fails as the transport rather than the name.
TEST(anEmptySharedMemoryPayloadFailsTheTransportNotTheName)
{
    ServingFileReader reader;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=s", ""));
    const std::string reply = outcome(replyStatus(t));
    CHECK_CSTR_EQ(reply.c_str(), "EBADF:shared memory transport failed");
}

// A name without its leading slash is invalid before any read is attempted.
// The reader here would happily serve it, which is what pins the refusal to
// the validator rather than to the double.
TEST(aSharedMemoryNameWithoutItsSlashIsInvalid)
{
    ServingFileReader reader;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=s", base64Text("noslash")));
    const std::string reply = outcome(replyStatus(t));
    CHECK_CSTR_EQ(reply.c_str(), "EINVAL:invalid shared memory name");
}

namespace {

/// Denies everything except the ACK channel, which is the smallest policy that
/// lets a refusal be heard. `respond()` consults the policy for every reply,
/// `Capability::None` included, so a blanket deny silences the very EPERM the
/// client needs in order to re-transmit — the case below this one pins that.
class DenyingButAudiblePolicy final : public Ubiquitty::CapabilityPolicy {
public:
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        return capability == Ubiquitty::Capability::None
            ? Ubiquitty::CapabilityDecision::Allow
            : Ubiquitty::CapabilityDecision::Deny;
    }
};

} // namespace

// A policy that denies the read turns both host-reaching transports into EPERM
// before any byte moves. The reader serves and the name is valid, so the policy
// is the only thing left to refuse — and EPERM is the code a client
// re-transmits after, because the user may grant what the terminal declined.
TEST(aDenyingCapabilityPolicyIsToldEPERMOnBothTransports)
{
    ServingFileReader reader;
    DenyingButAudiblePolicy policy;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    t.capabilityPolicy = &policy;

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=f", base64Text("/somewhere")));
    const std::string file = outcome(replyStatus(t));
    CHECK_CSTR_EQ(file.c_str(), "EPERM:capability denied");

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=s", base64Text("/somewhere")));
    const std::string shared = outcome(replyStatus(t));
    CHECK_CSTR_EQ(shared.c_str(), "EPERM:capability denied");

    t.capabilityPolicy = nullptr;
}

// A policy that denies Capability::None as well silences the ACK, and the
// client is refused without being told. Graphics.cpp's comment above
// kittyRespond says the ACK can never be an exfiltration reply because
// `Capability::None` is always allowed through the gate — but respond()
// enforces no such bypass: it asks the policy for every reply, None included.
// So the property is an obligation on every policy implementation, and no
// gate holds any implementation to it — this case pins the behaviour that
// exists, so a change on either side of the seam fails a test instead of
// moving silently. A silent denial costs a client its retry: it re-transmits
// on EPERM and cannot re-transmit on nothing.
TEST(aPolicyDenyingTheACKChannelSilencesTheRefusalItself)
{
    ServingFileReader reader;
    // The base class's own check answers Deny to everything, None included.
    Ubiquitty::CapabilityPolicy policy;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    t.capabilityPolicy = &policy;

    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=f", base64Text("/somewhere")));
    const std::string reply = outcome(replyStatus(t));
    CHECK_CSTR_EQ(reply.c_str(), "");

    t.capabilityPolicy = nullptr;
}

namespace {

/// Records what it was asked to read and refuses every read, so a case can see
/// whether a name reached the reader and in what form.
class RecordingFileReader final : public Ubiquitty::FileReadClient {
public:
    int calls = 0;
    std::string lastPath;
    bool wasAskedToConsume = false; // bool-ok: a recorded observation
    bool declaresTemporaryFiles = false; // bool-ok: a fixture switch, not an argument

    int readFile(const char* path, bool shouldConsume, unsigned char*, int) override // bool-ok: FileReadClient's signature
    {
        ++calls;
        lastPath = path ? path : "";
        wasAskedToConsume = shouldConsume;
        return -1;
    }
    [[nodiscard]] bool supportsTemporaryFileTransport() const override { return declaresTemporaryFiles; }
};

} // namespace

// The directory a shell reports is not a root for the file transport. OSC 7 is
// program output, and a remote host behind ssh writes one as easily as the
// local shell does. Resolving a relative path against it handed the reader
// `.ssh/id_ed25519` under whatever directory the sender had just named, without
// the sender needing the username. So Core takes absolute paths only,
// normalizes them on the string, and leaves where they may lie to the frontend
// gate, which reads only temporary directories.
TEST(aReportedDirectoryIsNotARootForTheFileTransport)
{
    RecordingFileReader reader;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    feed(t, "\033]7;file://remote.example/home/user\033\\");
    // The fixture first: the report stands, so what follows is about it.
    CHECK_CSTR_EQ(t.currentDirectory.c_str(), "/home/user");

    // Relative: refused, never resolved against the report.
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text(".ssh/id_ed25519")));
    const std::string relativeRefusal = outcome(replyStatus(t));
    CHECK_CSTR_EQ(relativeRefusal.c_str(), "EPERM:outside confinement");
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text("../../etc/passwd")));
    const std::string climbingRefusal = outcome(replyStatus(t));
    CHECK_CSTR_EQ(climbingRefusal.c_str(), "EPERM:outside confinement");
    CHECK(reader.calls == 0);

    // A `..` past the root: refused the same way.
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text("/../etc/passwd")));
    const std::string rootRefusal = outcome(replyStatus(t));
    CHECK_CSTR_EQ(rootRefusal.c_str(), "EPERM:outside confinement");
    CHECK(reader.calls == 0);

    // Absolute: normalized on the string and handed on, inside the report or
    // not, because the report decides nothing. The gate behind the reader does.
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text("/home/user/docs/../img")));
    CHECK(reader.calls == 1);
    CHECK_CSTR_EQ(reader.lastPath.c_str(), "/home/user/img");
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text("/tmp//./staged")));
    CHECK(reader.calls == 2);
    CHECK_CSTR_EQ(reader.lastPath.c_str(), "/tmp/staged");

    t.fileReadClient = nullptr;
}

// A reader that does not declare t=t is never asked for one. kitten icat
// probes with a t=t file and, once that works, names its PNGs with t=f
// wherever they live, so a reader confined to temporary directories answers
// the probe no and the image is sent inline. A report of "/" first, which made
// every path its descendant, changes nothing: the delete a t=t asks for never
// reaches the reader.
TEST(aTemporaryFileIsRefusedByAReaderThatDoesNotDeclareIt)
{
    RecordingFileReader reader;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    feed(t, "\033]7;file:///\033\\");

    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=t", base64Text("/tmp/tty-graphics-protocol-probe")));
    const std::string probe = outcome(replyStatus(t));
    CHECK_CSTR_EQ(probe.c_str(), "ENOTSUP:temporary file transport unavailable");
    feed(t, apc("a=T,f=24,s=1,v=1,i=1,t=t", base64Text("/tmp/krb5cc_501")));
    const std::string transfer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(transfer.c_str(), "ENOTSUP:temporary file transport unavailable");
    CHECK(reader.calls == 0);

    t.fileReadClient = nullptr;
}

// A t=t without the protocol's `tty-graphics-protocol` marker is refused before
// the reader is asked, transmitted or only probed, by a reader that does take
// t=t. Without the marker a t=t is a request to delete whatever file the sender
// names in /tmp — a credential cache, another program's socket file.
TEST(aTemporaryFileWithoutTheMarkerIsRefusedBeforeAnyRead)
{
    RecordingFileReader reader;
    reader.declaresTemporaryFiles = true;
    Ubiquitty::Terminal t;
    freshTerminal(t);
    t.fileReadClient = &reader;
    feed(t, "\033]7;file:///\033\\");

    feed(t, apc("a=T,f=24,s=1,v=1,i=1,t=t", base64Text("/tmp/krb5cc_501")));
    const std::string transfer = outcome(replyStatus(t));
    CHECK_CSTR_EQ(transfer.c_str(), "EPERM:outside confinement");
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=t", base64Text("/tmp/krb5cc_501")));
    const std::string probe = outcome(replyStatus(t));
    CHECK_CSTR_EQ(probe.c_str(), "EPERM:outside confinement");
    CHECK(reader.calls == 0);

    // With the marker the reader is asked, and asked to consume only by a
    // transfer — the gate behind it decides the rest.
    feed(t, apc("a=T,f=24,s=1,v=1,i=1,t=t", base64Text("/tmp/tty-graphics-protocol-frame")));
    CHECK(reader.calls == 1);
    CHECK_CSTR_EQ(reader.lastPath.c_str(), "/tmp/tty-graphics-protocol-frame");
    CHECK(reader.wasAskedToConsume);
    feed(t, apc("a=q,f=24,s=1,v=1,i=1,t=t", base64Text("/tmp/tty-graphics-protocol-frame")));
    CHECK(reader.calls == 2);
    CHECK(!reader.wasAskedToConsume);

    t.fileReadClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
