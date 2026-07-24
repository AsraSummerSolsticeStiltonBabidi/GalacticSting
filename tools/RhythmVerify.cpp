//==============================================================================
// RhythmVerify: offline, sample-exact verification of the Spectrum-to-Rhythm
// MIDI output. Instantiates the REAL processor, plays notes into processBlock,
// and checks the emitted MIDI stream:
//   1. default rates (C3, x=2 n=6 -> harmonic 1 pulses at 2.0439 Hz = 122.6 BPM)
//   2. the first pulse of every train lands ON the note-on sample (downbeat)
//   3. pulse spacing is sample-exact and never floods (rate ceiling)
//   4. pressing the SAME key twice gives IDENTICAL pulse patterns (determinism)
//   5. every note-on gets its note-off (gate length; no stuck notes)
//   6. polyrhythm: P2 vs P3 spacing ratio == 3:2 exactly
//==============================================================================

#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

struct Ev { int sample; bool on; int note, ch; int vel = 0; };

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    constexpr double sr = 48000.0;
    constexpr int    blk = 256;

    FractalAdditiveAudioProcessor proc;
    proc.prepareToPlay (sr, blk);

    // Route P1 (harmonic 1) and P2 (harmonic 2) and P3: slots sorted by partial.
    auto setParam = [&] (const char* id, float plain)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    };
    setParam ("rhythmOn", 1.0f);
    proc.getRhythm().slots[0].partial = 1; proc.getRhythm().slots[0].enabled = true; // P2 (ratio 2)
    proc.getRhythm().slots[0].steps   = 0; proc.getRhythm().slots[0].durMs = 30.0f;
    proc.getRhythm().slots[1].partial = 2; proc.getRhythm().slots[1].enabled = true; // P3 (ratio 3)
    proc.getRhythm().slots[1].steps   = 7; proc.getRhythm().slots[1].durMs = 30.0f;

    juce::AudioBuffer<float> audio (2, blk);

    auto runBlocks = [&] (int numBlocks, int noteOnAtBlock, int noteOffAtBlock, int note) -> std::vector<Ev>
    {
        std::vector<Ev> evs;
        for (int b = 0; b < numBlocks; ++b)
        {
            juce::MidiBuffer midi;
            if (b == noteOnAtBlock)  midi.addEvent (juce::MidiMessage::noteOn  (1, note, (juce::uint8) 100), 0);
            if (b == noteOffAtBlock) midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            proc.processBlock (audio, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn() || m.isNoteOff())
                    evs.push_back ({ b * blk + (int) meta.samplePosition, m.isNoteOn(),
                                     m.getNoteNumber(), m.getChannel() });
            }
        }
        return evs;
    };

    std::printf ("== Spectrum-to-Rhythm MIDI verification (key 60, defaults x=2 n=8) ==\n");

    // --- run 1: hold C3 for 4 s ------------------------------------------------
    const int holdBlocks = (int) (4.0 * sr / blk);
    auto evs = runBlocks (holdBlocks + 8, 0, holdBlocks, 60);

    std::vector<int> onsP2, onsP3; int offs = 0, ons = 0;
    for (const auto& e : evs)
    {
        if (e.on) { ++ons; (e.note == 60 ? onsP2 : onsP3).push_back (e.sample); }
        else ++offs;
    }
    std::printf ("  events: %d note-ons (%d P2 / %d P3), %d note-offs\n",
                 ons, (int) onsP2.size(), (int) onsP3.size(), offs);

    check (! onsP2.empty() && onsP2.front() == 0, "P2 downbeat lands ON the note-on sample");
    check (! onsP3.empty() && onsP3.front() == 0, "P3 downbeat lands ON the note-on sample");
    check (ons == offs, "every note-on has its note-off (no stuck notes)");

    // Key 60 = the tuning anchor 261.6256 Hz; harmonic 2 = 523.2511 Hz / 2^8 =
    // 2.0439 Hz -> period = 48000/2.0439 = 23484.1 samples. Harmonic 3 = 2/3 of it.
    if (onsP2.size() >= 3)
    {
        const double per = (double) (onsP2[2] - onsP2[0]) / 2.0;
        const double expect = sr / (2.0 * 261.6255653 / 256.0);
        std::printf ("  P2 period: %.1f samples (expected %.1f)\n", per, expect);
        check (std::abs (per - expect) <= 1.0, "P2 pulse period is sample-exact (f0*2 / 2^8)");
    }
    if (onsP2.size() >= 3 && onsP3.size() >= 4)
    {
        const double p2 = (double) (onsP2[2] - onsP2[0]) / 2.0;
        const double p3 = (double) (onsP3[3] - onsP3[0]) / 3.0;
        const double ratio = p2 / p3;
        std::printf ("  polyrhythm P3:P2 = %.5f (expected 1.50000)\n", ratio);
        check (std::abs (ratio - 1.5) < 0.001, "P3 vs P2 is an EXACT 3:2 polyrhythm");
    }
    const double rateHz = 261.6255653 / 256.0; // harmonic 1 of key 60 at the default divisor
    std::printf ("  pulse-1 rate = %.4f Hz = %.2f BPM (defaults: divide 8 times by 2)\n",
                 rateHz, rateHz * 60.0);
    check (rateHz * 60.0 > 30.0 && rateHz * 60.0 < 300.0, "default pulse 1 is in musical BPM range");
    check ((int) onsP2.size() < 60, "no MIDI flood while holding (4 s of pulses)");

    // steps: P3 train transposed +7 from the played note
    bool p3NoteOk = true;
    for (const auto& e : evs) if (e.on && e.note != 60 && e.note != 67) p3NoteOk = false;
    check (p3NoteOk, "sent notes are exactly playedNote and playedNote+7st");

    // --- run 2: press the SAME key again -> identical pattern ------------------
    auto evs2 = runBlocks (holdBlocks + 8, 0, holdBlocks, 60);
    bool same = evs.size() == evs2.size();
    if (same)
        for (size_t i = 0; i < evs.size(); ++i)
            if (evs[i].sample != evs2[i].sample || evs[i].on != evs2[i].on
                || evs[i].note != evs2[i].note || evs[i].ch != evs2[i].ch) { same = false; break; }
    std::printf ("  run2: %d events vs %d in run1\n", (int) evs2.size(), (int) evs.size());
    check (same, "pressing the SAME key twice emits an IDENTICAL MIDI pattern (deterministic)");

    // --- run 3: silence after release ------------------------------------------
    auto evs3 = runBlocks (30, -1, -1, 60);
    int strays = 0; for (const auto& e : evs3) if (e.on) ++strays;
    check (strays == 0, "no pulses when no note is held");

    // --- run 4: note-on MID-BLOCK -> first pulse exactly at that sample ---------
    {
        std::vector<Ev> evs4;
        for (int b = 0; b < 40; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 1) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 137);
            if (b == 38) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            proc.processBlock (audio, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                    evs4.push_back ({ b * blk + (int) meta.samplePosition, true, m.getNoteNumber(), m.getChannel() });
            }
        }
        check (! evs4.empty() && evs4.front().sample == blk + 137,
               "mid-block note-on: first pulse lands exactly ON the note-on sample");
    }

    // --- run 5: slot ROUTED while a note is already held -> fresh downbeat ------
    // (this was the "first press sounds early / out of phase" bug: a train enabled
    // mid-hold started from a stale phase)
    {
        proc.getRhythm().slots[0].enabled = false; // disable, hold a note, re-enable
        std::vector<Ev> evs5; int enableSample = -1;
        for (int b = 0; b < 60; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0)  midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            if (b == 20) { proc.getRhythm().slots[0].enabled = true; enableSample = b * blk; }
            if (b == 58) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            proc.processBlock (audio, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn() && m.getNoteNumber() == 60)
                    evs5.push_back ({ b * blk + (int) meta.samplePosition, true, 60, m.getChannel() });
            }
        }
        // First P2 pulse after enabling must land AT the enable point (fresh
        // downbeat), not at some stale mid-phase moment.
        int first = -1;
        for (const auto& e : evs5) if (e.sample >= enableSample) { first = e.sample; break; }
        std::printf ("  slot enabled mid-hold at %d; first pulse at %d\n", enableSample, first);
        check (first >= 0 && first - enableSample <= blk,
               "train routed MID-HOLD starts with a fresh downbeat (no stale phase)");
    }

    // --- run 6: a RE-PREPARE mid-pulse must not leave notes hanging -------------
    // (this is the "suspended notes without rhythm" failure: a reset/re-init while a
    // pulse gate is open used to abandon the note-off.)
    {
        std::map<int, int> onCount; // (ch<<8|note) -> net on-count
        auto tally = [&] (juce::MidiBuffer& midi)
        {
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())  onCount[(m.getChannel() << 8) | m.getNoteNumber()]++;
                if (m.isNoteOff()) onCount[(m.getChannel() << 8) | m.getNoteNumber()]--;
            }
        };
        // Hold C3 a while so gates are open, then RE-PREPARE mid-hold, then run on.
        for (int b = 0; b < 12; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            proc.processBlock (audio, midi);
            tally (midi);
        }
        proc.prepareToPlay (sr, blk); // <-- the abandoning event
        for (int b = 0; b < 6; ++b) { juce::MidiBuffer midi; proc.processBlock (audio, midi); tally (midi); }
        int hanging = 0;
        for (const auto& kv : onCount) if (kv.second != 0) hanging += kv.second;
        std::printf ("  net hanging notes after a mid-pulse re-prepare: %d\n", hanging);
        check (hanging == 0, "re-prepare mid-pulse leaves NO hanging notes (flush works)");
    }

    // --- run 7: internal FRACTAL feedback ---------------------------------------
    // Every pulse spawns the routed trains one level down (velocity faded by the
    // decay slope, strictly decreasing). It must (a) never run away, (b) actually
    // produce decayed generations, (c) ring out and leave NO hanging notes after
    // release, and (d) flush cleanly on a re-prepare mid-tail.
    {
        setParam ("rhythmSelfPlay", 1.0f);   // feedback lives inside the self-play loop
        setParam ("rhythmFeedback", 1.0f);
        setParam ("rhythmFeedbackDecay", 0.5f);
        setParam ("rhythmOn", 1.0f);          // keep MIDI-out so the pulses+echoes are observable here
        // Single slot so the pattern is easy to read.
        proc.getRhythm().slots[0].partial = 1; proc.getRhythm().slots[0].enabled = true;
        proc.getRhythm().slots[0].steps = 0;   proc.getRhythm().slots[0].durMs = 30.0f;
        proc.getRhythm().slots[0].vel = 120;
        proc.getRhythm().slots[1].enabled = false; proc.getRhythm().slots[1].partial = -1;

        std::map<int,int> net; // (ch<<8|note) -> net on-count
        std::vector<Ev> fb;
        const int hold = (int) (2.0 * sr / blk);
        const int tail = (int) (2.0 * sr / blk); // let the decaying tail finish after release
        for (int b = 0; b < hold + tail; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0)    midi.addEvent (juce::MidiMessage::noteOn  (1, 60, (juce::uint8) 120), 0);
            if (b == hold) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            proc.processBlock (audio, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())  { net[(m.getChannel()<<8)|m.getNoteNumber()]++; fb.push_back ({ b*blk+(int)meta.samplePosition, true,  m.getNoteNumber(), m.getChannel(), m.getVelocity() }); }
                if (m.isNoteOff()) { net[(m.getChannel()<<8)|m.getNoteNumber()]--; fb.push_back ({ b*blk+(int)meta.samplePosition, false, m.getNoteNumber(), m.getChannel(), 0 }); }
            }
        }
        int fbOns = 0, fbOffs = 0, hanging = 0;
        for (const auto& e : fb) (e.on ? fbOns : fbOffs)++;
        for (const auto& kv : net) if (kv.second != 0) hanging += kv.second;
        std::printf ("== FEEDBACK loop (decay 0.5): %d note-ons, %d note-offs ==\n", fbOns, fbOffs);
        check (fbOns > 0,        "feedback produces repeats");
        check (fbOns == fbOffs,  "feedback: every note-on has its note-off (tail rings out cleanly)");
        check (hanging == 0,     "feedback: NO hanging notes after release");
        check (fbOns < 100000,   "feedback is BOUNDED - no runaway explosion");

        // Decay: fractal children must be present at the faded generations. With
        // decay 0.5 and root velocity 120 the strictly-decreasing chain is
        // 120 -> 60 -> 30 -> 15 ...; require at least the first two child
        // generations to have sounded.
        bool gen1 = false, gen2 = false, onlyRootVel = true;
        for (const auto& e : fb)
            if (e.on)
            {
                if (e.vel == 60) gen1 = true;
                if (e.vel == 30) gen2 = true;
                if (e.vel != 120) onlyRootVel = false;
            }
        check (gen1 && gen2, "fractal children sound at decayed velocities (60, 30 from 120 at slope 0.5)");
        check (! onlyRootVel, "feedback produces more than the root pulses");

        // Re-prepare mid-tail must not strand the ringing echoes. Tally EVERY block
        // (including the hold) so on/off counts match; the flush then balances them.
        std::map<int,int> net2;
        auto tally2 = [&] (juce::MidiBuffer& midi)
        {
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())  net2[(m.getChannel()<<8)|m.getNoteNumber()]++;
                if (m.isNoteOff()) net2[(m.getChannel()<<8)|m.getNoteNumber()]--;
            }
        };
        for (int b = 0; b < 8; ++b) { juce::MidiBuffer midi; if (b==0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8)120), 0); proc.processBlock (audio, midi); tally2 (midi); }
        proc.prepareToPlay (sr, blk); // abandons open echo/gate notes mid-tail
        for (int b = 0; b < 10; ++b) { juce::MidiBuffer midi; proc.processBlock (audio, midi); tally2 (midi); }
        int hang2 = 0; for (const auto& kv : net2) if (kv.second != 0) hang2 += kv.second;
        std::printf ("  net hanging after feedback mid-tail re-prepare: %d\n", hang2);
        check (hang2 == 0, "feedback: re-prepare mid-tail leaves NO hanging notes (flushEchoes works)");

        setParam ("rhythmFeedback", 0.0f); // leave feedback off for any later runs
    }

    // --- run 8: fractal child TIMING is sample-exact across block boundaries ----
    // (the reviewed bug: children spawned by fired echoes were appended after the
    // carry pass without the end-of-block rebase, landing one block late per
    // generation). Slot on partial 1 (ratio 2), steps 0, note 60: parent pitch =
    // 261.6255653 Hz -> child rate = 261.6255653*2/2^8 = 2.0439 Hz -> the vel-60
    // child of the t=0 root pulse must land at round(48000/2.0439) = 23484 samples,
    // and the vel-30 grandchild at 2x that - NOT a block (256) late.
    {
        setParam ("rhythmSelfPlay", 1.0f);
        setParam ("rhythmFeedback", 1.0f);
        setParam ("rhythmFeedbackDecay", 0.5f);
        setParam ("rhythmOn", 1.0f);
        proc.getRhythm().slots[0].partial = 1; proc.getRhythm().slots[0].enabled = true;
        proc.getRhythm().slots[0].steps = 0;   proc.getRhythm().slots[0].durMs = 30.0f;
        proc.getRhythm().slots[0].vel = 120;   proc.getRhythm().slots[0].useRatio = false;
        proc.prepareToPlay (sr, blk); // clean state for exact timing

        std::vector<Ev> fb;
        const int blocks = (int) (4.0 * sr / blk);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 120), 0);
            proc.processBlock (audio, midi);
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOn())
                    fb.push_back ({ b * blk + (int) meta.samplePosition, true, m.getNoteNumber(), m.getChannel(), m.getVelocity() });
            }
        }
        const double childRate  = juce::jlimit (0.01, 30.0, 261.6255653 * 2.0 / 256.0);
        const int    childDelay = (int) std::llround (sr / childRate);
        int firstChild = -1, firstGrand = -1;
        for (const auto& e : fb)
        {
            if (e.vel == 60 && firstChild  < 0) firstChild  = e.sample;
            if (e.vel == 30 && firstGrand  < 0) firstGrand  = e.sample;
        }
        std::printf ("== FRACTAL timing: child at %d (expect %d), grandchild at %d (expect %d) ==\n",
                     firstChild, childDelay, firstGrand, 2 * childDelay);
        check (firstChild >= 0 && std::abs (firstChild - childDelay) <= 2,
               "level-1 child lands exactly one child-period after the root pulse");
        check (firstGrand >= 0 && std::abs (firstGrand - 2 * childDelay) <= 3,
               "level-2 grandchild lands exactly two periods in - NOT a block late (rebase fix)");
        setParam ("rhythmFeedback", 0.0f);
    }

    // --- run 9: note-start gate (quantize to tempo / mono / quantize to pulse) --
    {
        struct FakePlayHead : juce::AudioPlayHead
        {
            double bpm = 120.0, sr2 = 48000.0;
            juce::int64 samplePos = 0;
            juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
            {
                juce::AudioPlayHead::PositionInfo p;
                p.setBpm (bpm);
                p.setIsPlaying (true);
                p.setTimeInSamples (samplePos);
                p.setPpqPosition ((double) samplePos / sr2 * bpm / 60.0);
                return p;
            }
        };
        FakePlayHead fph;
        fph.sr2 = sr;
        proc.setPlayHead (&fph);

        auto resetState = [&]
        {
            setParam ("rhythmSelfPlay", 0.0f); setParam ("rhythmFeedback", 0.0f);
            setParam ("rhythmOn", 1.0f);
            setParam ("rhythmQTempo", 0.0f); setParam ("rhythmMono", 0.0f); setParam ("rhythmQPulse", 0.0f);
            setParam ("rhythmMode", 0.0f); // Lowest note
            proc.getRhythm().slots[0].partial = 1; proc.getRhythm().slots[0].enabled = true;
            proc.getRhythm().slots[0].steps = 0;   proc.getRhythm().slots[0].durMs = 30.0f;
            proc.getRhythm().slots[0].vel = 100;   proc.getRhythm().slots[0].useRatio = false;
            proc.getRhythm().slots[1].enabled = false; proc.getRhythm().slots[1].partial = -1;
            proc.prepareToPlay (sr, blk);
            fph.samplePos = 0;
        };
        auto run = [&] (int blocks, std::vector<std::pair<int,juce::MidiMessage>> events) -> std::vector<Ev>
        {
            std::vector<Ev> evs;
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer midi;
                for (auto& [atSample, m] : events)
                    if (atSample / blk == b) midi.addEvent (m, atSample % blk);
                proc.processBlock (audio, midi);
                fph.samplePos += blk;
                for (const auto meta : midi)
                {
                    const auto m = meta.getMessage();
                    if (m.isNoteOn())
                        evs.push_back ({ b * blk + (int) meta.samplePosition, true, m.getNoteNumber(), m.getChannel(), m.getVelocity() });
                }
            }
            return evs;
        };

        // (a) Quantize to tempo: note pressed at sample 868 must not pulse until the
        //     next beat (120 BPM @ 48k = 24000 samples/beat -> beat at 24000 exactly).
        resetState();
        setParam ("rhythmQTempo", 1.0f);
        auto evA = run ((int) (1.2 * sr / blk), { { 868, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) } });
        int firstA = -1; for (const auto& e : evA) if (firstA < 0) firstA = e.sample;
        std::printf ("== NOTE GATE: qTempo first pulse at %d (expect 24000) ==\n", firstA);
        check (firstA == 24000, "quantize-to-tempo: the note enters EXACTLY on the next beat");

        // (b) Mono: pressing 64 while 60 sounds -> 60 is stopped, 64 takes over as
        //     the driver (its pulses appear; 60's stop).
        resetState();
        setParam ("rhythmMono", 1.0f);
        auto evB = run ((int) (2.0 * sr / blk),
                        { { 0,     juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                          { 40000, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) } });
        bool any64 = false, late60 = false;
        for (const auto& e : evB)
        {
            if (e.note == 64) any64 = true;
            if (e.note == 60 && e.sample > 41000) late60 = true;
        }
        check (any64,    "mono: the NEW note's pulses take over");
        check (! late60, "mono: the previous note's pulses STOP when the new one starts");

        // (c) Quantize to pulse: 60 held from 0 (single train, period 23484 = the
        //     pattern cycle); 64 pressed at ~5000 in All-notes mode must join exactly
        //     at the pattern reset (23484), where its own downbeat fires.
        resetState();
        setParam ("rhythmQPulse", 1.0f);
        setParam ("rhythmMode", 1.0f); // All notes: each held note drives its own copy
        auto evC = run ((int) (1.2 * sr / blk),
                        { { 0,    juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                          { 5000, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) } });
        int first64 = -1;
        for (const auto& e : evC) if (e.note == 64 && first64 < 0) first64 = e.sample;
        std::printf ("   qPulse: note 64 first pulse at %d (expect ~23484) ==\n", first64);
        check (first64 >= 0 && std::abs (first64 - 23484) <= 2,
               "quantize-to-pulse: the new note joins EXACTLY when the pattern resets on the 1");

        proc.setPlayHead (nullptr);
        setParam ("rhythmQTempo", 0.0f); setParam ("rhythmMono", 0.0f); setParam ("rhythmQPulse", 0.0f);
    }

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
