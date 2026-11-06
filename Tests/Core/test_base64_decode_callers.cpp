// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_base64_decode_callers — every caller that sizes its decode buffer with
// `Codec::base64DecodedCapacity`, at each final-group shape.
//
// The capacity is exact, so a payload the decoder accepts fills the buffer to
// its last byte and leaves no slack for an off-by-one to hide in. Each case
// feeds payloads whose decoded length is a whole number of groups, one byte
// past one and two bytes past one, padded and unpadded, and asserts that the
// whole payload reached whatever the caller hands it to. Each also feeds a
// payload the decoder refuses, among them shapes with room for no byte at all,
// and asserts the refusal the caller gives for it.

#include "TerminalFixture.h"

#include "Core/Codec/Base64.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Sound.h"
#include "PAL/PAL.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

/// A payload's base64, padded, from the codec's own encoder.
std::string paddedBase64(std::string_view bytes)
{
    std::string encoded(Ubiquitty::Codec::base64EncodedLength(bytes.size()), '\0');
    const auto written = Ubiquitty::Codec::base64Encode(bytes, encoded);
    encoded.resize(written ? written->size() : 0);
    return encoded;
}

/// The same characters with the padding dropped, which every caller accepts.
std::string unpaddedBase64(std::string_view bytes)
{
    std::string encoded = paddedBase64(bytes);
    std::erase(encoded, '=');
    return encoded;
}

/// Both encodings of `bytes`, the shapes each case decodes.
std::array<std::string, 2> encodings(std::string_view bytes) { return { paddedBase64(bytes), unpaddedBase64(bytes) }; }

/// Bytes that do not repeat over any length used here, so a byte lost, moved or
/// added shows in the comparison.
std::string payloadOfLength(std::size_t length)
{
    std::string bytes(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
        bytes[index] = static_cast<char>(index * 37 + 11);
    return bytes;
}

/// Records the last clipboard write and how many there were.
class RecordingClipboard final : public Ubiquitty::ClipboardClient {
public:
    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> data) override
    {
        ++writes;
        written.assign(data.begin(), data.end());
    }

    int writes = 0;
    std::string written;
};

/// Serves one blob to whatever name is asked for, and records the name.
class RecordingFileReader final : public Ubiquitty::FileReadClient {
public:
    int readFile(const char* name, bool, unsigned char* out, int capacity) override // bool-ok: FileReadClient's signature
    {
        ++reads;
        lastName = name ? name : "";
        const int length = std::min(capacity, static_cast<int>(contents.size()));
        std::memcpy(out, contents.data(), static_cast<std::size_t>(length));
        return length;
    }

    std::vector<std::uint8_t> contents;
    std::string lastName;
    int reads = 0;
};

// What the PAL image decoder was last handed. A PAL decoder is a plain
// function pointer, so the record cannot live anywhere narrower than this.
std::vector<std::uint8_t> decodedContainer;
int containerDecodes = 0;

/// Records the container it is handed and answers a 1x1 opaque image.
bool recordingImageDecoder(std::span<const std::uint8_t> bytes, std::uint8_t** rgbaOut, int* widthOut, int* heightOut)
{
    ++containerDecodes;
    decodedContainer.assign(bytes.begin(), bytes.end());
    *rgbaOut = static_cast<std::uint8_t*>(std::malloc(4));
    if (!*rgbaOut)
        return false;
    std::memset(*rgbaOut, 0xFF, 4);
    *widthOut = 1;
    *heightOut = 1;
    return true;
}

Ubiquitty::Ticks stoppedClock() { return Ubiquitty::Ticks(0); }

/// A mono 8-bit WAV at the synth's rate, `frames` long: 44 bytes of headers
/// and one byte a frame, so its length steps through every remainder mod 3.
std::string eightBitWav(std::size_t frames)
{
    std::string wav;
    const auto appendU16 = [&wav](std::uint16_t value) {
        wav += static_cast<char>(value & 0xFF);
        wav += static_cast<char>(value >> 8);
    };
    const auto appendU32 = [&wav](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            wav += static_cast<char>((value >> shift) & 0xFF);
    };
    wav += "RIFF";
    appendU32(static_cast<std::uint32_t>(36 + frames));
    wav += "WAVE";
    wav += "fmt ";
    appendU32(16);
    appendU16(1); // integer PCM
    appendU16(1); // mono
    appendU32(Ubiquitty::VT::sampleRateHertz);
    appendU32(Ubiquitty::VT::sampleRateHertz); // bytes a second, one a frame
    appendU16(1); // bytes a frame
    appendU16(8); // bits a sample
    wav += "data";
    appendU32(static_cast<std::uint32_t>(frames));
    for (std::size_t frame = 0; frame < frames; ++frame)
        wav += static_cast<char>(128 + frame);
    return wav;
}

/// The replies pending on `t`, drained.
std::string replies(Ubiquitty::Terminal& t)
{
    std::array<char, 512> outputBuffer {};
    const int length = drain(t, outputBuffer);
    return std::string(outputBuffer.data(), static_cast<std::size_t>(std::max(length, 0)));
}

// Decoded lengths a whole number of groups, one past and two past. The large
// ones put a single OSC 52 SET or OSC 1337 Copy= past the 256-byte stack
// buffer, onto the heap buffer sized from the capacity.
constexpr std::array<std::size_t, 3> smallLengths { 3, 4, 5 };
constexpr std::array<std::size_t, 3> heapLengths { 300, 301, 302 };

} // namespace

TEST(osc52DecodesEveryFinalGroupIntoItsExactBuffer)
{
    RecordingClipboard clipboard;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clipboard;

    for (const std::size_t length : heapLengths) {
        const std::string payload = payloadOfLength(length);
        for (const std::string& encoded : encodings(payload)) {
            const int writes = clipboard.writes;
            feed(t, "\033]52;c;" + encoded + "\033\\");
            CHECK_EQ(clipboard.writes, writes + 1);
            CHECK_MSG(clipboard.written == payload, encoded.c_str());
        }
    }

    // A chunked transfer decodes its accumulated characters into the same
    // exact buffer, the split falling inside a group.
    for (const auto& lengths : { smallLengths, heapLengths }) {
        for (const std::size_t length : lengths) {
            const std::string payload = payloadOfLength(length);
            for (const std::string& encoded : encodings(payload)) {
                const std::size_t split = encoded.size() / 2 + 1;
                const int writes = clipboard.writes;
                feed(t, "\033]52;c;" + encoded.substr(0, split) + "!\033\\");
                CHECK_EQ(clipboard.writes, writes);
                feed(t, "\033]52;c;" + encoded.substr(split) + "\033\\");
                CHECK_EQ(clipboard.writes, writes + 1);
                CHECK_MSG(clipboard.written == payload, encoded.c_str());
            }
        }
    }

    // Data that is not base64 clears the selection, as xterm specifies: a
    // final group cut to one character on the heap path, and a chunked
    // transfer with room for no byte at all.
    const int writes = clipboard.writes;
    feed(t, "\033]52;c;" + unpaddedBase64(payloadOfLength(300)) + "Q\033\\");
    CHECK_EQ(clipboard.writes, writes + 1);
    CHECK(clipboard.written.empty());
    feed(t, "\033]52;c;" + paddedBase64(payloadOfLength(3)) + "\033\\");
    CHECK(!clipboard.written.empty());
    feed(t, "\033]52;c;Q!\033\\");
    feed(t, "\033]52;c;=\033\\");
    CHECK_EQ(clipboard.writes, writes + 3);
    CHECK(clipboard.written.empty());

    t.clipboardClient = nullptr;
}

TEST(osc1337CopyDecodesEveryFinalGroupIntoItsExactBuffer)
{
    RecordingClipboard clipboard;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clipboard;

    for (const std::size_t length : heapLengths) {
        const std::string payload = payloadOfLength(length);
        for (const std::string& encoded : encodings(payload)) {
            const int writes = clipboard.writes;
            feed(t, "\033]1337;Copy=:" + encoded + "\007");
            CHECK_EQ(clipboard.writes, writes + 1);
            CHECK_MSG(clipboard.written == payload, encoded.c_str());
        }
    }

    // Copy= drops malformed data without writing anything.
    const int writes = clipboard.writes;
    feed(t, "\033]1337;Copy=:" + unpaddedBase64(payloadOfLength(300)) + "Q\007");
    CHECK_EQ(clipboard.writes, writes);

    t.clipboardClient = nullptr;
}

TEST(inlineImageHandsTheDecoderEveryFinalGroupWhole)
{
    Ubiquitty::PAL::Context context;
    context.monotonicClock = &stoppedClock;
    context.imageDecoder = &recordingImageDecoder;
    const Ubiquitty::PAL::ScopedContext scopedContext(context);
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    for (const std::size_t length : { std::size_t { 30 }, std::size_t { 31 }, std::size_t { 32 } }) {
        const std::string payload = payloadOfLength(length);
        for (const std::string& encoded : encodings(payload)) {
            const int decodes = containerDecodes;
            decodedContainer.clear();
            feed(t, "\033]1337;File=width=1;height=1;inline=1:" + encoded + "\007");
            CHECK_EQ(containerDecodes, decodes + 1);
            CHECK_MSG(std::string(decodedContainer.begin(), decodedContainer.end()) == payload, encoded.c_str());
        }
    }

    // Neither a final group cut to one character nor a payload with room for
    // no byte reaches the decoder.
    const int decodes = containerDecodes;
    feed(t, "\033]1337;File=width=1;height=1;inline=1:" + unpaddedBase64(payloadOfLength(30)) + "Q\007");
    feed(t, "\033]1337;File=width=1;height=1;inline=1:=\007");
    CHECK_EQ(containerDecodes, decodes);
}

TEST(sampleDownloadDecodesEveryFinalGroupIntoItsExactBuffer)
{
    for (const std::size_t frames : { std::size_t { 16 }, std::size_t { 17 }, std::size_t { 18 } }) {
        const std::string wav = eightBitWav(frames);
        for (const std::string& encoded : encodings(wav)) {
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            feed(t, "\033P0;0;7+s" + encoded + "\033\\");
            CHECK_MSG(replies(t).find("\033P1;0;7+s\033\\") != std::string::npos, encoded.c_str());
            REQUIRE(t.sampleBank != nullptr);
            const std::vector<std::int16_t>& samples = t.sampleBank->slots[0];
            CHECK_EQ(samples.size(), frames);
            // The last frame sits in the payload's final group.
            if (samples.size() == frames)
                CHECK_EQ(samples.back(), static_cast<std::int16_t>(std::lround((frames - 1) / 128.0 * 32767.0)));
        }
    }

    // A refused download is answered with slot 0 and loads nothing.
    for (const std::string& malformed : { unpaddedBase64(eightBitWav(16)) + "Q", std::string("=") }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033P0;0;7+s" + malformed + "\033\\");
        CHECK(replies(t).find("\033P0;0;7+s\033\\") != std::string::npos);
        CHECK(!t.sampleBank || t.sampleBank->loadedMask() == 0u);
    }
}

TEST(kittyFilePathReachesTheReaderWholeInEveryFinalGroup)
{
    // Declared before the Terminal, which holds a CheckedPtr to it.
    RecordingFileReader reader;
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    reader.contents.assign(2 * 2 * 4, 0x7F);
    t.fileReadClient = &reader;

    std::uint32_t imageID = 20;
    for (const std::string_view path : { "/abcde", "/abcdef", "/abcdefg" }) {
        for (const std::string& encoded : encodings(path)) {
            reader.lastName.clear();
            feed(t, "\033_Ga=T,f=32,s=2,v=2,t=f,i=" + std::to_string(imageID) + ";" + encoded + "\033\\");
            Ubiquitty::drainAsyncDecodes(t);
            CHECK_MSG(reader.lastName == path, encoded.c_str());
            CHECK_MSG(replies(t).find(";OK\033\\") != std::string::npos, encoded.c_str());
            ++imageID;
        }
    }

    // Shapes the decoder refuses, two of them with room for no byte, are the
    // transport's own refusal, and the reader is never asked.
    const int reads = reader.reads;
    for (const std::string& malformed : { std::string("="), std::string("Q"), unpaddedBase64("/abcde") + "Q" }) {
        feed(t, "\033_Ga=T,f=32,s=2,v=2,t=f,i=40;" + malformed + "\033\\");
        Ubiquitty::drainAsyncDecodes(t);
        CHECK_MSG(replies(t).find("EBADF:file transport failed") != std::string::npos, malformed.c_str());
    }
    CHECK_EQ(reader.reads, reads);
}

UBIQUITTY_TEST_MAIN()
