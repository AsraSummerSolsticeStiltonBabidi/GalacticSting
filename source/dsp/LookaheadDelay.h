#pragma once

//==============================================================================
// ADAPTIVE LOOKAHEAD - the plugin delays ITSELF so the spectrum is ready on time.
//
// "habria alguna manera de hacer que el modo realtime se anticipara al midi que
//  viene y preparara el spectrum de antemano para tener 0 latencia en el cambio de
//  parciales cuando se usa compositivamente en el daw? se que al tocar eso es
//  imposible pero al escribir midi y reproducirlo estaria bien"
//
// A plugin cannot see future MIDI. It CAN arrive late on purpose:
//
//   setLatencySamples(B)  ->  the host's delay compensation shifts this plugin's
//                             output EARLIER by B, so a note the host stamps at t
//                             must be SOUNDED by us at t + B to land at t.
//   this class            ->  every incoming event is re-stamped B samples later, so
//                             the synth, the note gate and the rhythm engine all live
//                             on that same shifted timeline and stay sample-aligned
//                             with each other.
//   the peek below        ->  the realtime follower reads the note stream at its
//                             ORIGINAL time, i.e. B samples before it sounds, and the
//                             adaptive search gets that whole budget to finish in.
//
// The audio needs no delay line of its own: the synth renders the note late because
// the note reaches it late, and nothing else in this instrument is time-critical
// against the input. That is the whole trick, and it costs one MidiBuffer.
//
// WHAT THE USER PAYS: live playing feels the budget, honestly labelled. The rhythm
// MIDI OUT is delayed by it as well - it is generated from the same shifted stream,
// which is what keeps the pulses locked to the audio - and hosts differ in whether
// they compensate a plugin's MIDI output by its reported latency. The tooltip says so
// rather than leaving it to be discovered.
//
// TRANSPORT DISCONTINUITY. A loop jumping back must not replay what is still in
// flight: those events belong to bars the transport has left. flushKeepingNoteOffs()
// drops them but still DELIVERS any note-off caught in the ring (see its own note: the
// note-on it closes has already sounded and been counted downstream), and the processor
// pairs the flush with an all-notes-off so nothing can be stranded holding.
// A budget CHANGE flushes for the same reason - events already stamped for the old
// timeline would arrive at the wrong distance from the notes around them.
//
// RT contract: two reused MidiBuffers, pre-sized; no allocation, no locks, no strings.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <algorithm>

class LookaheadDelay
{
public:
    LookaheadDelay() = default;

    void prepare()
    {
        buf.ensureSize (8192);
        keep.ensureSize (8192);
        out.ensureSize (8192);
        flush();
    }

    // Drop everything still in flight. See the transport note above.
    void flush() noexcept { buf.clear(); }

    // The flush a TRANSPORT DISCONTINUITY wants: drop everything still in flight EXCEPT
    // the note-offs, which are re-stamped to sample 0 of the block being processed and
    // so leave with this very block.
    //
    // WHY A NOTE-OFF IS NOT THE SAME AS A NOTE-ON HERE. The note-on it belongs to left
    // the ring one budget EARLIER: it has already sounded and, more to the point, has
    // already been counted by everything downstream of this delay - the note gate, the
    // mono glide, the modulator tally, and the rhythm engine's held chord. Dropping only
    // its note-off leaves all four believing the key is still down, and the rhythm engine
    // then goes on generating pulse trains from a key nobody is holding and emitting that
    // MIDI to the DAW for the rest of the session. Silencing the voices - which the
    // processor does beside this call - cures the audio and none of that bookkeeping.
    // A note-off for a note-on that is dropped here too is harmless: it closes a key
    // nothing is holding, which every consumer downstream already treats as a no-op.
    //
    // RT-safe by construction: the kept events move between the two pre-sized member
    // buffers and never touch the caller's, so nothing here can allocate.
    void flushKeepingNoteOffs() noexcept
    {
        keep.clear();
        for (const auto meta : buf)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOff() || m.isAllNotesOff() || m.isAllSoundOff())
                keep.addEvent (m, 0);
        }
        buf.swapWith (keep);
    }

    bool isEmpty() const noexcept { return buf.isEmpty(); }

    // Audio thread, once per block. `io` comes in on the host's timeline and leaves on
    // the delayed one. delaySamples == 0 is a pure pass-through with no reordering.
    void process (juce::MidiBuffer& io, int numSamples, int delaySamples)
    {
        if (delaySamples <= 0 && buf.isEmpty())
            return;                                   // off, and nothing owed: untouched

        out.clear();
        keep.clear();

        // Events buffered by earlier blocks first, so that among events sharing a
        // sample position the OLDER one still comes out first (MidiBuffer keeps
        // insertion order within a timestamp, and MonoGlide downstream relies on it).
        for (const auto meta : buf)
        {
            const int t = (int) meta.samplePosition;
            if (t < numSamples) out.addEvent  (meta.getMessage(), t);
            else                keep.addEvent (meta.getMessage(), t - numSamples);
        }
        for (const auto meta : io)
        {
            const int t = (int) meta.samplePosition + delaySamples;
            if (t < numSamples) out.addEvent  (meta.getMessage(), t);
            else                keep.addEvent (meta.getMessage(), t - numSamples);
        }

        buf.swapWith (keep);
        io.swapWith (out);
    }

private:
    juce::MidiBuffer buf, keep, out;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LookaheadDelay)
};

//==============================================================================
// THE EARLY VIEW OF THE KEYBOARD, and why a second note-tracker is allowed here.
//
// The realtime follower normally reads the RhythmEngine's held-note bookkeeping,
// with a comment saying that a second tracker is a second answer to "which note are
// we on" and that the two drift. That still holds - which is why this one is only
// ever consulted while lookahead is engaged, and why it is fed from the very same
// MidiBuffer, one statement earlier. It is not a different answer; it is the same
// answer read B samples sooner, and it becomes the rhythm engine's own view exactly
// one budget later. With lookahead off nothing reads it at all.
//
// It stores KEYS and RAW WHEEL VALUES, not frequencies: the tuning and the bend range
// are the processor's to apply, so this cannot embed a stale idea of either.
//
// Written on the audio thread, read on the message thread, published through the same
// seqlock idiom RhythmEngine::publishHeld uses - a torn read of "3 notes, one of them
// from the previous chord" would be latched by the follower until the chord next
// changed, so a retry is cheaper than the wrong target.
//==============================================================================
class LookaheadPeek
{
public:
    LookaheadPeek() = default; // (the non-copyable macro suppresses the implicit one)

    static constexpr int kMaxHeld = 16;

    void reset() noexcept
    {
        seq.fetch_add (1, std::memory_order_release);
        pubN.store (0, std::memory_order_relaxed);
        seq.fetch_add (1, std::memory_order_release);
        n = 0;
        for (auto& w : wheel) w.store (0, std::memory_order_relaxed);
    }

    // Audio thread: the UNDELAYED stream, before LookaheadDelay::process re-stamps it.
    void observe (const juce::MidiBuffer& raw) noexcept
    {
        bool moved = false;
        for (const auto meta : raw)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn())
            {
                remove (m.getChannel(), m.getNoteNumber());
                if (n < kMaxHeld) held[(size_t) n++] = { m.getNoteNumber(), m.getChannel() };
                anchor.store (lowestKey(), std::memory_order_relaxed);
                moved = true;
            }
            else if (m.isNoteOff())
            {
                remove (m.getChannel(), m.getNoteNumber());
                moved = true;
            }
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                n = 0;
                moved = true;
            }
            else if (m.isPitchWheel())
            {
                wheel[(size_t) juce::jlimit (0, 15, m.getChannel() - 1)]
                    .store (m.getPitchWheelValue() - 8192, std::memory_order_relaxed);
                moved = true;   // a bend moves the pitches this chord is asking for
            }
        }
        if (moved) publish();
    }

    // Message thread. Returns how many keys are down and fills (key, channel) pairs.
    int getHeld (int* keyOut, int* chOut, int maxOut) const noexcept
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const unsigned s0 = seq.load (std::memory_order_acquire);
            if ((s0 & 1u) != 0) continue;
            const int c = std::min (maxOut, pubN.load (std::memory_order_relaxed));
            for (int i = 0; i < c; ++i)
            {
                const int packed = pubKey[(size_t) i].load (std::memory_order_relaxed);
                keyOut[i] = packed & 0xff;
                chOut[i]  = (packed >> 8) & 0xff;
            }
            std::atomic_thread_fence (std::memory_order_acquire);
            if (seq.load (std::memory_order_relaxed) == s0)
                return c;
        }
        return 0;
    }

    // Wheel deflection for a 1-based channel, CENTRED (0 = no bend, +-8192 = full).
    // Stored centred so a default-constructed peek reads "no bend" rather than "bent
    // all the way down", which is what a raw 14-bit zero would have meant.
    int wheelFor (int ch1) const noexcept
    { return wheel[(size_t) juce::jlimit (0, 15, ch1 - 1)].load (std::memory_order_relaxed); }

    // The lowest key held, kept after release, exactly like RhythmEngine::getAnchorNote:
    // the adaptation's register range hangs off it and must not vanish with the chord.
    int getAnchorNote() const noexcept { return anchor.load (std::memory_order_relaxed); }

private:
    struct K { int note = 0, ch = 1; };

    void remove (int ch, int note) noexcept
    {
        for (int i = 0; i < n; ++i)
            if (held[(size_t) i].note == note && held[(size_t) i].ch == ch)
            { held[(size_t) i] = held[(size_t) (--n)]; return; }
    }
    int lowestKey() const noexcept
    {
        int lo = -1;
        for (int i = 0; i < n; ++i) if (lo < 0 || held[(size_t) i].note < lo) lo = held[(size_t) i].note;
        return lo >= 0 ? lo : anchor.load (std::memory_order_relaxed);
    }
    void publish() noexcept
    {
        seq.fetch_add (1, std::memory_order_release);   // -> odd: readers retry
        for (int i = 0; i < n; ++i)
            pubKey[(size_t) i].store ((held[(size_t) i].ch << 8) | held[(size_t) i].note,
                                      std::memory_order_relaxed);
        pubN.store (n, std::memory_order_relaxed);
        seq.fetch_add (1, std::memory_order_release);   // -> even: consistent again
    }

    K   held[kMaxHeld] = {};
    int n = 0;                                   // audio thread only
    std::atomic<int>      pubKey[kMaxHeld] = {};
    std::atomic<int>      pubN   { 0 };
    std::atomic<unsigned> seq    { 0 };
    std::atomic<int>      anchor { -1 };
    std::atomic<int>      wheel[16] = {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LookaheadPeek)
};
