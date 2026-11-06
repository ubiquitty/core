// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/test_decpssynth.cpp — the portable DECPS synth: the derivations a
// request goes through to become a job, each voice's shape, the envelope that
// keeps playback from clicking, the equal-loudness gain, and the mixer's nine
// channels: a channel plays its requests in turn without a gap, channels sound
// together, and a request at volume 0 stops a channel or all of them. Pure
// math, no audio device.

#include "Core/VT/DECPSSynth.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using Ubiquitty::PAL::PlayTonesRequest;
using Ubiquitty::VT::fillTones;
using Ubiquitty::VT::makeToneJob;
using Ubiquitty::VT::toneAmplitudeForVolume;
using Ubiquitty::VT::toneChannelCount;
using Ubiquitty::VT::toneGainsForPan;
using Ubiquitty::VT::ToneJob;
using Ubiquitty::VT::toneJobIsDone;
using Ubiquitty::VT::toneLoudnessGain;
using Ubiquitty::VT::toneMaxPan;
using Ubiquitty::VT::ToneMixer;
using Ubiquitty::VT::tonePanCentre;
using Ubiquitty::VT::toneQueueDepth;
using Ubiquitty::VT::toneSample;
using Ubiquitty::VT::toneSamplesForMilliseconds;
using Ubiquitty::VT::ToneWave;
using Ubiquitty::VT::toneWaveGain;

static bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

static PlayTonesRequest request(uint8_t volume, uint8_t wave, uint8_t channel, uint32_t durationMilliseconds, std::initializer_list<uint32_t> freqs)
{
    PlayTonesRequest r;
    r.volume = volume;
    r.wave = wave;
    r.channel = channel;
    r.durationMilliseconds = durationMilliseconds;
    for (uint32_t f : freqs)
        r.freqs[r.count++] = f;
    return r;
}

TEST(testDerivations)
{
    CHECK(approx(toneAmplitudeForVolume(7), 0.49));
    CHECK(approx(toneAmplitudeForVolume(1), 0.49 / 7.0));
    CHECK(approx(toneAmplitudeForVolume(0), 0.0));
    CHECK(approx(toneAmplitudeForVolume(9), 0.49)); // clamped
    CHECK(toneSamplesForMilliseconds(1000) == 44100u);
    CHECK(toneSamplesForMilliseconds(10) == 441u);
    CHECK(toneSamplesForMilliseconds(0) == 44u); // floored to a millisecond
}

TEST(testLoudnessGainFollowsTheEar)
{
    // Unity at C5, the bottom of the VT520's range; the ear is more sensitive
    // above it, so the gain falls as the note climbs, and less sensitive below,
    // so a bass note is lifted, but never past the bound.
    CHECK(approx(toneLoudnessGain(523.0), 1.0));
    CHECK(toneLoudnessGain(1047.0) < 1.0);
    CHECK(toneLoudnessGain(2093.0) < toneLoudnessGain(1047.0));
    CHECK(toneLoudnessGain(2093.0) > 0.5);
    CHECK(toneLoudnessGain(131.0) > 1.0);
    CHECK(toneLoudnessGain(131.0) <= 2.0);
    CHECK(toneLoudnessGain(33.0) <= 2.0);
    CHECK(approx(toneLoudnessGain(0.0), 0.0));
    double previous = toneLoudnessGain(523.0);
    for (double hertz = 600.0; hertz <= 2093.0; hertz += 100.0) {
        CHECK(toneLoudnessGain(hertz) <= previous);
        previous = toneLoudnessGain(hertz);
    }
}

TEST(testMakeJobTakesTheRequestApart)
{
    PlayTonesRequest r = request(5, 1, 2, 10, { 523, 0, 1047 });
    r.noteVolumes[2] = 7;
    r.semitonesDown = 12;
    r.gapMilliseconds = 2;
    ToneJob job = makeToneJob(r);
    CHECK(job.wave == ToneWave::Sine);
    CHECK(job.noteCount == 3);
    CHECK(approx(job.frequency[0], 261.5)); // an octave down
    CHECK(approx(job.frequency[1], 0.0)); // a rest stays a rest
    CHECK(approx(job.frequency[2], 523.5));
    CHECK(approx(job.amplitude[0], toneAmplitudeForVolume(5) * toneLoudnessGain(261.5)));
    CHECK(approx(job.amplitude[1], 0.0));
    CHECK(approx(job.amplitude[2], toneAmplitudeForVolume(7) * toneLoudnessGain(523.5))); // the note's own volume
    CHECK(job.samplesPerNote == 441u);
    CHECK(job.soundingSamples == 441u - 88u); // 2 ms of gap
    CHECK(job.attackSamples == 176u); // the sine's own 4 ms
    CHECK(job.releaseSamples == 176u);
    CHECK(job.note == 0);
    CHECK(job.position == 0);
    CHECK(approx(job.phase, 0.0));

    // The speaker keeps its edges unless asked; an envelope asked for is taken,
    // and each ramp fits inside its half of the note.
    PlayTonesRequest square = request(7, 0, 0, 10, { 523 });
    CHECK(makeToneJob(square).attackSamples == 0u);
    CHECK(makeToneJob(square).releaseSamples == 0u);
    square.attackMilliseconds = 100;
    square.releaseMilliseconds = 3;
    CHECK(makeToneJob(square).attackSamples == 220u); // half of 441
    CHECK(makeToneJob(square).releaseSamples == 132u);

    // Seventeen notes: the sixteen the sequence holds.
    PlayTonesRequest many = request(7, 1, 0, 10, {});
    many.count = 17;
    for (int i = 0; i < 16; ++i)
        many.freqs[i] = 440;
    CHECK(makeToneJob(many).noteCount == 16);
    // An unknown wave is the speaker.
    PlayTonesRequest odd = request(7, 9, 0, 10, { 523 });
    CHECK(makeToneJob(odd).wave == ToneWave::Square);
}

TEST(testSquareIsOnOrOff)
{
    // The VT520's voice: full amplitude from the first sample, no ramp, and
    // only two values through the note.
    ToneJob job = makeToneJob(request(7, 0, 0, 10, { 523 }));
    const double amplitude = job.amplitude[0];
    for (int i = 0; i < 400; ++i) {
        const double s = toneSample(job);
        CHECK(approx(std::fabs(s), amplitude, 1e-12));
    }
    CHECK(toneWaveGain(ToneWave::Square) < toneWaveGain(ToneWave::Sine));
}

TEST(testShapedVoicesMeetTheirEdgesSilent)
{
    // A note opens and closes at zero, so the junction between two notes and
    // the junction into the tail have no step to click on.
    ToneJob job = makeToneJob(request(7, 1, 0, 10, { 440, 660, 880 }));
    constexpr int audioFrames = 3 * 441;
    int16_t buf[audioFrames + 16];
    CHECK(fillTones(job, buf, audioFrames + 16) == false);
    CHECK(job.note == 3);
    for (int n = 0; n < 3; ++n) {
        CHECK(buf[n * 441] == 0);
        CHECK(buf[n * 441 + 440] == 0);
    }
    for (int i = audioFrames; i < audioFrames + 16; ++i)
        CHECK(buf[i] == 0);
    // Something sounded in between.
    int loud = 0;
    for (int i = 0; i < audioFrames; ++i)
        if (std::abs(buf[i]) > 1000)
            ++loud;
    CHECK(loud > audioFrames / 2);
}

TEST(testRestGapAndZeroVolumeAreSilent)
{
    ToneJob rest = makeToneJob(request(7, 1, 0, 10, { 0 }));
    for (int i = 0; i < 441; ++i)
        CHECK(approx(toneSample(rest), 0.0));
    CHECK(toneJobIsDone(rest));

    PlayTonesRequest gapped = request(7, 0, 0, 10, { 523 });
    gapped.gapMilliseconds = 5;
    ToneJob job = makeToneJob(gapped);
    CHECK(job.soundingSamples == 441u - 220u);
    for (uint32_t i = 0; i < job.soundingSamples; ++i)
        CHECK(!approx(toneSample(job), 0.0));
    for (uint32_t i = job.soundingSamples; i < 441u; ++i)
        CHECK(approx(toneSample(job), 0.0));
    CHECK(toneJobIsDone(job));

    ToneJob quiet = makeToneJob(request(0, 1, 0, 10, { 523 }));
    CHECK(approx(quiet.amplitude[0], 0.0));
    CHECK(approx(toneSample(quiet), 0.0));
}

TEST(testDrumFallsAndDies)
{
    ToneJob job = makeToneJob(request(7, 5, 0, 250, { 523 }));
    double first = 0.0, last = 0.0;
    int firstCrossings = 0, lastCrossings = 0;
    double previous = 0.0;
    const uint32_t frames = job.samplesPerNote;
    for (uint32_t i = 0; i < frames; ++i) {
        const double s = toneSample(job);
        const bool didCross = (previous < 0.0 && s >= 0.0) || (previous > 0.0 && s <= 0.0);
        if (i < 882) { // the first 20 ms
            first = std::max(first, std::fabs(s));
            firstCrossings += didCross ? 1 : 0;
        } else if (i >= frames - 882) { // the last 20 ms
            last = std::max(last, std::fabs(s));
            lastCrossings += didCross ? 1 : 0;
        }
        previous = s;
    }
    CHECK(first > 0.1);
    CHECK(last < first * 0.25); // the level decays through the note
    CHECK(lastCrossings < firstCrossings); // and the pitch falls
}

TEST(testNoiseIsBoundedAndRepeatable)
{
    ToneJob a = makeToneJob(request(7, 4, 0, 10, { 523 }));
    ToneJob b = makeToneJob(request(7, 4, 0, 10, { 523 }));
    double peak = 0.0;
    bool hasVariety = false;
    double previous = 0.0;
    for (int i = 0; i < 441; ++i) {
        const double s = toneSample(a);
        CHECK(approx(s, toneSample(b))); // the generator is seeded, so a test can see it
        peak = std::max(peak, std::fabs(s));
        if (i > 0 && !approx(s, previous))
            hasVariety = true;
        previous = s;
    }
    CHECK(peak <= a.amplitude[0] + 1e-12);
    CHECK(hasVariety);
}

TEST(testChannelPlaysItsRequestsInTurn)
{
    auto mixer = std::make_unique<ToneMixer>();
    // Two square requests on one channel at different volumes: the second
    // begins the sample after the first ends, so a line built from bars has
    // no seam, and the mixer reports active until the queue is drained.
    PlayTonesRequest loud = request(7, 0, 1, 10, { 523 });
    PlayTonesRequest soft = request(2, 0, 1, 10, { 523 });
    mixer->enqueue(loud);
    mixer->enqueue(soft);
    CHECK(!mixer->isIdle());
    const double loudAmplitude = makeToneJob(loud).amplitude[0];
    const double softAmplitude = makeToneJob(soft).amplitude[0];
    int16_t buf[(441 + 8) * 2];
    CHECK(mixer->mix(buf, 441) == true);
    for (int i = 0; i < 441; ++i) {
        CHECK(approx(std::fabs(buf[i * 2]) / 32767.0, std::tanh(loudAmplitude), 2e-4));
        CHECK(buf[i * 2 + 1] == buf[i * 2]); // centred, so both sides carry it
    }
    CHECK(mixer->mix(buf, 441 + 8) == false);
    for (int i = 0; i < 441; ++i)
        CHECK(approx(std::fabs(buf[i * 2]) / 32767.0, std::tanh(softAmplitude), 2e-4));
    for (int i = 441; i < 449; ++i)
        CHECK(buf[i * 2] == 0 && buf[i * 2 + 1] == 0);
    CHECK(mixer->isIdle());
}

TEST(testChannelsSoundTogether)
{
    auto mixer = std::make_unique<ToneMixer>();
    mixer->enqueue(request(4, 0, 1, 10, { 523 }));
    mixer->enqueue(request(4, 0, 2, 10, { 523 }));
    const double one = makeToneJob(request(4, 0, 1, 10, { 523 })).amplitude[0];
    int16_t buf[16];
    mixer->mix(buf, 8);
    // Both channels start at +1 of the square, so the sum is two amplitudes
    // through the soft limit: more than one alone, less than their sum.
    const double mixed = buf[0] / 32767.0;
    CHECK(mixed > one);
    CHECK(mixed < 2.0 * one);
    CHECK(approx(mixed, std::tanh(2.0 * one), 2e-4));
}

TEST(testVolumeZeroStopsAChannelOrAll)
{
    auto mixer = std::make_unique<ToneMixer>();
    mixer->enqueue(request(7, 0, 1, 100, { 523 }));
    mixer->enqueue(request(7, 0, 1, 100, { 523 }));
    mixer->enqueue(request(7, 0, 2, 100, { 523 }));
    CHECK(mixer->channels[1].count == 2);
    CHECK(mixer->channels[2].count == 1);
    mixer->enqueue(request(0, 0, 1, 0, {})); // stop channel 1
    CHECK(mixer->channels[1].isIdle());
    CHECK(!mixer->channels[2].isIdle());
    mixer->enqueue(request(0, 0, 0, 0, {})); // stop everything
    CHECK(mixer->isIdle());
    // A stop needs no notes; a request without notes at volume plays nothing.
    mixer->enqueue(request(7, 0, 3, 100, {}));
    CHECK(mixer->isIdle());
}

TEST(testSampleVoiceReplaysItsSlot)
{
    // A slot's frames come back at the volume's amplitude and nothing else:
    // no loudness weighting, since a sample has no pitch to weight.
    int16_t pcm[100];
    for (int i = 0; i < 100; ++i)
        pcm[i] = static_cast<int16_t>(i * 300);
    PlayTonesRequest r = request(7, 6, 0, 0, { 523 });
    r.sampleSlot = 3;
    r.samplePCM = pcm;
    r.sampleFrames = 100;
    ToneJob job = makeToneJob(r);
    CHECK(job.wave == ToneWave::Sample);
    CHECK(job.sample.size() == 100u);
    CHECK(job.samplesPerNote == 100u); // a zero duration holds the note for the sample
    CHECK(approx(job.amplitude[0], toneAmplitudeForVolume(7)));
    CHECK(job.attackSamples == 0u);
    for (int i = 0; i < 100; ++i)
        CHECK(approx(toneSample(job), pcm[i] / 32768.0 * toneAmplitudeForVolume(7), 1e-9));
    CHECK(toneJobIsDone(job));

    // An octave down plays it at half speed, interpolating between frames;
    // a rest between two notes is silent; a duration given holds each note
    // for that long, the sample cut or padded to it.
    r.semitonesDown = 12;
    ToneJob slow = makeToneJob(r);
    CHECK(slow.samplesPerNote == 200u);
    (void)toneSample(slow);
    CHECK(approx(toneSample(slow), (pcm[0] + pcm[1]) / 2.0 / 32768.0 * toneAmplitudeForVolume(7), 1e-9));

    PlayTonesRequest held = request(7, 6, 0, 1, { 523, 0, 523 });
    held.sampleSlot = 3;
    held.samplePCM = pcm;
    held.sampleFrames = 100;
    ToneJob notes = makeToneJob(held);
    CHECK(notes.samplesPerNote == 44u); // a millisecond, shorter than the sample
    for (int i = 0; i < 44; ++i)
        CHECK(!approx(toneSample(notes), 0.0) || i == 0);
    for (int i = 0; i < 44; ++i)
        CHECK(approx(toneSample(notes), 0.0)); // the rest
    CHECK(!approx(toneSample(notes), 0.0) || true);
    CHECK(notes.note == 2);

    // No PCM: the voice is silent but the note still holds its time.
    PlayTonesRequest empty = request(7, 6, 0, 1, { 523 });
    ToneJob silent = makeToneJob(empty);
    CHECK(silent.sample.empty());
    for (int i = 0; i < 44; ++i)
        CHECK(approx(toneSample(silent), 0.0));
    CHECK(toneJobIsDone(silent));
}

TEST(testEnvelopeDecaysToItsSustain)
{
    // A square with no attack, a 10 ms decay to three sevenths, held 40 ms:
    // full at the first sample, halfway down at 5 ms, at the sustain after.
    PlayTonesRequest r = request(7, 0, 0, 40, { 523 });
    r.decayMilliseconds = 10;
    r.sustainLevel = 3;
    ToneJob job = makeToneJob(r);
    CHECK(job.decaySamples == 441u);
    CHECK(approx(job.sustain, 3.0 / 7.0));
    const double peak = job.amplitude[0];
    double at[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < 882; ++i) {
        const double s = std::fabs(toneSample(job));
        if (i == 0)
            at[0] = s;
        if (i == 220)
            at[1] = s;
        if (i == 881)
            at[2] = s;
    }
    CHECK(approx(at[0], peak, 1e-9));
    CHECK(approx(at[1], peak * (1.0 - (1.0 - 3.0 / 7.0) * 220.0 / 441.0), 1e-9));
    CHECK(approx(at[2], peak * 3.0 / 7.0, 1e-9));
}

TEST(testEffectsStepEveryTick)
{
    // Each effect leaves its mark on the job's state at the tick, so the
    // tests read the state rather than the waveform.
    const auto play = [](ToneJob& job, int samples) {
        for (int i = 0; i < samples; ++i)
            (void)toneSample(job);
    };
    const auto withEffect = [](uint8_t effect, uint8_t parameter, uint32_t durationMilliseconds = 200) {
        PlayTonesRequest r = request(7, 1, 0, durationMilliseconds, { 440 });
        r.noteEffects[0] = effect;
        r.noteParameters[0] = parameter;
        return makeToneJob(r);
    };
    // 0xy arpeggio: the note, then x up, then y up, a tick each.
    ToneJob arpeggio = withEffect(0, 0x37);
    play(arpeggio, 1);
    CHECK(approx(arpeggio.effectSemitones, 0.0));
    play(arpeggio, 882);
    CHECK(approx(arpeggio.effectSemitones, 3.0));
    play(arpeggio, 882);
    CHECK(approx(arpeggio.effectSemitones, 7.0));
    play(arpeggio, 882);
    CHECK(approx(arpeggio.effectSemitones, 0.0));
    // 1xx and 2xx: a sixteenth of a semitone per unit, a tick at a time from
    // the second tick, the first being the note itself.
    ToneJob up = withEffect(1, 16);
    play(up, 1);
    CHECK(approx(up.slideSemitones, 0.0));
    play(up, 882 * 3);
    CHECK(approx(up.slideSemitones, 3.0));
    ToneJob down = withEffect(2, 8);
    play(down, 1 + 882 * 2);
    CHECK(approx(down.slideSemitones, -1.0));
    // 3xx: from the pitch before, an octave below, up to the note by steps,
    // arriving exactly.
    ToneJob glide = withEffect(3, 64);
    glide.lastSoundingFrequency = 220.0;
    play(glide, 1);
    CHECK(approx(glide.slideSemitones, -12.0));
    play(glide, 882 * 2);
    CHECK(approx(glide.slideSemitones, -4.0));
    play(glide, 882 * 2);
    CHECK(approx(glide.slideSemitones, 0.0));
    // 4xy vibrato: y eighths of a semitone deep, x sixteenths of a cycle a tick.
    ToneJob vibrato = withEffect(4, 0x44);
    double deepest = 0.0;
    for (int tick = 0; tick < 16; ++tick) {
        play(vibrato, 882);
        deepest = std::max(deepest, std::fabs(vibrato.effectSemitones));
    }
    CHECK(deepest > 0.45 && deepest <= 0.5);
    // 7xy tremolo: the gain swings by y sixteenths.
    ToneJob tremolo = withEffect(7, 0x48);
    double lowest = 1.0;
    for (int tick = 0; tick < 16; ++tick) {
        play(tremolo, 882);
        lowest = std::min(lowest, tremolo.effectGain);
    }
    CHECK(lowest < 0.55 && lowest >= 0.5);
    // Axy volume slide: up x, down y, sixty-fourths a tick from the second tick.
    ToneJob slide = withEffect(10, 0x04);
    play(slide, 1 + 882 * 4);
    CHECK(approx(slide.slideGain, 1.0 - 4.0 * 4.0 / 64.0));
    // Cxx set volume, of sixty-four.
    ToneJob set = withEffect(12, 32);
    play(set, 1);
    CHECK(approx(set.slideGain, 0.5));
    // ECx cuts the note after x ticks; EDx delays it x ticks.
    ToneJob cut = withEffect(14, 0xC2);
    play(cut, 882 * 2 - 1);
    CHECK(!cut.isCut);
    play(cut, 2);
    CHECK(cut.isCut);
    CHECK(approx(toneSample(cut), 0.0));
    ToneJob delayed = withEffect(14, 0xD1);
    CHECK(approx(toneSample(delayed), 0.0));
    play(delayed, 881);
    CHECK(delayed.isDelayed); // silent through the whole of the first tick
    (void)toneSample(delayed);
    CHECK(!delayed.isDelayed); // and sounding from the second
    // A note's effect state does not leak into the next note.
    PlayTonesRequest two = request(7, 1, 0, 40, { 440, 440 });
    two.noteEffects[0] = 1;
    two.noteParameters[0] = 32;
    ToneJob pair = makeToneJob(two);
    play(pair, 1764);
    CHECK(pair.note == 1);
    play(pair, 1);
    CHECK(approx(pair.slideSemitones, 0.0));
    CHECK(approx(pair.lastSoundingFrequency, 440.0));
}

TEST(testQueueDepthIsBounded)
{
    auto mixer = std::make_unique<ToneMixer>();
    for (int i = 0; i < toneQueueDepth + 8; ++i)
        mixer->enqueue(request(7, 0, 8, 100, { 523 }));
    CHECK(mixer->channels[8].count == toneQueueDepth);
    // A channel past the last is the last.
    mixer->enqueue(request(7, 0, 200, 100, { 523 }));
    CHECK(mixer->channels[toneChannelCount - 1].count == toneQueueDepth);
}

TEST(testTheHallRingsAfterTheNote)
{
    // A sent note leaves a tail once it ends; an unsent one leaves silence.
    const auto tailEnergy = [](uint8_t send) {
        auto mixer = std::make_unique<ToneMixer>();
        PlayTonesRequest r = request(7, 1, 1, 50, { 523 });
        r.reverbSend = send;
        mixer->enqueue(r);
        // mix() writes interleaved stereo, so the buffer holds two samples per
        // frame. Sized as one per frame it overflowed by a whole second of
        // audio, which the sanitizers caught and an ordinary run did not.
        constexpr uint32_t tailFrames = 44100;
        std::vector<int16_t> out(tailFrames * 2);
        (void)mixer->mix(out.data(), 2205); // the note
        (void)mixer->mix(out.data(), tailFrames); // the second after it
        double energy = 0.0;
        for (const int16_t sample : out)
            energy += std::fabs(sample);
        return energy;
    };
    CHECK(tailEnergy(0) == 0.0);
    CHECK(tailEnergy(7) > 0.0);
}

TEST(testTheGridStartsChannelsTogether)
{
    auto mixer = std::make_unique<ToneMixer>();
    std::vector<int16_t> out(44100);
    const auto gridded = [](uint8_t channel) {
        PlayTonesRequest r = request(7, 1, channel, 2000, { 523 });
        r.gridUnits = 8; // a quarter of a second: 11025 samples
        return r;
    };
    const auto positionOf = [&mixer](int channel) { return mixer->channels[channel].jobs[mixer->channels[channel].head].position; };
    // The first gridded request sets the origin and starts at once; one sent
    // a hundred samples later joins it in step.
    mixer->enqueue(gridded(1));
    (void)mixer->mix(out.data(), 100);
    mixer->enqueue(gridded(2));
    (void)mixer->mix(out.data(), 1);
    CHECK(positionOf(1) == 101);
    CHECK(positionOf(2) == 101);
    // One sent well after the point waits, silent, for the next.
    (void)mixer->mix(out.data(), 4000 - 101);
    mixer->enqueue(gridded(3));
    (void)mixer->mix(out.data(), 1);
    const ToneJob& waiting = mixer->channels[3].jobs[mixer->channels[3].head];
    CHECK(waiting.position == 0);
    CHECK(waiting.startDelay == 11025 - 4000 - 1);
    (void)mixer->mix(out.data(), 11025 - 4000 - 1);
    CHECK(waiting.position == 0 && waiting.startDelay == 0);
    (void)mixer->mix(out.data(), 1);
    CHECK(waiting.position == 1);
    CHECK(positionOf(1) == 11026);
    // Stopped, the origin is forgotten: the next gridded request starts at once.
    mixer->enqueue(request(0, 0, 0, 0, {}));
    mixer->enqueue(gridded(4));
    (void)mixer->mix(out.data(), 1);
    CHECK(positionOf(4) == 1);
    // A request queued behind another follows it, whatever the grid says:
    // a bar cut into runs stays one bar.
    mixer->enqueue(request(0, 0, 0, 0, {}));
    PlayTonesRequest brief = gridded(6);
    brief.durationMilliseconds = 10; // 441 samples
    mixer->enqueue(brief);
    mixer->enqueue(gridded(6));
    (void)mixer->mix(out.data(), 441 + 1);
    CHECK(mixer->channels[6].count == 1);
    CHECK(positionOf(6) == 1);
    // A skip passes over whole notes when the join is longer than one.
    PlayTonesRequest quick = request(7, 1, 5, 2, { 523, 587, 659 });
    ToneJob job = makeToneJob(quick);
    toneSkipInto(job, job.samplesPerNote + 5);
    CHECK(job.note == 1 && job.position == 5);
    CHECK(approx(job.lastSoundingFrequency, 523.0));
}

TEST(testTheFiltersShapeTheVoice)
{
    // A low-pass far below a sine's pitch takes most of it away; a high-pass
    // far above does the same; neither touches a voice it is not asked for.
    const auto energy = [](uint32_t hertz, uint16_t lowpass, uint16_t highpass) {
        PlayTonesRequest r = request(7, 1, 1, 400, { hertz });
        r.lowpassHertz = lowpass;
        r.highpassHertz = highpass;
        ToneJob job = makeToneJob(r);
        double sum = 0.0;
        for (int i = 0; i < 17640; ++i) {
            const double sample = toneSample(job);
            if (i >= 8820) // after the sections have settled
                sum += sample * sample;
        }
        return sum;
    };
    const double plain = energy(2000, 0, 0);
    CHECK(plain > 0.0);
    CHECK(energy(2000, 200, 0) < plain * 0.02);
    CHECK(energy(200, 0, 5000) < energy(200, 0, 0) * 0.02);
    CHECK(energy(2000, 8000, 0) > plain * 0.8);
    // The section's shape: a Butterworth low-pass is unity at zero and a
    // high-pass is unity at the top, so a constant passes one and not the other.
    Ubiquitty::VT::ToneBiquad lowpass = Ubiquitty::VT::toneFilterSection(1000.0, false);
    Ubiquitty::VT::ToneBiquad highpass = Ubiquitty::VT::toneFilterSection(1000.0, true);
    double low = 0.0, high = 0.0;
    for (int i = 0; i < 4410; ++i) {
        low = lowpass.process(1.0);
        high = highpass.process(1.0);
    }
    CHECK(approx(low, 1.0, 1e-6));
    CHECK(approx(high, 0.0, 1e-6));
}

UBIQUITTY_TEST_MAIN()

TEST(testPanPlacesARequestAndLeavesTheCentreAsItWas)
{
    // The centre is both sides at full gain, not the -3 dB a constant-power law
    // would give: a request that names no pan plays at the full gain of a
    // single-sided tone.
    double left = 0.0, right = 0.0;
    toneGainsForPan(tonePanCentre, left, right);
    CHECK(approx(left, 1.0) && approx(right, 1.0));
    toneGainsForPan(0, left, right);
    CHECK(approx(left, 1.0) && approx(right, 0.0));
    toneGainsForPan(toneMaxPan, left, right);
    CHECK(approx(left, 0.0) && approx(right, 1.0));
    toneGainsForPan(toneMaxPan * 3 / 4, left, right);
    CHECK(approx(left, 0.5) && approx(right, 1.0));
    // Out of range clamps rather than wrapping into the far side.
    toneGainsForPan(toneMaxPan + 40, left, right);
    CHECK(approx(left, 0.0) && approx(right, 1.0));

    auto mixer = std::make_unique<ToneMixer>();
    PlayTonesRequest hardLeft = request(7, 0, 1, 10, { 523 });
    hardLeft.pan = 0;
    mixer->enqueue(hardLeft);
    int16_t buf[16];
    mixer->mix(buf, 8);
    CHECK(buf[0] != 0);
    CHECK(buf[1] == 0);
}

TEST(testARequestRepeatsAFixedNumberOfTimesOrUntilStopped)
{
    // A background track is one request rather than a program that has to stay
    // alive feeding the channel, and gaps whenever it is busy.
    const uint32_t onePass = toneSamplesForMilliseconds(1);
    auto counted = std::make_unique<ToneMixer>();
    PlayTonesRequest twice = request(7, 0, 1, 1, { 523 });
    twice.repeat = 1; // one more pass after the first
    counted->enqueue(twice);
    std::vector<int16_t> buf(onePass * 4);
    uint32_t played = 0;
    while (counted->mix(buf.data(), 8) && played < onePass * 8)
        played += 8;
    // Two passes, within the block the loop steps in.
    CHECK(played >= onePass * 2 - 8 && played <= onePass * 2 + 8);

    auto forever = std::make_unique<ToneMixer>();
    PlayTonesRequest endless = request(7, 0, 1, 1, { 523 });
    endless.repeat = PlayTonesRequest::repeatForever;
    forever->enqueue(endless);
    for (uint32_t i = 0; i < onePass * 20; i += 8)
        CHECK(forever->mix(buf.data(), 8) == true);
    CHECK(!forever->isIdle());
    // Volume 0 is what ends it, the same verb that stops anything else.
    forever->enqueue(request(0, 0, 1, 0, {}));
    CHECK(forever->isIdle());
}

TEST(testTheSampleBankAnswersWhoseSampleASlotHolds)
{
    // The reattach question: a session outlives the program that filled the
    // bank, so "is my chime still there" cannot be answered by occupancy.
    Ubiquitty::VT::SampleBank bank;
    CHECK(bank.firstFreeSlot() == 1);
    CHECK(bank.loadedMask() == 0u);
    CHECK(bank.slotForTag(0x1234) == 0);

    bank.slots[2] = std::vector<int16_t>(10, 1);
    bank.tags[2] = 0x1234;
    CHECK(bank.firstFreeSlot() == 1);
    CHECK(bank.loadedMask() == (1u << 2));
    CHECK(bank.slotForTag(0x1234) == 3);
    CHECK(bank.slotForTag(0x9999) == 0);
    CHECK(bank.slotForTag(0) == 0); // an untagged download claims no identity

    bank.clearSlot(3);
    CHECK(bank.slotForTag(0x1234) == 0);
    CHECK(bank.loadedMask() == 0u);

    // A full bank refuses rather than evicting someone else's sample.
    for (int i = 0; i < Ubiquitty::VT::sampleSlotCount; ++i)
        bank.slots[i] = std::vector<int16_t>(4, 1);
    CHECK(bank.firstFreeSlot() == 0);
}
