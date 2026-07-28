#pragma once

//==============================================================================
// NoteGate: pre-processes the merged input MIDI (played notes + on-screen keys)
// BEFORE the synth and the RhythmEngine see it, implementing three independent,
// default-off note-start modes:
//
//   Quantize to project tempo - a note may only START on a beat of the host BPM:
//       pressed mid-beat it is HELD BACK (silent) and released exactly on the next
//       beat sample. Needs the host playhead; with no tempo info (standalone,
//       transport stopped) notes start immediately.
//   Mono - when a new note starts, the note(s) currently sounding stop (their
//       note-offs are emitted at the same sample the new note starts).
//   Quantize to pulse - a new note waits for the moment the RUNNING polyrhythm
//       pattern resets (all subdivisions coincide on the 1 - the pattern cycle
//       computed by the RhythmEngine) and joins exactly there. With no pattern
//       running it falls back to Quantize-to-tempo (if on) or starts immediately.
//
// A note released while still held back is cancelled silently (it never sounded,
// so no note-off is emitted). All state is plain scalars - audio-thread safe.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

class NoteGate
{
public:
    NoteGate() = default; // (the non-copyable macro suppresses the implicit one)

    static constexpr int kMaxPending = 32; // deferred note-ons
    static constexpr int kMaxActive  = 32; // sounding notes (for Mono)

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        out.ensureSize (4096);
        pendingN = 0; // held-back notes never sounded -> nothing owed
        activeN  = 0;
        clock    = 0;
    }

    // Audio thread, once per block, BEFORE the synth renders and the RhythmEngine
    // consumes the buffer.
    //   beatOff  - samples from block start to the NEXT host beat (< 0 = no tempo)
    //   beatPer  - samples per beat (<= 0 = no tempo)
    //   resetOff - samples from block start to the next pulse-pattern reset
    //              (< 0 = no pattern running)
    void process (juce::MidiBuffer& io, int numSamples,
                  bool qTempo, bool mono, bool qPulse,
                  double beatOff, double beatPer, double resetOff,
                  double sinceAnchor = -1.0)
    {
        // Fast path: nothing enabled and nothing pending -> just keep the active-note
        // tally in sync (so Mono works the instant it is switched on).
        if (! qTempo && ! mono && ! qPulse && pendingN == 0)
        {
            for (const auto meta : io)
                trackActive (meta.getMessage());
            clock += numSamples;
            return;
        }

        out.clear();

        const juce::int64 blockStart = clock;
        auto nextBeatAt = [&] (juce::int64 t) -> juce::int64
        {
            if (! qTempo || beatPer <= 0.0 || beatOff < 0.0) return t; // no grid -> now
            const double first = (double) blockStart + beatOff;       // first beat >= block start
            if ((double) t <= first) return (juce::int64) std::llround (first);
            const double k = std::ceil (((double) t - first) / beatPer - 1.0e-9);
            return (juce::int64) std::llround (first + k * beatPer);
        };
        const juce::int64 patternReset = (qPulse && resetOff >= 0.0)
                                       ? blockStart + (juce::int64) std::llround (resetOff) : -1;
        // Pressed only a hair after the pattern started? Let it through NOW - the
        // engine aligns its phases to the running grid, so it enters IN PHASE with a
        // slightly advanced cycle instead of waiting a whole pattern.
        const bool nearAnchor = sinceAnchor >= 0.0 && sinceAnchor <= 0.12 * sr;

        auto startTimeFor = [&] (juce::int64 pressedAt) -> juce::int64
        {
            // Quantize-to-pulse only defers while something IS sounding (activeN
            // tracks that): with nothing held there is no pattern to join - deferring
            // would just swallow clicks (press+release before the stale reset point).
            if (patternReset >= 0 && patternReset >= pressedAt && activeN > 0 && ! nearAnchor)
                return patternReset;             // join the running pattern on its 1
            return nextBeatAt (pressedAt);       // else the beat grid (or immediately)
        };

        // Emit a note-on at `s` (block-relative), applying Mono first. A retrigger of
        // an already-sounding note gets a pre-off so voices can never stack with only
        // one note-off owed (the "eternal pulse").
        auto emitOn = [&] (int s, int note, int ch, juce::uint8 vel)
        {
            if (mono)
            {
                for (int i = 0; i < activeN; ++i)
                    out.addEvent (juce::MidiMessage::noteOff (active[i].ch, active[i].note), s);
                activeN = 0;
            }
            else
                for (int i = 0; i < activeN; ++i)
                    if (active[i].note == note && active[i].ch == ch)
                    {
                        out.addEvent (juce::MidiMessage::noteOff (ch, note), s);
                        active[i] = active[--activeN];
                        break;
                    }
            out.addEvent (juce::MidiMessage::noteOn (ch, note, vel), s);
            if (activeN < kMaxActive) active[activeN++] = { note, ch, s };
        };
        // The sample this (ch,note) was turned on at, if it was turned on during THIS
        // block; -1 otherwise. A note-on deferred to a later sample of the same block
        // (quantise-to-beat) must never be closed by a note-off stamped EARLIER than
        // it - the receiver would drop the orphan off and sustain the note for ever.
        auto onSampleOf = [&] (int note, int ch) -> int
        {
            for (int i = 0; i < activeN; ++i)
                if (active[i].note == note && active[i].ch == ch) return active[i].onAt;
            return -1;
        };

        // 1) Held-back notes are released IN ORDER, interleaved with this block's own
        // events - never all at once up front. Flushing them first could emit a
        // pending note's ON at a LATER sample than the note-OFF that this same block
        // carries for it: the receiver then sees an orphan off (discarded, it matches
        // nothing yet) followed by an on that nobody will ever close, i.e. a note
        // stuck for good. Releasing only what is due before each event keeps every
        // on/off pair in the right order.
        auto flushPendingUpTo = [&] (juce::int64 upto)
        {
            int w = 0;
            for (int i = 0; i < pendingN; ++i)
            {
                if (pending[i].due <= upto)
                    emitOn ((int) juce::jmax ((juce::int64) 0, pending[i].due - blockStart),
                            pending[i].note, pending[i].ch, pending[i].vel);
                else
                    pending[w++] = pending[i];
            }
            pendingN = w;
        };

        // 2) This block's events.
        for (const auto meta : io)
        {
            flushPendingUpTo (blockStart + juce::jlimit (0, juce::jmax (0, numSamples - 1),
                                                        (int) meta.samplePosition));
            const auto msg = meta.getMessage();
            const int  pos = juce::jlimit (0, juce::jmax (0, numSamples - 1), (int) meta.samplePosition);

            if (msg.isNoteOn())
            {
                // A retrigger REPLACES any pending copy of the same key (two pendings
                // sharing one future note-off was the stuck-note bug).
                for (int i = 0; i < pendingN; ++i)
                    if (pending[i].note == msg.getNoteNumber() && pending[i].ch == msg.getChannel())
                    { pending[i] = pending[--pendingN]; break; }

                const juce::int64 at  = blockStart + pos;
                const juce::int64 due = startTimeFor (at);
                if (due <= at)
                    emitOn (pos, msg.getNoteNumber(), msg.getChannel(), msg.getVelocity());
                else if (due < blockStart + numSamples)
                    emitOn ((int) (due - blockStart), msg.getNoteNumber(), msg.getChannel(), msg.getVelocity());
                else if (pendingN < kMaxPending)
                    pending[pendingN++] = { due, msg.getNoteNumber(), msg.getChannel(), msg.getVelocity() };
                else // queue full: start immediately rather than dropping the note
                    emitOn (pos, msg.getNoteNumber(), msg.getChannel(), msg.getVelocity());
            }
            else if (msg.isNoteOff())
            {
                // Released while still held back -> cancel silently (it never sounded).
                bool cancelled = false;
                for (int i = 0; i < pendingN; ++i)
                    if (pending[i].note == msg.getNoteNumber() && pending[i].ch == msg.getChannel())
                    {
                        pending[i] = pending[--pendingN];
                        cancelled  = true;
                        break;
                    }
                if (! cancelled)
                {
                    // Never stamp the off before the on that opened it in this block.
                    const int onAt = onSampleOf (msg.getNoteNumber(), msg.getChannel());
                    out.addEvent (msg, onAt >= 0 ? juce::jmax (pos, onAt) : pos);
                    removeActive (msg.getNoteNumber(), msg.getChannel());
                }
            }
            else
            {
                if (msg.isAllNotesOff() || msg.isAllSoundOff()) { pendingN = 0; activeN = 0; }
                out.addEvent (msg, pos);
            }
        }
        // Anything still due inside this block, after the last event.
        flushPendingUpTo (blockStart + numSamples - 1);

        io.swapWith (out);
        clock += numSamples;
    }

private:
    void trackActive (const juce::MidiMessage& msg)
    {
        if (msg.isNoteOn())
        {
            if (activeN < kMaxActive) active[activeN++] = { msg.getNoteNumber(), msg.getChannel() };
        }
        else if (msg.isNoteOff())
            removeActive (msg.getNoteNumber(), msg.getChannel());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            activeN = 0;
    }
    void removeActive (int note, int ch)
    {
        for (int i = 0; i < activeN; ++i)
            if (active[i].note == note && active[i].ch == ch)
            { active[i] = active[--activeN]; return; }
    }

    struct Pending { juce::int64 due; int note, ch; juce::uint8 vel; };
    struct Active  { int note, ch; int onAt = -1; }; // onAt = sample it started at, this block

    juce::MidiBuffer out; // reused every block (pre-sized in prepare)
    Pending pending[kMaxPending] = {};
    Active  active [kMaxActive]  = {};
    int     pendingN = 0, activeN = 0;
    double  sr = 44100.0;
    juce::int64 clock = 0; // absolute sample position (this gate's own frame)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteGate)
};
