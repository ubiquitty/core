// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_kitty_graphics_transports — where the pixels come from, and the ways that
// can go wrong.
//
// `t=` says the payload is not the image but a way to reach it: a path on disk,
// a temporary file to consume, or a shared-memory object. All three are names a
// remote program chose, handed to the frontend's own reader — so the terminal is
// being asked to open something on behalf of whatever is printing to it, and the
// refusals are the interesting half.
//
// The reader is an interface the frontend supplies, so these drive a double that
// serves bytes from memory. That keeps the cases about the protocol's handling —
// which names are admitted, what a short read does, what happens with no reader
// at all — rather than about any real filesystem.
//
// `o=z` is the other axis: the payload arrives deflated whatever the transport,
// and its inflate runs on a queue with its own worker and completion.

#include "TerminalFixture.h"

#include "Core/Codec/ZlibDeflate.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/Graphics.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

void feed(Ubiquitty::Terminal& t, const std::string& s)
{
    Ubiquitty::processBytes(&t, s);
}

int liveImageCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (const Ubiquitty::Image::TerminalImage& image : t.images.stored) {
        if (image.used && image.pixels)
            ++n;
    }
    return n;
}

const Ubiquitty::Image::TerminalImage* imageWithID(const Ubiquitty::Terminal& t, std::uint32_t id)
{
    for (const Ubiquitty::Image::TerminalImage& image : t.images.stored) {
        if (image.used && image.imageID == id)
            return &image;
    }
    return nullptr;
}

std::string base64(std::span<const std::uint8_t> bytes)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += alphabet[triple & 0x3F];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t single = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += alphabet[(single >> 18) & 0x3F];
        out += alphabet[(single >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t pair = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += alphabet[(pair >> 18) & 0x3F];
        out += alphabet[(pair >> 12) & 0x3F];
        out += alphabet[(pair >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

std::string base64Text(const std::string& s)
{
    return base64(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

std::string apc(const std::string& keys, const std::string& payload = "")
{
    return "\033_G" + keys + ";" + payload + "\033\\";
}

std::vector<std::uint8_t> rgbaPixels(int width, int height)
{
    std::vector<std::uint8_t> v(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
    return v;
}

/// Serves one blob to whatever name is asked for, and records what it was asked.
///
/// The protocol hands this a name a remote program chose, so the double answers
/// every name rather than checking them — the cases are about what the decoder
/// does with the bytes and with a refusal, not about a filesystem's own rules.
class MemoryFileReader final : public Ubiquitty::FileReadClient {
public:
    std::vector<std::uint8_t> contents;
    bool sharesMemory = false; // bool-ok: a fixture switch, not an argument
    int shortReadLimit = -1; // >= 0 serves at most this many bytes
    bool refuses = false; // bool-ok: a fixture switch, not an argument
    std::string lastName;
    bool wasAskedToConsume = false; // bool-ok: a recorded observation
    int readCount = 0;

    int readFile(const char* name, bool shouldConsume, unsigned char* out, int capacity) override // bool-ok: FileReadClient's signature; an override cannot narrow it
    {
        return serve(name, shouldConsume, out, capacity);
    }

    int readSharedMemory(const char* name, bool shouldConsume, unsigned char* out, int capacity) override // bool-ok: FileReadClient's signature; an override cannot narrow it
    {
        return sharesMemory ? serve(name, shouldConsume, out, capacity) : -1;
    }

    [[nodiscard]] bool supportsSharedMemoryTransport() const override { return sharesMemory; }

    [[nodiscard]] bool supportsTemporaryFileTransport() const override { return true; }

private:
    int serve(const char* name, bool shouldConsume, unsigned char* out, int capacity) // bool-ok: carries the overrides' own parameter through unchanged
    {
        lastName = name ? name : "";
        wasAskedToConsume = shouldConsume;
        ++readCount;
        if (refuses)
            return 0;
        int length = static_cast<int>(contents.size());
        if (shortReadLimit >= 0 && shortReadLimit < length)
            length = shortReadLimit;
        if (length > capacity)
            length = capacity;
        if (length > 0)
            std::memcpy(out, contents.data(), static_cast<std::size_t>(length));
        return length;
    }
};

} // namespace

// What the terminal SAYS when it refuses a transport, as distinct from what it
// stores. Every case in this file asserts the second — no image landed — and
// that stays true when a refusal is skipped entirely: the transfer then carries
// an unwritten buffer forward and stores nothing for a different reason.
//
// Two controls pass against the rest of this file and fail here: reporting
// success where the file transport has no reader, and leaving the error phrase
// unset on a malformed direct payload. The first is the one that matters —
// success with no buffer written is a null the caller adopts and decodes.
TEST(testARefusedTransportSaysWhy)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    CHECK(!t.fileReadClient);

    char outputBuffer[512];
    // q=0 is the default: the terminal answers both OK and errors.
    feed(t, apc("a=T,f=32,s=2,v=2,i=1,t=f", base64Text("/anywhere")));
    Ubiquitty::drainAsyncDecodes(t);
    const int refused = UbiquittyTest::drain(t, outputBuffer);
    const std::string refusal(outputBuffer, static_cast<std::size_t>(refused > 0 ? refused : 0));
    CHECK(refused > 0);
    CHECK_MSG(refusal.find("ENOTSUP") != std::string::npos, refusal.c_str());

    // A malformed direct payload names its OWN refusal. The dispatcher's
    // fallback also begins EINVAL, so the phrase is asserted whole: matching the
    // code alone cannot tell the two apart, and lets a control that deletes
    // this phrase pass.
    feed(t, apc("a=T,f=100,s=2,v=2,i=2,t=d,o=z", "!!!not-base64!!!"));
    Ubiquitty::drainAsyncDecodes(t);
    const int badPayload = UbiquittyTest::drain(t, outputBuffer);
    const std::string bad(outputBuffer, static_cast<std::size_t>(badPayload > 0 ? badPayload : 0));
    CHECK(badPayload > 0);
    CHECK_MSG(bad.find("EINVAL:invalid payload") != std::string::npos, bad.c_str());
}

TEST(testAFileTransportNeedsAReaderBeforeItNeedsAName)
{
    // Without a reader the terminal cannot open anything, and saying so is what
    // keeps a client from believing its image was stored. The name is not even
    // looked at.
    // Declared before the Terminal so it outlives it: the Terminal holds a
    // CheckedPtr, which traps if its referent is destroyed first.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    CHECK(!t.fileReadClient);

    for (const char* transport : { "t=f", "t=t", "t=s" }) {
        feed(t, apc(std::string("a=T,f=32,s=2,v=2,i=1,") + transport, base64Text("/anywhere")));
        Ubiquitty::drainAsyncDecodes(t);
        CHECK_MSG(liveImageCount(t) == 0, transport);
    }

    // With a reader, the same transfer lands — so the refusals are about the
    // reader's absence rather than about these keys.
    reader.contents = rgbaPixels(2, 2);
    t.fileReadClient = &reader;
    feed(t, apc("a=T,f=32,s=2,v=2,i=2,t=f", base64Text("/two-by-two")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 2) != nullptr, "a reader makes the file transport work");
    CHECK_MSG(reader.lastName == "/two-by-two", "and the name reaches it as sent");
}

TEST(testATemporaryFileIsConsumedAndAPlainOneIsNot)
{
    // `t=t` names a file the terminal is expected to remove once read; `t=f`
    // names one it must leave alone. Getting that backwards either leaves a
    // client's temporary files behind or deletes a file it still wanted.
    // Before the Terminal: it holds a CheckedPtr to this and traps if the
    // referent dies first.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    reader.contents = rgbaPixels(2, 2);
    t.fileReadClient = &reader;

    feed(t, apc("a=T,f=32,s=2,v=2,i=11,t=f", base64Text("/keep-me")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(imageWithID(t, 11) != nullptr);
    CHECK_MSG(!reader.wasAskedToConsume, "a plain file is left where it was");

    feed(t, apc("a=T,f=32,s=2,v=2,i=12,t=t", base64Text("/tmp/tty-graphics-protocol-take-me")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(imageWithID(t, 12) != nullptr);
    CHECK_MSG(reader.wasAskedToConsume, "a temporary file is consumed");

    // A query attempts the load without storing, and must not consume a file it
    // was only asked about — a probe that deleted the client's data would make
    // asking more expensive than transferring.
    reader.wasAskedToConsume = false;
    feed(t, apc("a=q,f=32,s=2,v=2,i=13,t=t", base64Text("/tmp/tty-graphics-protocol-only-asking")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 13) == nullptr, "a query stores nothing");
    CHECK_MSG(!reader.wasAskedToConsume, "and does not consume the file it probed");
}

TEST(testAReaderThatAnswersNothingOrTooLittleIsRefused)
{
    // The reader's answer is a length, and every value it can return has to be
    // handled: nothing at all, fewer bytes than the geometry needs, and exactly
    // enough. A short read installed as a whole image is a buffer with a tail
    // nothing wrote.
    // Before the Terminal: it holds a CheckedPtr to this and traps if the
    // referent dies first.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    reader.contents = rgbaPixels(4, 4);
    t.fileReadClient = &reader;

    reader.refuses = true;
    feed(t, apc("a=T,f=32,s=4,v=4,i=21,t=f", base64Text("/unreadable")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 21) == nullptr, "a reader that answers nothing stores nothing");

    reader.refuses = false;
    reader.shortReadLimit = 8; // far fewer than 4*4*4
    feed(t, apc("a=T,f=32,s=4,v=4,i=22,t=f", base64Text("/truncated")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 22) == nullptr, "a short read is refused rather than padded");

    reader.shortReadLimit = -1;
    feed(t, apc("a=T,f=32,s=4,v=4,i=23,t=f", base64Text("/whole")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 23) != nullptr, "the whole file loads");
    CHECK_MSG(reader.readCount >= 3, "every attempt reached the reader");
}

TEST(testAFileLongerThanItsGeometryIsRefusedRatherThanCropped)
{
    // The sibling case above stops at "exactly enough". Too many bytes is the
    // arm that cannot fail by arriving short, so nothing about it follows from
    // the short-read result: a reader fills up to the capacity it is given, and
    // the capacity here is the geometry's own byte count. An oversized file
    // therefore comes back at exactly the expected length, which is the value
    // the size check compares against — so the check agrees with a file it never
    // saw the end of, and the image installs cropped.
    //
    // Cropped is not a smaller image. The bytes kept are the first s*v*4 of a
    // wider picture, so every row after the first is offset by the difference
    // and the result is skewed, not truncated. Nothing in the reply says so.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.fileReadClient = &reader;

    // A 5x5 file offered as a 4x4 image: 100 bytes where the geometry needs 64.
    reader.contents = rgbaPixels(5, 5);
    feed(t, apc("a=T,f=32,s=4,v=4,i=41,t=f", base64Text("/oversized")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 41) == nullptr,
        "a file longer than its geometry is refused, not cropped to fit");

    // The same reader and the same geometry with a file that fits must still
    // install. A fixture that had stopped serving anything satisfies the
    // assertion above on its own, and this is what separates the two.
    reader.contents = rgbaPixels(4, 4);
    feed(t, apc("a=T,f=32,s=4,v=4,i=42,t=f", base64Text("/exact")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 42) != nullptr, "a file matching its geometry still loads");

    // The same oversized bytes down the direct transport, which carries its own
    // length and so can compare one. Whether the protocol wants an overlong
    // payload refused or its tail ignored is a policy question; the two
    // transports answering it differently for identical bytes is not.
    const std::vector<std::uint8_t> oversized = rgbaPixels(5, 5);
    feed(t, apc("a=T,f=32,s=4,v=4,i=43,t=d", base64(oversized)));
    Ubiquitty::drainAsyncDecodes(t);
    const bool isInstalledViaDirect = imageWithID(t, 43) != nullptr;
    const bool isInstalledViaFile = imageWithID(t, 41) != nullptr;
    CHECK_MSG(isInstalledViaDirect == isInstalledViaFile,
        "t=d and t=f agree about a payload longer than the geometry");
}

TEST(testACompressedFileMayBeLongerThanWhatItInflatesTo)
{
    // Compression does not only shrink. A stream of bytes with no redundancy
    // comes out of deflate larger than it went in, by the block header and the
    // adler tail — so the file holding it is longer than the geometry it
    // describes. Sizing the read to the inflated length truncates that file, and
    // a truncated deflate stream is an inflate failure, so a valid transfer is
    // refused for a reason that is about the buffer rather than about the image.
    //
    // The direct transport cannot reach this: its payload arrives whole and its
    // length is the payload's own.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    t.fileReadClient = &reader;

    // Incompressible by construction, and deterministic so the sizes below are
    // the same on every run and every platform.
    std::vector<std::uint8_t> raw(3 * 3 * 4);
    std::uint32_t state = 0x12345678u;
    for (std::uint8_t& byte : raw) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    const std::vector<std::uint8_t> deflated = Ubiquitty::Codec::Zlib::compress(
        std::span<const std::uint8_t>(raw.data(), raw.size()));
    CHECK_MSG(deflated.size() > raw.size(),
        "the fixture's bytes are incompressible, so the stream grew");

    reader.contents = deflated;
    feed(t, apc("a=T,f=32,s=3,v=3,i=51,t=f,o=z", base64Text("/grown")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 51) != nullptr,
        "a compressed file longer than its inflated size still loads");
}

TEST(testSharedMemoryNeedsTheReaderToSayItSupportsIt)
{
    // A reader that does not do shared memory must be told apart from one that
    // does and failed, because the first is a capability answer and the second
    // is an error about a specific object.
    // Before the Terminal: it holds a CheckedPtr to this and traps if the
    // referent dies first.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    reader.contents = rgbaPixels(2, 2);
    t.fileReadClient = &reader;

    reader.sharesMemory = false;
    feed(t, apc("a=T,f=32,s=2,v=2,i=31,t=s", base64Text("/ubiquitty-shm")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 31) == nullptr, "a reader without shared memory refuses");

    reader.sharesMemory = true;
    feed(t, apc("a=T,f=32,s=2,v=2,i=32,t=s", base64Text("/ubiquitty-shm")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 32) != nullptr, "and one with it succeeds");
    CHECK_MSG(reader.lastName == "/ubiquitty-shm", "the object name reaches the reader");
}

TEST(testASharedMemoryNameTheTerminalWillNotOpenIsRefused)
{
    // The name is chosen by whatever is printing to the terminal, so it is
    // checked before it reaches the reader. A name that escapes its namespace is
    // the one that matters: this is the terminal opening an object on behalf of
    // a remote program.
    // Before the Terminal: it holds a CheckedPtr to this and traps if the
    // referent dies first.
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    reader.contents = rgbaPixels(2, 2);
    reader.sharesMemory = true;
    t.fileReadClient = &reader;

    const int before = reader.readCount;
    for (const char* name : { "", "..", "../escape", "/a/b", "with/slash",
             "with\nnewline", "with\ttab" }) {
        feed(t, apc("a=T,f=32,s=2,v=2,i=41,t=s", base64Text(name)));
        Ubiquitty::drainAsyncDecodes(t);
        CHECK_MSG(imageWithID(t, 41) == nullptr, name);
    }
    CHECK_MSG(reader.readCount == before, "a refused name never reaches the reader");

    // The control: an ordinary name does reach it, so the refusals are about
    // the names rather than about shared memory being off.
    feed(t, apc("a=T,f=32,s=2,v=2,i=42,t=s", base64Text("/ubiquitty-42")));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 42) != nullptr, "an ordinary name is admitted");
    CHECK(reader.readCount > before);
}

TEST(testACompressedPayloadInflatesOnItsOwnQueue)
{
    // `o=z` says the bytes are deflated, whatever carried them. The inflate runs
    // on a queue of its own with a worker and a completion, so a compressed
    // transfer's effect is split across the parse and the drain exactly as the
    // PNG path's is.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    const std::vector<std::uint8_t> pixels = rgbaPixels(4, 3);
    const std::vector<std::uint8_t> deflated = Ubiquitty::Codec::Zlib::compress(
        std::span<const std::uint8_t>(pixels.data(), pixels.size()));
    CHECK_MSG(!deflated.empty(), "the fixture compressed something");

    feed(t, apc("a=T,f=32,s=4,v=3,i=51,o=z", base64(deflated)));
    Ubiquitty::drainAsyncDecodes(t);
    const Ubiquitty::Image::TerminalImage* image = imageWithID(t, 51);
    CHECK_MSG(image != nullptr, "a compressed transfer installs");
    CHECK(image->width == 4 && image->height == 3);
    CHECK_MSG(image->pixelLength == 4 * 3 * 4, "and inflates to the stated geometry");

    // Bytes that are not deflate data are refused rather than installed at
    // whatever length they happen to inflate to.
    feed(t, apc("a=T,f=32,s=4,v=3,i=52,o=z", base64(pixels)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 52) == nullptr, "undeflatable bytes are refused");

    // A compressed payload that inflates to the wrong size for the geometry is
    // the case a length and a geometry disagree, and it must not install.
    const std::vector<std::uint8_t> wrongSize = Ubiquitty::Codec::Zlib::compress(
        std::span<const std::uint8_t>(pixels.data(), pixels.size() / 2));
    feed(t, apc("a=T,f=32,s=4,v=3,i=53,o=z", base64(wrongSize)));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK_MSG(imageWithID(t, 53) == nullptr, "an inflate short of the geometry is refused");
}

// The two axes composed — a file transport carrying deflated bytes — has no
// case here. Driven through the double, `t=f,o=z` did not install and the
// same bytes without `o=z` did, which is the opposite of what the keys say and
// means one of two things: the compression key does not reach the file path, or
// the fixture's deflated blob is being read as pixels by length. Distinguishing
// them takes a measurement this file does not make, and a case asserting either
// reading would be pinning a guess about Core.
//
// Left unwritten rather than written wrong. The two axes are covered separately
// above, so what is missing is their composition.

/// Which shared-memory object names the terminal will open, at every bound.
///
/// `t=s` hands a name a remote program chose to the frontend's reader, so what
/// the validator admits is what a remote can ask the frontend to open. The
/// existing cases use valid names only, and the mutation gate reports the whole
/// validator as unguarded: its length bounds and its printable-byte range can
/// each be moved by one without a test noticing.
///
/// Each bound is checked from both sides, because a case that only tries the
/// rejected side agrees with a validator that rejects everything.
TEST(testWhichSharedMemoryNamesAreAdmitted)
{
    MemoryFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    reader.contents = rgbaPixels(2, 2);
    reader.sharesMemory = true;
    t.fileReadClient = &reader;

    // The shortest name the rule allows is two bytes, and the longest 31.
    const std::string longest = "/" + std::string(30, 'a');
    const std::string tooLong = "/" + std::string(31, 'a');

    struct Name {
        std::string name;
        bool isAdmitted;
        const char* why;
    };
    const Name names[] = {
        { "/a", true, "two bytes is the shortest allowed" },
        { "/", false, "one byte is shorter than the rule allows" },
        { longest, true, "thirty-one bytes is the longest allowed" },
        { tooLong, false, "thirty-two is longer than the rule allows" },
        { "ubiquitty", false, "a name must open with a slash" },
        { "/a/b", false, "and carry no other slash" },
        { std::string("/a\x20"
                      "b"),
            false, "space sits below the printable range" },
        { std::string("/a\x21"
                      "b"),
            true, "and 0x21 is its first member" },
        { std::string("/a\x7E"
                      "b"),
            true, "0x7E is the last member" },
        { std::string("/a\x7F"
                      "b"),
            false, "and 0x7F sits above it" },
    };

    int id = 60;
    for (const Name& probe : names) {
        ++id;
        reader.lastName.clear();
        feed(t, apc("a=T,f=32,s=2,v=2,i=" + std::to_string(id) + ",t=s", base64Text(probe.name)));
        Ubiquitty::drainAsyncDecodes(t);
        // The reader seeing the name is the discriminator. An image that fails
        // to load and a name that never left Core both leave no image, so
        // counting images agrees with itself on both arms.
        const bool didReachTheReader = reader.lastName == probe.name;
        CHECK_MSG(didReachTheReader == probe.isAdmitted, probe.why);
    }
}

UBIQUITTY_TEST_MAIN()
