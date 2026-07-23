#pragma once

//==============================================================================
// RhythmEngine: timbre -> rhythm. Translates SELECTED PARTIALS of the current
// spectrum into MIDI pulse trains on the plugin's MIDI OUTPUT, routable to any
// other track ("MIDI From: Fibló Galàctic" in Ableton).
//
// The mathematics (verifiable):
//   pulseRate(slot) = f0 * partialRatio / x^n        [pulses per second]
// where f0 is the EXACT frequency of the driving note - computed with the very
// same TuningManager call (+ pitch-bend semitones) the audio voices use - and
// x^n is the user divisor. So two slots on harmonics 3 and 2 pulse in an exact
// 3:2 polyrhythm, phase-locked: every slot's phase resets on note-on, making
// the first pulse of every train land ON the note-on sample.
//
// Sync guarantees (by construction, not by tuning):
//   - Pulses are generated in the SAME processBlock that renders the audio and
//     are stamped with exact intra-block sample positions -> sample-accurate.
//   - The phase accumulators advance per sample at rate/sr, exactly like the
//     partial oscillators do, from the same f0 -> the rhythm is proportional to
//     the timbre at all times (Base N morphs re-speed the pulses live).
//   - All state is scalar/atomic; no allocation or locking on the audio thread.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "SpectrumEngine.h"
#include "TuningManager.h"

#include <atomic>
#include <cmath>

class RhythmEngine
{
public:
    static constexpr int kSlots = 8;

    // One routable pulse train. The editor writes these atomics (message thread);
    // the audio thread only reads them - per-field atomics, no torn state matters
    // (each field is independently meaningful).
    struct Slot
    {
        std::atomic<int>   partial { -1 };    // template partial index (-1 = empty)
        std::atomic<int>   note    { 60 };    // MIDI note this train sends
        std::atomic<float> durMs   { 60.0f }; // gate length in ms
        std::atomic<bool>  enabled { false };
    };

    RhythmEngine (juce::AudioProcessorValueTreeState& s,
                  const spectrum::SpectrumEngine& eng, const TuningManager& tun)
        : apvts (s), engine (eng), tuning (tun)
    {
        onParam   = apvts.getRawParameterValue ("rhythmOn");
        xParam    = apvts.getRawParameterValue ("rhythmDivX");
        nParam    = apvts.getRawParameterValue ("rhythmDivN");
        bendParam = apvts.getRawParameterValue ("bendRange");
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (int i = 0; i < kSlots; ++i) { phase[i] = 0.0; gateLeft[i] = 0; }
        held = 0; f0 = 0.0; driverCh = 0;
        for (auto& b : channelBend) b = 0.0;
        displayF0.store (0.0f, std::memory_order_relaxed);
    }

    //==========================================================================
    // Audio thread. Call AFTER the synth has rendered from `io`: consumes the
    // block's input MIDI (note-on/off + pitch bend, to track the driving note
    // sample-accurately) and REPLACES the buffer with the generated pulse MIDI.
    void process (juce::MidiBuffer& io, int numSamples)
    {
        out.clear();

        const auto* tmpl = engine.getLive();
        refreshTemplateCache (tmpl);

        const bool on = onParam != nullptr && onParam->load() > 0.5f;
        const int  x  = juce::jlimit (2, 12, (int) xParam->load());
        const int  nD = juce::jlimit (1, 16, (int) nParam->load());
        divisor = std::pow ((double) x, (double) nD);

        int cursor = 0;
        for (const auto meta : io)
        {
            const auto msg = meta.getMessage();
            const int  pos = juce::jlimit (0, numSamples, (int) meta.samplePosition);
            render (tmpl, on, cursor, pos);
            cursor = juce::jmax (cursor, pos);

            if (msg.isNoteOn())
            {
                ++held;
                driverNote = msg.getNoteNumber();
                driverCh   = msg.getChannel();
                bendSemis  = channelBend[(size_t) juce::jlimit (0, 15, driverCh - 1)];
                updateF0();
                // Downbeat: phase=1 makes the very next rendered sample wrap ->
                // the first pulse of every train lands ON the note-on sample.
                for (int i = 0; i < kSlots; ++i) phase[i] = 1.0;
            }
            else if (msg.isNoteOff())
            {
                if (held > 0) --held;
                if (held == 0) displayF0.store (0.0f, std::memory_order_relaxed);
            }
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            {
                held = 0;
                displayF0.store (0.0f, std::memory_order_relaxed);
            }
            else if (msg.isPitchWheel())
            {
                static constexpr int bendValues[] = { 2, 12, 48 };
                const int idx = juce::jlimit (0, 2, bendParam != nullptr ? (int) bendParam->load() : 2);
                const double semis = ((double) msg.getPitchWheelValue() - 8192.0) / 8192.0
                                   * (double) bendValues[idx];
                channelBend[(size_t) juce::jlimit (0, 15, msg.getChannel() - 1)] = semis;
                if (msg.getChannel() == driverCh) { bendSemis = semis; updateF0(); }
            }
        }
        render (tmpl, on, cursor, numSamples);

        io.swapWith (out); // input events consumed; only our pulses leave the plugin
    }

    //==========================================================================
    // UI read-outs (message thread).
    float getRateHz (int slot) const noexcept
    { return juce::isPositiveAndBelow (slot, kSlots) ? lastRateHz[(size_t) slot].load (std::memory_order_relaxed) : 0.0f; }
    float getDriverF0() const noexcept { return displayF0.load (std::memory_order_relaxed); }

    Slot slots[kSlots];

private:
    void updateF0()
    {
        // EXACTLY what a voice does for its base pitch: tuning (incl. reference A
        // and octave transpose) x the per-channel bend in semitones.
        f0 = tuning.frequencyForNote (driverNote) * std::exp2 (bendSemis / 12.0);
        displayF0.store ((float) f0, std::memory_order_relaxed);
    }

    void refreshTemplateCache (const spectrum::SpectrumTemplate* tmpl)
    {
        const juce::uint32 g = tmpl != nullptr ? tmpl->generation : 0;
        if (tmpl != nullptr && g != cachedGen)
        {
            cachedGen = g;
            float m = 1.0e-9f;
            for (const auto& p : tmpl->partials) m = std::max (m, p.amp);
            maxAmp = m;
        }
    }

    void render (const spectrum::SpectrumTemplate* tmpl, bool on, int from, int to)
    {
        if (to <= from) return;

        // Per-segment constants (f0 is constant between MIDI events).
        const int count = tmpl != nullptr ? (int) tmpl->partials.size() : 0;
        double inc[kSlots]; int gateLen[kSlots]; int noteNum[kSlots]; juce::uint8 vel[kSlots];
        for (int i = 0; i < kSlots; ++i)
        {
            const int  p  = slots[i].partial.load (std::memory_order_relaxed);
            const bool en = slots[i].enabled.load (std::memory_order_relaxed);
            const bool active = on && en && held > 0 && f0 > 0.0
                             && p >= 0 && p < count;
            if (active)
            {
                const double hz = f0 * (double) tmpl->partials[(size_t) p].ratio / divisor;
                const double r  = juce::jlimit (0.01, 100.0, hz); // sane MIDI-rate window
                inc[i]     = r / sr;
                noteNum[i] = juce::jlimit (0, 127, slots[i].note.load (std::memory_order_relaxed));
                gateLen[i] = juce::jmax (1, (int) ((double) slots[i].durMs.load (std::memory_order_relaxed) * 0.001 * sr));
                // Velocity follows the partial's level in the timbre (sqrt for audibility).
                const float a = tmpl->partials[(size_t) p].amp;
                vel[i] = (juce::uint8) juce::jlimit (1, 127, (int) std::lround (127.0 * std::sqrt ((double) (a / maxAmp))));
                lastRateHz[(size_t) i].store ((float) r, std::memory_order_relaxed);
            }
            else
            {
                inc[i] = 0.0;
                gateLen[i] = 0; noteNum[i] = 0; vel[i] = 1;
                lastRateHz[(size_t) i].store (0.0f, std::memory_order_relaxed);
            }
        }

        for (int s = from; s < to; ++s)
            for (int i = 0; i < kSlots; ++i)
            {
                // Gate countdown runs regardless of held/enabled -> no stuck notes.
                if (gateLeft[i] > 0 && --gateLeft[i] == 0)
                    out.addEvent (juce::MidiMessage::noteOff (1, gateNote[i]), s);

                if (inc[i] > 0.0)
                {
                    phase[i] += inc[i];
                    if (phase[i] >= 1.0)
                    {
                        phase[i] -= (double) (int) phase[i]; // wrap (also handles the =1.0 downbeat reset)
                        if (gateLeft[i] > 0) { out.addEvent (juce::MidiMessage::noteOff (1, gateNote[i]), s); gateLeft[i] = 0; }
                        out.addEvent (juce::MidiMessage::noteOn (1, noteNum[i], vel[i]), s);
                        gateNote[i] = noteNum[i];
                        gateLeft[i] = gateLen[i];
                    }
                }
            }
    }

    juce::AudioProcessorValueTreeState& apvts;
    const spectrum::SpectrumEngine&     engine;
    const TuningManager&                tuning;
    std::atomic<float>* onParam = nullptr;
    std::atomic<float>* xParam  = nullptr;
    std::atomic<float>* nParam  = nullptr;
    std::atomic<float>* bendParam = nullptr;

    juce::MidiBuffer out;      // reused every block (capacity persists; no per-block allocation after warm-up)

    double sr        = 44100.0;
    double divisor   = 256.0;
    double f0        = 0.0;    // driving note's frequency (audio thread)
    int    held      = 0;      // held-note count; pulses run while > 0
    int    driverNote = 60, driverCh = 0;
    double bendSemis = 0.0;
    double channelBend[16] = {};

    double phase[kSlots]    = {};
    int    gateLeft[kSlots] = {};
    int    gateNote[kSlots] = {};

    juce::uint32 cachedGen = 0;
    float        maxAmp    = 1.0f;

    mutable std::atomic<float> lastRateHz[kSlots] = {}; // UI read-out
    std::atomic<float> displayF0 { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmEngine)
};
