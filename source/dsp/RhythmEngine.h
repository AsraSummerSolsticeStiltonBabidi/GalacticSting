#pragma once

//==============================================================================
// RhythmEngine: spectrum -> rhythm. Translates SELECTED PARTIALS of the current
// spectrum into MIDI pulse trains on the plugin's MIDI OUTPUT, routable to any
// other instrument track in the DAW.
//
// The mathematics (verifiable):
//   pulseRate(slot, heldNote) = f0(heldNote) * partialRatio / x^n   [pulses/s]
// f0 is the EXACT sounding frequency of the held note - the same TuningManager
// call (+ per-channel pitch bend) the audio voices use. Two slots on harmonics
// 3 and 2 pulse in an exact 3:2 polyrhythm; each train's phase resets on its
// note-on so the first pulse lands ON the note-on sample.
//
// Modes:
//   Lowest note - only the LOWEST currently-held note drives the pulse trains.
//   All notes   - every held note drives its own copy of the trains; the sent
//                 pitches are transposed by that note's interval from the lowest.
//
// Pitch of each train:
//   plain slot  - a fixed MIDI note (typed as "A4"), +- the held-note transposition.
//   ratio slot  - (MPE out) f0 * ratio (ratios "3", "3/2" or cents "1987.4c"):
//                 sent as nearest-note + per-note pitch bend on its own MIDI
//                 channel, so an MPE-capable receiver plays the EXACT pitch of
//                 that partial. With MPE out off, ratio slots round to the
//                 nearest 12ED2 note.
//
// Sync guarantees (by construction): pulses are generated in the SAME
// processBlock that renders the audio, stamped with exact intra-block sample
// positions; all state is scalar/atomic; no allocation or locking on the audio
// thread.
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
    static constexpr int kSlots    = 8;
    static constexpr int kMaxNotes = 10; // simultaneous held notes driving trains

    // One routable pulse train. The editor writes these atomics (message thread);
    // the audio thread only reads them.
    struct Slot
    {
        std::atomic<int>    partial  { -1 };    // template partial index (-1 = empty)
        std::atomic<int>    note     { 60 };    // plain pitch: MIDI note (typed "A4")
        std::atomic<bool>   useRatio { false }; // ratio pitch: f0 * ratio via MPE bend
        std::atomic<double> ratio    { 1.0 };   // linear ratio (from "3", "3/2" or "...c")
        std::atomic<float>  durMs    { 60.0f }; // gate = how long each sent note lasts
        std::atomic<bool>   enabled  { false };
    };

    RhythmEngine (juce::AudioProcessorValueTreeState& s,
                  const spectrum::SpectrumEngine& eng, const TuningManager& tun)
        : apvts (s), engine (eng), tuning (tun)
    {
        onParam   = apvts.getRawParameterValue ("rhythmOn");
        xParam    = apvts.getRawParameterValue ("rhythmDivX");
        nParam    = apvts.getRawParameterValue ("rhythmDivN");
        modeParam = apvts.getRawParameterValue ("rhythmMode");
        mpeParam  = apvts.getRawParameterValue ("rhythmMpe");
        bendParam = apvts.getRawParameterValue ("bendRange");
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (int i = 0; i < kSlots; ++i)
            for (int j = 0; j < kMaxNotes; ++j)
            { phase[i][j] = 0.0; gateLeft[i][j] = 0; }
        for (auto& h : heldCol) h.active = false;
        for (auto& b : channelBend) b = 0.0;
        driverCol = -1;
        displayF0.store (0.0f, std::memory_order_relaxed);
    }

    // Message thread: ask the audio thread to emit a controller (e.g. the DAW-tempo
    // CC) at the start of the next block. value 0..127; cc 1..127.
    void sendController (int cc, int value) noexcept
    {
        pendingCC.store ((juce::jlimit (1, 127, cc) << 8) | juce::jlimit (0, 127, value),
                         std::memory_order_relaxed);
    }

    //==========================================================================
    // Audio thread. Call AFTER the synth has rendered from `io`: consumes the
    // block's input MIDI (note-on/off + pitch bend, to track the held notes
    // sample-accurately) and REPLACES the buffer with the generated pulse MIDI.
    void process (juce::MidiBuffer& io, int numSamples)
    {
        out.clear();

        // Queued one-shot controller (DAW-tempo mapping) leaves at block start.
        if (const int pk = pendingCC.exchange (-1, std::memory_order_relaxed); pk >= 0)
            out.addEvent (juce::MidiMessage::controllerEvent (1, pk >> 8, pk & 0xff), 0);

        const auto* tmpl = engine.getLive();
        refreshTemplateCache (tmpl);

        const bool on  = onParam   != nullptr && onParam->load()   > 0.5f;
        const bool all = modeParam != nullptr && modeParam->load() > 0.5f;
        mpeOut         = mpeParam  != nullptr && mpeParam->load()  > 0.5f;
        const int  x   = juce::jlimit (1, 12, (int) xParam->load());
        const int  nD  = juce::jlimit (1, 16, (int) nParam->load());
        divisor = std::pow ((double) x, (double) nD);

        int cursor = 0;
        for (const auto meta : io)
        {
            const auto msg = meta.getMessage();
            const int  pos = juce::jlimit (0, numSamples, (int) meta.samplePosition);
            render (tmpl, on, all, cursor, pos);
            cursor = juce::jmax (cursor, pos);

            if (msg.isNoteOn())
            {
                int col = -1; // first free column
                for (int j = 0; j < kMaxNotes; ++j)
                    if (! heldCol[(size_t) j].active) { col = j; break; }
                if (col >= 0)
                {
                    auto& h = heldCol[(size_t) col];
                    h.active = true;
                    h.note   = msg.getNoteNumber();
                    h.ch     = juce::jlimit (1, 16, msg.getChannel());
                    h.f0     = noteHz (h.note, h.ch);
                    for (int i = 0; i < kSlots; ++i) phase[i][col] = 1.0; // downbeat at note-on
                }
                updateDriver();
            }
            else if (msg.isNoteOff())
            {
                for (int j = 0; j < kMaxNotes; ++j)
                {
                    auto& h = heldCol[(size_t) j];
                    if (h.active && h.note == msg.getNoteNumber() && h.ch == msg.getChannel())
                    {
                        closeColumnGates (j, pos);
                        h.active = false;
                        break;
                    }
                }
                updateDriver();
            }
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            {
                for (int j = 0; j < kMaxNotes; ++j)
                    if (heldCol[(size_t) j].active) { closeColumnGates (j, pos); heldCol[(size_t) j].active = false; }
                updateDriver();
            }
            else if (msg.isPitchWheel())
            {
                static constexpr int bendValues[] = { 2, 12, 48 };
                const int idx = juce::jlimit (0, 2, bendParam != nullptr ? (int) bendParam->load() : 2);
                const double semis = ((double) msg.getPitchWheelValue() - 8192.0) / 8192.0
                                   * (double) bendValues[idx];
                channelBend[(size_t) juce::jlimit (0, 15, msg.getChannel() - 1)] = semis;
                for (auto& h : heldCol)
                    if (h.active && h.ch == msg.getChannel())
                        h.f0 = noteHz (h.note, h.ch);
                updateDriver();
            }
        }
        render (tmpl, on, all, cursor, numSamples);

        io.swapWith (out); // input events consumed; only our pulses leave the plugin
    }

    //==========================================================================
    // UI read-outs (message thread).
    float getRateHz (int slot) const noexcept
    { return juce::isPositiveAndBelow (slot, kSlots) ? lastRateHz[(size_t) slot].load (std::memory_order_relaxed) : 0.0f; }
    float getDriverF0() const noexcept { return displayF0.load (std::memory_order_relaxed); }

    Slot slots[kSlots];

private:
    struct Held { bool active = false; int note = 60, ch = 1; double f0 = 0.0; };

    double noteHz (int note, int ch) const
    {
        return tuning.frequencyForNote (note)
             * std::exp2 (channelBend[(size_t) juce::jlimit (0, 15, ch - 1)] / 12.0);
    }

    void updateDriver()
    {
        driverCol = -1;
        int lowest = 128;
        for (int j = 0; j < kMaxNotes; ++j)
            if (heldCol[(size_t) j].active && heldCol[(size_t) j].note < lowest)
            { lowest = heldCol[(size_t) j].note; driverCol = j; }
        displayF0.store (driverCol >= 0 ? (float) heldCol[(size_t) driverCol].f0 : 0.0f,
                         std::memory_order_relaxed);
    }

    void closeColumnGates (int col, int pos)
    {
        for (int i = 0; i < kSlots; ++i)
            if (gateLeft[i][col] > 0)
            {
                out.addEvent (juce::MidiMessage::noteOff (gateChn[i][col], gateNote[i][col]), pos);
                gateLeft[i][col] = 0;
            }
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

    // The MIDI channel a train sends on: MPE out spreads the trains over the
    // member channels 2..16 (channel 1 = zone master); plain mode uses channel 1.
    int trainChannel (int slot, int col) const noexcept
    {
        return mpeOut ? 2 + ((slot * kMaxNotes + col) % 15) : 1;
    }

    void render (const spectrum::SpectrumTemplate* tmpl, bool on, bool all, int from, int to)
    {
        if (to <= from) return;

        const int count   = tmpl != nullptr ? (int) tmpl->partials.size() : 0;
        const int lowNote = driverCol >= 0 ? heldCol[(size_t) driverCol].note : 60;

        // Per-segment constants per train (f0 constant between MIDI events).
        double inc[kSlots][kMaxNotes];
        int    gateLen[kSlots], sendNote[kSlots][kMaxNotes], sendBend[kSlots][kMaxNotes];
        juce::uint8 vel[kSlots];
        bool slotOk[kSlots];
        for (int i = 0; i < kSlots; ++i)
        {
            const int  p  = slots[i].partial.load (std::memory_order_relaxed);
            slotOk[i] = on && slots[i].enabled.load (std::memory_order_relaxed)
                     && p >= 0 && p < count;
            gateLen[i] = juce::jmax (1, (int) ((double) slots[i].durMs.load (std::memory_order_relaxed) * 0.001 * sr));
            if (slotOk[i])
            {
                const float a = tmpl->partials[(size_t) p].amp;
                vel[i] = (juce::uint8) juce::jlimit (1, 127, (int) std::lround (127.0 * std::sqrt ((double) (a / maxAmp))));
            }
            else vel[i] = 1;

            float uiRate = 0.0f;
            for (int j = 0; j < kMaxNotes; ++j)
            {
                inc[i][j] = 0.0;
                const auto& h = heldCol[(size_t) j];
                const bool colOn = h.active && (all || j == driverCol);
                if (! (slotOk[i] && colOn && h.f0 > 0.0)) continue;

                const double ratio = (double) tmpl->partials[(size_t) p].ratio;
                const double hz    = h.f0 * ratio / divisor;
                const double r     = juce::jlimit (0.01, 100.0, hz);
                inc[i][j] = r / sr;
                if (j == driverCol) uiRate = (float) r;

                // Sent pitch: ratio slots = the EXACT pitch f0*slotRatio (note+bend);
                // plain slots = the typed note transposed by this held note's interval
                // from the lowest held note.
                if (slots[i].useRatio.load (std::memory_order_relaxed))
                {
                    const double tHz   = h.f0 * juce::jmax (1.0e-6, slots[i].ratio.load (std::memory_order_relaxed));
                    const double semis = 69.0 + 12.0 * std::log2 (tHz / 440.0);
                    const int    m     = juce::jlimit (0, 127, (int) std::lround (semis));
                    sendNote[i][j] = m;
                    // Per-note bend, assuming the MPE-default +-48 st range downstream.
                    sendBend[i][j] = juce::jlimit (0, 16383,
                        8192 + (int) std::lround ((semis - (double) m) / 48.0 * 8192.0));
                }
                else
                {
                    sendNote[i][j] = juce::jlimit (0, 127,
                        slots[i].note.load (std::memory_order_relaxed) + (h.note - lowNote));
                    sendBend[i][j] = 8192; // centre
                }
            }
            lastRateHz[(size_t) i].store (uiRate, std::memory_order_relaxed);
        }

        for (int s = from; s < to; ++s)
            for (int i = 0; i < kSlots; ++i)
                for (int j = 0; j < kMaxNotes; ++j)
                {
                    // Gate countdown runs regardless of held/enabled -> no stuck notes.
                    if (gateLeft[i][j] > 0 && --gateLeft[i][j] == 0)
                        out.addEvent (juce::MidiMessage::noteOff (gateChn[i][j], gateNote[i][j]), s);

                    if (inc[i][j] > 0.0)
                    {
                        phase[i][j] += inc[i][j];
                        if (phase[i][j] >= 1.0)
                        {
                            phase[i][j] -= (double) (int) phase[i][j];
                            const int ch = trainChannel (i, j);
                            if (gateLeft[i][j] > 0)
                            { out.addEvent (juce::MidiMessage::noteOff (gateChn[i][j], gateNote[i][j]), s); gateLeft[i][j] = 0; }
                            if (mpeOut) // per-note bend BEFORE the note-on (MPE convention)
                                out.addEvent (juce::MidiMessage::pitchWheel (ch, sendBend[i][j]), s);
                            out.addEvent (juce::MidiMessage::noteOn (ch, sendNote[i][j], vel[i]), s);
                            gateNote[i][j] = sendNote[i][j];
                            gateChn [i][j] = ch;
                            gateLeft[i][j] = gateLen[i];
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
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* mpeParam  = nullptr;
    std::atomic<float>* bendParam = nullptr;

    juce::MidiBuffer out; // reused every block (capacity persists after warm-up)

    double sr      = 44100.0;
    double divisor = 1.0;
    bool   mpeOut  = false;
    Held   heldCol[kMaxNotes];
    int    driverCol = -1;
    double channelBend[16] = {};

    double phase   [kSlots][kMaxNotes] = {};
    int    gateLeft[kSlots][kMaxNotes] = {};
    int    gateNote[kSlots][kMaxNotes] = {};
    int    gateChn [kSlots][kMaxNotes] = {};

    juce::uint32 cachedGen = 0;
    float        maxAmp    = 1.0f;

    std::atomic<int> pendingCC { -1 }; // packed (cc<<8)|value, -1 = none

    mutable std::atomic<float> lastRateHz[kSlots] = {};
    std::atomic<float> displayF0 { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmEngine)
};
