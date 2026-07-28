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

#include <juce_dsp/juce_dsp.h>

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
    // Every set is followed by a parameter POLL, because that is what a real host
    // does: the spectrum rebuild, the tuning mode, Reference A, Octave Transpose and
    // the bend range all reach the DSP from the processor's message timer, and an
    // offline harness has no message loop to run it. Without this the tests here
    // would quietly exercise a plugin still on its startup values - which is exactly
    // how a ratio-pulse test passed both with and without the bug it was written for.
    auto setParam = [&] (const char* id, float plain)
    {
        auto* p = proc.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
        proc.pollParametersForTests();
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

        // (c) Quantize to pulse, NEAR-simultaneous press: 64 at ~5000 (within the
        //     ~120ms window) enters IMMEDIATELY but PHASE-ALIGNED: its train behaves
        //     as if it had started AT the shared anchor (t=0), so its first pulse
        //     lands on ITS OWN first grid crossing = one 64-period from the anchor.
        resetState();
        setParam ("rhythmQPulse", 1.0f);
        setParam ("rhythmMode", 1.0f); // All notes: each held note drives its own copy
        const double f64  = 261.6255653 * std::pow (2.0, 4.0 / 12.0);
        const int    per64 = (int) std::llround (sr / juce::jlimit (0.01, 30.0, f64 * 2.0 / 256.0));
        auto evC = run ((int) (1.2 * sr / blk),
                        { { 0,    juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                          { 5000, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) } });
        int first64 = -1;
        for (const auto& e : evC) if (e.note == 64 && first64 < 0) first64 = e.sample;
        std::printf ("   qPulse near-press: note 64 first pulse at %d (expect ~%d)\n", first64, per64);
        check (first64 >= 0 && std::abs (first64 - per64) <= 2,
               "quantize-to-pulse: a near-simultaneous joiner enters IN PHASE on the shared grid");

        // (d) Quantize to pulse, LATE press: 64 at 40000 (past the window) waits for
        //     the pattern reset at 2 x 23484 = 46968 and enters exactly there.
        resetState();
        setParam ("rhythmQPulse", 1.0f);
        setParam ("rhythmMode", 1.0f);
        auto evD = run ((int) (1.5 * sr / blk),
                        { { 0,     juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                          { 40000, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) } });
        int firstD = -1;
        for (const auto& e : evD) if (e.note == 64 && firstD < 0) firstD = e.sample;
        std::printf ("   qPulse late press: note 64 first pulse at %d (expect ~46968)\n", firstD);
        check (firstD >= 0 && std::abs (firstD - 46968) <= 2,
               "quantize-to-pulse: a late joiner waits for the pattern reset on the 1");

        // (e) Mouse-style sequence (off 60 then on 64 in the same block) with qPulse
        //     on: nothing is left sounding, so 64 must start PROMPTLY (this was the
        //     everything-goes-silent bug: the stale reset point swallowed the click).
        resetState();
        setParam ("rhythmQPulse", 1.0f);
        setParam ("rhythmMode", 1.0f);
        auto evE = run ((int) (1.0 * sr / blk),
                        { { 0,     juce::MidiMessage::noteOn  (1, 60, (juce::uint8) 100) },
                          { 30000, juce::MidiMessage::noteOff (1, 60) },
                          { 30010, juce::MidiMessage::noteOn  (1, 64, (juce::uint8) 100) } });
        int firstE = -1;
        for (const auto& e : evE) if (e.note == 64 && firstE < 0) firstE = e.sample;
        std::printf ("   qPulse click-to-click: note 64 first pulse at %d (expect ~30010)\n", firstE);
        check (firstE >= 0 && firstE - 30010 <= blk,
               "quantize-to-pulse: with nothing sounding a new click starts promptly (no swallowed notes)");

        // (f) Rapid RETRIGGER of the same key with qPulse on must leave nothing
        //     hanging (duplicate-pending stuck-note bug).
        resetState();
        setParam ("rhythmQPulse", 1.0f);
        std::map<int,int> netR;
        {
            std::vector<std::pair<int,juce::MidiMessage>> evs = {
                { 0,     juce::MidiMessage::noteOn  (1, 60, (juce::uint8) 100) },
                { 30000, juce::MidiMessage::noteOn  (1, 64, (juce::uint8) 100) }, // deferred
                { 30500, juce::MidiMessage::noteOn  (1, 64, (juce::uint8) 100) }, // retrigger replaces pending
                { 31000, juce::MidiMessage::noteOff (1, 64) },                    // cancels it entirely
                { 60000, juce::MidiMessage::noteOff (1, 60) } };
            for (int b = 0; b < (int) (2.0 * sr / blk); ++b)
            {
                juce::MidiBuffer midi;
                for (auto& [atSample, m] : evs) if (atSample / blk == b) midi.addEvent (m, atSample % blk);
                proc.processBlock (audio, midi);
                fph.samplePos += blk;
                for (const auto meta : midi)
                {
                    const auto m = meta.getMessage();
                    if (m.isNoteOn())  netR[(m.getChannel()<<8)|m.getNoteNumber()]++;
                    if (m.isNoteOff()) netR[(m.getChannel()<<8)|m.getNoteNumber()]--;
                }
            }
        }
        int hangR = 0; for (const auto& kv : netR) if (kv.second > 0) hangR += kv.second;
        check (hangR == 0, "qPulse retrigger + early release leaves NOTHING hanging (eternal-pulse fix)");

        proc.setPlayHead (nullptr);
        setParam ("rhythmQTempo", 0.0f); setParam ("rhythmMono", 0.0f); setParam ("rhythmQPulse", 0.0f);
    }

    // --- run 10: MODULATORS - every note must move the mapped parameter ----------
    // The reported bug was "an envelope on Base fires on some keys and not others".
    // Play the SAME note ten times in a row and require the mapped parameter to be
    // displaced during every single one of them, and to return to its base after
    // each release - including a note that starts AND ends inside one block.
    {
        proc.prepareToPlay (sr, blk);
        setParam ("rhythmOn", 0.0f); setParam ("rhythmSelfPlay", 0.0f);
        setParam ("modE1A", 0.001f); setParam ("modE1D", 0.5f);
        setParam ("modE1S", 1.0f);   setParam ("modE1R", 0.001f);
        setParam ("modE1Range", 0.5f); setParam ("modE1On", 1.0f);
        setParam ("curveSlope", 1.0f);           // base value; the env rides on top
        proc.getModEngine().addTarget (0, "curveSlope");

        auto* raw  = proc.apvts.getRawParameterValue ("curveSlope");
        const float base = 1.0f;
        int firedOn = 0, restored = 0;
        for (int rep = 0; rep < 10; ++rep)
        {
            float peak = base;
            for (int b = 0; b < 8; ++b)   // note held for 8 blocks (~43 ms)
            {
                juce::MidiBuffer midi;
                if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                proc.processBlock (audio, midi);
                peak = std::max (peak, raw->load());
            }
            if (peak > base + 0.05f) ++firedOn;
            for (int b = 0; b < 12; ++b)  // release + settle
            {
                juce::MidiBuffer midi;
                if (b == 0) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                proc.processBlock (audio, midi);
            }
            if (std::abs (raw->load() - base) < 0.02f) ++restored;
        }
        std::printf ("== MODULATORS: env fired on %d/10 notes, returned to base %d/10 times ==\n",
                     firedOn, restored);
        check (firedOn == 10,  "a mapped envelope moves its target on EVERY note (no missed keys)");
        check (restored == 10, "the target returns to the knob's own value after every release");

        // A note that starts and ends inside ONE block must still release.
        {
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100), 0);
            midi.addEvent (juce::MidiMessage::noteOff (1, 62), blk / 2);
            proc.processBlock (audio, midi);
        }
        for (int b = 0; b < 20; ++b) { juce::MidiBuffer midi; proc.processBlock (audio, midi); }
        check (std::abs (raw->load() - base) < 0.02f,
               "a note that starts AND ends inside one block still releases (no stuck sustain)");

        // Setting a knob WHILE it is being modulated must leave the raw value (what
        // JUCE copies into its tree, and therefore into undo/redo and the saved
        // project) at exactly the value that was set - not at base+modulation.
        {
            juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            proc.processBlock (audio, midi);
            for (int b = 0; b < 6; ++b) { juce::MidiBuffer m2; proc.processBlock (audio, m2); }
        }
        check (raw->load() > base + 0.05f, "precondition: the envelope is displacing the value");
        // Exactly what a UI control does: a complete gesture around the change.
        {
            auto* p = proc.apvts.getParameter ("curveSlope");
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (2.0f));
            p->endChangeGesture();
        }
        { juce::MidiBuffer midi; proc.processBlock (audio, midi); }
        check (std::abs (raw->load() - 2.0f) < 0.02f,
               "a knob set while modulated reads back as the value you set (clean undo/save)");
        for (int b = 0; b < (int) (0.5 * sr / blk); ++b) { juce::MidiBuffer midi; proc.processBlock (audio, midi); }
        check (raw->load() > 2.0f + 0.05f, "modulation resumes by itself after the hold window");
        // HOST AUTOMATION (no gesture) must NOT pause the modulation.
        {
            auto* p = proc.apvts.getParameter ("curveSlope");
            p->setValueNotifyingHost (p->convertTo0to1 (1.5f));
            juce::MidiBuffer midi; proc.processBlock (audio, midi);
            check (raw->load() > 1.5f + 0.05f, "automation (no gesture) keeps modulating - not throttled");
        }
        { juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0); proc.processBlock (audio, midi); }
        setParam ("curveSlope", base);

        // Unmapping while modulated must hand the parameter straight back.
        {
            juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            proc.processBlock (audio, midi);
        }
        proc.getModEngine().unmapAll (0);
        for (int b = 0; b < 3; ++b) { juce::MidiBuffer midi; proc.processBlock (audio, midi); }
        check (std::abs (raw->load() - base) < 0.02f, "unmapping restores the base value (never stuck)");
        { juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0); proc.processBlock (audio, midi); }
    }

    // --- run 11: degree ratios are exact for ANY scale size -----------------------
    // The 128-key MIDI table only holds 128 degree CLASSES, so a scale with more
    // degrees than that cannot be read back through it. Every degree must still
    // report period^(d/N) for an N-EDO, including well past 128 degrees.
    {
        auto edoScl = [] (int n)
        {
            juce::String s;                       // ! file, description, count, !, intervals
            s << "! test.scl\n" << juce::String (n) << "-EDO test\n " << juce::String (n) << "\n!\n";
            for (int i = 1; i <= n; ++i) s << " " << juce::String (1200.0 * (double) i / (double) n, 6) << "\n";
            return s;
        };
        for (int n : { 12, 31, 72, 128, 129, 200, 311 })
        {
            juce::String err;
            const bool ok = proc.loadScalaText (edoScl (n), "edo" + juce::String (n), err);
            double worst = 0.0;
            int worstD = -1;
            if (ok)
                for (int d = 0; d <= n; ++d)
                {
                    const double want = std::pow (2.0, (double) d / (double) n);
                    const double got  = proc.getDegreeRatio (d);
                    const double errC = std::abs (1200.0 * std::log2 (got / want));
                    if (errC > worst) { worst = errC; worstD = d; }
                }
            std::printf ("== %d-EDO: worst degree error %.4f cents (degree %d) ==\n", n, worst, worstD);
            check (ok, "the synthetic .scl loads");
            check (ok && worst < 0.01, "every degree ratio is exact (no period lost, no duplicates)");
        }
        proc.clearScala();
    }

    // --- run 12: FULL STATE ROUND-TRIP -------------------------------------------
    // Configure everything at once, save through the host API, wipe the plugin
    // state, restore, and require every parameter AND every side structure to come
    // back identical - then save again and require the two blobs to match (that
    // catches state children stacking up on repeated saves).
    {
        auto edoScl = [] (int n)
        {
            juce::String s;
            s << "! rt.scl\n" << juce::String (n) << "-EDO\n " << juce::String (n) << "\n!\n";
            for (int i = 1; i <= n; ++i) s << " " << juce::String (1200.0 * (double) i / (double) n, 6) << "\n";
            return s;
        };
        juce::String err;

        // A state that touches every subsystem.
        setParam ("baseN", 7.0f);          setParam ("curveSlope", 2.345f);
        setParam ("harmonics", 2.5f);      setParam ("fractalLevel", 2.7f);
        setParam ("filterCutoff", 5500.0f); setParam ("masterGain", -6.5f);
        setParam ("modE2A", 0.234f);       setParam ("modL1Div", 5.0f);
        setParam ("timbreTemper", 1.0f);
        proc.getModEngine().unmapAll (0);
        proc.getModEngine().addTarget (1, "filterCutoff");
        proc.getModEngine().addTarget (3, "curveSlope");
        proc.getModEngine().addTarget (3, "masterGain");
        check (proc.loadScalaText (edoScl (19), "edo19", err), "round-trip: the 19-EDO scale loads");
        proc.getRhythm().slots[5].partial = 4; proc.getRhythm().slots[5].enabled = true;
        proc.getRhythm().slots[5].steps = 3;   proc.getRhythm().slots[5].durMs = 77.0f;
        proc.getRhythm().slots[5].vel = 99;
        // Tuning bank + Scala-as-timbre: both carry their own .scl text and must
        // never be silently dropped by a save/load round-trip (project reopen,
        // an update...), same as the active tuning itself.
        proc.bankAutoAdd ("rt-bank-11", edoScl (11));
        proc.bankAutoAdd ("rt-bank-13", edoScl (13));
        check (proc.loadTimbreText ("1/1 5/4 3/2 2/1", err), "round-trip: a typed timbre loads");
        float wave[ModEngine::kWaveN];
        for (int i = 0; i < ModEngine::kWaveN; ++i) wave[i] = 0.5f + 0.5f * std::sin (0.05f * (float) i);
        proc.getModEngine().setCustomWave (0, wave);

        std::map<juce::String, float> before;
        for (auto* ap : proc.getParameters())
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (ap))
                before[p->paramID] = p->getValue();

        juce::MemoryBlock saved;
        proc.getStateInformation (saved);

        // Wipe: defaults, no routings, no scale, no rhythm slot, flat wave.
        for (auto* ap : proc.getParameters())
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (ap))
                p->setValueNotifyingHost (p->getDefaultValue());
        for (int m = 0; m < ModEngine::kMods; ++m) proc.getModEngine().unmapAll (m);
        proc.clearScala();
        proc.getRhythm().slots[5].partial = -1; proc.getRhythm().slots[5].enabled = false;
        float flat[ModEngine::kWaveN];
        for (auto& f : flat) f = 0.0f;
        proc.getModEngine().setCustomWave (0, flat);
        for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i) proc.bankClearSlot (i);
        proc.clearTimbre();

        proc.setStateInformation (saved.getData(), (int) saved.getSize());

        int paramMismatch = 0;
        juce::String firstBad;
        for (auto* ap : proc.getParameters())
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (ap))
                if (std::abs (p->getValue() - before[p->paramID]) > 1.0e-5f)
                { if (firstBad.isEmpty()) firstBad = p->paramID; ++paramMismatch; }
        std::printf ("== ROUND-TRIP: %d parameter mismatches%s ==\n", paramMismatch,
                     firstBad.isEmpty() ? "" : (" (first: " + firstBad + ")").toRawUTF8());
        check (paramMismatch == 0, "every parameter survives a save/load round-trip");

        check (proc.getModEngine().numTargets (1) == 1
               && proc.getModEngine().targetId (1, 0) == "filterCutoff", "modulator 2's routing survives");
        check (proc.getModEngine().numTargets (3) == 2, "modulator 4 keeps BOTH of its targets");
        check (proc.getScaleDegrees() == 19, "the loaded scale survives");
        check (proc.getRhythm().slots[5].partial.load() == 4
               && proc.getRhythm().slots[5].vel.load() == 99, "the rhythm slot survives");
        bool waveOk = false;
        if (const float* w = proc.getModEngine().getCustomWave (0))
        {
            waveOk = true;
            for (int i = 0; i < ModEngine::kWaveN; ++i)
                if (std::abs (w[i] - wave[i]) > 2.0e-4f) { waveOk = false; break; }
        }
        check (waveOk, "the drawn LFO wave survives");
        {
            int filled = 0; bool namesOk = true;
            for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i)
                if (proc.getTuningBankName (i).isNotEmpty())
                {
                    ++filled;
                    if (proc.getTuningBankName (i) != "rt-bank-11" && proc.getTuningBankName (i) != "rt-bank-13")
                        namesOk = false;
                }
            check (filled == 2 && namesOk, "both tuning-bank .scl entries survive, by name");
        }
        // "1/1 5/4 3/2 2/1": 1/1 is the implicit fundamental (not a partial above
        // it), 2/1 is the period - loadTimbreText keeps 1/1 + 5/4 + 3/2 = 3 degrees.
        check (proc.timbreLoaded() && proc.getTimbreNoteCount() == 3,
               "the Scala-as-timbre .scl survives (its 3 degrees are back)");

        juce::MemoryBlock again;
        proc.getStateInformation (again);
        check (again.getSize() == saved.getSize(),
               "saving twice produces the same state size (no duplicated children)");
    }

    // --- run 12b: the envelope CURVE is what the modulator actually sends ----------
    // The editor draws value = shape(t, curve); this checks the engine agrees, that
    // the sustain node really sets the level, and that a second key retriggers from
    // zero (attack + decay audible again, not stuck at the top).
    {
        proc.prepareToPlay (sr, blk);
        for (int m = 0; m < ModEngine::kMods; ++m) proc.getModEngine().unmapAll (m);
        setParam ("modE1A", 0.001f); setParam ("modE1D", 0.400f);
        setParam ("modE1S", 0.000f); setParam ("modE1R", 0.001f);
        setParam ("modE1Range", 1.0f); setParam ("modE1On", 1.0f);
        setParam ("curveSlope", 0.0f);
        proc.getModEngine().addTarget (0, "curveSlope");
        auto* raw = proc.apvts.getRawParameterValue ("curveSlope");
        const float full = 4.0f; // curveSlope range 0..4 -> Range 1 spans the lot

        // Sample the decay leg half way through, for three curve settings.
        auto midDecay = [&] (float curve)
        {
            setParam ("modE1CurveD", curve);
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
              proc.processBlock (audio, m); }
            const int halfBlocks = (int) (0.200 * sr / blk);           // 0.2 s = half of D
            for (int b = 0; b < halfBlocks; ++b) { juce::MidiBuffer m; proc.processBlock (audio, m); }
            const float v = raw->load() / full;                        // 0..1
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
              proc.processBlock (audio, m); }
            for (int b = 0; b < 20; ++b) { juce::MidiBuffer m; proc.processBlock (audio, m); }
            return v;
        };
        const float lin = midDecay (0.0f), convex = midDecay (0.9f), concave = midDecay (-0.9f);
        std::printf ("== ENVELOPE CURVE: half-way down the decay - concave %.3f  straight %.3f  convex %.3f ==\n",
                     concave, lin, convex);
        check (std::abs (lin - 0.5f) < 0.08f, "a STRAIGHT decay is half way down at half the decay time");
        check (convex < lin - 0.08f, "a CONVEX decay has already fallen further (fast, then a long tail)");
        check (concave > lin + 0.08f, "a CONCAVE decay is still high (slow at first, then drops)");

        // Sustain: the level the editor's S node sets is where the envelope rests.
        setParam ("modE1D", 0.05f); setParam ("modE1S", 0.25f); setParam ("modE1CurveD", 0.0f);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
          proc.processBlock (audio, m); }
        for (int b = 0; b < (int) (0.4 * sr / blk); ++b) { juce::MidiBuffer m; proc.processBlock (audio, m); }
        check (std::abs (raw->load() / full - 0.25f) < 0.03f, "the sustain node sets the level it rests at");

        // A second key while the first sustains must restart from zero.
        setParam ("modE1A", 0.30f);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
          proc.processBlock (audio, m); }
        check (raw->load() / full < 0.10f, "a second key RESTARTS the envelope from zero (attack audible again)");
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
          m.addEvent (juce::MidiMessage::noteOff (1, 64), 1); proc.processBlock (audio, m); }
        for (int b = 0; b < 30; ++b) { juce::MidiBuffer m; proc.processBlock (audio, m); }
        proc.getModEngine().unmapAll (0);
        setParam ("modE1A", 0.1f); setParam ("modE1D", 0.3f); setParam ("modE1S", 0.7f);
        setParam ("modE1R", 0.4f); setParam ("curveSlope", 1.0f);
    }

    // --- run 13: a generated .scl carries its own patch ---------------------------
    {
        setParam ("curveSlope", 3.21f);
        setParam ("baseN", 11.0f);
        const auto scl = proc.currentTuningAsScl ("with-patch");
        check (scl.contains ("FIBLO-PATCH"), "a written .scl carries the patch as comments");
        // A plain Scala parser must still read it: the payload is comment lines only.
        juce::String err;
        check (proc.loadScalaText (scl, "with-patch", err), "the .scl with a patch still parses as a scale");

        setParam ("curveSlope", 0.5f);
        setParam ("baseN", 1.0f);
        const int degreesBefore = proc.getScaleDegrees();
        check (proc.loadSpectrumFromScl (scl), "the embedded patch loads back");
        check (std::abs (proc.apvts.getRawParameterValue ("curveSlope")->load() - 3.21f) < 1e-3f
               && (int) proc.apvts.getRawParameterValue ("baseN")->load() == 11,
               "it restores the SOUND it was saved with");
        check (proc.getScaleDegrees() == degreesBefore, "and leaves the loaded tuning alone");
        proc.clearScala();
    }

    // --- run 14: keyboard scale editing (add / edit / delete a degree) ------------
    {
        std::printf ("keyboard scale editing:\n");
        juce::String err;
        check (proc.loadScalaText ("! 5 equal steps\ntest\n 5\n!\n 240.0\n 480.0\n 720.0\n 960.0\n 2/1\n",
                                   "test", err), "a 5-degree test scale loads");
        check (proc.getScaleDegrees() == 5, "it has 5 degrees per period");

        // Ctrl+click's window types the interval against 0 - a ratio stays a ratio.
        check (proc.addNoteToTuning (1.5, err, "3/2"), "a typed 3/2 is added");
        check (proc.getScaleDegrees() == 6, "the scale grew by one degree");
        {
            long long p = 0, q = 0;
            bool found = false;
            for (int d = 1; d <= 6 && ! found; ++d)
                found = proc.getDegreeIsRational (d, p, q) && p == 3 && q == 2;
            check (found, "and it is stored as the exact ratio 3/2, not as cents");
        }
        check (! proc.addNoteToTuning (1.5, err, "3/2"), "adding the same pitch twice is refused");

        // A spec outside the period folds in, and is then written folded.
        check (proc.addNoteToTuning (2.5, err, "5/2"), "a 5/2 folds into the octave");
        {
            long long p = 0, q = 0; bool found = false;
            for (int d = 1; d <= 7 && ! found; ++d)
                found = proc.getDegreeIsRational (d, p, q) && p == 5 && q == 4;
            check (found, "and lands as 5/4, not as the unfolded 5/2");
        }

        // Delete: the degree leaves the scale, so it is gone from every period.
        const double gone = proc.getDegreeRatio (2);
        check (proc.deleteTuningDegree (2, err), "degree 2 is deleted");
        check (proc.getScaleDegrees() == 6, "the scale shrank by exactly one degree");
        {
            bool still = false;
            for (int d = 1; d <= 6; ++d)
                if (std::abs (proc.getDegreeRatio (d) - gone) < 1.0e-9) still = true;
            check (! still, "that pitch class is gone");
        }
        check (std::abs (proc.getDegreeRatio (proc.getScaleDegrees()) - 2.0) < 1.0e-9,
               "and the period is still the octave (kept across every repetition)");
        check (! proc.deleteTuningDegree (0, err), "the tonic cannot be deleted");
        check (! proc.deleteTuningDegree (proc.getScaleDegrees(), err), "nor can the period");

        // Editing must work from a NON-Scala tuning too - right-clicking a key in
        // MTS-ESP/12ED2 used to be refused silently, which read as "right-click is
        // broken". It renders the current tuning to .scl form and edits that.
        proc.clearScala();
        setParam ("tuningMode", 1.0f); // MTS-ESP (no master -> 12ED2); polls, so it really applies
        check (proc.getScaleDegrees() == 12, "a non-Scala tuning reports its 12 degrees");
        check (proc.editTuningDegree (7, "3/2", err), "a degree can be edited straight from it");
        check (proc.getTuningMode() == 2, "which lands the result in Scala mode");
        {
            long long p = 0, q = 0;
            check (proc.getDegreeIsRational (7, p, q) && p == 3 && q == 2,
                   "and degree 7 is now exactly 3/2");
        }
        check (proc.deleteTuningDegree (7, err), "and it can be deleted from there too");
        check (proc.getScaleDegrees() == 11, "leaving 11 degrees");
        proc.clearScala();
    }

    // --- run 15: a full state swap must never leave a note stuck sounding ---------
    {
        std::printf ("preset/project reload silences any sounding note:\n");
        FractalAdditiveAudioProcessor p2;
        p2.prepareToPlay (sr, blk);
        auto maxAbs = [] (const juce::AudioBuffer<float>& b)
        {
            float m = 0.0f;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    m = std::max (m, std::abs (b.getSample (ch, i)));
            return m;
        };

        juce::AudioBuffer<float> audio (2, blk);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
          p2.processBlock (audio, m); }
        // A few more blocks so the attack settles into a genuinely sounding note.
        float peak = 0.0f;
        for (int b = 0; b < 20; ++b)
        { juce::MidiBuffer m; p2.processBlock (audio, m); peak = std::max (peak, maxAbs (audio)); }
        check (peak > 0.001f, "the note is genuinely sounding before the swap");

        // A full state swap (what a preset load / project reload does), WITHOUT any
        // note-off - simulates the exact bug report: reset while a note sounds.
        juce::MemoryBlock state;
        p2.getStateInformation (state);
        p2.setStateInformation (state.getData(), (int) state.getSize());

        // The kill is armed at the top of the NEXT processBlock (audio-thread only -
        // see restoreState/processBlock): the voice enters its 5 ms fast-kill fade
        // right there, so the first block may still carry its tail, but it must be
        // fully silent well before "infinite" - a couple of blocks, not forever.
        { juce::MidiBuffer m; p2.processBlock (audio, m); }
        check (maxAbs (audio) < peak, "already fading on the very first block after the swap");
        for (int b = 0; b < 3; ++b) { juce::MidiBuffer m; p2.processBlock (audio, m); }
        for (int b = 0; b < 30; ++b)
        { juce::MidiBuffer m; p2.processBlock (audio, m); check (maxAbs (audio) < 1.0e-6f, "stays silent"); }
    }

    // --- run 16: a preset carries its tuning but never applies it -----------------
    {
        std::printf ("preset load leaves the tuning alone, but offers it:\n");
        juce::String err;

        // Scale A is active, then saved into preset "A" (so the file's SCALA child
        // is A's).
        check (proc.loadScalaText ("! A\ntest-a\n 3\n 100.0\n 200.0\n 2/1\n", "test-a", err),
               "scale A loads");
        const auto presetName = proc.savePreset ("__rhythmverify_tmp_a");
        check (presetName.isNotEmpty(), "preset A saves (tuning A travels in the file)");

        // Switch to a DIFFERENT scale B, then load the preset back.
        check (proc.loadScalaText ("! B\ntest-b\n 4\n 150.0\n 300.0\n 450.0\n 2/1\n", "test-b", err),
               "scale B loads");
        const int degreesB = proc.getScaleDegrees();
        check (proc.loadPreset (presetName), "the preset loads");
        check (proc.getScaleDegrees() == degreesB, "loading it did NOT change the tuning - B is still active");
        check (proc.hasPendingPresetTuning(), "but its own (different) tuning is offered");
        check (proc.getPendingPresetTuningName() == "test-a", "and it is named after the one that was saved");

        // The offer disappears once loaded, and B's degree count changes to A's (3).
        check (proc.loadPendingPresetTuning (err), "\"Load preset's tuning\" applies it");
        check (proc.getScaleDegrees() == 3, "tuning A is now active");
        check (! proc.hasPendingPresetTuning(), "the offer is gone now that it matches");

        // A preset saved from a NON-Scala tuning carries no SCALA child - the button
        // must not falsely offer something that was never there.
        proc.clearScala();
        setParam ("tuningMode", 0.0f); // plain 12ED2
        const auto presetName2 = proc.savePreset ("__rhythmverify_tmp_b");
        check (presetName2.isNotEmpty(), "a 12ED2 preset saves");
        check (proc.loadScalaText ("! C\ntest-c\n 2\n 500.0\n 2/1\n", "test-c", err), "scale C loads");
        check (proc.loadPreset (presetName2), "that 12ED2 preset loads");
        check (! proc.hasPendingPresetTuning(), "no tuning is falsely offered - it never had one");

        // Cleanup: remove the temp preset files this test created.
        FractalAdditiveAudioProcessor::getPresetFolder().getChildFile (presetName  + ".gsting").deleteFile();
        FractalAdditiveAudioProcessor::getPresetFolder().getChildFile (presetName2 + ".gsting").deleteFile();
        proc.clearScala();
    }

    // --- run 17: preset drift - a knob counts, a modulator's own motion doesn't ---
    {
        std::printf ("preset drift detection:\n");
        setParam ("masterGain", -3.0f);
        // The modulator that will drive masterGain is configured BEFORE saving, so
        // its own A/D/S/On settings are part of this preset's baseline - only
        // masterGain's RAW atomic should move once it starts modulating, never its
        // (or the envelope's own) base value.
        proc.getModEngine().unmapAll (0);
        proc.getModEngine().addTarget (0, "masterGain");
        setParam ("modE1A", 0.001f); setParam ("modE1D", 0.001f);
        setParam ("modE1S", 1.0f);   setParam ("modE1On", 1.0f);
        const auto presetName = proc.savePreset ("__rhythmverify_tmp_drift");
        check (presetName.isNotEmpty(), "drift-test preset saves");
        check (! proc.presetHasDrifted(), "freshly saved: no drift");

        check (proc.loadPreset (presetName), "it reloads");
        check (! proc.presetHasDrifted(), "freshly loaded: no drift");

        // The preset's OWN modulator driving masterGain must NOT count as drift -
        // it writes only the raw atomic, never the base value this compares against.
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
          proc.processBlock (audio, m); }
        for (int b = 0; b < 10; ++b) { juce::MidiBuffer m; proc.processBlock (audio, m); }
        check (! proc.presetHasDrifted(), "the preset's OWN modulator moving a target is not drift");
        proc.getModEngine().unmapAll (0);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0); proc.processBlock (audio, m); }

        // A plain knob move - what an envelope/LFO NEVER does - IS drift.
        setParam ("masterGain", -9.0f);
        check (proc.presetHasDrifted(), "moving a knob by hand IS drift");
        check (proc.getLoadedPresetName() == presetName, "still tracking the same preset's name");

        // Saving again re-baselines: the fresh file matches itself.
        const auto resaved = proc.savePreset (presetName);
        check (resaved == presetName, "re-saving under the same name works");
        check (! proc.presetHasDrifted(), "and drift clears - the new file matches now");

        setParam ("masterGain", 0.0f);
        FractalAdditiveAudioProcessor::getPresetFolder().getChildFile (presetName + ".gsting").deleteFile();
    }

    // --- run 18: the jingle is ON DEMAND only, never on its own -------------------
    {
        std::printf ("jingle: only ever when the title is clicked:\n");
        constexpr int jblk = 4096;
        auto maxAbsOf = [] (const juce::AudioBuffer<float>& b)
        {
            float m = 0.0f;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    m = std::max (m, std::abs (b.getSample (ch, i)));
            return m;
        };

        // Heap-allocated: this class is too large for a second live instance (on top
        // of `proc`, still alive for the rest of main()) to share the stack safely.
        auto pj = std::make_unique<FractalAdditiveAudioProcessor>();
        pj->prepareToPlay (sr, jblk);
        juce::AudioBuffer<float> jaudio (2, jblk);
        const int blocksFor7s = (int) std::ceil (7.0 * sr / jblk); // the jingle is ~5.4 s

        // A brand-new instance renders SILENCE: no MIDI, no click, no jingle.
        bool silentOnLoad = true;
        for (int b = 0; b < blocksFor7s; ++b)
        {
            juce::MidiBuffer m;
            pj->processBlock (jaudio, m);
            if (maxAbsOf (jaudio) > 1.0e-6f) silentOnLoad = false;
        }
        check (silentOnLoad, "a freshly loaded instance is SILENT - the jingle does not self-start");

        // Nor does a full state swap (preset load / project reload / Init) start it.
        juce::MemoryBlock state;
        pj->getStateInformation (state);
        pj->setStateInformation (state.getData(), (int) state.getSize());
        bool silentAfterReload = true;
        for (int b = 0; b < 20; ++b)
        {
            juce::MidiBuffer m;
            pj->processBlock (jaudio, m);
            if (maxAbsOf (jaudio) > 1.0e-6f) silentAfterReload = false;
        }
        check (silentAfterReload, "a state reload does not start it either");

        // Clicking the title (triggerJingle) is the ONLY thing that makes it sound.
        pj->triggerJingle();
        bool heardAfterClick = false;
        float loudest = 0.0f;
        for (int b = 0; b < blocksFor7s; ++b)
        {
            juce::MidiBuffer m;
            pj->processBlock (jaudio, m);
            const float pk = maxAbsOf (jaudio);
            loudest = std::max (loudest, pk);
            if (pk > 1.0e-4f) heardAfterClick = true;
        }
        check (heardAfterClick, "clicking the title DOES play it");
        check (loudest < 0.5f, "and it is levelled well below full scale (sits with the synth)");

        // It stops on its own and stays stopped.
        bool silentAgain = true;
        for (int b = 0; b < 20; ++b)
        {
            juce::MidiBuffer m;
            pj->processBlock (jaudio, m);
            if (maxAbsOf (jaudio) > 1.0e-6f) silentAgain = false;
        }
        check (silentAgain, "silent again once it has finished, with no second play");

        // Clicking again replays it - from the top, not overlapped.
        pj->triggerJingle();
        { juce::MidiBuffer m; pj->processBlock (jaudio, m); }
        check (maxAbsOf (jaudio) > 1.0e-6f, "a second click replays it");
    }


    // --- run 19: NoteGate never emits a note-off ahead of its own note-on ---------
    // A key released inside the very block that holds its deferred note-on used to
    // produce off-then-on: the receiver drops the orphan off and the on that follows
    // is never closed, so the voice drones until an all-notes-off. Ordering, not
    // timing, is what matters here - so the check is the ordering invariant itself.
    {
        std::printf ("note gate: a note-off can never precede its own note-on:\n");
        NoteGate gate;
        gate.prepare (sr);

        bool everBadOrder = false, everUnclosed = false;
        // Sweep the release across the whole block so no single lucky offset can
        // hide the bug, and sweep the beat grid so the deferral lands at many points.
        for (int beatOff = 16; beatOff <= 240 && ! everBadOrder; beatOff += 16)
        {
            NoteGate g;
            g.prepare (sr);
            for (int release = 1; release < 256; release += 7)
            {
                juce::MidiBuffer io;
                io.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                io.addEvent (juce::MidiMessage::noteOff (1, 60), release);
                // qTempo on, with the next beat inside this block at `beatOff`.
                g.process (io, 256, true, false, false, (double) beatOff, 24000.0, -1.0, -1.0);

                int onAt = -1, offAt = -1;
                for (const auto meta : io)
                {
                    const auto m = meta.getMessage();
                    if (m.getNoteNumber() != 60) continue;
                    if (m.isNoteOn()  && onAt  < 0) onAt  = (int) meta.samplePosition;
                    if (m.isNoteOff() && offAt < 0) offAt = (int) meta.samplePosition;
                }
                if (offAt >= 0 && (onAt < 0 || offAt < onAt)) everBadOrder = true;
                // A note-on emitted with no note-off anywhere after it, in this block
                // or owed later, is exactly the stuck note.
                if (onAt >= 0 && offAt < 0) everUnclosed = true;
                // Drain any deferred remainder so the next iteration starts clean.
                for (int k = 0; k < 200; ++k)
                { juce::MidiBuffer flush; g.process (flush, 256, true, false, false, -1.0, -1.0, -1.0, -1.0); }
            }
        }
        check (! everBadOrder, "no note-off is ever emitted before the matching note-on");
        check (! everUnclosed, "and no note-on is left without its note-off");
    }

    // --- run 20: a ratio pulse fed back into THIS synth lands on the pitch asked --
    // The pulse pitch used to be encoded as a 12-TET/440 key even when self-play
    // routed it straight back into the synth, which then resolved that key through
    // the active tuning AGAIN - applying Reference A and Octave Transpose twice.
    // octave = -1 put every ratio pulse exactly an octave flat.
    {
        std::printf ("self-play ratio pulses land on the pitch they ask for:\n");
        auto pr = std::make_unique<FractalAdditiveAudioProcessor>();
        pr->prepareToPlay (sr, blk);
        auto sp = [&] (const char* id, float v)
        { auto* p = pr->apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v));
          pr->pollParametersForTests(); };

        sp ("rhythmOn", 1.0f);
        sp ("rhythmSelfPlay", 1.0f);   // pulses feed THIS synth -> tuning-aware encoding
        sp ("rhythmMpe", 1.0f);        // ratio slots need per-note bend
        pr->getRhythm().slots[0].partial  = 1;
        pr->getRhythm().slots[0].enabled  = true;
        pr->getRhythm().slots[0].useRatio = true;
        pr->getRhythm().slots[0].ratio    = 2.0;   // exactly one octave above the played note
        pr->getRhythm().slots[0].durMs    = 40.0f;

        juce::AudioBuffer<float> ra (2, blk);

        // Resolve the (note, bend) actually sent EXACTLY as the receiving synth will:
        // key -> the active tuning's frequency, then the bend on top. That is what
        // exposes the double application - reading the engine's own intended sendHz
        // never could, because it equals the target by construction.
        auto pulseErrorCents = [&] (float refAHz, int octaveShift) -> double
        {
            sp ("refA", refAHz);
            sp ("octaveShift", (float) octaveShift);
            // Reference A and Octave Transpose reach the TuningManager from the
            // processor's parameter poll, which normally rides the message timer -
            // and this harness has no message loop. Without pumping it by hand the
            // tuning would stay at its defaults and the test would pass whatever the
            // encoding does.
            pr->pollParametersForTests();
            for (int b = 0; b < 4; ++b) { juce::MidiBuffer m; pr->processBlock (ra, m); }
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
              pr->processBlock (ra, m); }
            for (int b = 0; b < 20; ++b) { juce::MidiBuffer m; pr->processBlock (ra, m); }

            const double want = pr->getNoteFrequency (60) * 2.0;   // f0 * ratio
            const int    key  = pr->getRhythm().getSendNote (0, 0);
            const int    bend = pr->getRhythm().getSendBend (0, 0);
            const double semis = ((double) bend - 8192.0) / 8192.0 * pr->getRhythm().getBendRangeSemis();
            const double got  = key >= 0 ? pr->getNoteFrequency (key) * std::exp2 (semis / 12.0) : 0.0;

            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
              pr->processBlock (ra, m); }
            for (int b = 0; b < 20; ++b) { juce::MidiBuffer m; pr->processBlock (ra, m); }
            if (! (want > 0.0) || ! (got > 0.0)) return 1.0e9;
            return std::abs (1200.0 * std::log2 (got / want));
        };

        check (pulseErrorCents (440.0f,  0) < 1.0, "at A=440, octave 0: on pitch");
        check (pulseErrorCents (440.0f, -1) < 1.0, "with Octave Transpose -1: still on pitch (was an octave flat)");
        check (pulseErrorCents (432.0f,  0) < 1.0, "at A=432: still on pitch (was ~32 cents flat)");
        check (pulseErrorCents (432.0f,  1) < 1.0, "at A=432 with Octave +1: still on pitch");
        sp ("refA", 440.0f); sp ("octaveShift", 0.0f);
    }

    // --- run 21: equal-step detection drives the "any +10 +18" chord notation -----
    {
        std::printf ("equal-step tuning detection:\n");
        juce::String err;
        auto edo = [] (int n)
        {
            juce::String s;
            s << "! e.scl\n" << juce::String (n) << "-EDO\n " << juce::String (n) << "\n!\n";
            for (int i = 1; i <= n; ++i) s << " " << juce::String (1200.0 * (double) i / (double) n, 6) << "\n";
            return s;
        };
        check (proc.loadScalaText (edo (31), "edo31", err), "31-EDO loads");
        check (proc.tuningIsEqualStep(), "31-EDO is equal-step (chords are valid from any degree)");
        check (proc.loadScalaText (edo (12), "edo12", err), "12-EDO loads");
        check (proc.tuningIsEqualStep(), "12-EDO is equal-step");

        // A just scale is NOT equal-step - its chords are tied to specific degrees.
        check (proc.loadScalaText ("! ji.scl\n5-limit\n 5\n!\n 9/8\n 5/4\n 3/2\n 5/3\n 2/1\n", "ji", err),
               "a just pentatonic loads");
        check (! proc.tuningIsEqualStep(), "and it is NOT equal-step");

        // One degree nudged off the grid is enough to disqualify a scale.
        juce::String bent;
        bent << "! b.scl\nbent-12\n 12\n!\n";
        for (int i = 1; i <= 12; ++i)
            bent << " " << juce::String (100.0 * (double) i + (i == 5 ? 9.0 : 0.0), 6) << "\n";
        check (proc.loadScalaText (bent, "bent", err), "a 12-EDO with one degree bent 9 c loads");
        check (! proc.tuningIsEqualStep(), "and that alone makes it not equal-step");
        proc.clearScala();
    }

    // --- run 22: audit fixes - bank selector, timbre source, undo, steal envelope --
    {
        std::printf ("audit fixes:\n");
        juce::String err;

        // (a) Adding a tuning to the bank must report WHICH slot it landed in, and
        // the selector must follow. It used to be void and leave the combo on the
        // previous slot - and re-picking the entry you are already on does nothing in
        // JUCE, so recovering the right selection needed two separate picks.
        for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i) proc.bankClearSlot (i);
        const juce::String sclA = "! a\nbank-a\n 2\n 500.0\n 2/1\n";
        const juce::String sclB = "! b\nbank-b\n 3\n 300.0\n 700.0\n 2/1\n";
        const int slotA = proc.bankAutoAdd ("bank-a", sclA);
        const int slotB = proc.bankAutoAdd ("bank-b", sclB);
        check (slotA == 0 && slotB == 1, "each added tuning reports its own slot");
        check (proc.bankAutoAdd ("bank-a", sclA) == slotA,
               "re-adding the same scale reports the EXISTING slot, not a duplicate");
        proc.syncBankSelector (slotB);
        check (proc.getTuningBankSlotParam() == slotB + 1, "the selector follows the slot");
        proc.syncBankSelector (slotA);
        check (proc.getTuningBankSlotParam() == slotA + 1, "and follows it back");
        // A real call site: editing a degree stores the edited scale and must select it.
        check (proc.loadScalaText ("! c\nbank-c\n 3\n 300.0\n 700.0\n 2/1\n", "bank-c", err), "scale C loads");
        check (proc.editTuningDegree (1, "6/5", err), "a degree is edited (which banks the result)");
        check (proc.getTuningBankName (proc.getTuningBankSlotParam() - 1).isNotEmpty(),
               "and the selector points at a filled slot, not a stale one");

        // (b) A typed spectrum's source text must be readable back, or the Spectrum
        // Builder box comes up empty and the spectrum can only be edited from memory.
        check (proc.loadTimbreText ("1/1 5/4 3/2 2/1", err), "a typed spectrum loads");
        check (proc.getTimbreName() == "typed", "it is marked as typed");
        check (proc.getTimbreSource() == "1/1 5/4 3/2 2/1", "and its exact text can be read back");

        // (c) The PROCESSOR must open undo transactions, so edits made with the window
        // shut are still cut into ~1 s steps instead of piling into one unbounded one
        // (the first Ctrl+Z after opening the window then reverted everything back to
        // the values the plugin was instantiated with). Driven through the ValueTree
        // with the same UndoManager, which is what the grouping actually guards.
        proc.undoManager.clearUndoHistory();
        proc.undoManager.beginNewTransaction();
        proc.apvts.state.setProperty ("rvTestA", 1, &proc.undoManager);
        for (int i = 0; i < 70; ++i) proc.pollParametersForTests(); // > 60 ticks = > 1 s
        proc.apvts.state.setProperty ("rvTestB", 1, &proc.undoManager);
        check (proc.undoManager.undo(), "there is something to undo");
        check (! proc.apvts.state.hasProperty ("rvTestB"), "the LAST edit is undone");
        check (proc.apvts.state.hasProperty ("rvTestA"),
               "and the earlier one survives - the poll opened a transaction between them");
        proc.undoManager.undo();
        proc.apvts.state.removeProperty ("rvTestA", nullptr);
        proc.apvts.state.removeProperty ("rvTestB", nullptr);
        proc.undoManager.clearUndoHistory();
        proc.clearScala();
        proc.clearTimbre();
    }

    // --- run 23: per-note pitch bend must actually retune the voice ---------------
    // Reported: feeding MPE-retuned MIDI (31-EDO) makes some notes silent and others
    // sound identical, as if everything collapsed onto 12-EDO. That is what dropping
    // the bend looks like: several 31-EDO steps share a nearest 12-EDO key, so they
    // become the same pitch, and repeated note-ons on that same key steal each other.
    // Measure the RENDERED fundamental, which is the only thing that settles it.
    {
        std::printf ("per-note pitch bend retunes the voice:\n");
        constexpr int    fftOrder = 15;
        constexpr int    fftN     = 1 << fftOrder;
        constexpr int    pblk     = 512;

        auto renderFundamental = [&] (FractalAdditiveAudioProcessor& p, int ch, int note, int bend14)
        {
            juce::AudioBuffer<float> b (2, pblk);
            std::vector<float> mono;
            mono.reserve ((size_t) fftN + pblk);
            bool sent = false;
            while ((int) mono.size() < fftN + (int) (48000.0 * 0.25))
            {
                juce::MidiBuffer m;
                if (! sent)
                {
                    // Bend FIRST, then the note - the order a host uses for MPE.
                    m.addEvent (juce::MidiMessage::pitchWheel (ch, bend14), 0);
                    m.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) 110), 1);
                    sent = true;
                }
                b.clear();
                p.processBlock (b, m);
                for (int i = 0; i < pblk; ++i) mono.push_back (b.getSample (0, i));
            }
            std::vector<float> w (mono.end() - fftN, mono.end());
            for (int i = 0; i < fftN; ++i)
                w[(size_t) i] *= 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                          * (float) i / (float) (fftN - 1)));
            std::vector<float> fd ((size_t) fftN * 2, 0.0f);
            for (int i = 0; i < fftN; ++i) fd[(size_t) i] = w[(size_t) i];
            juce::dsp::FFT fft (fftOrder);
            fft.performRealOnlyForwardTransform (fd.data());
            // Lowest strong peak = the fundamental (partial 1 is the loudest here).
            double bestHz = 0.0; float bestMag = 0.0f;
            for (int k = 1; k <= fftN / 2; ++k)
            {
                const float re = fd[(size_t) (2 * k)], im = fd[(size_t) (2 * k + 1)];
                const float mg = std::sqrt (re * re + im * im);
                if (mg > bestMag) { bestMag = mg; bestHz = (double) k * 48000.0 / (double) fftN; }
            }
            return bestHz;
        };

        auto pm = std::make_unique<FractalAdditiveAudioProcessor>();
        auto sp = [&] (const char* id, float v)
        { auto* q = pm->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
          pm->pollParametersForTests(); };
        pm->prepareToPlay (48000.0, pblk);
        sp ("ampAttack", 0.001f); sp ("ampDecay", 0.001f); sp ("ampSustain", 1.0f);
        sp ("baseN", 1.0f);           // partial 1 = the played pitch, easiest to measure
        sp ("fractalLevel", 1.0f);
        sp ("tuningMode", 3.0f);      // MPE (bend input) - the reported case
        pm->pollParametersForTests();

        // +24 semitones of a +-48 range = quarter of the way up from centre.
        const int    bendUp   = 8192 + 8192 / 2;          // +24 st at +-48
        const double f0Plain  = pm->getNoteFrequency (60);
        const double got      = renderFundamental (*pm, 2, 60, bendUp);
        const double expect   = f0Plain * std::exp2 (24.0 / 12.0);
        const double errCents = (got > 0.0 && expect > 0.0)
                              ? std::abs (1200.0 * std::log2 (got / expect)) : 1.0e9;
        std::printf ("  ch2 note60 bend+24st: expected %.1f Hz, rendered %.1f Hz (%.0f c off)\n",
                     expect, got, errCents);
        check (errCents < 25.0, "a member-channel per-note bend really moves the pitch");

        // The MASTER channel of the zone. A host that does not know we accept MPE
        // sends notes and bend on channel 1, where the zone's MASTER bend range
        // applies - not the per-note one. If those ranges differ, the same bend
        // bytes mean a different pitch and the retuning silently collapses.
        {
            auto pm1 = std::make_unique<FractalAdditiveAudioProcessor>();
            auto sp1 = [&] (const char* id, float v)
            { auto* q = pm1->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
              pm1->pollParametersForTests(); };
            pm1->prepareToPlay (48000.0, pblk);
            sp1 ("ampAttack", 0.001f); sp1 ("ampDecay", 0.001f); sp1 ("ampSustain", 1.0f);
            sp1 ("baseN", 1.0f); sp1 ("fractalLevel", 1.0f);
            sp1 ("tuningMode", 3.0f);
            pm1->pollParametersForTests();
            const double got1 = renderFundamental (*pm1, 1, 60, bendUp);
            const double err1 = (got1 > 0.0) ? std::abs (1200.0 * std::log2 (got1 / expect)) : 1.0e9;
            std::printf ("  ch1 note60 bend+24st: expected %.1f Hz, rendered %.1f Hz (%.0f c off)\n",
                         expect, got1, err1);
            check (err1 < 25.0, "the SAME bend on channel 1 moves the pitch by the same amount");
        }

        // And in a non-MPE mode (legacy), where the bend range is the bendRange param.
        {
            auto pl = std::make_unique<FractalAdditiveAudioProcessor>();
            auto spl = [&] (const char* id, float v)
            { auto* q = pl->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
              pl->pollParametersForTests(); };
            pl->prepareToPlay (48000.0, pblk);
            spl ("ampAttack", 0.001f); spl ("ampDecay", 0.001f); spl ("ampSustain", 1.0f);
            spl ("baseN", 1.0f); spl ("fractalLevel", 1.0f);
            spl ("tuningMode", 1.0f);   // MTS-ESP (no master -> 12ED2), legacy zone
            pl->pollParametersForTests();
            const double gotL = renderFundamental (*pl, 1, 60, bendUp);
            const double errL = (gotL > 0.0) ? std::abs (1200.0 * std::log2 (gotL / expect)) : 1.0e9;
            std::printf ("  legacy ch1 bend+24st: expected %.1f Hz, rendered %.1f Hz (%.0f c off)\n",
                         expect, gotL, errL);
            check (errL < 25.0, "legacy mode applies the +-48 bend too");
        }
    }

    // --- run 24: POLYPHONIC per-note retuning, and the attack of a cold note ------
    {
        std::printf ("polyphonic MPE retuning and cold-note attack:\n");
        constexpr int    fo = 15, fn = 1 << fo, pb = 512;

        auto mk = [&] (float tuningMode)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (48000.0, pb);
            auto s = [&] (const char* id, float v)
            { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("ampAttack", 0.001f); s ("ampDecay", 0.001f); s ("ampSustain", 1.0f);
            s ("baseN", 1.0f); s ("fractalLevel", 1.0f); s ("curveSlope", 2.0f);
            s ("tuningMode", tuningMode);
            p->pollParametersForTests();
            return p;
        };

        // (a) Three notes held at once, each on its OWN channel with its OWN bend -
        // exactly what a host sends for a microtonal chord. All three pitches must be
        // present at the same time; if per-note bend is lost they collapse together.
        {
            auto p = mk (3.0f); // MPE
            juce::AudioBuffer<float> b (2, pb);
            std::vector<float> mono; mono.reserve ((size_t) fn + pb);
            bool sent = false;
            while ((int) mono.size() < fn + 12000)
            {
                juce::MidiBuffer m;
                if (! sent)
                {
                    // +4, +8, +12 semitones, each on its own member channel.
                    for (int k = 0; k < 3; ++k)
                    {
                        const double st = 4.0 * (double) (k + 1);
                        const int bend = 8192 + (int) std::lround (st / 48.0 * 8192.0);
                        m.addEvent (juce::MidiMessage::pitchWheel (2 + k, bend), 0);
                        m.addEvent (juce::MidiMessage::noteOn (2 + k, 60, (juce::uint8) 110), 1);
                    }
                    sent = true;
                }
                b.clear(); p->processBlock (b, m);
                for (int i = 0; i < pb; ++i) mono.push_back (b.getSample (0, i));
            }
            std::vector<float> w (mono.end() - fn, mono.end());
            for (int i = 0; i < fn; ++i)
                w[(size_t) i] *= 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                           * (float) i / (float) (fn - 1)));
            std::vector<float> fd ((size_t) fn * 2, 0.0f);
            for (int i = 0; i < fn; ++i) fd[(size_t) i] = w[(size_t) i];
            juce::dsp::FFT fft (fo);
            fft.performRealOnlyForwardTransform (fd.data());
            std::vector<float> mag ((size_t) fn / 2 + 1);
            float peak = 0.0f;
            for (int k = 0; k <= fn / 2; ++k)
            {
                const float re = fd[(size_t) (2 * k)], im = fd[(size_t) (2 * k + 1)];
                mag[(size_t) k] = std::sqrt (re * re + im * im);
                peak = std::max (peak, mag[(size_t) k]);
            }
            auto present = [&] (double hz)
            {
                const int c = (int) std::lround (hz * (double) fn / 48000.0);
                float best = 0.0f;
                for (int k = c - 6; k <= c + 6; ++k)
                    if (k >= 0 && k < (int) mag.size()) best = std::max (best, mag[(size_t) k]);
                return best > peak * 0.02f; // -34 dB below the loudest thing present
            };
            const double f0 = p->getNoteFrequency (60);
            int found = 0;
            for (int k = 0; k < 3; ++k)
                if (present (f0 * std::exp2 (4.0 * (double) (k + 1) / 12.0))) ++found;
            std::printf ("  three per-note-bent notes: %d of 3 distinct pitches sounding\n", found);
            check (found == 3, "a microtonal chord keeps THREE separate pitches, not one");
        }

        // (b) A COLD note with attack at minimum must reach full level almost at once.
        // A stolen voice deliberately glides (~7 ms) to hand over without a click; a
        // fresh one must not, or every note has an attack the envelope never asked for.
        {
            auto p = mk (1.0f);
            juce::AudioBuffer<float> b (2, 64);
            p->prepareToPlay (48000.0, 64);
            p->pollParametersForTests();
            // Let the spectrum settle BEFORE playing. The rebuild runs on a background
            // thread, so a note struck while it is still in flight legitimately slews
            // its partial amplitudes onto the arriving template - that is the morph
            // machinery doing its job, not an attack. Measuring without this settle
            // was measuring the test's own parameter changes.
            for (int i = 0; i < 200; ++i) { juce::MidiBuffer none; b.clear(); p->processBlock (b, none); }
            std::vector<float> mono;
            bool sent = false;
            for (int blk = 0; blk < 400; ++blk)
            {
                juce::MidiBuffer m;
                if (! sent) { m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0); sent = true; }
                b.clear(); p->processBlock (b, m);
                for (int i = 0; i < 64; ++i) mono.push_back (b.getSample (0, i));
            }
            // Envelope = RMS over a sliding window of one fundamental period. NOT the
            // peak: the peak of a sum of partials wanders with their phase relationship,
            // so a "rising peak" is mostly luck about which window caught them aligned.
            // RMS is stable for a steady tone whatever the phases do.
            const int win = (int) std::lround (48000.0 / p->getNoteFrequency (60));
            std::vector<float> env;
            env.reserve (mono.size());
            double acc2 = 0.0;
            for (int k = 0; k < win && k < (int) mono.size(); ++k) acc2 += (double) mono[(size_t) k] * mono[(size_t) k];
            for (size_t i = 0; i + (size_t) win < mono.size(); ++i)
            {
                env.push_back ((float) std::sqrt (acc2 / (double) win));
                acc2 -= (double) mono[i] * mono[i];
                acc2 += (double) mono[i + (size_t) win] * mono[i + (size_t) win];
            }
            float steady = 0.0f;
            for (size_t i = env.size() / 2; i < env.size(); ++i) steady = std::max (steady, env[i]);
            int to90 = -1;
            for (size_t i = 0; i < env.size(); ++i)
                if (env[i] >= steady * 0.9f) { to90 = (int) i; break; }
            const double msTo90 = to90 < 0 ? 1.0e9 : (double) to90 / 48.0;
            std::printf ("  cold note, attack 1 ms: reaches 90%% of level in %.2f ms\n", msTo90);
            std::printf ("    envelope shape:");
            for (double ms : { 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 })
            {
                const size_t i = (size_t) (ms * 48.0);
                if (i < env.size() && steady > 0.0f)
                    std::printf ("  %gms=%.0f%%", ms, 100.0 * (double) env[i] / (double) steady);
            }
            std::printf ("\n");
            check (msTo90 < 8.0, "a fresh note has no attack the envelope did not ask for");
        }
    }

    // --- run 25: the WHOLE amp envelope, cold and after stealing ------------------
    // "Notes sound as if they had no envelope until I press many, then some work."
    // So measure the full shape: does it attack, does it fall to the sustain level,
    // and does it actually stop after the note-off - on a fresh voice AND on a
    // stolen one.
    {
        std::printf ("full amp envelope, cold voice and stolen voice:\n");
        constexpr int eb = 64;
        auto envOf = [&] (FractalAdditiveAudioProcessor& p, const std::vector<float>& mono, double f0)
        {
            juce::ignoreUnused (p);
            const int w = juce::jmax (16, (int) std::lround (48000.0 / juce::jmax (20.0, f0)));
            std::vector<float> e; e.reserve (mono.size());
            double a2 = 0.0;
            for (int k = 0; k < w && k < (int) mono.size(); ++k) a2 += (double) mono[(size_t) k] * mono[(size_t) k];
            for (size_t i = 0; i + (size_t) w < mono.size(); ++i)
            { // clamp: the running sum drifts slightly negative on silence and
              // sqrt() of that is NaN - a measurement artefact, not the plugin's.
              e.push_back ((float) std::sqrt (std::max (0.0, a2) / (double) w));
              a2 -= (double) mono[i] * mono[i];
              a2 += (double) mono[i + (size_t) w] * mono[i + (size_t) w]; }
            return e;
        };

        auto run = [&] (int extraNotes, const char* label)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (48000.0, eb);
            auto s = [&] (const char* id, float v)
            { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("ampAttack", 0.050f);   // 50 ms - long enough to SEE
            s ("ampDecay",  0.100f);   // 100 ms down to...
            s ("ampSustain", 0.25f);   // ...a quarter level
            s ("ampRelease", 0.050f);
            s ("baseN", 1.0f); s ("fractalLevel", 1.0f); s ("curveSlope", 2.0f);
            s ("ampCurveA", 0.0f); s ("ampCurveD", 0.0f); s ("ampCurveR", 0.0f);
            p->pollParametersForTests();
            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 100; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }

            // Optionally burn voices first, so the measured note is a STEAL. They are
            // released again straight away and given time to die, so the measurement
            // below is of note 60 ALONE - otherwise the "tail" would just be the other
            // notes still held, which says nothing about note 60's envelope.
            if (extraNotes > 0)
            {
                for (int k = 0; k < extraNotes; ++k)
                { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 20 + k, (juce::uint8) 100), 0);
                  b.clear(); p->processBlock (b, m); }
                for (int k = 0; k < extraNotes; ++k)
                { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 20 + k), 0);
                  b.clear(); p->processBlock (b, m); }
                for (int i = 0; i < 400; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }
            }

            std::vector<float> mono;
            const int holdBlocks = (int) (0.30 * 48000.0 / eb);   // 300 ms held
            for (int blk = 0; blk < holdBlocks + 400; ++blk)
            {
                juce::MidiBuffer m;
                if (blk == 0)          m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
                if (blk == holdBlocks) m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                b.clear(); p->processBlock (b, m);
                for (int i = 0; i < eb; ++i) mono.push_back (b.getSample (0, i));
            }
            const auto e = envOf (*p, mono, p->getNoteFrequency (60));
            auto at = [&] (double ms) { const size_t i = (size_t) (ms * 48.0);
                                        return i < e.size() ? e[i] : 0.0f; };
            const float peak = at (55.0);                  // just after the 50 ms attack
            const float sus  = at (250.0);                 // well into sustain
            const float tail = at (300.0 + 200.0);         // 200 ms after note-off
            std::printf ("  %-8s peak@55ms=%.4f  sustain@250ms=%.4f (%.0f%% of peak)  tail@+200ms=%.5f\n",
                         label, peak, sus, peak > 0 ? 100.0 * sus / peak : 0.0, tail);
            return std::array<float, 3> { peak, sus, tail };
        };

        const auto cold = run (0,  "cold");
        const auto stol = run (34, "stolen");   // 34 > 32 voices -> the last note steals

        check (cold[0] > 1.0e-4f, "a cold note sounds at all");
        check (cold[1] < cold[0] * 0.55f, "a cold note DECAYS to its 25% sustain (it has an envelope)");
        check (cold[2] < cold[0] * 0.02f, "and it releases to silence after the note-off");
        check (stol[0] > 1.0e-4f, "a stolen note sounds too");
        check (stol[1] < stol[0] * 0.55f, "a stolen note also decays to sustain");
        check (stol[2] < stol[0] * 0.02f, "and also releases to silence");
    }

    // --- run 26: a fast morph must not CLICK -------------------------------------
    // "Two notes with an LFO on Base and morph on click very loudly - the same clicks
    // as morph speed at minimum with one note."
    // A click IS a discontinuity, so measure the thing itself: the largest jump from
    // one sample to the next, normalised by the signal's own peak. A smooth additive
    // tone has a bounded per-sample slope; a coherent switch of the whole partial bank
    // does not. Morph OFF is the control - the same template stream without any
    // crossfade - so the comparison isolates the morph rather than the rebuilds.
    {
        std::printf ("morph discontinuity under a stream of new templates:\n");
        constexpr int eb = 64;
        auto run = [&] (bool morphOn, float morphTime, const char* label)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (48000.0, eb);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            // Flat, fully-sustained envelope: any jump we measure is the morph, not the ADSR.
            s ("ampAttack", 0.002f); s ("ampDecay", 0.002f); s ("ampSustain", 1.0f);
            s ("ampCurveA", 0.0f);   s ("ampCurveD", 0.0f);
            s ("fractalLevel", 1.0f); s ("curveSlope", 2.0f); s ("baseN", 1.0f);
            s ("morph", morphOn ? 1.0f : 0.0f);
            s ("morphTime", morphTime);

            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 60; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }
            { // two notes, exactly as reported
                juce::MidiBuffer m;
                m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                m.addEvent (juce::MidiMessage::noteOn (1, 67, 0.8f), 1);
                b.clear(); p->processBlock (b, m);
            }
            for (int i = 0; i < 60; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }

            float maxJump = 0.0f, peak = 0.0f, prev = 0.0f;
            bool  have = false;
            for (int step = 0; step < 20; ++step)
            {
                s ("baseN", 1.0f + (float) (step % 6));  // sweep Base the way the LFO does
                juce::Thread::sleep (14);                 // let the builder thread publish
                for (int i = 0; i < 10; ++i)
                {
                    juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                    const float* L = b.getReadPointer (0);
                    for (int k = 0; k < eb; ++k)
                    {
                        const float x = L[k];
                        peak = std::max (peak, std::abs (x));
                        if (have) maxJump = std::max (maxJump, std::abs (x - prev));
                        prev = x; have = true;
                    }
                }
            }
            const float ratio = peak > 0.0f ? maxJump / peak : 0.0f;
            std::printf ("  %-24s peak=%.4f  maxJump=%.4f  jump/peak=%.3f\n",
                         label, peak, maxJump, ratio);
            return ratio;
        };

        const float off  = run (false, 0.001f, "morph off (control)");
        const float fast = run (true,  0.001f, "morph on, time=minimum");
        const float slow = run (true,  0.300f, "morph on, time=300 ms");

        check (off > 0.0f, "the control case actually made sound");
        // Generous margin: we are catching a full-scale coherent impulse (jump/peak near
        // 1), not a few percent of extra slope.
        check (fast <= off * 2.5f + 0.02f, "a minimum-time morph does not click");
        check (slow <= off * 2.5f + 0.02f, "a morph restarted every rebuild does not click");
    }

    // --- run 27: MTS-ESP is adopted on a NEW instance only, never on a restored one --
    // "I only wanted it to happen when Fibló loads for the FIRST time. If the project is
    // reopened I don't want it, because it would break the settings the user put in -
    // that would be a hard blow for the user."
    // The adoption itself needs a live MTS master, which an offline harness cannot
    // provide, so what is checked here is the gate in front of it: is it still armed?
    {
        std::printf ("MTS-ESP adoption is armed only for a brand-new instance:\n");
        constexpr int eb = 64;
        auto fresh = [&]
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (48000.0, eb);
            return p;
        };

        {   // a brand-new instance stays armed while the host is still handing things over
            auto p = fresh();
            for (int i = 0; i < 5; ++i) p->pollParametersForTests();
            check (p->mtsAdoptionArmedForTests(),
                   "a new instance is still armed a few ticks in (the host may yet send state)");
            for (int i = 0; i < 40; ++i) p->pollParametersForTests();
            check (! p->mtsAdoptionArmedForTests(),
                   "and it is consumed once, shortly after load");
        }
        {   // a REOPENED project: the host hands over saved state
            auto src = fresh();
            if (auto* q = src->apvts.getParameter ("tuningMode"))
                q->setValueNotifyingHost (q->convertTo0to1 (2.0f)); // Scala
            juce::MemoryBlock saved;
            src->getStateInformation (saved);

            auto p = fresh();
            p->setStateInformation (saved.getData(), (int) saved.getSize());
            check (! p->mtsAdoptionArmedForTests(),
                   "a restored project disarms it, so the saved tuning mode is never overwritten");
            for (int i = 0; i < 60; ++i) p->pollParametersForTests();
            const float mode = p->apvts.getRawParameterValue ("tuningMode")->load();
            check (std::abs (mode - 2.0f) < 0.5f,
                   "and the restored tuning mode survives the load (still Scala)");
        }
        {   // a host handing over EMPTY or unreadable state must not look brand new
            auto p = fresh();
            p->setStateInformation (nullptr, 0);
            check (! p->mtsAdoptionArmedForTests(),
                   "empty state disarms it too (restoreState bails out before its own reset)");

            auto q = fresh();
            const char junk[] = "not a valid plugin chunk at all";
            q->setStateInformation (junk, (int) sizeof (junk));
            check (! q->mtsAdoptionArmedForTests(),
                   "and so does unreadable state");
        }
    }

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
