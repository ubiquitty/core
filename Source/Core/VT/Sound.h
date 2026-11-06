// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/VT/Sound.h — the sample bank a DCS + s download fills, the sound a
// host forwards to the windows attached to a session, and the WAV decoder
// between the wire and the synth. Header-only and plain C++: the bank is a set
// of PCM buffers, the pending sound a ring of requests, the decoder arithmetic.
//
// A sample is downloaded the way a soft font is: a DCS carries the file, a slot
// number names it, and a DECPS note on the sample voice (wave 6) plays the slot
// the note numbers. The bank is held by pointer off the Terminal and dropped
// with it on RIS, as the soft font is.

#include "PAL/PAL.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace Ubiquitty::VT {

/// One slot per DECPS note number, 1 to 25.
constexpr int sampleSlotCount = 25;
/// The synth's rate; a slot holds mono signed-16 frames at it.
constexpr uint32_t sampleRateHertz = 44100;
/// Ten seconds: a longer download is cut, so a slot bounds what a program can
/// make the terminal hold.
constexpr uint32_t sampleMaxFrames = 441000;

/// The samples a program downloaded, one slot per note number.
struct SampleBank {
    std::array<std::vector<int16_t>, sampleSlotCount> slots; // mono signed-16 at sampleRateHertz; empty = no sample
    // The tag its downloader gave each slot, 0 for none. A slot number alone
    // cannot answer "is the sample I uploaded still here": a program that
    // reattaches to a session finds slot 3 full and has no way to tell its own
    // chime from the one another program put there while it was away. The tag
    // is the downloader's name for its sample, and the only thing that makes
    // the question answerable.
    std::array<uint16_t, sampleSlotCount> tags {};
    uint32_t serial = 0; // bumped by every download, so a mirror knows it is behind

    /// The slot a note names, or nullptr when the note is out of range or the
    /// slot holds nothing.
    ///
    /// - Parameters:
    ///   - number: The note, 1 to 25.
    /// - Returns: The slot's frames, or nullptr.
    [[nodiscard]] const std::vector<int16_t>* slot(int number) const
    {
        if (number < 1 || number > sampleSlotCount || slots[number - 1].empty())
            return nullptr;
        return &slots[number - 1];
    }

    /// The slot holding the sample a program tagged, or 0 when none does.
    ///
    /// The reattach question: a build finishes, the program wants its chime,
    /// and this says whether it still has one or must upload again.
    ///
    /// - Parameters:
    ///   - tag: The downloader's name for its sample; 0 matches nothing.
    /// - Returns: The slot, 1 to 25, or 0.
    [[nodiscard]] int slotForTag(uint16_t tag) const
    {
        if (tag == 0)
            return 0;
        for (int i = 0; i < sampleSlotCount; ++i)
            if (tags[i] == tag && !slots[i].empty())
                return i + 1;
        return 0;
    }

    /// The lowest slot holding nothing, or 0 when every slot is taken.
    ///
    /// What a download that names slot 0 is asking for. Slot numbers are
    /// global to the terminal and programs picked their own, so two of them
    /// reaching for slot 1 overwrote each other with nothing said; asking the
    /// bank is what makes a chime survive the next program that wants one.
    ///
    /// - Returns: A free slot number, 1 to 25, or 0.
    [[nodiscard]] int firstFreeSlot() const
    {
        for (int i = 0; i < sampleSlotCount; ++i)
            if (slots[i].empty())
                return i + 1;
        return 0;
    }

    /// The slots that hold a sample, bit 0 standing for slot 1.
    ///
    /// A program cannot otherwise tell that its sample is gone: RIS empties the
    /// bank and says nothing, so a chime uploaded once silently becomes
    /// silence. One integer answers "is mine still there" for the whole bank.
    ///
    /// - Returns: The occupancy bitmask.
    [[nodiscard]] uint32_t loadedMask() const
    {
        uint32_t mask = 0;
        for (int i = 0; i < sampleSlotCount; ++i)
            if (!slots[i].empty())
                mask |= 1u << i;
        return mask;
    }

    /// Empties a slot, freeing it for the next download that asks for one.
    ///
    /// - Parameters:
    ///   - number: The slot, 1 to 25; anything else is ignored.
    void clearSlot(int number)
    {
        if (number < 1 || number > sampleSlotCount)
            return;
        slots[number - 1] = {};
        tags[number - 1] = 0;
        ++serial;
    }
};

/// Admission of one complete pending sound and its immutable sample snapshot.
enum class PendingSoundAdmission : uint8_t {
    Queued,
    Invalid,
    OverLimit,
    NoMemory,
};

/// Parsed requests with the exact PCM selected at DECPS. Full queues and
/// snapshot refusal leave the entire preceding queue unchanged.
/// The PCM budget is separate from the effect log and its encoding scratch.
struct PendingSound {
    static constexpr int depth = 32;
    static constexpr std::size_t maxPCMBytes = 4 * 1024 * 1024;

    /// A request borrows its own snapshot until this entry is removed.
    struct Entry {
        PAL::PlayTonesRequest request;
        uint32_t sampleSerial = 0;
        std::unique_ptr<int16_t[]> pcm;
    };

    std::array<Entry, depth> requests;
    int head = 0;
    int count = 0;
    std::size_t pcmBytes = 0;
    bool hasResetStop = false;
    Entry resetStop;

    /// Copies the exact parsed sample before any mutable bank can change it.
    /// Admission reserves an ordered slot for a deferred reset stop first.
    /// A refused capture never evicts a preceding request.
    ///
    /// - Parameters:
    ///   - request: The request as the parser built it.
    ///   - sampleSerial: The bank revision selected by that parsed request.
    /// - Returns: Whole-operation admission, without a mutable-bank fallback.
    [[nodiscard]] PendingSoundAdmission push(const PAL::PlayTonesRequest& request, uint32_t sampleSerial)
    {
        if (request.sampleFrames > sampleMaxFrames || (request.sampleFrames && (!request.samplePCM || request.wave != 6)))
            return PendingSoundAdmission::Invalid;
        if (count + (hasResetStop ? 1 : 0) >= depth)
            return PendingSoundAdmission::OverLimit;
        const std::size_t bytes = static_cast<std::size_t>(request.sampleFrames) * sizeof(int16_t);
        if (bytes > maxPCMBytes - pcmBytes)
            return PendingSoundAdmission::OverLimit;
        std::unique_ptr<int16_t[]> captured;
        if (bytes) {
            captured.reset(new (std::nothrow) int16_t[request.sampleFrames]);
            if (!captured)
                return PendingSoundAdmission::NoMemory;
            std::memcpy(captured.get(), request.samplePCM, bytes);
        }
        if (hasResetStop) {
            requests[(head + count) % depth] = {};
            ++count;
            hasResetStop = false;
        }
        Entry& slot = requests[(head + count) % depth];
        slot.request = request;
        slot.sampleSerial = request.wave == 6 ? sampleSerial : 0;
        slot.pcm = std::move(captured);
        slot.request.samplePCM = slot.pcm.get();
        pcmBytes += bytes;
        ++count;
        return PendingSoundAdmission::Queued;
    }

    /// Borrows the oldest request until a successful consumer removes it.
    ///
    /// - Returns: The immutable entry, or null when the queue is empty.
    [[nodiscard]] const Entry* front() const { return count ? &requests[head] : hasResetStop ? &resetStop
                                                                                             : nullptr; }

    /// Orders one channel-zero reset stop after all accepted operations.
    /// Repeated resets with no intervening sound share the same stop marker.
    /// A full ring never evicts an accepted request to make room for this stop.
    void queueResetStop()
    {
        if (hasResetStop)
            return;
        if (count) {
            const auto& last = requests[(head + count - 1) % depth].request;
            if (last.volume == 0 && last.channel == 0)
                return;
        }
        hasResetStop = true;
    }

    /// Retires the oldest accepted request and releases its exact PCM charge.
    /// Call only after the downstream owner has accepted the whole sound.
    void pop()
    {
        if (count == 0) {
            hasResetStop = false;
            return;
        }
        pcmBytes -= static_cast<std::size_t>(requests[head].request.sampleFrames) * sizeof(int16_t);
        requests[head] = {};
        head = (head + 1) % depth;
        --count;
    }
};

namespace WavDetail {

/// How a WAV file's samples are encoded: `fmt ` tag 1 is integer PCM at 8 to
/// 32 bits, tag 3 is IEEE float at 32.
enum class SampleEncoding : uint8_t {
    Integer,
    Float,
};

/// Reads a little-endian 16-bit field, the way RIFF lays one out.
///
/// - Parameters:
///   - p: The field's first byte; two are read.
/// - Returns: The value.
inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

/// Reads a little-endian 32-bit field, the way RIFF lays one out.
///
/// - Parameters:
///   - p: The field's first byte; four are read.
/// - Returns: The value.
inline uint32_t readU32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }

/// Reads one frame of the file as a mono value: the channels averaged, the
/// integer widths scaled, a float taken as it is.
///
/// - Parameters:
///   - frame: The frame's first byte; `channels` samples of `bitsPerSample` are read.
///   - channels: How many samples the frame interleaves.
///   - bitsPerSample: 8, 16, 24 or 32.
///   - encoding: Integer PCM, or IEEE float at 32 bits.
/// - Returns: The frame's value in -1..1.
inline double monoFrame(const uint8_t* frame, int channels, int bitsPerSample, SampleEncoding encoding)
{
    double sum = 0.0;
    const int bytesPerSample = bitsPerSample / 8;
    for (int channel = 0; channel < channels; ++channel) {
        const uint8_t* p = frame + channel * bytesPerSample;
        double value = 0.0;
        if (encoding == SampleEncoding::Float) {
            float f = 0.0f;
            std::memcpy(&f, p, sizeof(f));
            value = f;
        } else if (bitsPerSample == 8) {
            value = (static_cast<int>(p[0]) - 128) / 128.0;
        } else if (bitsPerSample == 16) {
            value = static_cast<int16_t>(readU16(p)) / 32768.0;
        } else if (bitsPerSample == 24) {
            int32_t v = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) | (static_cast<int32_t>(p[2]) << 16);
            if (v & 0x800000)
                v -= 0x1000000;
            value = v / 8388608.0;
        } else {
            value = static_cast<int32_t>(readU32(p)) / 2147483648.0;
        }
        sum += value;
    }
    return std::clamp(sum / channels, -1.0, 1.0);
}

} // namespace WavDetail

/// Decodes a WAV file into the bank's format.
///
/// RIFF/WAVE with a PCM or IEEE-float `fmt ` chunk, plain or extensible, 8, 16,
/// 24 or 32 bits, any channel count mixed to mono, any rate resampled to the
/// synth's by linear interpolation, cut at `sampleMaxFrames`. Anything else is
/// refused, and the output is left untouched.
///
/// - Parameters:
///   - bytes: The file.
///   - out: Receives the frames on success.
/// - Returns: `true` when the file decoded.
[[nodiscard]] inline bool decodeWav(std::span<const uint8_t> bytes, std::vector<int16_t>& out)
{
    using namespace WavDetail;
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return false;
    int channels = 0, bitsPerSample = 0;
    uint32_t rate = 0;
    WavDetail::SampleEncoding encoding = WavDetail::SampleEncoding::Integer;
    bool sawFormat = false;
    const uint8_t* data = nullptr;
    std::size_t dataLength = 0;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const uint8_t* chunk = bytes.data() + offset;
        const uint32_t length = readU32(chunk + 4);
        const std::size_t bodyOffset = offset + 8;
        if (length > bytes.size() - bodyOffset)
            return false;
        const uint8_t* body = bytes.data() + bodyOffset;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && length >= 16) {
            uint16_t format = readU16(body);
            channels = readU16(body + 2);
            rate = readU32(body + 4);
            bitsPerSample = readU16(body + 14);
            if (format == 0xFFFE && length >= 40)
                format = readU16(body + 24); // the extensible header's sub-format
            encoding = format == 3 ? WavDetail::SampleEncoding::Float : WavDetail::SampleEncoding::Integer;
            if ((format != 1 && format != 3) || channels < 1 || channels > 8 || rate < 1000 || rate > 384000)
                return false;
            if (encoding == WavDetail::SampleEncoding::Float ? bitsPerSample != 32 : (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32))
                return false;
            sawFormat = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = body;
            dataLength = length;
        }
        offset = bodyOffset + length + (length & 1u); // chunks are word-aligned
    }
    if (!sawFormat || !data)
        return false;
    const std::size_t frameBytes = static_cast<std::size_t>(channels) * static_cast<std::size_t>(bitsPerSample / 8);
    const std::size_t sourceFrames = dataLength / frameBytes;
    if (sourceFrames == 0)
        return false;
    const double step = static_cast<double>(rate) / sampleRateHertz; // source frames per output frame
    std::size_t outputFrames = static_cast<std::size_t>(std::llround(sourceFrames / step));
    outputFrames = std::min<std::size_t>(std::max<std::size_t>(outputFrames, 1), sampleMaxFrames);
    std::vector<int16_t> frames(outputFrames);
    for (std::size_t i = 0; i < outputFrames; ++i) {
        const double position = i * step;
        const std::size_t index = std::min<std::size_t>(static_cast<std::size_t>(position), sourceFrames - 1);
        const double fraction = position - static_cast<double>(index);
        const double a = monoFrame(data + index * frameBytes, channels, bitsPerSample, encoding);
        const double b = index + 1 < sourceFrames ? monoFrame(data + (index + 1) * frameBytes, channels, bitsPerSample, encoding) : a;
        frames[i] = static_cast<int16_t>(std::lround(std::clamp(a * (1.0 - fraction) + b * fraction, -1.0, 1.0) * 32767.0));
    }
    out = std::move(frames);
    return true;
}

} // namespace Ubiquitty::VT
