#pragma once

//==============================================================================
// MONOPHONIC GLIDE - the note-stream half.
//
// "haz que en pitch tenga un boton llamado monophonic glide y que al activarlo el
//  sinte no puede producir acordes pero al pulsar una nota y luego otra se hace
//  pitchbend de una a la otra en un tiempo especificado manualmente en ms"
//
// The feature is deliberately split in two, and this file is the SMALLER half:
//
//   * WHO SOUNDS  - the processor drops the synth to ONE voice while the mode is on.
//     That single fact is what makes chords impossible, and it is also what makes
//     the glide possible: with no free voice left, JUCE's MPESynthesiser starts the
//     new note by calling noteStarted() on the voice that is already sounding (a
//     STEAL), which is exactly a takeover. AdditiveVoice recognises that case and
//     bends its pitch instead of restriking (see AdditiveVoice::noteStarted).
//   * WHAT SOUNDS - this class, which only has to add the ONE event MIDI does not
//     carry by itself: the return to a key that is still held.
//
// THE PRIORITY RULE IS LAST-NOTE, and it is not a coin toss. The user's sentence is
// "press a note and then another and it bends from one to the other" - unqualified.
// Low-note priority answers a rising second note by REFUSING to move, which
// contradicts the request in the most common gesture there is. So the newest key
// always wins; a low-note rule would be a different instrument.
//
// A NOTE-OFF WHILE ANOTHER KEY IS HELD returns to the most recently pressed key that
// is still down - classic mono legato. That return is emitted as a plain note-on
// stamped at the SAME sample as, and BEFORE, the note-off that caused it:
//   - before, because the note-on steals the sounding voice (a legato takeover, so
//     it glides and does not retrigger) and the note-off that follows then matches
//     no voice at all. Emitted the other way round the voice would first be released
//     - envelope into its tail - and the return would have to re-attack it.
//   - as a plain note-on, because JUCE's MPEInstrument handles a second note-on for
//     an already-registered (channel, note) by retiring the old entry first, so its
//     note table stays consistent and the key's real note-off still lands later.
// MidiBuffer keeps insertion order among events sharing a sample position, so "the
// on before the off" survives the buffer.
//
// WHAT THIS CLASS DOES NOT DO: it never swallows an event. Every note-on and every
// note-off the player produced still reaches the synth, the rhythm engine and the
// modulator tally in its original order. That is not tidiness - the modulator gate
// counts held (channel, key) pairs, and a swallowed note-off would leave a key
// "held" for ever and the envelopes would never release.
//
// The stack is tracked even while the mode is OFF, so switching it on under a held
// chord finds the keys already there instead of an empty history.
//
// RT contract: plain scalars and two reused MidiBuffers; no allocation, no locks,
// no strings on the audio thread.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

class MonoGlide
{
public:
    MonoGlide() = default;

    // More than a hand can hold; a sustain pedal chord cannot realistically pass it.
    static constexpr int kMaxHeld = 32;

    void prepare()
    {
        out.ensureSize (4096);
        n = 0;
    }

    // Held keys are forgotten, nothing is emitted. Called wherever the processor
    // decides every voice is silenced anyway (a state swap, a self-play routing
    // change): a stack that outlived the notes would "return" to a key nothing is
    // sounding for.
    void reset() noexcept { n = 0; }

    // Audio thread, once per block, AFTER the note gate and BEFORE the synth.
    // `on` is the live parameter; when it is false this only keeps the stack in step.
    void process (juce::MidiBuffer& io, int numSamples, bool on)
    {
        out.clear();
        const int last = juce::jmax (0, numSamples - 1);

        for (const auto meta : io)
        {
            const auto msg = meta.getMessage();
            const int  pos = juce::jlimit (0, last, (int) meta.samplePosition);

            if (msg.isNoteOn())
            {
                push (msg.getChannel(), msg.getNoteNumber(), msg.getVelocity());
                out.addEvent (msg, pos);
            }
            else if (msg.isNoteOff())
            {
                // Was this the key the single voice is actually sounding? Only the top
                // of the stack is; releasing an inner key changes nothing at all.
                const bool wasTop = n > 0
                                 && held[(size_t) (n - 1)].note == msg.getNoteNumber()
                                 && held[(size_t) (n - 1)].ch   == msg.getChannel();
                removeKey (msg.getChannel(), msg.getNoteNumber());
                if (on && wasTop && n > 0)
                {
                    const auto& h = held[(size_t) (n - 1)];
                    // Velocity 0 IS a note-off in MIDI, so a key captured at velocity 0
                    // (some controllers, some sequencers) must not be returned to as one.
                    out.addEvent (juce::MidiMessage::noteOn (h.ch, h.note,
                                                             (juce::uint8) juce::jmax<int> (1, h.vel)), pos);
                }
                out.addEvent (msg, pos);
            }
            else
            {
                if (msg.isAllNotesOff() || msg.isAllSoundOff()) n = 0;
                out.addEvent (msg, pos);
            }
        }

        io.swapWith (out);
    }

    // Tests only: how many keys the stack believes are down, and the winner.
    int  heldCountForTests()   const noexcept { return n; }
    int  winnerNoteForTests()  const noexcept { return n > 0 ? held[(size_t) (n - 1)].note : -1; }

private:
    struct Key { int note = 0, ch = 1; int vel = 100; };

    void push (int ch, int note, int vel) noexcept
    {
        // A retrigger of a key already down MOVES it to the top rather than adding a
        // second entry: two entries share one note-off, and the survivor would then be
        // returned to by a key that is no longer held.
        removeKey (ch, note);
        if (n >= kMaxHeld)
        {
            // Past a full hand of keys the OLDEST is forgotten, never the newest: the
            // newest is the one that has to sound. A key forgotten this way simply gets
            // no legato return when it is released - it was never going to be reached.
            for (int i = 1; i < n; ++i) held[(size_t) (i - 1)] = held[(size_t) i];
            --n;
        }
        held[(size_t) n++] = { note, ch, vel };
    }

    void removeKey (int ch, int note) noexcept
    {
        for (int i = 0; i < n; ++i)
            if (held[(size_t) i].note == note && held[(size_t) i].ch == ch)
            {
                for (int j = i + 1; j < n; ++j) held[(size_t) (j - 1)] = held[(size_t) j];
                --n;
                return;
            }
    }

    juce::MidiBuffer out;              // reused every block (pre-sized in prepare)
    Key held[kMaxHeld] = {};
    int n = 0;                         // press order: held[n-1] is the winner

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonoGlide)
};
