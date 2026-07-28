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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numeric>

class RhythmEngine
{
public:
    // "Unlimited" pulses: bounded only by what the machine can hold. 128 simultaneous
    // trains is far beyond any musical use; state arrays stay a few tens of KB and the
    // per-sample loop only walks the ACTIVE slots (work list), so unused slots cost 0.
    static constexpr int kSlots    = 128;
    static constexpr int kMaxNotes = 10; // simultaneous held notes driving trains

    // One routable pulse train. The editor writes these atomics (message thread);
    // the audio thread only reads them. The pitch is ALWAYS relative to the note
    // being played (never fixed): plain = scale steps from it ("7st", "7st-" =
    // down); ratio (MPE out) = f0 * ratio, also from the played note.
    struct Slot
    {
        std::atomic<int>    partial  { -1 };    // template partial index (-1 = empty)
        std::atomic<int>    steps    { 0 };     // plain pitch: +-scale steps from the played note
        std::atomic<bool>   useRatio { false }; // ratio pitch: f0 * ratio via MPE bend
        std::atomic<double> ratio    { 1.0 };   // linear ratio (from "3", "3/2" or "...c")
        std::atomic<float>  durMs    { 60.0f }; // gate = how long each sent note lasts
        std::atomic<int>    vel      { 100 };   // MIDI velocity of every pulse this slot sends (1..127)
        std::atomic<bool>   enabled  { false };
    };

    RhythmEngine (juce::AudioProcessorValueTreeState& s,
                  const spectrum::SpectrumEngine& eng, const TuningManager& tun)
        : apvts (s), engine (eng), tuning (tun)
    {
        onParam   = apvts.getRawParameterValue ("rhythmOn");
        selfParam = apvts.getRawParameterValue ("rhythmSelfPlay");
        fbParam   = apvts.getRawParameterValue ("rhythmFeedback");
        fbDecayParam = apvts.getRawParameterValue ("rhythmFeedbackDecay");
        velSlopeParam = apvts.getRawParameterValue ("rhythmVelSlope");
        qPulseParam  = apvts.getRawParameterValue ("rhythmQPulse");
        octParam  = apvts.getRawParameterValue ("rhythmOctAll");
        stepsAllParam = apvts.getRawParameterValue ("rhythmStepsAll");
        xParam    = apvts.getRawParameterValue ("rhythmDivX");
        nParam    = apvts.getRawParameterValue ("rhythmDivN");
        modeParam = apvts.getRawParameterValue ("rhythmMode");
        mpeParam  = apvts.getRawParameterValue ("rhythmMpe");
        bendParam = apvts.getRawParameterValue ("bendRange");
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        // Pre-size the output buffer to the worst-case event burst (echo bend+on+off,
        // main trains, pending flush) so out.addEvent never allocates in processBlock.
        out.ensureSize ((size_t) (3 * kFbMaxOn + kFbMaxOff + 4 * kSlots * kMaxNotes
                                  + kSlots * kMaxNotes + kFbMaxOff) * 16);
        // Before wiping the gate state, remember any OUTPUT notes that are still
        // sounding so the next block can send their note-offs. Otherwise a re-prepare
        // (sample-rate/block-size change, plugin re-init, transport re-arm...) would
        // abandon them, leaving stuck "suspended notes without rhythm" in the receiver.
        flushOpenGates();
        flushEchoes();
        for (int i = 0; i < kSlots; ++i)
            for (int j = 0; j < kMaxNotes; ++j)
            { phase[i][j] = 0.0; gateLeft[i][j] = 0; wasOn[i][j] = false; }
        for (auto& h : heldCol) h.active = false;
        for (auto& a : alignCol) a = false;
        for (auto& b : channelBend) b = 0.0;
        driverCol = -1; lastDriver = -1;
        engineClock = 0; patternAnchor = -1; patternCycle = 0;
        displayF0.store (0.0f, std::memory_order_relaxed);
    }

    // Quantize-to-pulse support: samples (from the CURRENT block start, i.e. call
    // this from processBlock BEFORE rhythm.process) until the running pattern's next
    // full reset - the moment every train's subdivision coincides on the 1. Returns
    // < 0 when no pattern is running (nothing to sync to).
    double samplesToNextReset() const noexcept
    {
        if (driverCol < 0 || patternCycle <= 0 || patternAnchor < 0) return -1.0;
        const juce::int64 dt = engineClock - patternAnchor;
        if (dt <= 0) return (double) -dt; // anchor still ahead (same-block note-on)
        const juce::int64 k = (dt + patternCycle - 1) / patternCycle; // ceil
        return (double) (patternAnchor + k * patternCycle - engineClock);
    }
    // Samples since the running pattern's anchor (< 0 = no pattern) - lets the gate
    // treat near-simultaneous presses as "join in phase now" instead of deferring.
    double samplesSinceAnchor() const noexcept
    {
        if (driverCol < 0 || patternAnchor < 0) return -1.0;
        return (double) (engineClock - patternAnchor);
    }

    // Queue note-offs for every currently-open gate (message thread, when audio is
    // stopped). The next process() block emits them so nothing hangs.
    void flushOpenGates() noexcept
    {
        for (int i = 0; i < kSlots; ++i)
            for (int j = 0; j < kMaxNotes; ++j)
                if (gateLeft[i][j] > 0)
                {
                    if (pendingOffN < (int) (sizeof (pendingOff) / sizeof (pendingOff[0])))
                        pendingOff[pendingOffN++] = { gateChn[i][j], gateNote[i][j] };
                    gateLeft[i][j] = 0; // idempotent: a bare reset() must not double-off later
                }
    }

    // Owe a note-off for every echo that is currently sounding, then drop all pending
    // echoes. Unfired echo note-ons never sounded, so they need no note-off.
    void flushEchoes() noexcept
    {
        for (int k = 0; k < echoOffN; ++k)
            if (pendingOffN < (int) (sizeof (pendingOff) / sizeof (pendingOff[0])))
                pendingOff[pendingOffN++] = { echoOff[k].ch, echoOff[k].note };
        echoOnN = 0; echoOffN = 0;
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

        // Flush note-offs for gates abandoned by a reset/re-prepare (no hanging notes).
        for (int k = 0; k < pendingOffN; ++k)
            out.addEvent (juce::MidiMessage::noteOff (pendingOff[(size_t) k].ch, pendingOff[(size_t) k].note), 0);
        pendingOffN = 0;

        // Queued one-shot controller (DAW-tempo mapping) leaves at block start.
        if (const int pk = pendingCC.exchange (-1, std::memory_order_relaxed); pk >= 0)
            out.addEvent (juce::MidiMessage::controllerEvent (1, pk >> 8, pk & 0xff), 0);

        const auto* tmpl = engine.getLive();
        refreshTemplateCache (tmpl);
        keyHzValid = false; // the tuning cannot change mid-block: rebuild at most once here

        // Generate pulses whenever EITHER the MIDI-out master or the "play in Fibló"
        // (self-play) toggle is on - self-play must still make pulses when MIDI-out is off.
        const bool on  = (onParam   != nullptr && onParam->load()   > 0.5f)
                      || (selfParam != nullptr && selfParam->load() > 0.5f);
        // Switching the engine OFF must not strand whatever is sounding: close every
        // open gate and drop the queued echoes, so no pulse note is left held (that
        // is what droned on under the spectrum after toggling the panel's buttons).
        if (wasOnEngine && ! on) { flushOpenGates(); flushEchoes(); }
        wasOnEngine = on;
        const bool all = modeParam != nullptr && modeParam->load() > 0.5f;
        mpeOut         = mpeParam  != nullptr && mpeParam->load()  > 0.5f;
        const int  x   = juce::jlimit (2, 12, (int) xParam->load());
        const int  nD  = juce::jlimit (1, 16, (int) nParam->load());
        divisor = std::pow ((double) x, (double) nD);

        // Internal feedback: only inside the "play in Fibló" loop (the user's "without
        // MIDI out" case) - it must not flood a DAW track with recursive pulses.
        feedbackActive = (fbParam   != nullptr && fbParam->load()   > 0.5f)
                      && (selfParam != nullptr && selfParam->load() > 0.5f);
        feedbackDecay  = fbDecayParam != nullptr
                       ? (double) juce::jlimit (0.05f, 0.95f, fbDecayParam->load()) : 0.5;

        // FRACTAL feedback snapshot: the routed slots' partial ratios (child pulse
        // RATES) and pitch specs (child pulse PITCHES), so echoes can spawn their own
        // children outside render(). Rebuilt every block (cheap, atomics only).
        fbN = 0;
        if (feedbackActive && tmpl != nullptr)
            for (int i = 0; i < kSlots && fbN < kSlots; ++i)
            {
                const int p = slots[i].partial.load (std::memory_order_relaxed);
                if (! slots[i].enabled.load (std::memory_order_relaxed) || p < 0 || p >= (int) tmpl->partials.size())
                    continue;
                fbPartialRatio[fbN] = (double) tmpl->partials[(size_t) p].ratio;
                fbUseRatio[fbN]     = slots[i].useRatio.load (std::memory_order_relaxed);
                fbPitchRatio[fbN]   = juce::jmax (1.0e-6, slots[i].ratio.load (std::memory_order_relaxed));
                fbSteps[fbN]        = slots[i].steps.load (std::memory_order_relaxed);
                ++fbN;
            }

        int cursor = 0;
        for (const auto meta : io)
        {
            const auto msg = meta.getMessage();
            const int  pos = juce::jlimit (0, numSamples, (int) meta.samplePosition);
            render (tmpl, on, all, cursor, pos);
            cursor = juce::jmax (cursor, pos);
            handleEvent (msg, pos, all);
        }
        render (tmpl, on, all, cursor, numSamples);

        fireEchoes (numSamples); // emit due feedback pulses (fired ones spawn children)

        engineClock += numSamples; // absolute sample frame (pattern anchor/cycle)

        io.swapWith (out); // input events consumed; only our pulses leave the plugin
    }

    // Emit every feedback echo whose time has come this block and carry the rest
    // forward. Fired echoes then spawn their own children (the recursion step) -
    // appended AFTER the pass, which is exact because a child's delay is >= one train
    // period (>= sr/30 samples), always beyond the current sample. MidiBuffer sorts by
    // sample, so add order does not matter.
    void fireEchoes (int numSamples)
    {
        // Note-offs owed by echoes that sounded in earlier blocks.
        int w = 0;
        for (int k = 0; k < echoOffN; ++k)
        {
            if (echoOff[k].at < numSamples)
                out.addEvent (juce::MidiMessage::noteOff (echoOff[k].ch, echoOff[k].note), juce::jmax (0, echoOff[k].at));
            else { echoOff[k].at -= numSamples; echoOff[w++] = echoOff[k]; }
        }
        echoOffN = w;

        // Due echo note-ons. An echo is only EMITTED if its note-off has a guaranteed
        // place (same block, or room in echoOff) - a saturated queue drops the echo
        // entirely rather than ever leaving a hanging note. The note-off is computed
        // from the CLAMPED fire sample so a rebased-negative `at` (large host buffers)
        // can never produce an off before its on.
        w = 0;
        firedN = 0;
        const int n0 = echoOnN; // children appended below are NOT part of this pass
        for (int k = 0; k < n0; ++k)
        {
            EchoOn e = echoOn[k];
            if (e.at < numSamples)
            {
                const int s     = juce::jmax (0, e.at);
                const int offAt = s + juce::jmax (1, e.gate);
                if (offAt >= numSamples && echoOffN >= kFbMaxOff)
                    continue; // no room to owe the note-off -> drop the echo (never hang)
                if (mpeOut) out.addEvent (juce::MidiMessage::pitchWheel (e.ch, e.bend), s);
                out.addEvent (juce::MidiMessage::noteOn (e.ch, e.note, (juce::uint8) e.vel), s);
                if (offAt < numSamples)
                    out.addEvent (juce::MidiMessage::noteOff (e.ch, e.note), offAt);
                else
                    echoOff[echoOffN++] = { offAt - numSamples, e.note, e.ch };
                if (firedN < kFbMaxFired) fired[firedN++] = e; // spawn its children after the pass
            }
            else { e.at -= numSamples; echoOn[w++] = e; }
        }
        echoOnN = w;

        // Recursion: every fired echo becomes a parent - "the pulses of each pulse".
        // Their children's `at` is written relative to THIS block's start, but the
        // carry pass above has already rebased everything else to the NEXT block -
        // so rebase the freshly spawned children too, or every generation >= 2 would
        // land one block late (a buffer-size-dependent drift between fractal levels).
        const int spawnBase = echoOnN;
        for (int k = 0; k < firedN; ++k)
            spawnChildren (juce::jmax (0, fired[k].at), fired[k].hz, fired[k].note,
                           fired[k].vel, fired[k].gate);
        for (int i = spawnBase; i < echoOnN; ++i)
            echoOn[i].at -= numSamples;
    }

    // A pulse (main or echo) fired at sample `s`, sounding pitch `parentHz` / note
    // `parentNote`, velocity `parentVel`, gate `gate`: spawn ONE child per routed slot -
    // the child's RATE comes from the slot's partial ratio applied to the PARENT'S pitch
    // (rate = parentHz * r_j / divisor, so the child set is the whole rhythm pattern
    // rebuilt one level down, time-scaled by the parent's pitch), its PITCH is the
    // slot's interval FROM the parent's pitch, its velocity is the parent's * decay,
    // and it inherits the parent's gate. Each child spawns again when it fires -> a
    // self-similar fractal cascade. STRICTLY convergent: the child velocity is forced
    // below the parent's (jmin with parentVel-1), so depth is finite; queue caps bound
    // width; a full queue drops children (density saturated), never hangs.
    void spawnChildren (int s, double parentHz, int parentNote, int parentVel, int gate)
    {
        if (! feedbackActive || parentHz <= 0.0 || fbN == 0) return;
        const int cv = juce::jmin ((int) std::lround ((double) parentVel * feedbackDecay),
                                   parentVel - 1); // strict decrease -> guaranteed to die out
        if (cv < 1) return;
        for (int k = 0; k < fbN; ++k)
        {
            if (echoOnN >= kFbMaxOn) return; // saturated: drop (bounded density), never hang
            const double rate = juce::jlimit (0.01, 30.0, parentHz * fbPartialRatio[k] / divisor);
            const int    at   = s + (int) std::llround (sr / rate);
            int note; int bend = 8192; double hz;
            if (fbUseRatio[k])
            {
                // Same receiver-dependent encoding as the parent trains, and hz is
                // set to what will REALLY sound so the next generation's rates
                // follow the pitch actually produced rather than an intended one.
                const double want = parentHz * fbPitchRatio[k];
                int m = 0, b = 8192; double produced = want;
                encodePitch (want, pulsesFeedOwnSynth(), m, b, produced);
                note = m;
                hz   = produced;
                if (mpeOut) bend = b;
            }
            else
            {
                note = juce::jlimit (0, 127, parentNote + fbSteps[k]);
                hz   = tuning.frequencyForNote (note); // lock-free atomic read (RT-safe)
            }
            const int ch = mpeOut ? 2 + (echoChanRot++ % 14) : 1; // 16 reserved for the drone
            echoOn[echoOnN++] = { at, note, ch, cv, bend, juce::jmax (1, gate), hz };
        }
    }

private:
    void handleEvent (const juce::MidiMessage& msg, int pos, bool all)
    {
        if (msg.isNoteOn())
        {
            const bool wasEmpty = driverCol < 0; // no note was driving -> this one anchors the grid
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
                updateDriver();
                const bool qp = qPulseParam != nullptr && qPulseParam->load() > 0.5f;
                if (wasEmpty || patternAnchor < 0)
                    patternAnchor = engineClock + pos;   // fresh grid: this note is the "1"
                else if (qp)
                {
                    // Landing exactly ON a pattern reset (the gate defers late joiners
                    // there) = "se suma en el 1": plain downbeat. Anywhere else (the
                    // near-simultaneous window) = join IN PHASE with the running grid.
                    const juce::int64 since = engineClock + pos - patternAnchor;
                    const juce::int64 rem   = patternCycle > 0 ? since % patternCycle : 1;
                    if (! (rem <= 3 || (patternCycle > 0 && patternCycle - rem <= 3)))
                        alignCol[col] = true;
                }
            }
            else
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
                    alignCol[j] = false;
                    break;
                }
            }
            updateDriver();
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            for (int j = 0; j < kMaxNotes; ++j)
                if (heldCol[(size_t) j].active) { closeColumnGates (j, pos); heldCol[(size_t) j].active = false; alignCol[j] = false; }
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

        // DETERMINISM: whenever the DRIVING note changes in Lowest-note mode, the
        // new driver's trains restart from a fresh downbeat instead of resuming a
        // frozen mid-phase - the same key always yields the same rhythm.
        if (driverCol != lastDriver)
        {
            if (! all && driverCol >= 0)
            {
                for (int i = 0; i < kSlots; ++i) phase[i][driverCol] = 1.0;
                patternAnchor = engineClock + pos; // fresh downbeat = the pattern's new "1"
            }
            lastDriver = driverCol;
        }
    }

public:
    //==========================================================================
    // UI read-outs (message thread).
    float getRateHz (int slot) const noexcept
    { return juce::isPositiveAndBelow (slot, kSlots) ? lastRateHz[(size_t) slot].load (std::memory_order_relaxed) : 0.0f; }
    float getDriverF0() const noexcept { return displayF0.load (std::memory_order_relaxed); }
    // The pitch a slot's pulses will REALLY sound at, for the given held-note column
    // (0 = the driver). Published for the verifiers, which check that a ratio pulse
    // lands on f0 * ratio under any Reference A / Octave Transpose.
    float getSendHz (int slot, int col) const noexcept
    {
        return (juce::isPositiveAndBelow (slot, kSlots) && juce::isPositiveAndBelow (col, kMaxNotes))
             ? lastSendHz[(size_t) slot][(size_t) col].load (std::memory_order_relaxed) : 0.0f;
    }
    // The (note, bend) actually PUT ON THE WIRE for a slot/column - so a verifier can
    // resolve them the way the receiver will and check the pitch that results.
    int getSendNote (int slot, int col) const noexcept
    {
        return (juce::isPositiveAndBelow (slot, kSlots) && juce::isPositiveAndBelow (col, kMaxNotes))
             ? lastSendNote[(size_t) slot][(size_t) col].load (std::memory_order_relaxed) : -1;
    }
    int getSendBend (int slot, int col) const noexcept
    {
        return (juce::isPositiveAndBelow (slot, kSlots) && juce::isPositiveAndBelow (col, kMaxNotes))
             ? lastSendBend[(size_t) slot][(size_t) col].load (std::memory_order_relaxed) : 8192;
    }
    double getBendRangeSemis() const noexcept { return bendRangeSemis(); }
    // True when at least one partial is routed - self-play only engages then, so the
    // synth still plays its held notes normally when no rhythm is set up.
    bool hasEnabledSlot() const noexcept
    {
        for (int i = 0; i < kSlots; ++i)
            if (slots[i].enabled.load (std::memory_order_relaxed)
                && slots[i].partial.load (std::memory_order_relaxed) >= 0)
                return true;
        return false;
    }

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

    // The MIDI channel a train sends on: MPE out spreads the trains over member
    // channels 2..15; plain mode uses channel 1. Channel 16 is RESERVED for the
    // self-play drone ("note under pulses") - a pulse sharing the drone's channel
    // and note would kill the drone voice with its gate note-off.
    int trainChannel (int slot, int col) const noexcept
    {
        return mpeOut ? 2 + ((slot * kMaxNotes + col) % 14) : 1;
    }

    void render (const spectrum::SpectrumTemplate* tmpl, bool on, bool all, int from, int to)
    {
        if (to <= from) return;

        const int count = tmpl != nullptr ? (int) tmpl->partials.size() : 0;
        // Global octave shift of ALL pulses (+-4): ratio pitches x 2^oct; steps pitches
        // shift by whole periods of the active scale. Level-0 only - echoes inherit it
        // through their parent's pitch.
        const int octAll = octParam != nullptr ? juce::jlimit (-4, 4, (int) octParam->load()) : 0;
        // Global STEPS offset on top of each slot's own "steps from the note". Only
        // applies to step-pitched slots (a ratio slot has no step to shift), and it
        // is a plain parameter so a modulator can sweep it.
        const int stepsAll = stepsAllParam != nullptr
                           ? juce::jlimit (-48, 48, (int) std::lround (stepsAllParam->load())) : 0;
        const int octSteps = octAll * juce::jmax (1, tuning.degreesPerPeriod());
        const double octMul = std::exp2 ((double) octAll);
        // Who hears these pulses decides how their pitch is encoded (see encodePitch).
        const bool toOwnSynth = pulsesFeedOwnSynth();

        // Per-segment constants per train (f0 constant between MIDI events), and the
        // WORK LIST: only slots that can pulse now or still have an open gate are
        // visited in the per-sample loop - unused slots (kSlots is large) cost nothing.
        nWork = 0;
        for (int i = 0; i < kSlots; ++i)
        {
            const int  p  = slots[i].partial.load (std::memory_order_relaxed);
            const bool ok = on && slots[i].enabled.load (std::memory_order_relaxed)
                         && p >= 0 && p < count;
            gateLen[i] = juce::jmax (1, (int) ((double) slots[i].durMs.load (std::memory_order_relaxed) * 0.001 * sr));
            // Velocity of every pulse this slot sends: the per-slot user value (1..127)
            // times the VELOCITY SLOPE, a tilt across the spectrum. 0 = flat (every
            // slot keeps its own velocity). Positive tilts the HIGH partials down (at
            // the top of the range nothing is left); negative tilts the low ones down.
            {
                float v = (float) juce::jlimit (1, 127, slots[i].vel.load (std::memory_order_relaxed));
                const float sl = velSlopeParam != nullptr ? velSlopeParam->load() : 0.0f;
                const float k  = std::abs (sl);
                if (k > 1.0e-4f && count > 1 && p >= 0)
                {
                    const float u = (float) p / (float) (count - 1);   // 0 = lowest partial
                    const float x = sl > 0.0f ? u : 1.0f - u;          // which end fades first
                    // A straight ramp anchored at the near end. k = 0.5 reaches zero
                    // exactly at the far end; beyond that it keeps steepening, and at
                    // the very top it is vertical - no pulse keeps any velocity.
                    // Ramp: zero at the far end by k = 0.5, steeper after that. The
                    // last stretch ALSO fades the near end, so the whole column walks
                    // down to silence instead of jumping there at the very top.
                    const float ramp = juce::jlimit (0.0f, 1.0f, 1.0f - x * k / std::max (1.0e-3f, 1.0f - k));
                    const float tail = k > 0.8f ? juce::jlimit (0.0f, 1.0f, (1.0f - k) / 0.2f) : 1.0f;
                    v *= ramp * tail;
                }
                velArr[i] = (juce::uint8) juce::jlimit (0, 127, (int) std::lround (v));
            }

            bool anyGate = false;
            float uiRate = 0.0f;
            for (int j = 0; j < kMaxNotes; ++j)
            {
                if (gateLeft[i][j] > 0) anyGate = true;
                inc[i][j] = 0.0;
                const auto& h = heldCol[(size_t) j];
                const bool colOn = h.active && (all || j == driverCol);
                if (! (ok && colOn && h.f0 > 0.0)) continue;

                const double ratio = (double) tmpl->partials[(size_t) p].ratio;
                const double hz    = h.f0 * ratio / divisor;
                // Hard rhythmic ceiling: an under-divided rate must NEVER become a
                // 100-notes-per-second MIDI flood (that reads as a broken drone and
                // chokes the receiving synth's voice pool).
                const double r     = juce::jlimit (0.01, 30.0, hz);
                inc[i][j] = r / sr;
                if (j == driverCol) uiRate = (float) r;
                // Fresh DOWNBEAT whenever a train BECOMES active mid-hold (slot just
                // routed, On just flipped, mode switched...): otherwise it would start
                // from a stale mid-phase and the first cycle sounds early/shifted.
                // Quantize-to-pulse joiners instead align to the RUNNING grid: phases
                // as if the train had started at the pattern anchor, so its pulses
                // land on the shared subdivisions (slightly advanced, in phase).
                if (! wasOn[i][j])
                {
                    if (alignCol[j] && patternAnchor >= 0)
                    {
                        const double cyc = (double) (engineClock + from - patternAnchor) * inc[i][j];
                        const double ph  = cyc - std::floor (cyc);
                        phase[i][j] = ph < 1.0e-9 ? 1.0 : ph; // exactly on a grid point -> fire now
                    }
                    else
                        phase[i][j] = 1.0;
                }

                // Sent pitch: ratio slots = the EXACT pitch f0*slotRatio (note+bend),
                // encoded for whoever RECEIVES it (see encodePitch); plain slots = the
                // typed note transposed by this held note's interval from the lowest
                // held note. sendHz = the pitch that will really sound, which is what
                // the fractal children's rates are derived from.
                if (slots[i].useRatio.load (std::memory_order_relaxed))
                {
                    const double tHz = h.f0 * juce::jmax (1.0e-6, slots[i].ratio.load (std::memory_order_relaxed)) * octMul;
                    int m = 0, bend = 8192; double produced = tHz;
                    encodePitch (tHz, toOwnSynth, m, bend, produced);
                    sendNote[i][j] = m;
                    sendHz  [i][j] = produced;
                    sendBend[i][j] = bend;
                }
                else
                {
                    // Scale steps FROM the played note (each MIDI note = one scale
                    // step of the active tuning), never a fixed pitch.
                    sendNote[i][j] = juce::jlimit (0, 127,
                        h.note + slots[i].steps.load (std::memory_order_relaxed) + octSteps + stepsAll);
                    sendHz  [i][j] = tuning.frequencyForNote (sendNote[i][j]);
                    sendBend[i][j] = 8192; // centre
                }
            }
            for (int j = 0; j < kMaxNotes; ++j) wasOn[i][j] = inc[i][j] > 0.0;
            for (int j = 0; j < kMaxNotes; ++j)
            {
                lastSendHz  [(size_t) i][(size_t) j].store ((float) sendHz[i][j], std::memory_order_relaxed);
                lastSendNote[(size_t) i][(size_t) j].store (sendNote[i][j], std::memory_order_relaxed);
                lastSendBend[(size_t) i][(size_t) j].store (sendBend[i][j], std::memory_order_relaxed);
            }
            lastRateHz[(size_t) i].store (uiRate, std::memory_order_relaxed);
            if (ok || anyGate) workList[nWork++] = i;
        }

        // Pattern cycle (quantize-to-pulse): the driver's trains all coincide on the
        // "1" every  L / baseRate  seconds, where the rates rationalize to n_k/d_k of
        // the slowest and L = lcm(d_k). L is capped (an absurd rationalization would
        // mean waiting minutes) - the cap only coarsens WHERE a new note may join.
        if (driverCol >= 0)
        {
            double base = 0.0;
            for (int wi = 0; wi < nWork; ++wi)
            {
                const double r = inc[workList[wi]][driverCol] * sr;
                if (r > 0.0 && (base <= 0.0 || r < base)) base = r;
            }
            if (base > 0.0)
            {
                long long L = 1;
                for (int wi = 0; wi < nWork; ++wi)
                {
                    const double r = inc[workList[wi]][driverCol] * sr;
                    if (! (r > 0.0)) continue;
                    // best small fraction n/d ~ r/base (continued-fraction style scan)
                    long long bn = 1, bd = 1; double be = 1.0e18;
                    for (long long d = 1; d <= 16; ++d)
                    {
                        const long long n = (long long) std::llround (r / base * (double) d);
                        if (n <= 0) continue;
                        const double err = std::abs (r / base - (double) n / (double) d);
                        if (err < be - 1.0e-12) { be = err; bn = n; bd = d; }
                    }
                    const long long g = std::gcd (bn, bd);
                    L = std::lcm (L, bd / (g > 0 ? g : 1));
                    if (L > 96) { L = 96; break; } // practical cap
                }
                patternCycle = (juce::int64) std::llround ((double) L / base * sr);
            }
            else patternCycle = 0;
        }
        else patternCycle = 0;

        for (int s = from; s < to; ++s)
            for (int wi = 0; wi < nWork; ++wi)
            {
                const int i = workList[wi];
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
                            // Another column already gating the SAME channel+note (two
                            // held notes whose steps land on one pitch): close it first,
                            // or the receiver stacks two voices sharing one note-off -
                            // the "eternal pulse".
                            for (int j2 = 0; j2 < kMaxNotes; ++j2)
                                if (j2 != j && gateLeft[i][j2] > 0
                                    && gateNote[i][j2] == sendNote[i][j] && gateChn[i][j2] == ch)
                                { out.addEvent (juce::MidiMessage::noteOff (ch, sendNote[i][j]), s); gateLeft[i][j2] = 0; }
                            if (mpeOut) // per-note bend BEFORE the note-on (MPE convention)
                                out.addEvent (juce::MidiMessage::pitchWheel (ch, sendBend[i][j]), s);
                            out.addEvent (juce::MidiMessage::noteOn (ch, sendNote[i][j], velArr[i]), s);
                            gateNote[i][j] = sendNote[i][j];
                            gateChn [i][j] = ch;
                            gateLeft[i][j] = gateLen[i];
                            // Fractal feedback: this pulse becomes a parent - it spawns
                            // the full set of routed trains one level down, anchored at
                            // ITS pitch, velocity and gate.
                            if (feedbackActive)
                                spawnChildren (s, sendHz[i][j], sendNote[i][j], (int) velArr[i], gateLen[i]);
                        }
                    }
                }
            }
    }

    juce::AudioProcessorValueTreeState& apvts;
    const spectrum::SpectrumEngine&     engine;
    const TuningManager&                tuning;
    std::atomic<float>* onParam = nullptr;
    std::atomic<float>* selfParam = nullptr; // "play in Fibló" - interpret pulses internally
    std::atomic<float>* fbParam   = nullptr; // internal feedback loop on/off
    std::atomic<float>* fbDecayParam = nullptr; // feedback volume slope (velocity kept per loop)
    std::atomic<float>* velSlopeParam = nullptr; // velocity tilt across the spectrum
    std::atomic<float>* qPulseParam = nullptr; // quantize-to-pulse (phase-aligned joiners)
    std::atomic<float>* octParam  = nullptr; // global octave shift of all pulses (-4..+4)
    std::atomic<float>* stepsAllParam = nullptr; // global scale-step offset of step-pitched pulses
    std::atomic<float>* xParam  = nullptr;
    std::atomic<float>* nParam  = nullptr;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* mpeParam  = nullptr;
    std::atomic<float>* bendParam = nullptr;

    // Encode a fractional-semitone offset as a 14-bit pitch wheel value using the
    // range the SYNTH is actually set to. This used to assume the MPE default of
    // +-48 unconditionally, so with the range on +-2 or +-12 every JI pulse landed
    // at 1/24 or 1/4 of its intended detune - the "JI breaks the sound" report.
    int encodeBend (double semitoneOffset) const noexcept
    {
        static constexpr double bendValues[] = { 2.0, 12.0, 48.0 };
        const int idx = juce::jlimit (0, 2, bendParam != nullptr ? (int) bendParam->load() : 2);
        const double range = bendValues[idx];
        return juce::jlimit (0, 16383, 8192 + (int) std::lround (semitoneOffset / range * 8192.0));
    }
    // Widest detune the current bend range can actually express, for clamping.
    double bendRangeSemis() const noexcept
    {
        static constexpr double bendValues[] = { 2.0, 12.0, 48.0 };
        return bendValues[juce::jlimit (0, 2, bendParam != nullptr ? (int) bendParam->load() : 2)];
    }

    // Ascending table of the ACTIVE tuning's frequency for every MIDI key, rebuilt
    // once per render segment (the tuning cannot change mid-block). Lets a target
    // pitch be turned into "nearest key of THIS tuning + residual bend" with a
    // binary search instead of a 128-entry scan per slot per held note.
    double keyHz[128] {};
    bool   keyHzValid = false;
    void buildKeyTable() noexcept
    {
        for (int n = 0; n < 128; ++n) keyHz[n] = tuning.frequencyForNote (n);
        keyHzValid = true;
    }

    // Turn an absolute target frequency into the (note, bend) pair to SEND, and
    // report the pitch that will actually result.
    //
    // Which encoding is right depends on WHO receives it:
    //  - external MIDI out: the receiver has its own tuning, so the only meaningful
    //    thing to hand it is the absolute pitch as a 12-TET/440 key plus bend.
    //  - self-play: the note goes back into THIS synth, which resolves the key
    //    through the active tuning again. Sending a 12-TET key there applied the
    //    tuning (and Reference A, and Octave Transpose) a SECOND time - with
    //    octave = -1 every ratio pulse came out exactly an octave flat, at A=432
    //    about 32 cents flat, and in a Scala tuning arbitrarily off. So here the
    //    key is picked from the tuning's own table and the bend is the residual
    //    against THAT key, which lands the pulse on the pitch actually asked for.
    void encodePitch (double targetHz, bool forSynth,
                      int& noteOut, int& bendOut, double& producedHz) noexcept
    {
        targetHz = juce::jmax (1.0e-6, targetHz);
        if (! forSynth)
        {
            const double semis = 69.0 + 12.0 * std::log2 (targetHz / 440.0);
            const int    m     = juce::jlimit (0, 127, (int) std::lround (semis));
            noteOut    = m;
            bendOut    = encodeBend (semis - (double) m);
            producedHz = targetHz;
            return;
        }

        if (! keyHzValid) buildKeyTable();
        // Nearest key by frequency. The table is ascending for every tuning we can
        // produce, so lower_bound gives the insertion point and the answer is one of
        // its two neighbours.
        const auto* first = keyHz;
        const auto* it = std::lower_bound (first, first + 128, targetHz);
        int m = (int) (it - first);
        if (m >= 128) m = 127;
        if (m > 0 && std::abs (std::log2 (keyHz[m - 1] / targetHz))
                   <= std::abs (std::log2 (keyHz[m] / targetHz))) --m;

        const double base = juce::jmax (1.0e-9, keyHz[m]);
        const double want = 12.0 * std::log2 (targetHz / base);
        // A pulse further from every key than the bend range can reach cannot be
        // placed exactly; report what will really sound so the fractal children's
        // rates stay in step with it rather than with a pitch nobody hears.
        const double lim  = bendRangeSemis();
        const double used = juce::jlimit (-lim, lim, want);
        noteOut    = m;
        bendOut    = encodeBend (used);
        producedHz = base * std::exp2 (used / 12.0);
    }

    // Self-play routes the pulses back into this synth, so it decides the encoding.
    // (When the MIDI output is on as well, self-play still wins: that is the path
    // the user is actually hearing.)
    bool pulsesFeedOwnSynth() const noexcept
    {
        return selfParam != nullptr && selfParam->load() > 0.5f && hasEnabledSlot();
    }

    juce::MidiBuffer out; // reused every block (capacity persists after warm-up)

    double sr      = 44100.0;
    double divisor = 1.0;
    bool   mpeOut  = false;
    bool   feedbackActive = false; // this block: self-play + feedback on -> schedule echoes
    double feedbackDecay  = 0.5;   // velocity kept per loop
    bool   wasOnEngine    = false; // engine state last block (to close gates when it goes off)
    juce::int64 engineClock   = 0;  // absolute sample frame
    juce::int64 patternAnchor = -1; // sample of the driver's last downbeat (the pattern's "1")
    juce::int64 patternCycle  = 0;  // samples until the whole polyrhythm coincides again (0 = none)
    Held   heldCol[kMaxNotes];
    bool   alignCol[kMaxNotes] = {}; // quantize-to-pulse joiner: align phases to the grid
    int    driverCol  = -1;
    int    lastDriver = -1; // to detect driver changes (fresh downbeat in Lowest mode)
    double channelBend[16] = {};

    double phase   [kSlots][kMaxNotes] = {};
    bool   wasOn   [kSlots][kMaxNotes] = {}; // train was active last segment (downbeat on activation)
    int    gateLeft[kSlots][kMaxNotes] = {};
    int    gateNote[kSlots][kMaxNotes] = {};
    int    gateChn [kSlots][kMaxNotes] = {};

    // render() per-segment caches (members, not stack: kSlots is large) + the work
    // list of slots the per-sample loop actually visits.
    double      inc     [kSlots][kMaxNotes] = {};
    int         sendNote[kSlots][kMaxNotes] = {};
    int         sendBend[kSlots][kMaxNotes] = {};
    double      sendHz  [kSlots][kMaxNotes] = {}; // sounding pitch of each train (fractal children's rates)
    int         gateLen [kSlots] = {};
    juce::uint8 velArr  [kSlots] = {};
    int         workList[kSlots] = {};
    int         nWork = 0;

    //-- Internal FRACTAL feedback ("the pulses of each pulse") -----------------
    // Every fired pulse spawns one child per routed slot (rate from the slot's partial
    // ratio applied to the PARENT's pitch; pitch = the slot's interval from the parent;
    // velocity = parent's * decay, strictly decreasing; gate inherited). Children spawn
    // again when they fire -> a self-similar cascade, bounded by the velocity floor
    // (depth) and these fixed queues (width). echoOn holds future note-ons; echoOff the
    // note-offs owed by echoes already sounding, so a reset can flush them cleanly.
    static constexpr int kFbMaxOn    = 4096; // pending echo note-ons
    static constexpr int kFbMaxOff   = 1024; // owed echo note-offs (sounding echoes)
    static constexpr int kFbMaxFired = 256;  // echoes fired per block that get to spawn
    struct EchoOn  { int at, note, ch, vel, bend, gate; double hz; }; // at = samples until it fires; hz = its sounding pitch (children's rates)
    struct EchoOff { int at, note, ch; };
    EchoOn  echoOn [kFbMaxOn]  = {};
    EchoOff echoOff[kFbMaxOff] = {};
    EchoOn  fired  [kFbMaxFired] = {};       // this block's fired echoes (spawn after the pass)
    int     echoOnN = 0, echoOffN = 0, firedN = 0;
    unsigned echoChanRot = 0;                // rotates echo MPE channels over 2..16

    // Per-block snapshot of the routed slots for spawning (audio thread only).
    int    fbN = 0;
    double fbPartialRatio[kSlots] = {};      // child RATE = parentHz * this / divisor
    bool   fbUseRatio[kSlots] = {};          // child PITCH: ratio from parent vs steps from parent
    double fbPitchRatio[kSlots] = {};
    int    fbSteps[kSlots] = {};

    // Note-offs owed to the receiver after a reset abandoned open gates (emitted at
    // the start of the next process block so no note hangs). Sized to also absorb a
    // full flush of sounding echoes.
    struct Pending { int ch, note; };
    Pending pendingOff[kSlots * kMaxNotes + kFbMaxOff] = {};
    int     pendingOffN = 0;

    juce::uint32 cachedGen = 0;
    float        maxAmp    = 1.0f;

    std::atomic<int> pendingCC { -1 }; // packed (cc<<8)|value, -1 = none

    mutable std::atomic<float> lastRateHz[kSlots] = {};
    mutable std::atomic<float> lastSendHz[kSlots][kMaxNotes] = {}; // see getSendHz
    mutable std::atomic<int>   lastSendNote[kSlots][kMaxNotes] = {};
    mutable std::atomic<int>   lastSendBend[kSlots][kMaxNotes] = {};
    std::atomic<float> displayF0 { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmEngine)
};
