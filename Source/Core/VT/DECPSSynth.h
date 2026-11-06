// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/VT/DECPSSynth.h — the portable DECPS (DEC Play Sound) synth.
//
// A DECPS request is a sequence of notes, one after another, so the VT520's
// speaker could play it with one tone generator: a square wave, switched on
// and off. That voice is here, and beside it the voices and the channels the
// request's sub-parameters name (PAL::PlayTonesRequest): sine, triangle,
// sawtooth, noise and a drum, an envelope per note, and nine channels that
// each play their requests in turn and sound together, mixed with a soft
// limit. Everything is plain floating-point math with no platform dependency;
// a frontend that can open an audio device owns one ToneMixer, feeds it every
// request and pulls its output. The note→frequency mapping and the parse live
// in Core/VT/Prefixed.cpp's csiH_decps.

#include "Core/VT/Sound.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Ubiquitty::VT {

constexpr double toneSampleRate = 44100.0;
constexpr int toneMaxNotes = PAL::PlayTonesRequest::maxNotes;
/// Channel 0 is the terminal's speaker, the one a plain DECPS plays through;
/// 1 to 8 are the tracker's.
constexpr int toneChannelCount = 9;
/// Requests a channel holds beyond the one playing. A request past the depth
/// is dropped rather than growing the queue without bound.
constexpr int toneQueueDepth = 32;
/// A tracker's tick, the step of every effect: fifty a second.
constexpr uint32_t toneTickSamples = 882;

/// The tracker effects a note can carry, in the XM numbering.
enum class ToneEffect : uint8_t {
    Arpeggio = 0,
    PortamentoUp = 1,
    PortamentoDown = 2,
    TonePortamento = 3,
    Vibrato = 4,
    Tremolo = 7,
    SampleOffset = 9,
    VolumeSlide = 10,
    SetVolume = 12,
    Extended = 14, ///< The high nibble picks: 9 retrigger, 12 note cut, 13 note delay.
};

/// The voices a request can ask for, by its wave sub-parameter.
enum class ToneWave : uint8_t {
    Square = 0, ///< The VT520's speaker: on or off, no envelope of its own.
    Sine = 1,
    Triangle = 2,
    Sawtooth = 3,
    Noise = 4, ///< White noise; the note's pitch is ignored, a rest is still silent.
    Drum = 5, ///< A sine whose pitch falls two octaves and whose level decays through the note.
    Sample = 6, ///< A downloaded sample, replayed on every sounding note; transposition slows or speeds it.
};

// The top of each request parameter: the plain form's from the VT520 manual,
// the sub-parameters' from the synth. The parser clamps to these, and DECRQSS
// `,~` reports them, so a program can tell the tracker from a terminal whose
// DECPS is the speaker alone before it sends a score.
constexpr int toneMaxVolume = 7; // and the sustain level, on the same scale
constexpr int toneMaxWave = static_cast<int>(ToneWave::Sample);
constexpr int toneMaxChannel = toneChannelCount - 1;
constexpr int toneMaxEnvelopeMilliseconds = 5000; // attack, decay and release each
constexpr int toneMaxSemitonesDown = 48;
constexpr int toneMaxDurationUnits = 255; // and the gap, which cannot exceed the duration
constexpr int toneNoteCount = 49; // C5 to C9; the VT520 stops at 25
constexpr int toneMaxEffect = 15;
constexpr int toneMaxParameter = 255;
constexpr int toneMaxGridUnits = 255; // in 1/32 s, so a grid of up to eight seconds
constexpr int toneMaxCutoffHertz = 20000; // a filter's cutoff; the top of hearing
constexpr int toneMaxPan = 64; // hard right; 0 is hard left and 32 the centre
constexpr int tonePanCentre = toneMaxPan / 2;
constexpr int toneMaxRepeat = 255; // and the value that means "until stopped"

[[nodiscard]] inline ToneWave toneWaveFor(uint8_t code) { return code <= toneMaxWave ? static_cast<ToneWave>(code) : ToneWave::Square; }

/// The two side gains for a pan position, 0 hard left to 64 hard right.
///
/// Centre is 1 and 1 rather than the 0.707 a constant-power law would give: a
/// caller that names no pan must not pay three decibels for the parameter's
/// existence, and a centred request must be as loud as the same request on one
/// channel. Each side falls linearly to silence at the far edge.
///
/// - Parameters:
///   - pan: The position, clamped into 0 to `toneMaxPan`.
///   - left: Receives the left gain, 0 to 1.
///   - right: Receives the right gain, 0 to 1.
inline void toneGainsForPan(int pan, double& left, double& right)
{
    const double position = std::clamp(pan, 0, toneMaxPan) / static_cast<double>(toneMaxPan);
    left = std::min(1.0, 2.0 * (1.0 - position));
    right = std::min(1.0, 2.0 * position);
}

/// Maps a DECPS volume 1..7 to a modest amplitude (0.07..0.49), keeping headroom
/// against the sum of channels. Volume 0 is silence.
[[nodiscard]] inline double toneAmplitudeForVolume(uint8_t volume) { return (std::min<int>(volume, 7) / 7.0) * 0.49; }

/// Maps a per-note duration in milliseconds to a sample count, floored to one
/// sample so a note is never zero-length.
[[nodiscard]] inline uint32_t toneSamplesForMilliseconds(uint32_t milliseconds)
{
    if (milliseconds == 0)
        milliseconds = 1;
    const uint32_t samples = static_cast<uint32_t>(toneSampleRate * milliseconds / 1000.0);
    return samples == 0 ? 1u : samples;
}

/// The gain that makes a note at `hertz` as loud as the same amplitude at C5.
///
/// The ear is most sensitive around three kilohertz, so a sine at 2 kHz sounds
/// louder than one at 500 Hz at the same amplitude, and a sequence that climbs
/// gets louder as it climbs. The correction follows the A-weighting curve
/// (IEC 61672), the standard fit of the equal-loudness contours, at half its
/// strength since the contours flatten at listening level, and is bounded to
/// one octave of gain either way so a bass note cannot be boosted into the
/// limiter.
[[nodiscard]] inline double toneLoudnessGain(double hertz)
{
    if (hertz <= 0.0)
        return 0.0;
    const auto weighting = [](double f) {
        const double f2 = f * f;
        const double ratio = (12194.0 * 12194.0 * f2 * f2)
            / ((f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
        return 20.0 * std::log10(ratio) + 2.0;
    };
    const double decibels = (weighting(hertz) - weighting(523.0)) * 0.5;
    return std::clamp(std::pow(10.0, -decibels / 20.0), 0.5, 2.0);
}

/// The gain that brings a voice to the loudness of a sine at the same amplitude:
/// a square wave carries far more energy and harmonics in the ear's sensitive
/// band, a triangle a little less than a sine, noise spreads its energy wide.
[[nodiscard]] inline double toneWaveGain(ToneWave wave)
{
    switch (wave) {
    case ToneWave::Square:
        return 0.45;
    case ToneWave::Sine:
        return 1.0;
    case ToneWave::Triangle:
        return 1.15;
    case ToneWave::Sawtooth:
        return 0.55;
    case ToneWave::Noise:
        return 0.35;
    case ToneWave::Drum:
        return 1.2;
    case ToneWave::Sample:
        return 1.0;
    }
    return 1.0;
}

/// The attack and release a voice takes when the request names none, in
/// milliseconds. A step at a note's edge is a broadband click, so the smooth
/// voices ramp over 4 ms, short enough to leave the pitch intact; the speaker,
/// the noise and the drum keep their edges, which is their character.
[[nodiscard]] inline uint16_t toneDefaultRamp(ToneWave wave)
{
    return (wave == ToneWave::Square || wave == ToneWave::Noise || wave == ToneWave::Drum || wave == ToneWave::Sample) ? 0 : 4;
}

/// A second-order filter section, the cookbook's, in transposed direct form
/// II: a Butterworth low-pass or high-pass at a cutoff, applied to a job's
/// output after its envelope and before its send.
struct ToneBiquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;

    /// The section's next output for `input`.
    ///
    /// - Parameters:
    ///   - input: The sample going in.
    [[nodiscard]] double process(double input)
    {
        const double output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }
};

/// The section for a cutoff: the low-pass keeps what lies below it, the
/// high-pass what lies above, each falling twelve decibels an octave past it
/// with a Butterworth's flat top. A cutoff at or past the Nyquist frequency
/// is pulled just under it, so the section stays stable.
///
/// - Parameters:
///   - cutoffHertz: The cutoff, above zero.
///   - isHighpass: A high-pass rather than a low-pass.
[[nodiscard]] inline ToneBiquad toneFilterSection(double cutoffHertz, bool isHighpass) // bool-ok: the two sections differ by one sign in their numerator
{
    const double hertz = std::clamp(cutoffHertz, 1.0, toneSampleRate * 0.49);
    const double w0 = 2.0 * M_PI * hertz / toneSampleRate;
    const double cosine = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * M_SQRT1_2);
    const double a0 = 1.0 + alpha;
    ToneBiquad section;
    if (isHighpass) {
        section.b0 = (1.0 + cosine) / 2.0 / a0;
        section.b1 = -(1.0 + cosine) / a0;
        section.b2 = (1.0 + cosine) / 2.0 / a0;
    } else {
        section.b0 = (1.0 - cosine) / 2.0 / a0;
        section.b1 = (1.0 - cosine) / a0;
        section.b2 = (1.0 - cosine) / 2.0 / a0;
    }
    section.a1 = -2.0 * cosine / a0;
    section.a2 = (1.0 - alpha) / a0;
    return section;
}

/// Whether a job's repeats are a count it spends or a loop that outlasts them.
enum class ToneRepeat : uint8_t {
    Counted, ///< `repeatsLeft` passes remain, then the job leaves the channel.
    UntilStopped, ///< The job repeats until the channel is stopped.
};

/// One request being played: its notes as frequencies and amplitudes, the
/// per-note hold, envelope and gap in samples, and the cursors ``toneSample``
/// advances. Plain data, so the synthesis is testable without an audio device.
struct ToneJob {
    ToneWave wave = ToneWave::Square;
    int noteCount = 0;
    std::array<double, toneMaxNotes> frequency = {}; // Hz; 0 = a rest
    std::array<double, toneMaxNotes> amplitude = {}; // 0..1, after volume, voice and loudness
    std::array<uint8_t, toneMaxNotes> effect = {}; // ToneEffect per note
    std::array<uint8_t, toneMaxNotes> parameter = {}; // the effect's byte
    uint32_t samplesPerNote = 1;
    uint32_t soundingSamples = 1; // samplesPerNote less the gap
    uint32_t attackSamples = 0;
    uint32_t decaySamples = 0;
    double sustain = 1.0; // the level the decay falls to, of the peak
    uint32_t releaseSamples = 0;
    int note = 0; // index of the note sounding
    uint32_t position = 0; // sample within the note
    double phase = 0.0; // oscillator phase, wrapped to [0, 2π)
    uint32_t noise = 0x2545F491u; // the noise voice's generator state
    std::vector<int16_t> sample; // the sample voice's frames, the job's own copy
    double sampleBaseStep = 1.0; // frames the cursor moves a sample: the transposition
    double sampleStep = 1.0; // the base step bent by the note's effects
    double sampleCursor = 0.0; // position in the sample, in frames
    // The effects' state, reset at every note: the pitch a slide or an
    // arpeggio adds in semitones, the vibrato and tremolo phases in cycles,
    // the gain a slide or a set volume leaves, and the tick last stepped.
    double slideSemitones = 0.0;
    double effectSemitones = 0.0;
    double vibratoPhase = 0.0;
    double tremoloPhase = 0.0;
    double slideGain = 1.0;
    double effectGain = 1.0;
    int lastTick = -1;
    bool isCut = false;
    bool isDelayed = false;
    double lastSoundingFrequency = 0.0; // the pitch a tone portamento starts from
    double reverbSend = 0.0; // the share of the job sent to the hall, 0..1
    bool hasLowpass = false;
    bool hasHighpass = false;
    ToneBiquad lowpass; // the job's filters, their state carried across its notes
    ToneBiquad highpass;
    uint32_t gridSamples = 0; // the grid the job starts on, in samples; 0 = none
    bool isPlaced = false; // whether the job's start has been set against the grid
    uint32_t startDelay = 0; // samples of silence before the job starts, from the grid
    double gainLeft = 1.0; // the pan's two side gains, applied where the channel mixes
    double gainRight = 1.0;
    uint16_t repeatsLeft = 0; // passes still to play after this one
    ToneRepeat repeat = ToneRepeat::Counted;
};

/// Whether a finished job plays again rather than leaving the channel.
///
/// - Parameters:
///   - job: The job whose repeat state is read.
/// - Returns: Whether another pass follows this one.
[[nodiscard]] inline bool toneJobRepeats(const ToneJob& job) { return job.repeat == ToneRepeat::UntilStopped || job.repeatsLeft > 0; }

[[nodiscard]] inline bool toneJobIsDone(const ToneJob& job) { return job.note >= job.noteCount; }

/// Seeds a job from a request: the voice, each note's frequency after the
/// transposition and its amplitude after the volume (the note's own where it
/// has one), the voice's gain and the loudness gain, the hold, the gap, and
/// the envelope, the voice's own where the request leaves it.
[[nodiscard]] inline ToneJob makeToneJob(const PAL::PlayTonesRequest& request)
{
    ToneJob job;
    job.wave = toneWaveFor(request.wave);
    job.noteCount = std::clamp(request.count, 0, toneMaxNotes);
    const double transposition = std::pow(2.0, -static_cast<double>(std::min<int>(request.semitonesDown, 48)) / 12.0);
    const double voiceGain = toneWaveGain(job.wave);
    for (int i = 0; i < job.noteCount; ++i) {
        const double hertz = request.freqs[i] * transposition;
        job.frequency[i] = hertz;
        const uint8_t volume = request.noteVolumes[i] != 0 ? request.noteVolumes[i] : request.volume;
        // A sample has no pitch to weight; its level is the volume's.
        const double loudness = job.wave == ToneWave::Sample ? 1.0 : toneLoudnessGain(hertz);
        job.amplitude[i] = hertz > 0.0 ? toneAmplitudeForVolume(volume) * voiceGain * loudness : 0.0;
    }
    job.samplesPerNote = toneSamplesForMilliseconds(request.durationMilliseconds);
    if (job.wave == ToneWave::Sample) {
        if (request.samplePCM && request.sampleFrames > 0)
            job.sample.assign(request.samplePCM, request.samplePCM + std::min<uint32_t>(request.sampleFrames, sampleMaxFrames));
        job.sampleBaseStep = transposition;
        job.sampleStep = transposition;
        // A duration of zero holds each note for the sample, at its speed.
        if (request.durationMilliseconds == 0)
            job.samplesPerNote = std::max<uint32_t>(1u, static_cast<uint32_t>(std::ceil(job.sample.size() / job.sampleStep)));
    }
    const uint32_t gap = static_cast<uint32_t>(toneSampleRate * request.gapMilliseconds / 1000.0);
    job.soundingSamples = gap >= job.samplesPerNote ? 1u : job.samplesPerNote - gap;
    const uint16_t attack = request.attackMilliseconds == PAL::PlayTonesRequest::waveDefaultEnvelope ? toneDefaultRamp(job.wave) : request.attackMilliseconds;
    const uint16_t release = request.releaseMilliseconds == PAL::PlayTonesRequest::waveDefaultEnvelope ? toneDefaultRamp(job.wave) : request.releaseMilliseconds;
    // Each ramp fits inside the half of the note it shapes; the decay takes
    // what the attack leaves of the first half.
    const uint32_t half = job.soundingSamples / 2;
    job.attackSamples = std::min(static_cast<uint32_t>(toneSampleRate * attack / 1000.0), half);
    job.decaySamples = std::min(static_cast<uint32_t>(toneSampleRate * request.decayMilliseconds / 1000.0), half - job.attackSamples);
    job.sustain = std::min<int>(request.sustainLevel, 7) / 7.0;
    job.releaseSamples = std::min(static_cast<uint32_t>(toneSampleRate * release / 1000.0), half);
    for (int i = 0; i < job.noteCount; ++i) {
        job.effect[i] = request.noteEffects[i];
        job.parameter[i] = request.noteParameters[i];
    }
    job.reverbSend = std::min<int>(request.reverbSend, toneMaxVolume) / static_cast<double>(toneMaxVolume);
    job.hasLowpass = request.lowpassHertz > 0;
    job.hasHighpass = request.highpassHertz > 0;
    if (job.hasLowpass)
        job.lowpass = toneFilterSection(std::min<int>(request.lowpassHertz, toneMaxCutoffHertz), false);
    if (job.hasHighpass)
        job.highpass = toneFilterSection(std::min<int>(request.highpassHertz, toneMaxCutoffHertz), true);
    job.gridSamples = static_cast<uint32_t>(toneSampleRate * std::min<int>(request.gridUnits, toneMaxGridUnits) / 32.0);
    toneGainsForPan(request.pan, job.gainLeft, job.gainRight);
    job.repeat = request.repeat == PAL::PlayTonesRequest::repeatForever ? ToneRepeat::UntilStopped : ToneRepeat::Counted;
    job.repeatsLeft = job.repeat == ToneRepeat::UntilStopped ? 0 : request.repeat;
    return job;
}

/// Returns a finished job to its first note so it plays again.
///
/// The seeded parameters stay — the notes, the envelope, the pan, the sample's
/// frames — and only the cursors and the per-note effect state reset, so a loop
/// costs nothing beyond what the first pass already built. The filter sections
/// keep their state across the seam: clearing them would put a step at every
/// loop point, which is the one place a listener would hear it.
///
/// - Parameters:
///   - job: The finished job to rewind; its repeat count is spent here.
inline void toneRewindJob(ToneJob& job)
{
    if (job.repeat == ToneRepeat::Counted && job.repeatsLeft > 0)
        --job.repeatsLeft;
    job.note = 0;
    job.position = 0;
    job.phase = 0.0;
    job.sampleCursor = 0.0;
    job.sampleStep = job.sampleBaseStep;
    job.lastTick = -1;
    job.isCut = false;
    job.isDelayed = false;
    job.slideSemitones = 0.0;
    job.effectSemitones = 0.0;
    job.vibratoPhase = 0.0;
    job.tremoloPhase = 0.0;
    job.slideGain = 1.0;
    job.effectGain = 1.0;
    job.lastSoundingFrequency = 0.0;
}

/// Steps a note's effect at a tick: the arpeggio's turn, a slide's step, the
/// vibrato's and tremolo's phases, a volume slide, a set volume, a retrigger,
/// a cut or a delay.
///
/// - Parameters:
///   - job: The job whose sounding note carries the effect.
///   - tick: The tick within the note, from 0.
inline void toneStepEffect(ToneJob& job, int tick)
{
    const ToneEffect effect = static_cast<ToneEffect>(job.effect[job.note]);
    const uint8_t parameter = job.parameter[job.note];
    const int x = parameter >> 4;
    const int y = parameter & 15;
    switch (effect) {
    case ToneEffect::Arpeggio:
        job.effectSemitones = parameter == 0 ? 0.0 : (tick % 3 == 0 ? 0.0 : (tick % 3 == 1 ? x : y));
        break;
    // The slides step from the note's second tick, as a tracker's do: the
    // first tick is the note itself.
    case ToneEffect::PortamentoUp:
        if (tick > 0)
            job.slideSemitones += parameter / 16.0;
        break;
    case ToneEffect::PortamentoDown:
        if (tick > 0)
            job.slideSemitones -= parameter / 16.0;
        break;
    case ToneEffect::TonePortamento:
        // From the last pitch toward the note's own, a step a tick, arriving
        // exactly rather than overshooting.
        if (tick == 0)
            break;
        if (job.slideSemitones > 0.0)
            job.slideSemitones = std::max(0.0, job.slideSemitones - parameter / 16.0);
        else if (job.slideSemitones < 0.0)
            job.slideSemitones = std::min(0.0, job.slideSemitones + parameter / 16.0);
        break;
    case ToneEffect::Vibrato:
        job.effectSemitones = std::sin(2.0 * M_PI * job.vibratoPhase) * (y / 8.0);
        job.vibratoPhase += x / 16.0;
        break;
    case ToneEffect::Tremolo:
        job.effectGain = std::max(0.0, 1.0 + std::sin(2.0 * M_PI * job.tremoloPhase) * (y / 16.0));
        job.tremoloPhase += x / 16.0;
        break;
    case ToneEffect::SampleOffset:
        if (tick == 0)
            job.sampleCursor = std::min<double>(parameter * 256.0, job.sample.empty() ? 0.0 : static_cast<double>(job.sample.size() - 1));
        break;
    case ToneEffect::VolumeSlide:
        if (tick > 0)
            job.slideGain = std::clamp(job.slideGain + (x - y) / 64.0, 0.0, 1.0);
        break;
    case ToneEffect::SetVolume:
        job.slideGain = std::min<int>(parameter, 64) / 64.0;
        break;
    case ToneEffect::Extended:
        if (x == 9 && y > 0 && tick > 0 && tick % y == 0) {
            job.phase = 0.0;
            job.sampleCursor = 0.0;
        } else if (x == 12) {
            job.isCut = tick >= y;
        } else if (x == 13) {
            job.isDelayed = tick < y;
        }
        break;
    }
}

/// The next sample of a job, advancing its cursors; 0 once every note is done.
///
/// A note is silent in its gap, in a rest, and at zero amplitude. The attack
/// rises from zero at the note's first sample, the decay falls to the sustain
/// level, and the release reaches zero at the last sounding sample, so a
/// shaped note meets its boundaries silent. Every tick the note's effect is
/// stepped, and the pitch and gain it leaves apply until the next.
[[nodiscard]] inline double toneSample(ToneJob& job)
{
    if (toneJobIsDone(job))
        return 0.0;
    const double hertz = job.frequency[job.note];
    double amplitude = job.amplitude[job.note];
    double sample = 0.0;
    const int tick = static_cast<int>(job.position / toneTickSamples);
    if (tick != job.lastTick) {
        if (job.lastTick < 0) {
            // The note begins: its effect state is fresh, and a tone
            // portamento starts from the pitch before it.
            job.slideSemitones = 0.0;
            job.effectSemitones = 0.0;
            job.vibratoPhase = 0.0;
            job.tremoloPhase = 0.0;
            job.slideGain = 1.0;
            job.effectGain = 1.0;
            job.isCut = false;
            job.isDelayed = false;
            if (static_cast<ToneEffect>(job.effect[job.note]) == ToneEffect::TonePortamento && job.lastSoundingFrequency > 0.0 && hertz > 0.0)
                job.slideSemitones = 12.0 * std::log2(job.lastSoundingFrequency / hertz);
        }
        job.lastTick = tick;
        toneStepEffect(job, tick);
    }
    if (job.position < job.soundingSamples && hertz > 0.0 && amplitude > 0.0 && !job.isCut && !job.isDelayed) {
        double gain = job.sustain;
        if (job.attackSamples > 0 && job.position < job.attackSamples)
            gain = static_cast<double>(job.position) / job.attackSamples;
        else if (job.decaySamples > 0 && job.position < job.attackSamples + job.decaySamples)
            gain = 1.0 - (1.0 - job.sustain) * static_cast<double>(job.position - job.attackSamples) / job.decaySamples;
        const uint32_t remaining = job.soundingSamples - job.position;
        if (job.releaseSamples > 0 && remaining <= job.releaseSamples)
            gain *= static_cast<double>(remaining - 1) / job.releaseSamples;
        gain *= job.slideGain * job.effectGain;
        double frequency = hertz * std::pow(2.0, (job.slideSemitones + job.effectSemitones) / 12.0);
        if (job.wave == ToneWave::Drum) {
            // The pitch falls from the note two octaves in a few tens of
            // milliseconds and the level decays through the note: a kick.
            const double seconds = job.position / toneSampleRate;
            frequency = hertz * (0.25 + 0.75 * std::exp(-seconds / 0.03));
            amplitude *= std::exp(-seconds / 0.12);
        }
        if (job.wave == ToneWave::Sample)
            job.sampleStep = job.sampleBaseStep * (frequency / hertz);
        const double turn = job.phase / (2.0 * M_PI); // 0..1 through the cycle
        switch (job.wave) {
        case ToneWave::Sine:
        case ToneWave::Drum:
            sample = std::sin(job.phase);
            break;
        case ToneWave::Square:
            sample = turn < 0.5 ? 1.0 : -1.0;
            break;
        case ToneWave::Triangle:
            sample = 4.0 * std::fabs(turn - 0.5) - 1.0;
            break;
        case ToneWave::Sawtooth:
            sample = 2.0 * turn - 1.0;
            break;
        case ToneWave::Noise:
            job.noise ^= job.noise << 13;
            job.noise ^= job.noise >> 17;
            job.noise ^= job.noise << 5;
            sample = (job.noise / 4294967296.0) * 2.0 - 1.0;
            break;
        case ToneWave::Sample: {
            // The frame under the cursor, interpolated with the next, and the
            // cursor moved by the transposition's step: silence past the end.
            const std::size_t index = static_cast<std::size_t>(job.sampleCursor);
            if (index + 1 < job.sample.size()) {
                const double fraction = job.sampleCursor - static_cast<double>(index);
                sample = (job.sample[index] * (1.0 - fraction) + job.sample[index + 1] * fraction) / 32768.0;
            } else if (index < job.sample.size()) {
                sample = job.sample[index] / 32768.0;
            }
            job.sampleCursor += job.sampleStep;
            break;
        }
        }
        sample *= amplitude * gain;
        if (job.hasLowpass)
            sample = job.lowpass.process(sample);
        if (job.hasHighpass)
            sample = job.highpass.process(sample);
        job.phase += 2.0 * M_PI * frequency / toneSampleRate;
        if (job.phase >= 2.0 * M_PI)
            job.phase -= 2.0 * M_PI;
    }
    if (++job.position >= job.samplesPerNote) {
        if (hertz > 0.0)
            job.lastSoundingFrequency = hertz;
        job.position = 0;
        job.phase = 0.0;
        job.sampleCursor = 0.0;
        job.lastTick = -1;
        ++job.note;
    }
    return sample;
}

/// Fills a buffer with one job's next slice, tail-padded with silence once every
/// note has been emitted, so a queue drains to quiet.
///
/// - Parameters:
///   - job: The request to advance; its cursors are updated.
///   - out: Destination for `frames` signed-16 PCM samples.
///   - frames: How many samples to write.
/// - Returns: `true` while notes remain, `false` once the tail is padded.
inline bool fillTones(ToneJob& job, int16_t* out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i)
        out[i] = static_cast<int16_t>(std::clamp(toneSample(job), -1.0, 1.0) * 32767.0);
    return !toneJobIsDone(job);
}

/// Moves a job `samples` into its playing, as though it had started that long
/// ago: the notes those samples cover are passed over, and the cursors of the
/// note reached are set to the point within it.
///
/// - Parameters:
///   - job: The job to move, at its start.
///   - samples: How far in to move it.
inline void toneSkipInto(ToneJob& job, uint32_t samples)
{
    while (!toneJobIsDone(job) && samples >= job.samplesPerNote - job.position) {
        samples -= job.samplesPerNote - job.position;
        if (job.frequency[job.note] > 0.0)
            job.lastSoundingFrequency = job.frequency[job.note];
        job.position = 0;
        ++job.note;
    }
    if (toneJobIsDone(job))
        return;
    job.position += samples;
    job.phase = std::fmod(2.0 * M_PI * job.frequency[job.note] * job.position / toneSampleRate, 2.0 * M_PI);
    job.sampleCursor = job.position * job.sampleStep;
    job.lastTick = -1;
}

/// The grid the channels start on. Its origin is where the first request that
/// named a grid started, and it is forgotten when every channel falls silent
/// or the synth is stopped, so the next such request starts at once and sets
/// it afresh.
struct ToneGrid {
    bool hasOrigin = false;
    uint64_t origin = 0;
};

/// Sets a job's start against the grid, once, as it comes to the head of its
/// channel at `clock`. A job without a grid starts at once, as does the first
/// with one, which sets the origin. After that a job starts on the grid: at
/// its next point, or, if the last point passed less than a tenth of the grid
/// ago, in step with it, as though it had started there. So requests sent
/// together start together to the sample, a part entering on an idle channel
/// and sent early waits for the bar, and one sent a little late joins it.
///
/// - Parameters:
///   - job: The job at the head of its channel, not yet placed.
///   - clock: The mixer's sample count now.
///   - grid: The grid the channels share; set by the first gridded job.
inline void placeToneJob(ToneJob& job, uint64_t clock, ToneGrid& grid)
{
    job.isPlaced = true;
    if (job.gridSamples == 0)
        return;
    if (!grid.hasOrigin) {
        grid.hasOrigin = true;
        grid.origin = clock;
        return;
    }
    const uint64_t since = (clock - grid.origin) % job.gridSamples;
    if (since == 0)
        return;
    if (since < job.gridSamples / 10)
        toneSkipInto(job, static_cast<uint32_t>(since));
    else
        job.startDelay = static_cast<uint32_t>(job.gridSamples - since);
}

/// A hall the channels send to: Schroeder's design as Freeverb tuned it, four
/// parallel combs, each damped by a one-pole low-pass in its feedback, into
/// two allpasses in series, sized for 44.1 kHz and about a second of decay.
/// Mono, since the output is.
struct ToneReverb {
    /// A feedback comb: a delay line fed back through a one-pole low-pass, so
    /// the echoes darken as they fade.
    struct Comb {
        std::vector<float> line;
        std::size_t index = 0;
        float store = 0.0f;

        /// The comb's next output for `input`, fed back damped.
        ///
        /// - Parameters:
        ///   - input: The sample going in.
        [[nodiscard]] float process(float input)
        {
            const float output = line[index];
            store = output * 0.75f + store * 0.25f;
            line[index] = input + store * 0.86f;
            if (++index == line.size())
                index = 0;
            return output;
        }
    };
    /// A Schroeder allpass: a delay that smears the combs' echoes without
    /// colouring them.
    struct Allpass {
        std::vector<float> line;
        std::size_t index = 0;

        /// The allpass's next output for `input`: the delay's, less the input.
        ///
        /// - Parameters:
        ///   - input: The sample going in.
        [[nodiscard]] float process(float input)
        {
            const float delayed = line[index];
            line[index] = input + delayed * 0.5f;
            if (++index == line.size())
                index = 0;
            return delayed - input;
        }
    };
    std::array<Comb, 4> combs;
    std::array<Allpass, 2> allpasses;

    /// Sizes the delay lines to Freeverb's lengths at 44.1 kHz, all silent.
    ToneReverb()
    {
        const std::array<std::size_t, 4> combLengths = { 1116, 1188, 1277, 1356 };
        for (std::size_t i = 0; i < combs.size(); ++i)
            combs[i].line.assign(combLengths[i], 0.0f);
        const std::array<std::size_t, 2> allpassLengths = { 556, 441 };
        for (std::size_t i = 0; i < allpasses.size(); ++i)
            allpasses[i].line.assign(allpassLengths[i], 0.0f);
    }

    /// The hall's next sample for `input`, the channels' sends summed.
    ///
    /// - Parameters:
    ///   - input: The sends, summed.
    [[nodiscard]] double process(double input)
    {
        float sum = 0.0f;
        for (Comb& comb : combs)
            sum += comb.process(static_cast<float>(input));
        float output = sum * 0.25f;
        for (Allpass& allpass : allpasses)
            output = allpass.process(output);
        return output * 0.6;
    }
};

/// One channel: the request playing and the ones queued behind it, in a ring.
struct ToneChannel {
    std::array<ToneJob, toneQueueDepth> jobs;
    int head = 0;
    int count = 0;

    [[nodiscard]] bool isIdle() const { return count == 0; }

    /// Queues a job behind the ones the channel holds; a channel at depth drops it.
    /// A job queued behind another follows it the sample after it ends, so a
    /// bar cut into runs stays one bar; only a job that finds the channel idle
    /// is placed against the grid.
    ///
    /// - Parameters:
    ///   - job: The job to play once the queue ahead of it has played.
    void push(const ToneJob& job)
    {
        if (count == toneQueueDepth)
            return;
        ToneJob& slot = jobs[(head + count) % toneQueueDepth];
        slot = job;
        slot.isPlaced = count > 0;
        ++count;
    }

    /// Empties the channel, the playing job included.
    void clear()
    {
        head = 0;
        count = 0;
    }

    /// Mixes the channel's next sample into the stereo pair.
    ///
    /// The playing job's sample, panned by its own position, and the next job's
    /// the sample after it finishes, so requests join without a gap. A job
    /// coming to the head is placed against the grid first, and one placed
    /// ahead of its point is silent until it. A job that finished but still has
    /// repeats rewinds in place instead of leaving, so the pass after it starts
    /// on the very next sample and no silence falls at the loop point.
    ///
    /// - Parameters:
    ///   - clock: The mixer's sample count, the grid's time base.
    ///   - grid: The grid the channels share.
    ///   - toHall: The sum of the channels' sends, this one's added.
    ///   - left: The left side of the mix, this channel's contribution added.
    ///   - right: The right side of the mix.
    void sample(uint64_t clock, ToneGrid& grid, double& toHall, double& left, double& right)
    {
        while (count > 0) {
            ToneJob& job = jobs[head];
            if (!job.isPlaced)
                placeToneJob(job, clock, grid);
            if (toneJobIsDone(job)) {
                if (!toneJobRepeats(job)) {
                    head = (head + 1) % toneQueueDepth;
                    --count;
                    continue;
                }
                toneRewindJob(job);
            }
            if (job.startDelay > 0) {
                --job.startDelay;
                return;
            }
            const double value = toneSample(job);
            toHall += value * job.reverbSend;
            left += value * job.gainLeft;
            right += value * job.gainRight;
            return;
        }
    }

    /// The channel's next sample summed to one side, without a grid or a hall.
    ///
    /// The shape a test drives a single channel through, and the reason the
    /// stereo pair above is not the only way in.
    [[nodiscard]] double sample()
    {
        ToneGrid none;
        double toHall = 0.0, left = 0.0, right = 0.0;
        sample(0, none, toHall, left, right);
        return (left + right) * 0.5;
    }
};

/// The nine channels summed, their sends through the hall, with a soft limit
/// so a chord of channels cannot clip. A frontend owns one, hands it every
/// request and pulls its output.
struct ToneMixer {
    std::array<ToneChannel, toneChannelCount> channels;
    ToneGrid grid;
    ToneReverb hall;
    uint64_t clock = 0; // samples mixed so far, the grid's time base

    [[nodiscard]] bool isIdle() const
    {
        for (const ToneChannel& channel : channels)
            if (!channel.isIdle())
                return false;
        return true;
    }

    /// Takes a request: at volume 0 it stops its channel, on channel 0 every
    /// channel and the grid with them; otherwise its job queues on its channel.
    ///
    /// - Parameters:
    ///   - request: The parsed DECPS request.
    void enqueue(const PAL::PlayTonesRequest& request)
    {
        const int index = std::min<int>(request.channel, toneChannelCount - 1);
        if (request.volume == 0) {
            if (index == 0) {
                for (ToneChannel& channel : channels)
                    channel.clear();
                grid.hasOrigin = false;
            } else {
                channels[index].clear();
            }
            return;
        }
        if (request.count <= 0)
            return;
        channels[index].push(makeToneJob(request));
    }

    /// Mixes the next `frames` frames into `out` as interleaved stereo.
    ///
    /// Each channel is panned by its playing request; the hall the channels
    /// share is mono and added to both sides, which is what a small reverb on a
    /// notification or a jingle wants and costs one filter chain rather than
    /// two. The soft limit is per side, so a hard-panned part cannot clip the
    /// side it sits on.
    ///
    /// - Parameters:
    ///   - out: Destination, which must hold `frames * 2` signed-16 samples:
    ///     a frame is a left and a right, interleaved.
    ///   - frames: How many frames to write.
    /// - Returns: `true` while any channel still has a request to play.
    bool mix(int16_t* out, uint32_t frames)
    {
        for (uint32_t i = 0; i < frames; ++i) {
            double left = 0.0;
            double right = 0.0;
            double toHall = 0.0;
            for (ToneChannel& channel : channels)
                channel.sample(clock, grid, toHall, left, right);
            const double toBoth = hall.process(toHall);
            out[i * 2] = static_cast<int16_t>(std::tanh(left + toBoth) * 32767.0);
            out[i * 2 + 1] = static_cast<int16_t>(std::tanh(right + toBoth) * 32767.0);
            ++clock;
            if (grid.hasOrigin && isIdle())
                grid.hasOrigin = false;
        }
        return !isIdle();
    }
};

} // namespace Ubiquitty::VT
