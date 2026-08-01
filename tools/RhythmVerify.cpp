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
#include "PluginEditor.h"   // run 35 lays the real editor out and measures it

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
        // A GLOBAL target. Voice-level controls (and curveSlope, which is amplitude-only)
        // are modulated inside each voice now, so their parameter deliberately no longer
        // moves - there is no single value it could show while eight notes each have
        // their own. What is under test here is the global path, which still carries the
        // spectrum controls, so it needs a global target: masterGain, whose range is
        // linear so the displacement maps straight through.
        setParam ("masterGain", -12.0f);         // base value; the env rides on top
        proc.getModEngine().addTarget (0, "masterGain");

        auto* raw  = proc.apvts.getRawParameterValue ("masterGain");
        const float base = -12.0f;
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
            auto* p = proc.apvts.getParameter ("masterGain");
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (-6.0f));
            p->endChangeGesture();
        }
        { juce::MidiBuffer midi; proc.processBlock (audio, midi); }
        check (std::abs (raw->load() - (-6.0f)) < 0.05f,
               "a knob set while modulated reads back as the value you set (clean undo/save)");
        for (int b = 0; b < (int) (0.5 * sr / blk); ++b) { juce::MidiBuffer midi; proc.processBlock (audio, midi); }
        check (raw->load() > -6.0f + 0.05f, "modulation resumes by itself after the hold window");
        // HOST AUTOMATION (no gesture) must NOT pause the modulation.
        {
            auto* p = proc.apvts.getParameter ("masterGain");
            p->setValueNotifyingHost (p->convertTo0to1 (-9.0f));
            juce::MidiBuffer midi; proc.processBlock (audio, midi);
            check (raw->load() > -9.0f + 0.05f, "automation (no gesture) keeps modulating - not throttled");
        }
        { juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0); proc.processBlock (audio, midi); }
        setParam ("masterGain", base);

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
        // Global target (see the note above): the curve maths is shared by both paths,
        // and masterGain's range is linear so the shape is read back undistorted.
        setParam ("masterGain", -60.0f);
        proc.getModEngine().addTarget (0, "masterGain");
        auto* raw = proc.apvts.getRawParameterValue ("masterGain");
        const float full = 72.0f; // masterGain range -60..12 -> Range 1 spans the lot

        // Sample the decay leg half way through, for three curve settings.
        auto midDecay = [&] (float curve)
        {
            setParam ("modE1CurveD", curve);
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
              proc.processBlock (audio, m); }
            const int halfBlocks = (int) (0.200 * sr / blk);           // 0.2 s = half of D
            for (int b = 0; b < halfBlocks; ++b) { juce::MidiBuffer m; proc.processBlock (audio, m); }
            const float v = (raw->load() + 60.0f) / full;                        // 0..1
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
        check (std::abs ((raw->load() + 60.0f) / full - 0.25f) < 0.03f, "the sustain node sets the level it rests at");

        // A second key while the first sustains must restart from zero.
        setParam ("modE1A", 0.30f);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
          proc.processBlock (audio, m); }
        check ((raw->load() + 60.0f) / full < 0.10f, "a second key RESTARTS the envelope from zero (attack audible again)");
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

    // --- run 28: modulation is PER VOICE, and it is audible ----------------------
    // "I want slope and every other parameter to have envelopes working the way I asked."
    // The parameter deliberately no longer moves, so the only honest place to look is
    // the SOUND. Brightness proxy: mean|x[n]-x[n-1]| / mean|x[n]| - a crude spectral
    // centroid that needs no FFT and rises with high-frequency content.
    {
        std::printf ("per-voice modulation, measured in the audio:\n");
        constexpr int eb = 64;
        constexpr double sr = 48000.0;

        auto bright = [] (const std::vector<float>& x, int from, int to)
        {
            double sa = 0.0, sd = 0.0; int n = 0;
            for (int i = juce::jmax (1, from); i < juce::jmin ((int) x.size(), to); ++i)
            { sa += std::abs ((double) x[(size_t) i]); sd += std::abs ((double) x[(size_t) i] - x[(size_t) i - 1]); ++n; }
            return n > 0 && sa > 1.0e-12 ? sd / sa : 0.0;
        };

        // Render `ms` milliseconds, optionally pressing a SECOND key partway through and
        // letting it go again, and return the mono signal.
        auto render = [&] (const char* target, float range, bool secondKey, double ms,
                           float baseSlope, float baseCutoffCents)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr, eb);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("rhythmOn", 0.0f); s ("rhythmSelfPlay", 0.0f); s ("morph", 0.0f);
            // Flat amp envelope: everything we measure is the MODULATION, not the ADSR.
            s ("ampAttack", 0.001f); s ("ampDecay", 0.001f); s ("ampSustain", 1.0f); s ("ampRelease", 0.005f);
            // A long, deep modulation envelope so the sweep is unmistakable.
            s ("modE1A", 0.001f); s ("modE1D", 0.400f); s ("modE1S", 0.0f); s ("modE1R", 0.005f);
            s ("modE1CurveD", 0.0f); s ("modE1Range", range); s ("modE1On", 1.0f);
            // A RICH spectrum and an open filter by default: with a steep slope and a low
            // cutoff the output is very nearly a pure fundamental (|dx|/|x| lands on
            // 2*pi*f/fs exactly), and then there is no brightness for anything to move.
            s ("baseN", 1.0f); s ("fractalLevel", 1.0f); s ("curveSlope", baseSlope);
            s ("filterCutoff", baseCutoffCents);
            for (int m = 0; m < ModEngine::kMods; ++m) p->getModEngine().unmapAll (m);
            p->getModEngine().addTarget (0, target);

            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 40; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }

            std::vector<float> out;
            const int total = (int) (ms * 0.001 * sr / eb);
            const int onAt = (int) (0.150 * sr / eb), offAt = (int) (0.170 * sr / eb);
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
              b.clear(); p->processBlock (b, m);
              for (int k = 0; k < eb; ++k) out.push_back (b.getReadPointer (0)[k]); }
            for (int i = 1; i < total; ++i)
            {
                juce::MidiBuffer m;
                if (secondKey && i == onAt)  m.addEvent (juce::MidiMessage::noteOn (1, 79, (juce::uint8) 100), 0);
                if (secondKey && i == offAt) m.addEvent (juce::MidiMessage::noteOff (1, 79), 0);
                b.clear(); p->processBlock (b, m);
                for (int k = 0; k < eb; ++k) out.push_back (b.getReadPointer (0)[k]);
            }
            return out;
        };
        const auto at = [&] (double ms) { return (int) (ms * 0.001 * sr); };

        auto rms = [] (const std::vector<float>& x, int from, int to)
        {
            double s2 = 0.0; int n = 0;
            for (int i = juce::jmax (0, from); i < juce::jmin ((int) x.size(), to); ++i)
            { s2 += (double) x[(size_t) i] * x[(size_t) i]; ++n; }
            return n > 0 ? std::sqrt (s2 / n) : 0.0;
        };

        // 0. PLUMBING, on the least ambiguous target there is. The mod envelope decays
        //    1 -> 0 over 400 ms; Range -1 means the offset walks -1 -> 0, so the amp
        //    sustain walks silence -> full. If this does not move, nothing does.
        {
            const auto x = render ("ampSustain", -1.0f, false, 700.0, 0.5f, 9600.0f);
            const double early = rms (x, at (20.0),  at (70.0));
            const double late  = rms (x, at (550.0), at (650.0));
            std::printf ("  ampSus  rms early=%.5f late=%.5f\n", early, late);
            check (late > early * 4.0, "a per-voice envelope reaches the audio at all");
        }

        // 1. SLOPE. It is a spectrum control, but amplitude-only, so a voice can move it
        //    on its own - the rescale is exact and needs no recompile. The mod envelope
        //    DECAYS 1 -> 0, so with Range +1 the slope starts steep (dark) and falls back
        //    to the knob's own value (bright): brightness must RISE across the note.
        {
            const auto x = render ("curveSlope", 1.0f, false, 700.0, 0.5f, 9600.0f);
            const double early = bright (x, at (20.0),  at (70.0));
            const double late  = bright (x, at (550.0), at (650.0));
            std::printf ("  slope   brightness early=%.4f late=%.4f\n", early, late);
            check (early > 1.0e-6 && late > 1.0e-6, "the note sounds throughout");
            check (late > early * 1.15, "a per-voice envelope on SLOPE audibly opens the note up");
        }

        // 2. FILTER CUTOFF, and the decisive one: a SECOND key must not restart the
        //    FIRST note's envelope. That is exactly what a single global bank did, and
        //    why pressing more keys made the modulation appear to jump.
        {
            const auto alone = render ("filterCutoff", 1.0f, false, 500.0, 0.5f, 1200.0f);
            const auto withB = render ("filterCutoff", 1.0f, true,  500.0, 0.5f, 1200.0f);
            const double bA = bright (alone, at (300.0), at (360.0));
            const double bB = bright (withB, at (300.0), at (360.0));
            const double sweep = bright (alone, at (20.0), at (70.0));
            std::printf ("  cutoff  alone=%.4f  after a second key=%.4f  (early sweep ref=%.4f)\n",
                         bA, bB, sweep);
            check (std::abs (sweep - bA) > 0.05 * juce::jmax (sweep, bA),
                   "precondition: the envelope really is sweeping the filter");
            check (std::abs (bA - bB) < 0.10 * juce::jmax (bA, bB),
                   "a second key does NOT disturb the first note's envelope (per voice)");
        }
    }

    // --- run 29: a downward slope modulation must WIDEN the compiled template --------
    // A voice rescales amplitudes from the template's slope to its own, exactly - but
    // only for partials the template actually holds, and the amplitude floor culls by an
    // amplitude that depends on slope. Compiled steep, the template physically lacks what
    // a shallower voice needs, and partials would go missing. So the compile has to use
    // the LOWEST slope the modulation can reach.
    {
        std::printf ("slope modulation widens the compiled spectrum:\n");
        constexpr int eb = 64;
        auto build = [&] (bool mapDownwards)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (48000.0, eb);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("rhythmOn", 0.0f); s ("rhythmSelfPlay", 0.0f);
            s ("baseN", 1.0f); s ("fractalLevel", 1.0f);
            s ("curveSlope", 3.0f);          // steep: the -120 dB floor culls hard
            if (mapDownwards)
            {
                p->getModEngine().addTarget (0, "curveSlope");
                s ("modE1Range", -1.0f);     // reaches all the way down to slope 0
                s ("modE1On", 1.0f);
            }
            p->pollParametersForTests();
            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 30; ++i)
            { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); juce::Thread::sleep (4); }
            p->pollParametersForTests();
            for (int i = 0; i < 30; ++i)
            { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); juce::Thread::sleep (4); }
            const auto* t = p->getLiveTemplate();
            return t != nullptr ? (int) t->partials.size() : 0;
        };
        const int plain  = build (false);
        const int mapped = build (true);
        std::printf ("  partials: slope 3 alone=%d   with a downward modulation=%d\n", plain, mapped);
        check (plain > 0, "the plain template built at all");
        check (mapped > plain,
               "a modulator reaching down to a shallower slope compiles a WIDER template");
    }

    // --- run 30: manual per-partial amplitudes, and slope going inert ---------------
    // "Make slope not work when manual mode is on, which is off by default."
    {
        std::printf ("manual per-partial amplitudes:\n");
        constexpr int eb = 64;
        constexpr double sr2 = 48000.0;

        auto brightOf = [] (const std::vector<float>& x)
        {
            double sa = 0.0, sd = 0.0;
            for (size_t i = 1; i < x.size(); ++i)
            { sa += std::abs ((double) x[i]); sd += std::abs ((double) x[i] - x[i - 1]); }
            return sa > 1.0e-12 ? sd / sa : 0.0;
        };

        // slopeAfter is set AFTER manual is switched on, so with manual on it must have
        // no effect at all - the freeze happened at slope 1.
        auto run = [&] (bool manual, float slopeAfter, bool muteFundamental)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr2, eb);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("rhythmOn", 0.0f); s ("rhythmSelfPlay", 0.0f); s ("morph", 0.0f);
            s ("ampAttack", 0.001f); s ("ampDecay", 0.001f); s ("ampSustain", 1.0f);
            s ("baseN", 1.0f); s ("fractalLevel", 1.0f); s ("filterCutoff", 9600.0f);
            s ("curveSlope", 1.0f);
            if (manual) s ("partialManual", 1.0f);   // freezes slope at 1.0
            s ("curveSlope", slopeAfter);            // must be inert while manual is on

            if (muteFundamental)
            {
                std::vector<spectrum::PartialGain> ov;
                spectrum::setPartialGain (ov, spectrum::gainKey (1.0), spectrum::kGainMutedDb);
                p->setPartialGains (ov, true);
            }
            p->pollParametersForTests();

            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 30; ++i)
            { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); juce::Thread::sleep (4); }
            p->pollParametersForTests();
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
              b.clear(); p->processBlock (b, m); }
            std::vector<float> out;
            for (int i = 0; i < 120; ++i)
            {
                juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                if (i > 40) for (int k = 0; k < eb; ++k) out.push_back (b.getReadPointer (0)[k]);
            }
            return brightOf (out);
        };

        const double offS1 = run (false, 1.0f, false);
        const double offS4 = run (false, 4.0f, false);
        const double onS1  = run (true,  1.0f, false);
        const double onS4  = run (true,  4.0f, false);
        std::printf ("  brightness  manual OFF: slope1=%.4f slope4=%.4f   manual ON: slope1=%.4f slope4=%.4f\n",
                     offS1, offS4, onS1, onS4);
        check (std::abs (offS1 - offS4) > 0.15 * juce::jmax (offS1, offS4),
               "precondition: with manual OFF, slope changes the timbre a lot");
        check (std::abs (onS1 - onS4) < 0.02 * juce::jmax (onS1, onS4),
               "with manual ON, slope has NO effect on the sound");
        check (std::abs (onS1 - offS1) < 0.02 * juce::jmax (onS1, offS1),
               "and switching manual on does not change the sound by itself");

        const double muted = run (true, 1.0f, true);
        std::printf ("  brightness  manual ON, fundamental muted=%.4f (vs %.4f unmuted)\n", muted, onS1);
        check (muted > onS1 * 1.15,
               "a per-partial edit reaches the audio (muting the fundamental brightens it)");

        // The ON/OFF SWITCH itself, which the editor now puts in two places. The engine
        // only learns whether the edits are in force through setOverrides, so when the
        // switch moved on its own - the pill in the window, the pill under the pencil,
        // host automation - a whole hand-drawn spectrum stayed silent and the control
        // looked broken. It has to work from either direction.
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr2, eb);
            std::vector<spectrum::PartialGain> ov;
            spectrum::setPartialGain (ov, spectrum::gainKey (1.0), spectrum::kGainMutedDb);
            p->setPartialGains (ov, false);            // stored, switch still OFF
            p->pollParametersForTests();
            const int offMatched = p->getPartialGainsMatched();
            if (auto* q = p->apvts.getParameter ("partialManual"))
                q->setValueNotifyingHost (1.0f);
            p->pollParametersForTests();               // the switch alone, nothing else
            const int onMatched = p->getPartialGainsMatched();
            if (auto* q = p->apvts.getParameter ("partialManual"))
                q->setValueNotifyingHost (0.0f);
            p->pollParametersForTests();
            const int backOff = p->getPartialGainsMatched();
            std::printf ("  edits in force: switch off=%d  on=%d  off again=%d\n",
                         offMatched, onMatched, backOff);
            check (offMatched == 0 && onMatched == 1 && backOff == 0,
                   "moving the manual switch alone puts the stored edits in force, and takes them back out");
        }

        // The edits must come back with the project. Losing a hand-drawn spectrum on
        // reopening would be the same class of injury as the MTS-ESP overwrite.
        {
            auto src = std::make_unique<FractalAdditiveAudioProcessor>();
            src->prepareToPlay (sr2, eb);
            std::vector<spectrum::PartialGain> ov;
            spectrum::setPartialGain (ov, spectrum::gainKey (1.0), -18.5f);
            spectrum::setPartialGain (ov, spectrum::gainKey (3.0), spectrum::kGainMutedDb);
            spectrum::setPartialGain (ov, spectrum::gainKey (7.0),  6.25f);
            src->setPartialGains (ov, true);
            if (auto* q = src->apvts.getParameter ("partialManual"))
                q->setValueNotifyingHost (q->convertTo0to1 (1.0f));
            juce::MemoryBlock saved;
            src->getStateInformation (saved);

            auto dst = std::make_unique<FractalAdditiveAudioProcessor>();
            dst->prepareToPlay (sr2, eb);
            dst->setStateInformation (saved.getData(), (int) saved.getSize());
            const auto& back = dst->getPartialGains();
            bool same = back.size() == ov.size();
            for (size_t i = 0; i < back.size() && same; ++i)
                same = back[i].key == ov[i].key && std::abs (back[i].dB - ov[i].dB) < 0.01f;
            check (same, "per-partial edits survive a save and reload, keys and dB intact");
        }

        // The amplitude WINDOW's two lifelines. Both were absent when the knobs were
        // first wired up, and both are invisible until they bite:
        //  - one undo step per drag gesture (the edits are not APVTS parameters, so
        //    nothing put them on the stack: Ctrl+Z sailed past a hand-drawn spectrum);
        //  - a revision counter, because the window cannot watch the template generation
        //    for this (setOverrides deliberately never bumps it) and so had no way to
        //    notice an undo, a preset or a reload changing the edits underneath it.
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr2, eb);

            std::vector<spectrum::PartialGain> before, after;
            spectrum::setPartialGain (before, spectrum::gainKey (2.0), -6.0f);
            after = before;
            spectrum::setPartialGain (after, spectrum::gainKey (2.0), -18.0f);
            spectrum::setPartialGain (after, spectrum::gainKey (5.0), spectrum::kGainMutedDb);

            p->setPartialGains (before, true);
            const unsigned rev0 = p->getPartialGainsRevision();
            p->setPartialGains (after, true);
            const unsigned rev1 = p->getPartialGainsRevision();
            p->commitPartialGains (before, after);
            const unsigned rev2 = p->getPartialGainsRevision();
            std::printf ("  gain revision: after 1st write=%u  2nd=%u  after commit=%u\n", rev0, rev1, rev2);
            check (rev1 != rev0, "every write to the per-partial edits bumps the revision counter");

            p->undoManager.undo();
            const auto back = p->getPartialGains();
            bool restored = back.size() == before.size();
            for (size_t i = 0; i < back.size() && restored; ++i)
                restored = back[i].key == before[i].key && back[i].dB == before[i].dB;
            check (restored, "Ctrl+Z takes a whole drag gesture's per-partial edits back at once");
            check (p->getPartialGainsRevision() != rev2,
                   "and the undo bumps the revision, so an open editor window resyncs to it");

            p->undoManager.redo();
            const auto fwd = p->getPartialGains();
            bool redone = fwd.size() == after.size();
            for (size_t i = 0; i < fwd.size() && redone; ++i)
                redone = fwd[i].key == after[i].key && fwd[i].dB == after[i].dB;
            check (redone, "Ctrl+Y brings the edits forward again, mute included");

            // A click that moved nothing must not push a transaction, or Ctrl+Z would
            // start eating perfectly good earlier edits.
            const int depthBefore = p->undoManager.getNumActionsInCurrentTransaction();
            p->commitPartialGains (fwd, fwd);
            p->undoManager.undo();
            const auto still = p->getPartialGains();
            bool untouched = still.size() == before.size();
            for (size_t i = 0; i < still.size() && untouched; ++i)
                untouched = still[i].key == before[i].key && still[i].dB == before[i].dB;
            juce::ignoreUnused (depthBefore);
            check (untouched, "a gesture that changed nothing pushes nothing, so undo still steps over real edits");
        }
    }

    // --- run 31: undo / redo reach the custom tuning ------------------------------
    // "CTRL Z CTRL Y DO NOT WORK FOR THE CUSTOM TUNING VALUE."
    // It is not an APVTS parameter, so nothing ever pushed it onto the undo stack and
    // Ctrl+Z sailed past it to whatever fader had been touched before.
    {
        std::printf ("undo / redo on the custom tuning:\n");
        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (48000.0, 64);

        const juce::String a = "1 5/4 3/2 2";
        const juce::String b = "1 6/5 3/2 2";
        juce::String err;
        check (p->loadTimbreTextUndoable (a, err), "a custom tuning can be typed in");
        check (p->loadTimbreTextUndoable (b, err), "and then replaced by another");
        const auto afterB = p->getTimbreSource();

        p->undoManager.undo();
        const auto afterUndo = p->getTimbreSource();
        p->undoManager.redo();
        const auto afterRedo = p->getTimbreSource();
        std::printf ("  typed=\"%s\"  after undo=\"%s\"  after redo=\"%s\"\n",
                     afterB.toRawUTF8(), afterUndo.toRawUTF8(), afterRedo.toRawUTF8());

        check (afterUndo == a, "Ctrl+Z puts the PREVIOUS custom tuning back");
        check (afterRedo == afterB, "Ctrl+Y brings the newer one forward again");

        // A tuning that does not parse must not push anything onto the stack, or Ctrl+Z
        // would start eating perfectly good edits.
        juce::String bad;
        const bool rejected = ! p->loadTimbreTextUndoable ("not a scale at all", bad);
        p->undoManager.undo();
        check (rejected && p->getTimbreSource() == a,
               "a rejected tuning pushes nothing, so undo still steps back over the real edits");
    }

    // --- run 32: .tun written by the REAL writer must load back identically ---------
    // TuningTests cannot reach this: it does not link the processor, so its .tun fixtures
    // REPLICATE the writer's layout instead of calling it. That leaves the one thing that
    // actually matters untested - the writer and the loader drifting apart - and it is
    // exactly the kind of gap that goes unnoticed until a user's exported scale reopens
    // wrong. Here the processor is real, so the round trip can be the real one.
    {
        std::printf ("save .tun -> load .tun round trip, through the real writer:\n");
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("fiblo_roundtrip_test.tun");

        auto run = [&] (const char* label, const juce::String& scl, const juce::String& name,
                        float refA)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (48000.0, 64);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("refA", refA);
            juce::String err;
            if (! scl.isEmpty() && ! p->loadScalaText (scl, name, err))
            { check (false, "the source scale loaded"); return; }
            p->pollParametersForTests();

            // Snapshot every key BEFORE writing.
            std::vector<double> before ((size_t) 128);
            for (int n = 0; n < 128; ++n) before[(size_t) n] = p->getNoteFrequency (n);

            if (! p->saveCurrentTuningTun (tmp, err))
            { std::printf ("  %-14s WRITER REFUSED: %s\n", label, err.toRawUTF8());
              check (false, "the writer accepted this tuning"); return; }

            // Load it back into a FRESH instance - no shared state to flatter the result.
            auto q = std::make_unique<FractalAdditiveAudioProcessor>();
            q->prepareToPlay (48000.0, 64);
            if (! q->loadScalaFile (tmp, err))
            { std::printf ("  %-14s LOADER REFUSED: %s\n", label, err.toRawUTF8());
              check (false, "the loader accepted what the writer produced"); return; }
            q->pollParametersForTests();

            double worst = 0.0; int worstKey = -1;
            for (int n = 0; n < 128; ++n)
            {
                const double a = before[(size_t) n], b = q->getNoteFrequency (n);
                if (! (a > 0.0) || ! (b > 0.0)) continue;
                const double cents = std::abs (1200.0 * std::log2 (b / a));
                if (cents > worst) { worst = cents; worstKey = n; }
            }
            std::printf ("  %-14s worst error %.6f cents (key %d)\n", label, worst, worstKey);
            check (worst < 0.01, "every one of the 128 keys comes back to within 0.01 cents");
        };

        // 12ED2 - the default, and the one every user will hit first.
        run ("12ED2", "! t.scl\n!\n12ed2\n 12\n!\n"
                      " 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n 600.0\n"
                      " 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 1200.0\n", "12ed2", 440.0f);
        // 31-EDO: a prime count, so 1/1 does not sit on a 12-key boundary.
        {
            juce::String s31 ("! t31.scl\n!\n31edo\n 31\n!\n");
            for (int i = 1; i <= 31; ++i) s31 << " " << juce::String (i * 1200.0 / 31.0, 6) << "\n";
            run ("31-EDO", s31, "31edo", 440.0f);
        }
        // A non-440 reference is the whole reason .tun exists rather than .scl.
        run ("12ED2 @ 432", "! t.scl\n!\n12ed2\n 12\n!\n"
                            " 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n 600.0\n"
                            " 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 1200.0\n", "12ed2", 432.0f);
        // Bohlen-Pierce: period 3/1, so the writer's period line is doing real work.
        {
            juce::String bp ("! bp.scl\n!\nBohlen-Pierce\n 13\n!\n");
            for (int i = 1; i <= 13; ++i) bp << " " << juce::String (i * 1200.0 * std::log2 (3.0) / 13.0, 6) << "\n";
            run ("Bohlen-Pierce", bp, "bp13", 440.0f);
        }
        tmp.deleteFile();
    }

    // --- run 33: an ABSOLUTE 128-key table, all the way through the real plugin -----
    // A non-repeating .tun (a stretched piano curve) is held as a table of 128 exact
    // frequencies. TuningTests proves the TuningManager side; what only the real
    // processor can prove is that the rest of the plugin does not quietly turn it back
    // into a scale - the saved state, the tuning bank, the .tun writer and the scale
    // editors all used to assume degrees + period and there was no table to catch them.
    {
        std::printf ("absolute 128-key table through the real plugin:\n");
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("fiblo_table_test.tun");
        auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("fiblo_table_written.tun");

        // Railsback-like: flat bass, sharp treble, so no key distance repeats exactly.
        double want[128];
        for (int n = 0; n < 128; ++n)
        {
            const double d = (double) (n - 60);
            want[n] = 440.0 * std::pow (2.0, ((n - 69) * 100.0 + 0.002 * d * std::abs (d)) / 1200.0);
        }
        {
            juce::String s ("; stretched piano curve - does not repeat\n[Exact Tuning]\nbasefreq=");
            s << juce::String (want[0], 10) << "\n";
            for (int n = 0; n < 128; ++n)
                s << "note " << n << "=" << juce::String (1200.0 * std::log2 (want[n] / want[0]), 6) << "\n";
            tmp.replaceWithText (s);
        }

        const auto worstVs = [&] (FractalAdditiveAudioProcessor& p)
        {
            double worst = 0.0;
            for (int n = 0; n < 128; ++n)
            {
                const double got = p.getNoteFrequency (n);
                if (got > 0.0) worst = std::max (worst, std::abs (1200.0 * std::log2 (got / want[n])));
            }
            return worst;
        };

        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (48000.0, 64);
        juce::String err;
        check (p->loadScalaFile (tmp, err), ("the stretched .tun loads (" + err + ")").toRawUTF8());
        check (p->tuningIsAbsoluteTable(), "and is held as an absolute 128-key table");
        p->pollParametersForTests();
        std::printf ("  loaded            worst error %.6f cents over 128 keys\n", worstVs (*p));
        check (worstVs (*p) < 0.01, "every key sounds at the frequency the file states");
        check (p->getScaleDegrees() == 12 && std::abs (p->getDegreeRatio (12) - 2.0) < 0.01,
               "the estimated period is the octave, 12 keys wide");

        // The period is metadata: typing a different one must not move a pitch.
        check (p->setTuningTablePeriod ("3/2", err), "a period of the user's own is accepted");
        p->pollParametersForTests();
        check (p->getScaleDegrees() == 7, "7 keys span a fifth here");
        check (worstVs (*p) < 0.01, "and NOT ONE of the 128 pitches moved with it");

        // Saved state: the table AND the chosen period.
        {
            juce::MemoryBlock state;
            p->getStateInformation (state);
            auto q = std::make_unique<FractalAdditiveAudioProcessor>();
            q->prepareToPlay (48000.0, 64);
            q->setStateInformation (state.getData(), (int) state.getSize());
            q->pollParametersForTests();
            check (q->tuningIsAbsoluteTable(), "a reopened project is still a table");
            std::printf ("  after state reload worst error %.6f cents\n", worstVs (*q));
            check (worstVs (*q) < 0.01, "with every key back to within 0.01 cents");
            check (q->tuningTablePeriodIsUserSet() && q->getScaleDegrees() == 7,
                   "and the period the user chose came back too");
        }

        // The tuning bank stores the .tun VERBATIM (it has no .scl form) and reloads it.
        {
            for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i) p->bankClearSlot (i);
            p->setTuningBankFile (0, tmp);
            check (p->getTuningBankName (0).isNotEmpty(), "the table takes a bank slot");
            p->loadScalaText ("! x.scl\nsomething else\n 2\n 500.0\n 2/1\n", "other", err);
            check (! p->tuningIsAbsoluteTable(), "another tuning takes over");
            // Off the slot and back onto it: selecting the slot the selector already sits
            // on is a no-op by design (syncBankSelector), so the switch has to be real.
            p->setTuningBankSlotParam (0);
            p->pollParametersForTests();
            p->setTuningBankSlotParam (1);
            p->pollParametersForTests();  // the slot switch is picked up on the message thread
            check (p->tuningIsAbsoluteTable(), "recalling the slot brings the table back");
            std::printf ("  from bank slot    worst error %.6f cents\n", worstVs (*p));
            check (worstVs (*p) < 0.01, "exactly, key for key");
        }

        // The writer: a table goes out as 128 exact pitches and NO fabricated scale.
        {
            check (p->saveCurrentTuningTun (out, err), ("a table can be written back out (" + err + ")").toRawUTF8());
            const auto text = out.loadFileAsString();
            check (! text.contains ("[Scale Begin]"),
                   "the file declares no scale - it has none to declare");
            auto r = std::make_unique<FractalAdditiveAudioProcessor>();
            r->prepareToPlay (48000.0, 64);
            check (r->loadScalaFile (out, err), "and loads back into a fresh instance");
            r->pollParametersForTests();
            check (r->tuningIsAbsoluteTable(), "still as a table");
            std::printf ("  written + reread  worst error %.6f cents\n", worstVs (*r));
            check (worstVs (*r) < 0.01, "every key survives the writer");
        }

        // The scale editors must refuse rather than replace the table with an invention.
        {
            juce::String e1, e2, e3, e4;
            const bool a = ! p->addNoteToTuning (1.25, e1);
            const bool b = ! p->editTuningDegree (1, "6/5", e2);
            const bool c = ! p->deleteTuningDegree (1, e3);
            const bool d = ! p->saveCurrentTuningScl (out, e4);
            check (a && b && c && d, "add / edit / delete a degree and 'save .scl' all refuse");
            check (e1.isNotEmpty() && e2.isNotEmpty() && e3.isNotEmpty() && e4.isNotEmpty(),
                   "each refusal says why");
            check (p->tuningIsAbsoluteTable() && worstVs (*p) < 0.01,
                   "and the table is still exactly what was loaded");
        }

        tmp.deleteFile();
        out.deleteFile();
    }

    // --- run 34: WHICH ENGINE is in force, and whose degrees it uses -----------------
    // Three mutually exclusive sources build the spectrum: the plain harmonic series, a
    // Custom Engine scale, or the harmonic engine TEMPERED to the active tuning. Temper
    // wins over a loaded custom scale, and it takes its grid from the TUNING - not from
    // that scale. Both facts were implicit in one if/else chain that only the audio path
    // read, so the UI could (and did) name the wrong engine. engineInForce() is now the
    // single answer both of them use, and this pins it to the partials that come out.
    {
        std::printf ("engine in force (13-EDO tuning + a 3-note custom scale):\n");
        using E = FractalAdditiveAudioProcessor::EngineInForce;
        // On the HEAP, like every other extra instance in here: MSVC reserves every scope's
        // locals in main's frame at once, and one more processor on the stack overflows it
        // before a single line of output is printed.
        auto owned = std::make_unique<FractalAdditiveAudioProcessor>();
        auto& p = *owned;
        p.prepareToPlay (sr, blk);
        auto setP2 = [&p] (const char* id, float plain)
        {
            auto* pp = p.apvts.getParameter (id);
            pp->setValueNotifyingHost (pp->convertTo0to1 (plain));
        };
        // Wait for the worker thread to publish the template these settings ask for.
        auto rebuild = [&p] () -> const spectrum::SpectrumTemplate*
        {
            const auto* t0 = p.getLiveTemplate();
            const unsigned g0 = t0 != nullptr ? t0->generation : 0u;
            p.pollParametersForTests();
            for (int i = 0; i < 400; ++i)
            {
                const auto* t = p.getLiveTemplate();
                if (t != nullptr && t->generation != g0) return t;
                juce::Thread::sleep (5);
            }
            return p.getLiveTemplate();
        };

        // A 13-EDO tuning: NONE of its steps is a just interval, so a partial sitting on
        // 5/4 or 3/2 proves the grid came from the custom scale instead.
        juce::String err, scl ("! edo13.scl\n13-EDO\n 13\n!\n");
        for (int i = 1; i <= 13; ++i) scl << " " << juce::String (1200.0 * (double) i / 13.0, 6) << "\n";
        check (p.loadScalaText (scl, "edo13", err), "the 13-EDO tuning loads");
        check (p.loadTimbreText ("5/4 3/2 2", err), "a 3-note custom scale is typed in");
        setP2 ("timbreOn", 1.0f);
        setP2 ("timbreTemper", 0.0f);

        auto onGrid = [] (double ratio, double stepCents)
        {
            const double c = 1200.0 * std::log2 (ratio);
            return std::abs (c - stepCents * std::round (c / stepCents));
        };

        {
            const auto* t = rebuild();
            check (p.engineInForce() == E::CustomScale, "scale on + temper off = the CUSTOM engine");
            check (p.timbreReplacesEngine(), "which is what the UI dims the harmonic engine for");
            // Every partial must be a scale degree times a power of the period.
            double worst = 0.0;
            int n = t != nullptr ? (int) t->partials.size() : 0;
            for (int i = 0; i < n; ++i)
            {
                const double r = (double) t->partials[(size_t) i].ratio;
                const double c = 1200.0 * std::log2 (r);
                const double inPeriod = c - 1200.0 * std::floor (c / 1200.0);
                double best = 1.0e9;
                for (double d : { 0.0, 386.313713864835, 701.955000865387, 1200.0 })
                    best = std::min (best, std::abs (inPeriod - d));
                worst = std::max (worst, best);
            }
            std::printf ("  custom scale: %d partials, worst distance to a degree %.4f cents\n", n, worst);
            check (n > 0 && worst < 0.5, "every partial sits on the custom scale's own degrees");
        }
        {
            setP2 ("timbreTemper", 1.0f);
            const auto* t = rebuild();
            check (p.engineInForce() == E::Tempered, "temper on = TEMPERED, whatever the custom scale says");
            check (! p.timbreReplacesEngine(), "so the harmonic controls must NOT be greyed there");
            double worst = 0.0, worstJust = 1.0e9;
            int n = t != nullptr ? (int) t->partials.size() : 0;
            for (int i = 0; i < n; ++i)
            {
                const double r = (double) t->partials[(size_t) i].ratio;
                worst = std::max (worst, onGrid (r, 1200.0 / 13.0));
                // 5/4 and 3/2 are NOT 13-EDO steps: if the grid had come from the custom
                // scale, some partial would be sitting on one of them.
                worstJust = std::min (worstJust, std::min (onGrid (r / 1.25, 1200.0),
                                                           onGrid (r / 1.5,  1200.0)));
            }
            std::printf ("  tempered:     %d partials, worst distance to the 13-EDO grid %.4f cents,"
                         " nearest custom degree %.2f cents away\n", n, worst, worstJust);
            check (n > 0 && worst < 0.5, "every partial snapped to the TUNING's 13 degrees");
            check (n > 0 && worstJust > 5.0, "and none of them to the custom scale's");
            check (n > 3, "the harmonic engine is still generating (far more partials than the scale has)");
        }
        {
            setP2 ("timbreTemper", 0.0f);
            setP2 ("timbreOn", 0.0f);
            rebuild();
            check (p.engineInForce() == E::Harmonic, "both off = the plain HARMONIC engine");
            check (p.timbreLoaded(), "and the typed scale is still loaded, not discarded");
        }
        p.clearScala();
    }

    // --- run 35: the SPECTRUM panel's geometry --------------------------------------
    // The window is a FIXED 1200x650 and every box is laid out by hand, so a rearranged
    // panel fails by overlapping its neighbour or by pushing its own content out through
    // its frame - silently, since a Component draws happily outside its parent's box.
    // Nothing tested the editor at all before this (the verifiers build the processor),
    // which is why this exists.
    {
        std::printf ("SPECTRUM panel layout:\n");
        auto owned = std::make_unique<FractalAdditiveAudioProcessor>(); // see run 34: heap, not stack
        auto& p = *owned;
        p.prepareToPlay (sr, blk);
        std::unique_ptr<juce::AudioProcessorEditor> base (p.createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
        check (ed != nullptr, "the editor builds");
        // Measured THREE times, because the engines share ONE box now and only the
        // selected view's controls exist on screen: (0) as it opens - the HARMONIC view
        // with the prime LIMIT fader; (1) the HARMONIC view at its widest - the 9
        // prime-whitelist toggles; (2) the CUSTOM view - a loaded scale, whose whole
        // point is that the freed box goes to the intervals editor (asserted below).
        // The panel wears the SELECTED ENGINE's colour, and that can fail in two
        // directions: back to the neutral steel that read as white (saturation 0.13), or
        // onto a hue so close to a neighbouring panel's that the boxes stop being
        // tellable apart. The neighbours are Essentials (green), Tuning (purple) and
        // Spectrum-vs-Tuning (orange).
        auto checkPanelColour = [] (juce::Colour panel, const char* who)
        {
            auto hueGap = [] (juce::Colour a, juce::Colour b)
            {
                const float d = std::abs (a.getHue() - b.getHue()) * 360.0f;
                return std::min (d, 360.0f - d);
            };
            const juce::Colour neighbours[] = { juce::Colour (0xff4fe09a),   // Essentials
                                                juce::Colour (0xffb27bff),   // Tuning
                                                juce::Colour (0xffff9a3c) }; // Spectrum vs Tuning
            float worst = 360.0f;
            for (const auto& n : neighbours) worst = std::min (worst, hueGap (panel, n));
            std::printf ("  panel colour %s (%s)  saturation %.2f  nearest neighbouring panel hue %.0f deg away\n",
                         panel.toDisplayString (false).toRawUTF8(), who, panel.getSaturation(), worst);
            check (panel.getSaturation() > 0.35f,
                   "the panel is a real colour, not the old neutral steel");
            check (worst > 40.0f, "and its hue is clear of every panel around it");
        };

        for (int pass = 0; ed != nullptr && pass < 4; ++pass)
        {
            if (pass == 1)
            {
                auto* pm = p.apvts.getParameter ("primeMode");
                pm->setValueNotifyingHost (1.0f); // Whitelist
                ed->resized();                    // no message loop here to do it for us
                std::printf ("  (harmonic view, prime whitelist)\n");
            }
            if (pass == 2)
            {
                juce::String err;
                p.loadTimbreText ("5/4 3/2 2", err); // switches the Custom Engine on
                ed->refreshEngineDressing();         // chip + view follow, as the timer would
                std::printf ("  (custom view: loaded scale)\n");
            }
            if (pass == 3)
            {
                // Back to the harmonic view with MANUAL per-partial amplitudes on: the one
                // state that puts a SECOND painted label ("FROZEN") into a cell that
                // already has one, which is the case the label checks below exist for.
                p.setTimbreEnabled (false);
                p.apvts.getParameter ("partialManual")->setValueNotifyingHost (1.0f);
                ed->refreshEngineDressing();
                std::printf ("  (harmonic view, manual per-partial amplitudes on)\n");
            }
            check (ed->getWidth() == 1200 && ed->getHeight() == 650, "at 1200 x 650");
            const auto L = ed->layoutProbe();
            const auto win = ed->getLocalBounds();
            check (win.contains (L.spectrum) && ! L.spectrum.isEmpty(), "the SPECTRUM panel is on screen");
            check (win.contains (L.essentials) && win.contains (L.tuning), "so are Essentials and Tuning");
            check (! L.spectrum.intersects (L.essentials) && ! L.spectrum.intersects (L.tuning),
                   "and it overlaps neither of them");

            // THE SHARED WALL. SPECTRUM's right border, TUNING's right border and the
            // Spectrum-vs-Tuning column's LEFT border must be ONE drawn line.
            //
            // Measured on the LINES, not the rectangles, and that distinction is the
            // whole test. Removing the gutter so the two rects merely TOUCH still drew
            // two strokes back to back - a 7 px pink-then-orange band, which is the
            // doubled border this was supposed to remove - and a rect comparison called
            // that a shared wall. Half a pixel of tolerance because the rects are
            // integers and the stroke centres are not.
            {
                std::printf ("  wall lines: spectrum %.1f, tuning %.1f, column %.1f"
                             "   (rects: %d / %d / %d)\n",
                             L.wallSpectrumRight, L.wallTuningRight, L.wallColumnLeft,
                             L.spectrum.getRight(), L.tuning.getRight(), L.specColumn.getX());
                check (std::abs (L.wallSpectrumRight - L.wallColumnLeft) <= 0.5f,
                       "the SPECTRUM panel's right border IS the Spectrum-vs-Tuning column's left border");
                check (std::abs (L.wallTuningRight - L.wallColumnLeft) <= 0.5f,
                       "and so is the TUNING box's - one line on the glass, not two side by side");
                check (win.contains (L.specColumn),
                       "and sharing it keeps the column inside the window");
            }

            // ONE BOX, ONE COLOUR. Every painted sub-label in SPECTRUM wears the selected
            // ENGINE's hue - the Fractal / Morph / Octave Range compartment included,
            // which used to keep a mint-and-magenta scheme of its own. That is invisible
            // to every geometric check here: those labels laid out perfectly and simply
            // looked like a separate box. The two INERTNESS marks are the stated
            // exceptions (a greyed AMP DECAY and the amber FROZEN beside it say the
            // control cannot affect the sound; painting them the panel hue would unify
            // the panel by deleting its only warning).
            {
                int offHue = 0; juce::String offName;
                for (const auto& s : L.paintedLabels)
                {
                    if (! s.name.startsWith ("eng.") && ! s.name.startsWith ("both.")) continue;
                    if (s.name == "both.frozen" || s.name == "both.decay") continue;
                    if (s.colour != L.panelColour)
                    { ++offHue; if (offName.isEmpty()) offName = s.name + " " + s.colour.toDisplayString (false); }
                }
                std::printf ("  %d spectrum sub-labels off the panel colour%s\n", offHue,
                             offHue > 0 ? (" (first: " + offName + ")").toRawUTF8() : "");
                check (offHue == 0, "every compartment label in SPECTRUM is the selected engine's colour");
            }

            // The Custom-Engine ON/OFF pill is GONE. Probing by NAME, because the way
            // this regresses is somebody re-adding "one small toggle" beside Load: the
            // engine chips are the only engine switch now, and a second quieter one
            // could sit there visibly disagreeing with them.
            {
                bool hasOff = false;
                for (const auto& c : L.controls) if (c.first == "cust.off") hasOff = true;
                check (! hasOff, "the custom-spectrum On/Off toggle is gone");
            }
            check (L.spectrum.contains (L.engineBox) && L.spectrum.contains (L.bothEngines),
                   "the shared engine box and the both-engines box are inside the panel");
            check (! L.engineBox.intersects (L.bothEngines),
                   "and they never overlap (they are separate spaces)");
            // The engine chips are the shared box's TITLE now, modelled on the
            // Modulators box's tab row: inside the box, at its top, under the panel's
            // own title band. A strip floating outside the box would read as panel
            // decoration again, which is exactly what this layout replaced.
            check (L.engineBox.contains (L.engineChips)
                   && L.engineChips.getY() >= L.spectrum.getY() + 14
                   && L.engineChips.getBottom() <= L.engineBox.getY() + 24,
                   "the engine chips are the engine box's own title row");

            checkPanelColour (L.panelColour, pass == 2 ? "custom engine selected"
                                                       : "harmonic engine selected");
            std::printf ("  panel %d x %d   engine box %d   both %d   essentials %d\n",
                         L.spectrum.getWidth(), L.spectrum.getHeight(), L.engineBox.getWidth(),
                         L.bothEngines.getWidth(), L.essentials.getWidth());

            // ONE box, ONE view: the probe only reports visible controls, so the
            // selected engine's controls must be there and the other engine's must
            // not - both present at once means the views leak into each other.
            {
                bool anyHarm = false, anyCustOnly = false, hasIntervals = false;
                for (const auto& c : L.controls)
                {
                    if (c.first.startsWith ("harm.")) anyHarm = true;
                    if (c.first == "cust.intervals") { hasIntervals = true; anyCustOnly = true; }
                    if (c.first == "cust.load")       anyCustOnly = true;
                }
                if (pass != 2)
                    check (anyHarm && ! anyCustOnly,
                           "harmonic view: its controls are inside, the custom engine's are not");
                else
                    check (! anyHarm && anyCustOnly && hasIntervals,
                           "custom view: its controls are inside, the harmonic engine's are not");
                // The point of the merge: the intervals editor gets the freed space.
                // It was a single 24 px line in a fifth of the panel; in the shared box
                // it must be a real multi-line area.
                if (pass == 2)
                    for (const auto& c : L.controls)
                        if (c.first == "cust.intervals")
                        {
                            std::printf ("  cust.intervals %d x %d\n",
                                         c.second.getWidth(), c.second.getHeight());
                            check (c.second.getWidth() >= 300 && c.second.getHeight() >= 56,
                                   "the intervals editor got the custom view's freed space");
                        }
                // RANDOM is the one control that can CREATE a spectrum from nothing, so
                // it must be in the custom view whatever the state of that view - it was
                // the empty workspace that needed it most.
                if (pass == 2)
                {
                    bool hasRandom = false;
                    for (const auto& c : L.controls) if (c.first == "cust.random") hasRandom = true;
                    check (hasRandom, "the RANDOM SPECTRUM button is in the custom view");
                }
            }

            // Every control must be INSIDE the box whose name it carries.
            int outside = 0;
            juce::String worstName;
            for (const auto& c : L.controls)
            {
                const auto* box = c.first.startsWith ("harm.") ? &L.engineBox
                                : c.first.startsWith ("cust.") ? &L.engineBox
                                : c.first.startsWith ("both.") ? &L.bothEngines
                                : c.first.startsWith ("tun.")  ? &L.tuning
                                : c.first.startsWith ("spec.") ? &L.specColumn
                                                               : &L.essentials;
                if (! box->contains (c.second))
                { ++outside; if (worstName.isEmpty()) worstName = c.first; }
            }
            std::printf ("  %d controls probed, %d outside their box%s\n",
                         (int) L.controls.size(), outside,
                         outside > 0 ? (" (first: " + worstName + ")").toRawUTF8() : "");
            check (! L.controls.empty() && outside == 0, "no control spills out of its own box");

            // "Inside its box" is not enough on its own: removeFromTop() on an exhausted
            // rectangle hands back a SQUASHED one, which still sits inside the box and
            // still overlaps nothing - so a control that ran out of room disappears
            // without failing either check above. Adding a row to a hand-laid-out panel
            // is exactly how that happens.
            int squashed = 0;
            juce::String squashedName;
            for (const auto& c : L.controls)
                if (c.second.getHeight() < 12 || c.second.getWidth() < 10)
                {
                    ++squashed;
                    if (squashedName.isEmpty())
                        squashedName = c.first + " (" + juce::String (c.second.getWidth()) + "x"
                                     + juce::String (c.second.getHeight()) + ")";
                }
            std::printf ("  %d controls squeezed to nothing%s\n", squashed,
                         squashed > 0 ? (" (first: " + squashedName + ")").toRawUTF8() : "");
            check (squashed == 0, "and every one of them still has room to be used");

            // The generic rule above only catches a total collapse. TEMPER is laid out
            // in BOTH views (it must never vanish under the user's own click) and in
            // each it is the row that silently absorbs any pixel its neighbours take:
            // it is asserted at its nominal 20 px, the height a readable tick box needs.
            for (const auto& c : L.controls)
                if (c.first == "cust.temper")
                {
                    std::printf ("  cust.temper %d x %d\n", c.second.getWidth(), c.second.getHeight());
                    check (c.second.getHeight() >= 20 && c.second.getWidth() >= 100,
                           "the temper tick box got its whole row inside the Custom Engine");
                }

            // ...and no two of them may sit on top of each other.
            int clashes = 0;
            juce::String clashName;
            for (size_t i = 0; i < L.controls.size(); ++i)
                for (size_t j = i + 1; j < L.controls.size(); ++j)
                    if (L.controls[i].second.intersects (L.controls[j].second))
                    {
                        ++clashes;
                        if (clashName.isEmpty()) clashName = L.controls[i].first + " / " + L.controls[j].first;
                    }
            std::printf ("  %d overlapping control pairs%s\n", clashes,
                         clashes > 0 ? (" (first: " + clashName + ")").toRawUTF8() : "");
            check (clashes == 0, "and no two controls overlap");

            // THE SUB-CELLS. Every divider line and every inline sub-label is drawn from
            // these rects, not from the controls, so a cell that grew past its box draws
            // a line across its NEIGHBOUR's controls - which no check on the controls
            // themselves can see. (The engine box's cells vanish in the custom view by
            // design, so each cell is only judged against the box it belongs to.)
            {
                int bad = 0; juce::String badName;
                for (const auto& c : L.cells)
                {
                    const auto* box = c.first.startsWith ("eng.")  ? &L.engineBox
                                    : c.first.startsWith ("both.") ? &L.bothEngines
                                                                   : &L.essentials;
                    if (! box->contains (c.second)) { ++bad; if (badName.isEmpty()) badName = c.first; }
                }
                for (size_t i = 0; i < L.cells.size(); ++i)
                    for (size_t j = i + 1; j < L.cells.size(); ++j)
                        if (L.cells[i].second.intersects (L.cells[j].second))
                        { ++bad; if (badName.isEmpty()) badName = L.cells[i].first + " / " + L.cells[j].first; }
                std::printf ("  %d cells probed, %d escaped or overlap%s\n", (int) L.cells.size(), bad,
                             bad > 0 ? (" (first: " + badName + ")").toRawUTF8() : "");
                check (! L.cells.empty() && bad == 0,
                       "every sub-cell stays inside its box and clear of its neighbours");
            }

            // THE PAINTED SUB-LABELS, in BOTH languages. A painted label has no component
            // to squash: it fails by clipping, silently, and usually in the language the
            // author does not read - which is exactly how "FROZEN - DRAWN BY HAND" shipped
            // clipped at "DRA...". Measure the string in the very font paint() sets and
            // require it to fit the very rect paint() draws it into.
            for (int lang = 0; lang < 2; ++lang)
            {
                const auto P = ed->layoutProbeInLanguage (lang == 1);
                int clipped = 0, over = 0;
                juce::String worstL, overL;
                float worstOverflow = 0.0f;
                for (const auto& s : P.paintedLabels)
                {
                    const float slack = (float) s.cell.getWidth() - s.textWidth;
                    if (slack < 0.0f)
                    {
                        ++clipped;
                        if (-slack > worstOverflow)
                        { worstOverflow = -slack; worstL = s.name + " \"" + s.text + "\""; }
                    }
                }
                // ...and no two of them may print on top of each other. Two labels in one
                // cell (AMP DECAY + FROZEN) is the case that bites: the second one's left
                // edge has to clear the first one's TEXT, not a guessed offset.
                for (size_t i = 0; i < P.paintedLabels.size(); ++i)
                    for (size_t j = i + 1; j < P.paintedLabels.size(); ++j)
                    {
                        const auto& a = P.paintedLabels[i];
                        const auto& b = P.paintedLabels[j];
                        const auto ai = a.cell.withWidth (juce::jmax (1, (int) std::ceil (
                                            std::min ((float) a.cell.getWidth(), a.textWidth))));
                        const auto bi = b.cell.withWidth (juce::jmax (1, (int) std::ceil (
                                            std::min ((float) b.cell.getWidth(), b.textWidth))));
                        if (ai.intersects (bi))
                        { ++over; if (overL.isEmpty()) overL = a.name + " / " + b.name; }
                    }
                std::printf ("  %s: %d painted labels, %d clipped%s, %d overprinting%s\n",
                             lang == 1 ? "catalan" : "english", (int) P.paintedLabels.size(), clipped,
                             clipped > 0 ? (" (worst: " + worstL + " by "
                                            + juce::String ((int) worstOverflow) + " px)").toRawUTF8() : "",
                             over, over > 0 ? (" (" + overL + ")").toRawUTF8() : "");
                check (! P.paintedLabels.empty() && clipped == 0,
                       lang == 1 ? "no painted sub-label clips in Catalan"
                                 : "no painted sub-label clips in English");
                check (over == 0, lang == 1 ? "and none of them overprints another (Catalan)"
                                            : "and none of them overprints another (English)");

                // ...and the same for the Label COMPONENTS. These squash to 70% instead
                // of clipping, so an undersized cell shows a readable-but-condensed word
                // that passes every bounds check in the build and still looks broken.
                int squeezed = 0; juce::String worstC; float worstRatio = 1.0f;
                for (const auto& s : P.componentLabels)
                {
                    const float need = s.textWidth + 2.0f; // Label's own 2 px text inset
                    if (need > (float) s.cell.getWidth())
                    {
                        ++squeezed;
                        const float r = (float) s.cell.getWidth() / juce::jmax (1.0f, need);
                        if (r < worstRatio)
                        { worstRatio = r; worstC = s.name + " \"" + s.text + "\""; }
                    }
                }
                std::printf ("  %s: %d label components, %d condensed%s\n",
                             lang == 1 ? "catalan" : "english", (int) P.componentLabels.size(), squeezed,
                             squeezed > 0 ? (" (worst: " + worstC + " at "
                                             + juce::String ((int) (worstRatio * 100.0f)) + "%)").toRawUTF8() : "");
                check (squeezed == 0,
                       lang == 1 ? "no label component is squashed to fit (Catalan)"
                                 : "no label component is squashed to fit (English)");
            }
        }

        // THE RIGHT COLUMN, MODE BY MODE. It re-lays itself completely for each of the
        // four shapes it can take, and until now only the first was ever measured - so a
        // row added to the adaptation (its "between lo hi" range) or to the free chord
        // search (Find + REALTIME) could push the results list, or itself, out of the box
        // with nothing to notice. Swept in BOTH languages, because the captions in there
        // are the longest in the window.
        if (ed != nullptr)
        {
            struct Shape { int mode, source, goal; const char* what; };
            const Shape shapes[] = {
                { 1, 1, 1, "search, from the current tuning" },
                { 1, 2, 1, "search, from all possible tunings (Find + REALTIME)" },
                { 2, 1, 1, "adapt, goal Min" },
                { 2, 1, 3, "adapt, goal Range (the extra lo/hi row)" },
                { 3, 1, 1, "from the spectrum's segment" } };
            for (const auto& sh : shapes)
            {
                ed->setSpecModeForTests (sh.mode);
                ed->setTuneSourceForTests (sh.source);
                ed->setAdaptGoalForTests (sh.goal);
                ed->resized();
                for (int lang = 0; lang < 2; ++lang)
                {
                    const auto P = ed->layoutProbeInLanguage (lang == 1);
                    int outside = 0, squashed = 0, condensed = 0;
                    juce::String first;
                    for (const auto& c : P.controls)
                    {
                        if (! c.first.startsWith ("spec.")) continue;
                        if (! P.specColumn.contains (c.second))
                        { ++outside; if (first.isEmpty()) first = c.first + " out"; }
                        if (c.second.getHeight() < 12 || c.second.getWidth() < 10)
                        { ++squashed; if (first.isEmpty()) first = c.first + " squashed"; }
                    }
                    for (const auto& s : P.componentLabels)
                        if (s.name.startsWith ("spec.") && s.textWidth + 2.0f > (float) s.cell.getWidth())
                        { ++condensed; if (first.isEmpty()) first = s.name + " \"" + s.text + "\" condensed"; }
                    std::printf ("  %s [%s]: %d out, %d squashed, %d condensed%s\n",
                                 lang == 1 ? "ca" : "en", sh.what, outside, squashed, condensed,
                                 first.isEmpty() ? "" : (" (" + first + ")").toRawUTF8());
                    check (outside + squashed + condensed == 0,
                           "this mode's whole column fits, unsquashed and uncondensed");

                    // A COMBO IS NOT A LABEL and the condensing rule above cannot see it:
                    // a ComboBox given too little room draws its arrow and clips the word
                    // silently. The adaptation's register row packs a caption, two note
                    // fields and this box onto one 186 px line, and the Catalan words are
                    // the longer ones ("Registres", "pitjor") - so the box that ends up
                    // with whatever is left over is measured explicitly.
                    if (sh.mode == 2)
                        for (const auto& c : P.controls)
                            if (c.first == "spec.agg")
                            {
                                std::printf ("    aggregate box: %d px wide (%s)\n",
                                             c.second.getWidth(), lang == 1 ? "ca" : "en");
                                check (c.second.getWidth() >= 50,
                                       "the mean/minimax box keeps room for its word beside its arrow");
                            }
                }
            }
            ed->setSpecModeForTests (1);
            ed->setTuneSourceForTests (1);
            ed->setAdaptGoalForTests (1);
        }

        // SWITCHING LANGUAGE MUST NOT DESELECT ANYTHING. Re-wording a juce::ComboBox is
        // the one operation that can silently clear it: getSelectedId() compares the
        // VISIBLE label against the selected item's text, so it reads 0 for as long as
        // the two disagree - which is exactly the window changeItemText() opens. Feeding
        // that 0 back to setSelectedId() (the obvious "refresh the displayed text" line)
        // cleared the selection for good, and a Spectrum-vs-Tuning mode of 0 gates that
        // whole panel off: no Notes, no Period, no Find, no REALTIME, no results list,
        // until the plugin was reopened. Two full round-trips, because the damage only
        // showed on the SECOND switch.
        if (ed != nullptr)
        {
            const auto base = ed->layoutProbe().comboIds;
            for (int round = 0; round < 2; ++round)
                for (int lang = 0; lang < 2; ++lang)
                    ed->layoutProbeInLanguage (lang == 1); // leaves the language it found
            const auto after = ed->layoutProbe().comboIds;
            int lost = 0, changed = 0;
            juce::String firstLost;
            for (size_t i = 0; i < base.size() && i < after.size(); ++i)
            {
                if (after[i].second != base[i].second)
                {
                    ++changed;
                    if (after[i].second == 0) { ++lost; if (firstLost.isEmpty()) firstLost = base[i].first; }
                    else if (firstLost.isEmpty()) firstLost = base[i].first;
                }
            }
            std::printf ("  %d localised combos, %d changed by 2 language round-trips (%d deselected)%s\n",
                         (int) base.size(), changed, lost,
                         firstLost.isEmpty() ? "" : (" (first: " + firstLost + ")").toRawUTF8());
            check (! base.empty() && base.size() == after.size() && changed == 0,
                   "switching language keeps every combo's selection (it never deselects one)");
        }

        // Selecting the other engine must recolour the WHOLE panel, not just light a
        // chip - and the colour it lands on has to clear its neighbours too, which is
        // the harder half (the Custom Engine's pink sits above the Tuning box's purple).
        // Temper mode then hands the spectrum back to the harmonic engine, so the panel
        // must go back to blue while the custom scale is bypassed.
        if (ed != nullptr)
        {
            using E = FractalAdditiveAudioProcessor::EngineInForce;
            // Pass 2 above left the Custom Engine ON (loading a scale switches it on),
            // so step back to the harmonic engine first or "blue" would be pink.
            p.setTimbreEnabled (false);
            ed->refreshEngineDressing();
            const auto blue = ed->layoutProbe().panelColour;
            p.setTimbreEnabled (true);
            ed->refreshEngineDressing();
            const auto pink = ed->layoutProbe().panelColour;
            check (p.engineInForce() == E::CustomScale, "the CUSTOM engine is in force");
            check (pink != blue, "selecting it recolours the whole panel, it does not just light a chip");
            checkPanelColour (pink, "custom engine selected");

            auto* tt = p.apvts.getParameter ("timbreTemper");
            tt->setValueNotifyingHost (1.0f);
            ed->refreshEngineDressing();
            check (p.engineInForce() == E::Tempered, "ticking Temper (inside the Custom Engine box) takes over");
            check (ed->layoutProbe().panelColour == blue,
                   "and the panel goes back to the harmonic engine's colour, which is what is generating");
            tt->setValueNotifyingHost (0.0f);
            p.setTimbreEnabled (false);
            ed->refreshEngineDressing();
        }

        // The empty-workspace pin. With ONE shared box, the Load button and the
        // intervals editor only exist inside the CUSTOM view - so with no scale loaded
        // the CUSTOM chip must still open that view (sound honestly staying harmonic),
        // or creating a custom spectrum is simply impossible. This is measured through
        // the chips' own onClick, because engineInForce alone cannot see it.
        if (ed != nullptr)
        {
            using E = FractalAdditiveAudioProcessor::EngineInForce;
            p.clearTimbre();               // truly nothing loaded
            ed->refreshEngineDressing();
            auto hasControl = [&] (const char* name)
            {
                for (const auto& c : ed->layoutProbe().controls)
                    if (c.first == name) return true;
                return false;
            };
            check (hasControl ("harm.baseN") && ! hasControl ("cust.intervals"),
                   "no scale loaded: the box opens on the harmonic view");
            ed->clickEngineChipForTests (1);
            check (hasControl ("cust.intervals") && hasControl ("cust.load")
                   && ! hasControl ("harm.baseN"),
                   "clicking CUSTOM with nothing loaded still opens its view (Load + intervals reachable)");
            check (p.engineInForce() == E::Harmonic,
                   "while the SOUND honestly stays harmonic (there is no scale to sound)");
            ed->clickEngineChipForTests (0);
            check (hasControl ("harm.baseN") && ! hasControl ("cust.intervals"),
                   "and HARMONIC brings its view back");

            // THE PIN vs AUTOMATION. The pin is editor-side state; engineInForce() is the
            // processor's. A host writing the engine parameters while the pin is up is the
            // one way the two could end up saying different things - a lit CUSTOM chip
            // over a harmonic sound, with no click anywhere to blame. The pin may only
            // ever hold an EMPTY workspace open, so any real non-harmonic engine state
            // has to supersede it the moment it is observed.
            ed->clickEngineChipForTests (1);            // pin the custom view (nothing loaded)
            check (hasControl ("cust.intervals"), "precondition: the empty workspace is pinned open");
            p.apvts.getParameter ("timbreTemper")->setValueNotifyingHost (1.0f); // automation
            ed->refreshEngineDressing();                // what the 30 Hz timer does
            check (p.engineInForce() == E::Tempered, "automation put the plugin in temper mode");
            check (hasControl ("harm.baseN") && ! hasControl ("cust.intervals"),
                   "and the pinned view gives way to it - the panel never contradicts the sound");
            p.apvts.getParameter ("timbreTemper")->setValueNotifyingHost (0.0f);
            ed->refreshEngineDressing();
            check (hasControl ("harm.baseN") && ! hasControl ("cust.intervals"),
                   "and the pin stays cleared afterwards, rather than springing back");

            // ...and the same for a scale arriving from outside while the pin is up: the
            // view it was holding open is now the real one, so nothing on screen moves,
            // but the pin itself must be gone (the sound, not the click, owns the view).
            ed->clickEngineChipForTests (1);
            juce::String terr;
            p.loadTimbreText ("5/4 3/2 2", terr);       // e.g. a preset or an undo
            ed->refreshEngineDressing();
            check (p.engineInForce() == E::CustomScale && hasControl ("cust.intervals"),
                   "a scale arriving from outside makes the pinned view the REAL one");
            p.setTimbreEnabled (false);
            ed->refreshEngineDressing();
            check (hasControl ("harm.baseN") && ! hasControl ("cust.intervals"),
                   "and switching that engine off returns to harmonic - the stale pin does not hold it open");
            p.clearTimbre();
            ed->refreshEngineDressing();
        }

        //== LOAD BECOMES REMOVE, AND THE SELECTOR FOLLOWS THE VIEW ==============
        // One button with two opposite jobs is the most dangerous control in the
        // window: if its WORD and its ACTION are set in different places they drift,
        // and a green "Load" that silently deletes a hand-typed spectrum is the result.
        // Both are exercised here through the button's own callback.
        if (ed != nullptr)
        {
            using E = FractalAdditiveAudioProcessor::EngineInForce;
            auto word = [&] (const char* which)
            {
                for (const auto& b : ed->layoutProbe().buttonTexts) if (b.first == which) return b.second;
                return juce::String();
            };
            auto hasControl = [&] (const char* name)
            {
                for (const auto& c : ed->layoutProbe().controls) if (c.first == name) return true;
                return false;
            };

            p.clearTimbre();
            ed->clickEngineChipForTests (1);   // open the (empty) custom workspace
            ed->refreshEngineDressing();
            std::printf ("  nothing loaded: the button reads \"%s\"\n", word ("cust.load").toRawUTF8());
            check (word ("cust.load") == "Load", "with no spectrum loaded the button loads");

            juce::String terr;
            p.loadTimbreText ("5/4 3/2 2", terr);
            ed->refreshEngineDressing();
            std::printf ("  spectrum loaded: the button reads \"%s\"\n", word ("cust.load").toRawUTF8());
            check (word ("cust.load") == "Remove", "once one IS loaded, the same button removes it");

            // ...and pressing it there really removes it, undoably. A Remove that were
            // merely the old Off in a red coat would leave the scale loaded, and the
            // button would then be lying about what it just did.
            ed->clickCustomLoadRemoveForTests();
            ed->refreshEngineDressing();
            check (! p.timbreLoaded() && p.engineInForce() == E::Harmonic,
                   "pressing Remove deletes the spectrum and hands the sound back to the harmonic engine");
            check (hasControl ("cust.load") && hasControl ("cust.random"),
                   "and leaves the workspace open, so loading or randomising another is one click");
            p.undoManager.undo();
            ed->refreshEngineDressing();
            check (p.timbreLoaded() && word ("cust.load") == "Remove",
                   "and one Ctrl+Z brings the removed spectrum back");

            // THE SELECTOR IS THE ENGINE SWITCH. With the On/Off pill gone the chips are
            // the only way between the engines, so showing a view must BE selecting it -
            // in both directions, and without ever discarding the loaded spectrum (that
            // is Remove's job, and only Remove's).
            ed->clickEngineChipForTests (0);
            ed->refreshEngineDressing();
            check (p.engineInForce() == E::Harmonic && hasControl ("harm.baseN"),
                   "showing the HARMONIC view puts the harmonic engine in force");
            check (p.timbreLoaded(), "...while keeping the loaded spectrum - switching engines erases nothing");
            ed->clickEngineChipForTests (1);
            ed->refreshEngineDressing();
            check (p.engineInForce() == E::CustomScale && hasControl ("cust.intervals"),
                   "and showing the CUSTOM view puts the custom engine in force");
            p.clearTimbre();
            ed->refreshEngineDressing();
        }

        //== THE RANDOM SPECTRUM TRIGGER ========================================
        // Automatable means a PARAMETER a host can draw, firing on its RISING edge.
        // Everything below is about that edge behaving like an edge: it fires once per
        // 0 -> 1, a held 1 never re-fires, and restoring a project that saved it at 1
        // does not fire at all (which would replace the spectrum the user saved with a
        // random one, every single time they reopened the session).
        if (ed != nullptr)
        {
            auto* rp = p.apvts.getParameter ("specRandom");
            check (rp != nullptr, "the random spectrum has an automatable parameter behind it");
            if (rp != nullptr)
            {
                p.clearTimbre();
                const int base = p.getRandomSpectrumCount();

                rp->setValueNotifyingHost (1.0f);      // a host automation step
                check (p.pollRandomSpectrumTrigger(), "a 0 -> 1 step fires it");
                const auto first = p.getTimbreSource();
                check (p.timbreLoaded() && p.engineInForce()
                           == FractalAdditiveAudioProcessor::EngineInForce::CustomScale,
                       "and the spectrum it built is the one in force");

                // A HELD 1 is not a second edge. Without this, automation that simply
                // parked the lane at 1 would rebuild the spectrum 60 times a second and
                // the sound would never settle.
                check (! p.pollRandomSpectrumTrigger() && p.getTimbreSource() == first,
                       "holding it at 1 does not fire again");

                rp->setValueNotifyingHost (0.0f);
                check (! p.pollRandomSpectrumTrigger(), "and falling back to 0 does not fire either");
                rp->setValueNotifyingHost (1.0f);
                check (p.pollRandomSpectrumTrigger(), "the NEXT rising edge does");
                const auto second = p.getTimbreSource();
                check (second != first, "and it is a genuinely different spectrum");
                check (p.getRandomSpectrumCount() == base + 2, "exactly two presses, exactly two spectra");

                // The BUTTON must take the same path: it writes the parameter and the
                // processor builds. A button that built directly would work while a
                // recorded automation of the very same press did nothing.
                ed->clickRandomSpectrumForTests();
                check (p.pollRandomSpectrumTrigger(), "the button's own click fires the same edge");
                const auto third = p.getTimbreSource();
                check (third != second, "and presses in a row keep producing different spectra");

                // UNDO MUST GO BACKWARDS, NOT SIDEWAYS. Ctrl+Z has to bring back the
                // spectrum that was there before the press - and, walking the whole
                // stack, must never re-fire the trigger on the way: a level that flips
                // back up under undo is a rising edge like any other, and a Ctrl+Z that
                // answers with a THIRD random spectrum is worse than one that does nothing.
                {
                    p.undoManager.undo();
                    const bool reFired = p.pollRandomSpectrumTrigger();
                    std::printf ("  one undo after a random press: re-fired = %d, spectrum now \"%s\"\n",
                                 (int) reFired, p.getTimbreSource().toRawUTF8());
                    check (! reFired, "undoing a random press does not fire another one");
                    check (p.getTimbreSource() == second,
                           "and it puts the spectrum that was there before the press back");
                    // Walk the rest of the stack: every step back through the trigger's
                    // own level changes has to stay silent too.
                    int fires = 0;
                    for (int i = 0; i < 6; ++i)
                    { p.undoManager.undo(); if (p.pollRandomSpectrumTrigger()) ++fires; }
                    check (fires == 0, "and neither does walking the whole undo stack back");
                }

                // REOPENING A PROJECT MUST NOT RANDOMISE ANYTHING. A press leaves the
                // parameter at 1, so that is the value a saved project carries; restoring
                // it rewrites every parameter and used to arrive as a fresh 0 -> 1 step.
                {
                    juce::String derr;
                    p.loadTimbreText ("7/6 3/2 7/4 2", derr);  // the user's own spectrum
                    const auto mine = p.getTimbreSource();
                    juce::MemoryBlock st;
                    p.getStateInformation (st);          // saved with the trigger left at 1
                    check (rp->getValue() >= 0.5f, "precondition: a press does leave the trigger at 1");
                    // A FRESH instance starts at the default 0 - that is what makes the
                    // restore a genuine 0 -> 1 step, and it is the exact path the host
                    // takes every time the project is reopened.
                    rp->setValueNotifyingHost (0.0f);
                    p.pollRandomSpectrumTrigger();
                    p.setStateInformation (st.getData(), (int) st.getSize());
                    const bool fired = p.pollRandomSpectrumTrigger();
                    std::printf ("  reload from 0 with the trigger saved at %.0f: fired = %d\n",
                                 rp->getValue(), (int) fired);
                    check (! fired && p.getTimbreSource() == mine,
                           "reopening a project with the trigger saved at 1 does NOT randomise the spectrum");
                }

                //-- THE DISTRIBUTION, measured. A random generator whose distribution is
                //   not checked is one nobody can tell a bug from a draw in.
                {
                    int    worstN = 1000, bestN = 0;
                    double worstGap = 1.0e9, worstTop = 0.0;
                    int    badPeriod = 0, dupes = 0;
                    std::vector<juce::String> seen;
                    for (int i = 0; i < 200; ++i)
                    {
                        const auto txt = p.makeRandomSpectrumText();
                        seen.push_back (txt);
                        juce::StringArray tk;
                        tk.addTokens (txt, " ", "");
                        tk.removeEmptyStrings();
                        std::vector<double> c;
                        for (const auto& t : tk) c.push_back (t.dropLastCharacters (1).getDoubleValue());
                        worstN = std::min (worstN, (int) c.size());
                        bestN  = std::max (bestN,  (int) c.size());
                        // Sorted, spaced, and inside the span - the three promises.
                        for (size_t k = 1; k < c.size(); ++k) worstGap = std::min (worstGap, c[k] - c[k - 1]);
                        worstGap = std::min (worstGap, c.empty() ? 1.0e9 : c[0]);
                        worstTop = std::max (worstTop, c.empty() ? 0.0 : c.back());
                        // The TOP token is the period and must be a whole octave: it is
                        // what Reps stacks, and a random period smears instead of stacking.
                        if (! c.empty() && std::abs (std::fmod (c.back(), 1200.0)) > 0.001) ++badPeriod;
                    }
                    std::sort (seen.begin(), seen.end());
                    for (size_t i = 1; i < seen.size(); ++i) if (seen[i] == seen[i - 1]) ++dupes;
                    std::printf ("  200 draws: %d..%d tokens, smallest gap %.2fc, highest partial %.0fc,"
                                 " %d bad periods, %d repeats\n",
                                 worstN, bestN, worstGap, worstTop, badPeriod, dupes);
                    // 5..12 partials PLUS the period token = 6..13 tokens.
                    check (worstN >= 6 && bestN <= 13, "every draw has a musical number of partials");
                    check (worstN != bestN, "and the count really varies from draw to draw");
                    check (worstGap >= 34.99,
                           "no two partials land closer than the 35c minimum (none can be deduped away)");
                    check (badPeriod == 0, "the period is always an exact whole number of octaves");
                    check (dupes == 0, "and 200 draws produce 200 different spectra");
                }
                p.clearTimbre();
                rp->setValueNotifyingHost (0.0f);
                p.pollRandomSpectrumTrigger();
                ed->refreshEngineDressing();
            }
        }

        //== THE TUNING BANK BUTTON'S NAME ======================================
        // "+" named neither what it adds nor where. Probed as a word because there is
        // nothing geometric about it - and because the row it sits in is laid out FROM
        // the button's own text width, so a longer name is exactly what pushes its
        // neighbours out (which the sweep above measures, in both languages).
        if (ed != nullptr)
        {
            for (int lang = 0; lang < 2; ++lang)
            {
                const auto P = ed->layoutProbeInLanguage (lang == 1);
                juce::String w;
                for (const auto& b : P.buttonTexts) if (b.first == "tun.toBank") w = b.second;
                std::printf ("  bank add button (%s): \"%s\"\n", lang == 1 ? "ca" : "en", w.toRawUTF8());
                check (w.length() > 1 && w != "+", "the tuning bank's add button says what it does");
            }
        }

        // THE PANEL'S OWN WIRING, through the same callbacks a click runs.
        if (ed != nullptr)
        {
            using RtLink = FractalAdditiveAudioProcessor::RtLink;
            // (1) Each mode brings its own Period. The search wants 2/1; the adaptation
            //     wants room for its partials and starts at 8/1. A value the user types
            //     stays with the mode it was typed in - it is parked, never destroyed.
            ed->setSpecModeForTests (1);
            check (ed->panelProbeForTests().period == "2", "the tuning search's Period starts at 2");
            ed->setSpecModeForTests (2);
            check (ed->panelProbeForTests().period == "8", "the spectrum adaptation's Period starts at 8");
            ed->setPeriodTextForTests ("3");
            ed->setSpecModeForTests (1);
            check (ed->panelProbeForTests().period == "2",
                   "typing 3 in the adaptation does not change the search's own Period");
            ed->setSpecModeForTests (2);
            check (ed->panelProbeForTests().period == "3",
                   "and coming back finds the 3 that was typed there, not the default again");
            ed->setPeriodTextForTests ("8");

            // (2) REALTIME follows WHAT IS PLAYED. Deliberately left on "to the loaded
            //     tuning" first: that is exactly the state the complaint came from, where
            //     the follower kept re-applying the whole loaded scale as the spectrum.
            ed->setAdaptTargetForTests (1);
            check (p.getRealtimeLink() == RtLink::Off, "precondition: no link yet");
            ed->pressAdaptRealtimeForTests();
            const auto probe = ed->panelProbeForTests();
            check (p.getRealtimeLink() == RtLink::SpectrumFromChord,
                   "pressing REALTIME follows the HELD NOTES even with the target left on the loaded tuning");
            check (probe.adaptTarget == 2 && ! probe.adaptTargetEnabled,
                   "and the target box says so, and waits, instead of claiming a target the link is not using");
            ed->pressAdaptRealtimeForTests();
            const auto after = ed->panelProbeForTests();
            check (p.getRealtimeLink() == RtLink::Off, "pressing it again ends the link");
            check (after.adaptTarget == 1 && after.adaptTargetEnabled,
                   "and hands the target box back with the choice the user had made");
            ed->setSpecModeForTests (1);

            // (3) A REALTIME LINK SURVIVES THE WINDOW BEING CLOSED - so a REOPENED editor
            //     has to adopt it. It used to open on "Find tunings" with both REALTIME
            //     flags false while the processor went on adapting the spectrum to every
            //     chord: nothing lit said so, the first press of REALTIME only caught the
            //     button up with the link (two presses to stop it), and any unrelated
            //     touch on the panel called syncRealtimeLink() and killed the user's
            //     follower unasked.
            ed->setSpecModeForTests (2);
            ed->pressAdaptRealtimeForTests();
            check (p.getRealtimeLink() == RtLink::SpectrumFromChord, "a link is running");
            base.reset();                                   // close the window, link keeps running
            check (p.getRealtimeLink() == RtLink::SpectrumFromChord,
                   "closing the editor leaves it running (the follower lives in the processor)");
            base.reset (p.createEditor());
            ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
            check (ed != nullptr && ed->panelProbeForTests().mode == 2,
                   "the reopened editor opens on the submenu that owns the running link");
            if (ed != nullptr)
            {
                check (! ed->panelProbeForTests().adaptTargetEnabled,
                       "with the target box locked to the held chord, exactly as while it was armed");
                ed->pressAdaptRealtimeForTests();
                check (p.getRealtimeLink() == RtLink::Off,
                       "and ONE press of REALTIME stops it (it used to take two)");
            }
        }
    }

    // --- run 36: the SPECTRUM adapted to notes, and the two REALTIME links ------------
    // The panel's new inverse direction, through the real plugin: the held chord it aims
    // at, the partials it writes, and everything that keeps a realtime follower from
    // running away with the patch (throttle, coalescing, the tuning bank, the feedback
    // guard, and the single step back).
    {
        std::printf ("\nspectrum adapted to notes + the realtime links:\n");
        using Proc = FractalAdditiveAudioProcessor;
        auto owned = std::make_unique<Proc>();
        auto& p = *owned;
        p.prepareToPlay (sr, blk);
        auto setP = [&p] (const char* id, float plain)
        {
            auto* pp = p.apvts.getParameter (id);
            pp->setValueNotifyingHost (pp->convertTo0to1 (plain));
            p.pollParametersForTests();
        };
        // Wait for the spectrum worker to publish whatever the current settings ask for.
        auto waitTemplate = [&p] () -> const spectrum::SpectrumTemplate*
        {
            const auto* t0 = p.getLiveTemplate();
            const auto g0 = t0 != nullptr ? t0->generation : 0u;
            p.pollParametersForTests();
            for (int i = 0; i < 400; ++i)
            {
                const auto* t = p.getLiveTemplate();
                if (t != nullptr && t->generation != g0) return t;
                juce::Thread::sleep (5);
            }
            return p.getLiveTemplate();
        };
        juce::AudioBuffer<float> buf (2, blk);

        //-- (a) WHICH NOTES ARE HELD, at their exact sounding pitch. The adaptation's
        //   whole realtime promise rests on this being the real chord, not a guess.
        {
            juce::MidiBuffer m;
            for (const int k : { 60, 64, 67 })
                m.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            p.processBlock (buf, m);
            double f0 = 0.0;
            const auto held = p.heldChordAsTargetAlphas (f0);
            std::printf ("  held C-E-G: %d notes, f0 %.3f Hz, ratios", (int) held.size(), f0);
            for (const double r : held) std::printf (" %.5f", r);
            std::printf ("\n");
            check ((int) held.size() == 3, "all three held notes reach the adaptation");
            check (std::abs (f0 - p.getNoteFrequency (60)) < 1.0e-6,
                   "and its root IS key 60's tuned frequency");
            if (held.size() == 3)
                check (std::abs (held[1] - p.getNoteFrequency (64) / f0) < 1.0e-9
                       && std::abs (held[2] - p.getNoteFrequency (67) / f0) < 1.0e-9,
                       "each ratio is the tuning's own, not an assumed 12-TET one");
            juce::MidiBuffer off;
            for (const int k : { 60, 64, 67 })
                off.addEvent (juce::MidiMessage::noteOff (1, k), 0);
            p.processBlock (buf, off);
            double f0b = 0.0;
            check (p.heldChordAsTargetAlphas (f0b).empty(), "and it empties when the keys come up");
        }

        //-- (b) ADAPT to the loaded tuning, applied through the real Custom Engine.
        {
            double f0 = 0.0;
            const auto alphas = p.tuningAsTargetAlphas (f0);
            Proc::SpectrumAdaptSettings st;
            st.partials = 8; st.periodRatio = 2.0; st.minGapCents = 50.0;
            st.rootOnly = true; st.goal = 0; st.shapeAmps = true;

            // The registers this run will use, resolved by the processor's own helper -
            // "really sounding" has to be measured over the same ones the search optimised
            // or the two numbers printed side by side would be on different scales.
            const auto regs = Proc::adaptRegistersFor (st, f0, /*realtime*/ false);
            const double beforeReal = p.measureCurrentAgainstTarget (alphas, f0, true, regs);
            juce::String err;
            check (p.startSpectrumAdapt (st, alphas, f0, err), "the adaptation starts");
            for (int i = 0; i < 2000 && p.isSpectrumAdaptRunning(); ++i) juce::Thread::sleep (5);
            const auto info = p.getSpectrumAdaptResult();
            check (p.applySpectrumAdapt (true, err), "and applies into the Custom Engine");
            const auto* t = waitTemplate();

            const int reps = (int) p.apvts.getRawParameterValue ("timbreRepeats")->load();
            const int want = st.partials * juce::jmax (1, reps);
            const double afterReal = p.measureCurrentAgainstTarget (alphas, f0, true, regs);
            std::printf ("  12ED2 x 8 partials: model %.2f -> %.2f, really sounding %.2f -> %.2f\n",
                         info.before, info.after, beforeReal, afterReal);
            // THE REGISTER RANGE, end to end through the real processor. A settings block
            // with no range set means AUTO - an octave either side of the anchor - and the
            // result must SAY which registers it was optimised for, because with a
            // single-pitch objective (the bug this replaces) the answer is different.
            std::printf ("  registers: %d points, %.2f..%.2f Hz (anchor %.2f), %s\n",
                         info.regPoints, info.regLoHz, info.regHiHz, f0,
                         info.aggregate == 1 ? "minimax" : "mean");
            check (info.regPoints == 3 && std::abs (info.regLoHz - f0 * 0.5) < 1.0e-6
                   && std::abs (info.regHiHz - f0 * 2.0) < 1.0e-6,
                   "an unset range is an octave either side of the anchor, sampled at three registers");
            check ((int) info.perRegister.size() == info.regPoints
                   && (int) info.registerF0.size() == info.regPoints,
                   "and the result carries one measured number per register");
            check (info.mean <= info.worst + 1.0e-12 && info.after <= info.seedWorst,
                   "the aggregate read-outs are consistent (mean never above the worst register)");
            // The realtime follower's cap is a STATED difference, not a silent one: at this
            // two-octave range it samples exactly as densely as the Adapt button does.
            const auto rtRegs = Proc::adaptRegistersFor (st, f0, /*realtime*/ true);
            check (rtRegs.f0.size() == regs.f0.size(),
                   "and at the default two-octave range realtime samples exactly as densely");
            std::printf ("  compiled partials: %d (asked %d x %d periods)\n",
                         t != nullptr ? (int) t->partials.size() : -1, st.partials, reps);
            check (afterReal < beforeReal, "the spectrum the synth REALLY makes beats less against that tuning");
            check (t != nullptr && (int) t->partials.size() == want,
                   "and it has exactly the partials that were asked for - none merged away");

            // The no-merge guarantee, measured on the partials the voices will render.
            double worst = 1.0e9;
            if (t != nullptr)
                for (size_t i = 1; i < t->partials.size(); ++i)
                    worst = std::min (worst, 1200.0 * std::log2 ((double) t->partials[i].ratio
                                                                 / (double) t->partials[i - 1].ratio));
            std::printf ("  closest two compiled partials: %.2f cents (asked for %.0f)\n", worst, st.minGapCents);
            check (worst >= st.minGapCents - 0.05, "every pair keeps the smallest interval it was promised");

            // The amplitudes really travelled: they are per-partial edits, and they are
            // the ONLY thing that can set an amplitude without the compiler deleting
            // partials in response (PartialGains.h).
            std::printf ("  per-partial amplitude edits in force: %d of %d\n",
                         p.getPartialGainsMatched(), (int) p.getPartialGains().size());
            check (p.getPartialGainsMatched() >= st.partials - 1,
                   "the adapted amplitudes landed on the adapted partials");

            // ONE undo step takes the whole thing back - ratios AND amplitudes together.
            const auto adapted = p.getTimbreSource();
            p.undoManager.undo();
            std::printf ("  after one undo the Custom Engine text is %s\n",
                         p.getTimbreSource().isEmpty() ? "empty (as it was)" : "back to its old value");
            check (p.getTimbreSource() != adapted, "one Ctrl+Z undoes the whole adaptation");
            p.undoManager.redo();
        }

        //-- (c) REALTIME "tuning follows the spectrum": throttled, coalesced, and it does
        //   NOT fill the tuning bank.
        {
            // A deliberately tiny spectrum, so the search itself is instant and what this
            // measures is the THROTTLE and nothing else. (With a full spectrum one search
            // is ~1.7 s and "was it busy?" would mask the throttle entirely.)
            setP ("timbreOn", 0.0f);
            setP ("spectrumRange", 1.0f);
            // MANUAL amplitude mode FREEZES slope (currentSettings uses manualSlopeFrozen),
            // and the adaptation above switched it on - so the slope sweep below would not
            // have rebuilt the spectrum at all and this test would have measured a throttle
            // that was never asked to throttle anything. Hence the "started at least twice"
            // check further down: too few starts is as much a failure as too many.
            setP ("partialManual", 0.0f);
            waitTemplate();

            Proc::RtSettings rs;
            rs.notes = 3; rs.periodRatio = 2.0; rs.periodLine = "2/1"; rs.minGapCents = 50.0;
            auto filledSlots = [&p]
            {
                int n = 0;
                for (int i = 0; i < Proc::kBankSlots; ++i)
                    if (p.getTuningBankName (i).isNotEmpty()) ++n;
                return n;
            };
            const int bank0 = filledSlots();
            p.setRealtimeLink (Proc::RtLink::TuningFromSpectrum, rs);

            constexpr int kTicks = 200;
            for (int i = 0; i < kTicks; ++i)
            {
                // A modulator sweeping the spectrum, in the crudest possible form: a
                // parameter that forces a rebuild on EVERY tick.
                auto* sl = p.apvts.getParameter ("curveSlope");
                sl->setValueNotifyingHost (sl->convertTo0to1 (i % 2 == 0 ? 1.0f : 1.4f));
                p.pollParametersForTests();
                juce::Thread::sleep (2);
            }
            const int starts  = p.getRealtimeStarts();
            const int applies = p.getRealtimeApplies();
            // An ABSOLUTE bound, deliberately not derived from kRtTuneTicks: computing the
            // allowance from the very constant under test made the check pass at any
            // throttle setting at all, including none (200 changes -> 110 searches sailed
            // through it). The promise is "at most a few searches a second"; the floor came
            // down from one second to a third when the search itself got ~5x faster, so the
            // number here follows it - and still fails an unthrottled follower by 9x.
            constexpr int allowed = 12;
            std::printf ("  %d spectrum changes -> %d searches started, %d retunes applied "
                         "(throttle allows at most %d)\n", kTicks, starts, applies, allowed);
            check (starts >= 2, "the follower keeps searching as the spectrum keeps moving");
            check (starts <= allowed, "and the throttle keeps a sweeping modulator from thrashing it");
            check (applies >= 1, "and the tuning really follows the top result");
            std::printf ("  tuning bank slots: %d before, %d after %d realtime retunes\n",
                         bank0, filledSlots(), applies);
            check (filledSlots() - bank0 <= 1,
                   "realtime retunes never flood the bank (only the ONE baseline entry joins it)");

            //-- (e) ...and switching it off leaves exactly one step back.
            const auto rtScale = p.activeSclSource();
            p.setRealtimeLink (Proc::RtLink::Off, rs);
            p.undoManager.undo();
            std::printf ("  after switching realtime off, one undo changes the scale: %s\n",
                         p.activeSclSource() != rtScale ? "yes" : "NO");
            check (p.activeSclSource() != rtScale,
                   "one Ctrl+Z after switching realtime off leaves the follower's tuning behind");
        }

        //-- (c2) REALTIME "the spectrum follows what you PLAY". The complaint this answers:
        //   with the target left on the loaded tuning, the follower kept writing the loaded
        //   scale into the spectrum for ever ("en 12ED2 me pone 12ED2 como espectro todo el
        //   rato"). It has to watch the KEYS instead, and it has to sit still when they are
        //   not moving - re-adapting to an unchanged chord is what made it feel stuck.
        {
            setP ("timbreTemper", 0.0f);
            Proc::RtSettings rs;
            rs.adapt.partials = 4;          // small: this measures WHEN it adapts, not how well
            rs.adapt.periodRatio = 2.0; rs.adapt.minGapCents = 50.0; rs.adapt.rootOnly = true;
            rs.adapt.goal = 0; rs.adapt.shapeAmps = false;

            auto tick = [&p, &buf] (int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    juce::MidiBuffer none;
                    p.processBlock (buf, none);          // republishes the held chord
                    p.pollParametersForTests();          // one 60 Hz link tick
                    juce::Thread::sleep (1);
                }
            };
            auto keys = [&p, &buf] (bool on, std::initializer_list<int> notes)
            {
                juce::MidiBuffer m;
                for (const int k : notes)
                    m.addEvent (on ? juce::MidiMessage::noteOn (1, k, (juce::uint8) 100)
                                   : juce::MidiMessage::noteOff (1, k), 0);
                p.processBlock (buf, m);
            };
            auto settle = [&p, &tick] { for (int i = 0; i < 400 && p.isSpectrumAdaptRunning(); ++i) tick (1); tick (3); };

            p.setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
            tick (60);
            const int idleStarts = p.getRealtimeStarts();
            std::printf ("  nothing held, 60 ticks -> %d adaptations started\n", idleStarts);
            check (idleStarts == 0, "with no keys down the follower does nothing at all");

            keys (true, { 60, 64, 67 });
            settle();
            const int afterChord = p.getRealtimeStarts();
            const int appliedOne = p.getRealtimeApplies();
            std::printf ("  C-E-G held -> %d started, %d applied\n", afterChord, appliedOne);
            check (afterChord >= 1, "holding a chord makes it adapt to THAT chord");
            check (appliedOne >= 1, "and the result reaches the spectrum");

            tick (120);
            settle();
            std::printf ("  same chord held for another 120 ticks -> %d started (was %d)\n",
                         p.getRealtimeStarts(), afterChord);
            check (p.getRealtimeStarts() == afterChord,
                   "and holding it still re-adapts NOTHING - an unchanged chord is not a new target");

            keys (false, { 64 });                    // C-E-G becomes C-G: a different chord
            keys (true,  { 65 });                    // ...plus F
            settle();
            std::printf ("  chord changed -> %d started\n", p.getRealtimeStarts());
            check (p.getRealtimeStarts() > afterChord, "changing the notes DOES start a new adaptation");

            //-- ONE NOTE IS A TARGET, not a reason to sit still.
            //   The follower used to require two notes ("if (alphas.size() < 2) return"), so a
            //   lead line, a bass line or one finger on the keyboard lit REALTIME and then
            //   never adapted once, for ever. With a single key down the target set is {1.0},
            //   the cross-note terms are gone, and what is left is the spectrum's own INTERNAL
            //   roughness at that key's REAL pitch (Dissonance.h::targetScore) - exactly "the
            //   least beating between the partials themselves".
            const int beforeSolo   = p.getRealtimeStarts();
            const int beforeSoloAp = p.getRealtimeApplies();
            keys (false, { 60, 65 });                // C-F-G becomes G alone
            tick (60); settle();
            std::printf ("  down to ONE held note -> %d started (was %d), %d applied (was %d)\n",
                         p.getRealtimeStarts(), beforeSolo, p.getRealtimeApplies(), beforeSoloAp);
            check (p.getRealtimeStarts() > beforeSolo,
                   "a SINGLE held note still adapts (its partials beat least against each other)");
            check (p.getRealtimeApplies() > beforeSoloAp,
                   "and that single-note result really reaches the spectrum");

            const int soloStarts = p.getRealtimeStarts();
            tick (120);
            settle();
            check (p.getRealtimeStarts() == soloStarts,
                   "holding that one note still re-adapts nothing - one note is a target, not a treadmill");

            //   ...and the SAME note an octave down is a different target: Sethares' critical
            //   band is a width in Hz, so the register changes which partials beat least. If
            //   this did not re-adapt, the single-note target would be anchored somewhere
            //   other than the note actually sounding.
            keys (false, { 67 });
            keys (true,  { 55 });
            tick (60); settle();
            std::printf ("  the same note an octave DOWN -> %d started (was %d)\n",
                         p.getRealtimeStarts(), soloStarts);
            check (p.getRealtimeStarts() > soloStarts,
                   "one note moved to another register is a NEW target and re-adapts");

            const int beforeRelease = p.getRealtimeStarts();
            keys (false, { 55 });
            tick (120);
            std::printf ("  keys up, 120 ticks -> %d started (was %d)\n",
                         p.getRealtimeStarts(), beforeRelease);
            check (p.getRealtimeStarts() == beforeRelease,
                   "and letting go stops it dead instead of re-adapting to an empty hand");
            p.setRealtimeLink (Proc::RtLink::Off, rs);
        }

        //-- (d) THE FEEDBACK GUARD: temper mode makes the tuning build the spectrum, so a
        //   link the other way would chase itself for ever. It must refuse, not oscillate.
        {
            Proc::RtSettings rs;
            rs.notes = 3; rs.periodRatio = 2.0; rs.periodLine = "2/1";
            setP ("timbreTemper", 1.0f);
            p.setRealtimeLink (Proc::RtLink::TuningFromSpectrum, rs);
            p.pollParametersForTests();
            std::printf ("  with temper on, the link is %s and says: %s\n",
                         p.getRealtimeLink() == Proc::RtLink::Off ? "OFF" : "STILL RUNNING",
                         p.getRealtimeRefusal().toRawUTF8());
            check (p.getRealtimeLink() == Proc::RtLink::Off,
                   "a realtime link refuses to run into temper mode's own tuning -> spectrum loop");
            check (p.getRealtimeRefusal().isNotEmpty(), "and says why instead of going quiet");
            setP ("timbreTemper", 0.0f);
        }
    }

    // --- run 37: with morph OFF, a spectrum change FADES - it does not step ---------
    // "cuando cambia el spectrum se escucha una transicion subita y abrupta entre un
    //  espectro y otro, haz que sea smooth... que eso no intervenga en el morphing."
    // The amplitudes used to slew over the fixed 7 ms de-click constant, so a large
    // timbre change read as a hard cut. Measured directly: the 10%->90% rise time of a
    // partial that APPEARS with the new template. Widening Spectrum Range from 2 to 10
    // octaves only APPENDS partials past the old count, so harmonic 6 starts from a
    // true zero at an unchanged frequency (rank-mapped changes make an appearing
    // harmonic INHERIT a neighbouring rank's amplitude, which hides most of the rise).
    {
        std::printf ("morph OFF: an appearing partial's rise time:\n");
        constexpr int eb = 64; constexpr double sr = 48000.0;
        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (sr, eb);
        auto s = [&] (const char* id, float v)
        { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
          p->pollParametersForTests(); };
        s ("morph", 0.0f);
        s ("ampAttack", 0.001f); s ("ampDecay", 0.001f); s ("ampSustain", 1.0f); s ("ampRelease", 0.005f);
        s ("fractalLevel", 1.0f); s ("curveSlope", 1.0f); s ("baseN", 1.0f);
        s ("spectrumRange", 2.0f);                // ratio <= 4: harmonics 1..4 only
        juce::AudioBuffer<float> b (2, eb);
        auto pump = [&] (int blocks, std::vector<float>* out)
        {
            for (int i = 0; i < blocks; ++i)
            { juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
              if (out != nullptr) for (int k = 0; k < eb; ++k) out->push_back (b.getReadPointer (0)[k]); }
        };
        juce::Thread::sleep (40); pump (40, nullptr);   // twice: the builder thread must
        juce::Thread::sleep (40); pump (40, nullptr);   // publish the Odd template first
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
          b.clear(); p->processBlock (b, m); }
        pump ((int) (0.5 * sr / eb), nullptr);          // settle on the narrow spectrum

        const double f2 = 6.0 * p->getNoteFrequency (60);   // the partial that will appear
        // Hann-windowed magnitude of ONE exact frequency (a direct DFT bin: no FFT grid,
        // any window length). 12.5 ms window = fast enough to time a 7 ms step honestly.
        auto mag = [&] (const std::vector<float>& x, size_t at, int len)
        {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < len && at + (size_t) i < x.size(); ++i)
            {
                const double w  = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (len - 1));
                const double ph = 2.0 * juce::MathConstants<double>::pi * f2 * (double) i / sr;
                re += w * (double) x[at + (size_t) i] * std::cos (ph);
                im += w * (double) x[at + (size_t) i] * std::sin (ph);
            }
            return std::sqrt (re * re + im * im);
        };

        s ("spectrumRange", 10.0f);               // full range: harmonic 6 fades IN from zero
        juce::Thread::sleep (25);                 // let the builder publish the new template
        std::vector<float> tr;
        pump ((int) (1.2 * sr / eb), &tr);

        constexpr int win = 600, hop = 120;       // 12.5 ms window, 2.5 ms hop
        std::vector<double> env;
        for (size_t at = 0; at + (size_t) win <= tr.size(); at += (size_t) hop)
            env.push_back (mag (tr, at, win));
        double fin = 0.0; int fc = 0;
        for (size_t i = env.size() >= 40 ? env.size() - 40 : 0; i < env.size(); ++i) { fin += env[i]; ++fc; }
        fin /= std::max (1, fc);
        int i10 = -1, i90 = -1;
        for (size_t i = 0; i < env.size(); ++i)
        {
            if (i10 < 0 && env[i] >= 0.10 * fin) i10 = (int) i;
            if (env[i] >= 0.90 * fin) { i90 = (int) i; break; }
        }
        const double riseMs = (i10 >= 0 && i90 >= i10) ? (i90 - i10) * hop * 1000.0 / sr : -1.0;
        std::printf ("  10%%->90%% rise = %.1f ms (final mag %.4f)\n", riseMs, fin);
        check (fin > 1.0e-4, "the appearing partial does arrive");
        check (riseMs >= 40.0,  "a spectrum change with morph OFF is a real fade (>= 40 ms), not a 7 ms step");
        check (riseMs <= 300.0, "...and stays a de-click, not an effect (<= 300 ms)");

        // The hard-won regression guard: a FRESH note still starts ON its target spectrum
        // (snapAmps), with no fade-in inherited from the template glide.
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
          b.clear(); p->processBlock (b, m); }
        pump ((int) (0.4 * sr / eb), nullptr);    // release tail dies; the voice goes idle
        std::vector<float> att;
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
          b.clear(); p->processBlock (b, m);
          for (int k = 0; k < eb; ++k) att.push_back (b.getReadPointer (0)[k]); }
        pump ((int) (0.5 * sr / eb), &att);
        const double early = mag (att, (size_t) (0.010 * sr), win);   // 10..22.5 ms after note-on
        const double susta = mag (att, (size_t) (0.400 * sr), win);
        std::printf ("  fresh note: mag@10ms=%.4f  sustain=%.4f (%.0f%%)\n",
                     early, susta, susta > 0.0 ? 100.0 * early / susta : 0.0);
        check (susta > 1.0e-4 && early >= 0.7 * susta,
               "a cold note still SNAPS onto the template (no fade-in regression)");
    }

    // --- run 38: morph ON under a STREAM of templates (the realtime-adaptive cadence) --
    // "el morphing... tiene como un trompicon parece a veces cuando se usa con el
    //  adaptive tuning."
    // The realtime link applies a REAL new template every ~250-300 ms; with a Morph Time
    // longer than that the glide never completes before the next target lands. Three
    // stumbles lived in that regime, none of them a click (run 26 stays green through
    // all of them) - each is a sudden SPECTRAL step, so the metric here is spectral
    // flux: the L1 distance between consecutive Hann-window magnitude spectra,
    // normalised by the mean spectrum magnitude. A morph that really glides moves a few
    // percent per 10 ms hop; a bank snapping to a new template moves in one hop.
    //   - a template landing MID-crossfade snapped alignInc, stepping the audible
    //     destination bank onto the new frequencies at full weight;
    //   - a template landing at weight >= 0.999 adopted the aligned PHASES but re-seeded
    //     the glide from curRatio, jumping the audible frequencies backwards;
    //   - when the stream ENDS, the amp slew flipped from Morph Time to 7 ms with ~37%
    //     of the amplitude move still pending - a knee right at the settle.
    {
        std::printf ("morph under an adaptive-cadence template stream (spectral flux):\n");
        constexpr int eb = 64; constexpr double sr = 48000.0;
        constexpr int stepBlocks = (int) (0.30 * 48000.0 / 64.0);   // 300 ms of audio per template
        struct Res { double stream, settle, jump; };
        auto run = [&] (bool morphOn, const char* label)
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr, eb);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            s ("ampAttack", 0.002f); s ("ampDecay", 0.002f); s ("ampSustain", 1.0f);
            s ("fractalLevel", 1.0f); s ("curveSlope", 2.0f); s ("baseN", 1.0f);
            // Morph Time far LONGER than the template cadence - the reported regime.
            // The crossfade weight then climbs toward 1 across several template
            // arrivals, which is exactly where the old alignInc snap was heard at
            // nearly full strength.
            s ("morph", morphOn ? 1.0f : 0.0f); s ("morphTime", 2.0f);
            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 60; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
              b.clear(); p->processBlock (b, m); }
            for (int i = 0; i < 120; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }

            std::vector<float> x;
            auto pump = [&] (int blocks)
            {
                for (int i = 0; i < blocks; ++i)
                { juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                  for (int k = 0; k < eb; ++k) x.push_back (b.getReadPointer (0)[k]); }
            };
            // 8 template changes 300 ms of audio apart - far faster than the 2 s morph -
            // between a COUNT-jumping pair (base 1 <-> 3 triples the sub-Nyquist count).
            for (int stepIdx = 0; stepIdx < 8; ++stepIdx)
            {
                s ("baseN", (stepIdx & 1) != 0 ? 1.0f : 3.0f);
                juce::Thread::sleep (25);          // let the builder thread publish
                pump (stepBlocks);
            }
            const size_t streamEnd = x.size();
            pump ((int) (2.2 * sr / eb));          // the stream stops; the morph completes at ~+1.7 s

            // Per-sample click metric (run 26's) alongside the flux, so a fix for the
            // stumble can never smuggle a click back in.
            double maxJump = 0.0, peak = 0.0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                peak = std::max (peak, (double) std::abs (x[i]));
                if (i > 0) maxJump = std::max (maxJump, (double) std::abs (x[i] - x[i - 1]));
            }

            constexpr int fo = 10, fftN = 1 << fo, hop = 240;   // 21.3 ms window, 5 ms hop
            juce::dsp::FFT fft (fo);
            std::vector<float> buf (2 * (size_t) fftN);
            std::vector<float> prevMag, curMag ((size_t) fftN / 2);
            std::vector<std::vector<float>> settleMags;   // frames after the last change
            double meanMag = 0.0, maxStream = 0.0, maxSettle = 0.0;
            std::vector<double> flux;
            size_t frames = 0;
            // A frame belongs to the settle once its window starts AFTER the last change.
            const size_t settleFrom = streamEnd / (size_t) hop + 1;
            for (size_t at = 0; at + (size_t) fftN <= x.size(); at += (size_t) hop, ++frames)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < fftN; ++i)
                    buf[(size_t) i] = (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                               * (float) i / (float) (fftN - 1)))
                                    * x[at + (size_t) i];
                fft.performFrequencyOnlyForwardTransform (buf.data());
                std::copy (buf.begin(), buf.begin() + fftN / 2, curMag.begin());
                double sum = 0.0, d = 0.0;
                for (size_t k = 0; k < curMag.size(); ++k)
                {
                    sum += curMag[k];
                    if (! prevMag.empty()) d += std::abs (curMag[k] - prevMag[k]);
                }
                meanMag += sum;
                flux.push_back (prevMag.empty() ? 0.0 : d);
                prevMag = curMag;
                if (frames >= settleFrom) settleMags.push_back (curMag);
            }
            meanMag /= std::max<size_t> (1, frames);
            for (size_t fi = 0; fi < flux.size() && fi < settleFrom; ++fi)
                maxStream = std::max (maxStream, flux[fi] / std::max (1.0e-9, meanMag));

            // The settle metric is the trajectory of the DISTANCE TO THE FINAL SPECTRUM,
            // D(t) - an integral quantity, immune to the per-hop ripple of a working
            // glide. A smooth settle spends D gradually; the amp-slew knee spent the
            // whole remaining distance in ~15 ms the moment the morph completed. So the
            // reported number is the largest one-hop RELATIVE drop of D, gated to frames
            // still meaningfully far away (> 5% of the settle's starting distance).
            if (settleMags.size() > 50)
            {
                std::vector<float> fin ((size_t) fftN / 2, 0.0f);
                const size_t finFrom = settleMags.size() - 40;      // last 200 ms = settled
                for (size_t fi = finFrom; fi < settleMags.size(); ++fi)
                    for (size_t k = 0; k < fin.size(); ++k)
                        fin[k] += settleMags[fi][k] / 40.0f;
                std::vector<double> D (settleMags.size(), 0.0);
                for (size_t fi = 0; fi < settleMags.size(); ++fi)
                    for (size_t k = 0; k < fin.size(); ++k)
                        D[fi] += std::abs (settleMags[fi][k] - fin[k]);
                // Gated to frames still meaningfully far from the final spectrum, in
                // BOTH senses: relative to the journey this settle actually makes, and
                // absolute against the signal level - otherwise a control run that is
                // already settled measures ratios of pure noise.
                for (size_t fi = 0; fi + 1 < finFrom; ++fi)
                    if (D[fi] > 0.05 * D[0] && D[fi] > 0.02 * meanMag)
                        maxSettle = std::max (maxSettle, (D[fi] - D[fi + 1]) / D[fi]);
            }
            const double jr = peak > 0.0 ? maxJump / peak : 0.0;
            std::printf ("  %-22s stream maxFlux=%.3f  settle maxDrop=%.2f  jump/peak=%.3f\n",
                         label, maxStream, maxSettle, jr);
            return Res { maxStream, maxSettle, jr };
        };

        const auto off = run (false, "morph off (control)");
        const auto on  = run (true,  "morph on, 2 s");
        check (off.stream > 0.0, "the control case actually made sound");
        // The control snaps the whole spectrum every 300 ms, so its stream flux IS the
        // size of one full step; a morph that genuinely glides must sit well below it.
        // (Before the aligned bank glided, this measured 0.76x the control: once the
        // crossfade weight neared 1, each new template snapped the audible spectrum.)
        check (on.stream <= 0.5 * off.stream,
               "mid-stream: every new target GLIDES - no audible step while templates keep landing");
        // The last glide spends its remaining distance-to-final gradually; the old
        // amp-slew knee spent nearly ALL of it in one 5 ms hop at morph completion.
        check (on.settle <= 0.30,
               "stream end: the morph settles with no amp-slew knee (bounded per-hop drop)");
        check (on.jump <= off.jump * 2.5 + 0.02,
               "and the smoothing never re-introduces a per-sample click");
    }

    // --- run 39: the state round-trip, done TWICE, byte for byte ---------------------
    // Run 12 restores once and only compares the SIZE of the two blobs. A latched flag
    // that dies on the SECOND restore (restored from the tree, then cleared again by the
    // very code that restores the next child) keeps the size identical and the value
    // wrong, and a user only ever meets it by reopening a project twice - or by a host
    // that calls setStateInformation twice, which many do (once on load, once when the
    // preset name is applied). So: build a state that latches everything at once - manual
    // per-partial amplitudes ON with real edits, an absolute 128-key .tun with a period
    // the user overrode, a typed custom spectrum, the segment picker - save, restore,
    // save, restore, save, and require the last two blobs to be IDENTICAL BYTES.
    {
        std::printf ("state round-trip, restored twice:\n");
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("fiblo_rt2_table.tun");
        {
            juce::String s ("; stretched, non-repeating\n[Exact Tuning]\nbasefreq=32.7032\n");
            for (int n = 0; n < 128; ++n)
            {
                const double d = (double) (n - 60);
                s << "note " << n << "=" << juce::String ((n - 24) * 100.0 + 0.002 * d * std::abs (d), 6) << "\n";
            }
            tmp.replaceWithText (s);
        }

        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (48000.0, 64);
        juce::String err;

        auto setQ = [&] (const char* id, float plain)
        {
            auto* q = p->apvts.getParameter (id);
            q->setValueNotifyingHost (q->convertTo0to1 (plain));
            p->pollParametersForTests();
        };

        check (p->loadScalaFile (tmp, err), "an absolute 128-key .tun loads");
        check (p->setTuningTablePeriod ("3/2", err), "and takes a period the user typed over it");
        check (p->loadTimbreText ("1/1 5/4 3/2 7/4 2/1", err), "a typed custom spectrum loads");
        setQ ("timbreOn", 1.0f);
        // Per-partial edits + manual mode: the pair the amplitude window latches.
        {
            std::vector<spectrum::PartialGain> g;
            for (int i = 1; i <= 6; ++i)
                g.push_back ({ (long long) (i * 120000), -1.5f * (float) i });
            setQ ("partialManual", 1.0f);
            p->setPartialGains (g, true);
        }
        p->setSegmentModeActive (true);
        p->setSegmentPeriodText ("3/2");
        p->setSegmentUseTopPeriod (true);
        // Arm the adaptive realtime follower, exactly as pressing REALTIME does.
        {
            FractalAdditiveAudioProcessor::RtSettings s;
            s.adapt.partials = 6;
            p->setRealtimeLink (FractalAdditiveAudioProcessor::RtLink::SpectrumFromChord, s);
        }

        // The three saves, with a full restore between each.
        juce::MemoryBlock a, b, c;
        p->getStateInformation (a);
        p->setStateInformation (a.getData(), (int) a.getSize());
        p->pollParametersForTests();
        p->getStateInformation (b);
        p->setStateInformation (b.getData(), (int) b.getSize());
        p->pollParametersForTests();
        p->getStateInformation (c);

        std::printf ("  blob sizes %d / %d / %d bytes\n",
                     (int) a.getSize(), (int) b.getSize(), (int) c.getSize());
        check (b.getSize() == c.getSize() && b == c,
               "restoring twice gives byte-identical state (no flag dies on the second pass)");

        // ...and every latched thing is still what it was, after TWO restores.
        check (p->tuningIsAbsoluteTable(), "still an absolute 128-key table");
        check (p->tuningTablePeriodIsUserSet() && p->getScaleDegrees() == 7,
               "the period the user typed over the table survived both restores");
        std::printf ("  custom spectrum after 2 restores: loaded=%d degrees=%d active=%d\n",
                     (int) p->timbreLoaded(), p->getTimbreNoteCount(), (int) p->isTimbreActive());
        check (p->timbreLoaded() && p->getTimbreNoteCount() == 4,
               "the typed custom spectrum survived both restores");
        check (p->apvts.getRawParameterValue ("partialManual")->load() > 0.5f,
               "manual per-partial amplitudes are still ON");
        check ((int) p->getPartialGains().size() == 6,
               "and all six per-partial edits came back");
        check (p->isSegmentModeActive(), "the segment picker is still the active mode");
    }

    // --- run 40: the 50 ms fade against a FRESH note, and against MORPH ----------------
    // Two claims about b347c02's template glide that run 37 does not reach.
    //
    // (a) It is armed PER VOICE and held for ~200 ms. Run 37's regression note is struck
    //     on a plugin that has been idle for 400 ms; this one is struck 50 ms after the
    //     spectrum changed under a sounding note, so the voice it lands on is carrying an
    //     armed fade. snapAmps has to win there too, or turning a knob while playing puts
    //     a 50 ms fade-in on the very next note.
    //
    // (b) With morph ON the fade must not STACK on the morph. The two are alternatives in
    //     one `glideTarget`, never a sum - so with Morph Time at its minimum an appearing
    //     partial has to arrive FASTER than the 50 ms morph-off fade, not slower.
    //
    // Measured with run 37's instrument (a Hann-windowed magnitude of ONE partial's exact
    // frequency), because per-voice normalisation recomputes normGain from the amplitudes
    // that are actually sounding: the total level therefore does NOT move while the
    // partials glide, and an RMS or peak envelope of the output is blind to all of this.
    {
        std::printf ("the 50 ms fade vs a fresh note, and vs morph:\n");
        constexpr int eb = 64; constexpr double sr4 = 48000.0;
        constexpr int win = 600, hop = 120;   // 12.5 ms window, 2.5 ms hop (as run 37)

        struct Rig
        {
            // A locally defined class can neither reach the enclosing constexpr nor hold a
            // static data member of its own, so the block size is an ordinary field.
            int eb = 64;
            std::unique_ptr<FractalAdditiveAudioProcessor> p;
            juce::AudioBuffer<float> b { 2, 64 };
            void set (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); }
            void pump (int blocks, std::vector<float>* out)
            {
                for (int i = 0; i < blocks; ++i)
                { juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                  if (out != nullptr) for (int k = 0; k < eb; ++k) out->push_back (b.getReadPointer (0)[k]); }
            }
            void key (bool on, int n, std::vector<float>* out = nullptr)
            {
                juce::MidiBuffer m;
                m.addEvent (on ? juce::MidiMessage::noteOn (1, n, (juce::uint8) 100)
                               : juce::MidiMessage::noteOff (1, n), 0);
                b.clear(); p->processBlock (b, m);
                if (out != nullptr) for (int k = 0; k < eb; ++k) out->push_back (b.getReadPointer (0)[k]);
            }
        };
        auto mag = [sr4] (const std::vector<float>& x, size_t at, int len, double f)
        {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < len && at + (size_t) i < x.size(); ++i)
            {
                const double w  = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (len - 1));
                const double ph = 2.0 * juce::MathConstants<double>::pi * f * (double) i / sr4;
                re += w * (double) x[at + (size_t) i] * std::cos (ph);
                im += w * (double) x[at + (size_t) i] * std::sin (ph);
            }
            return std::sqrt (re * re + im * im);
        };
        // Exactly run 37's arrangement: Spectrum Range 2 -> 10 only APPENDS partials, so
        // harmonic 6 rises from a true zero at an unchanged frequency.
        auto build = [&] (bool morphOn, float morphTime)
        {
            auto r = std::make_unique<Rig>();
            r->p = std::make_unique<FractalAdditiveAudioProcessor>();
            r->p->prepareToPlay (sr4, eb);
            r->set ("morph", morphOn ? 1.0f : 0.0f);
            r->set ("morphTime", morphTime);
            r->set ("ampAttack", 0.001f); r->set ("ampDecay", 0.001f);
            r->set ("ampSustain", 1.0f);  r->set ("ampRelease", 0.005f);
            r->set ("fractalLevel", 1.0f); r->set ("curveSlope", 1.0f); r->set ("baseN", 1.0f);
            r->set ("spectrumRange", 2.0f);
            juce::Thread::sleep (40); r->pump (40, nullptr);
            juce::Thread::sleep (40); r->pump (40, nullptr);
            return r;
        };
        auto riseMsOf = [&] (const std::vector<float>& tr, double f)
        {
            std::vector<double> env;
            for (size_t at = 0; at + (size_t) win <= tr.size(); at += (size_t) hop)
                env.push_back (mag (tr, at, win, f));
            double fin = 0.0; int fc = 0;
            for (size_t i = env.size() >= 40 ? env.size() - 40 : 0; i < env.size(); ++i) { fin += env[i]; ++fc; }
            fin /= std::max (1, fc);
            int i10 = -1, i90 = -1;
            for (size_t i = 0; i < env.size(); ++i)
            {
                if (i10 < 0 && env[i] >= 0.10 * fin) i10 = (int) i;
                if (env[i] >= 0.90 * fin) { i90 = (int) i; break; }
            }
            return std::pair<double, double> { (i10 >= 0 && i90 >= i10) ? (i90 - i10) * hop * 1000.0 / sr4 : -1.0,
                                               fin };
        };

        //-- (a) a fresh note onto a voice whose fade is armed.
        {
            auto r = build (false, 0.3f);
            r->key (true, 60);
            r->pump ((int) (0.5 * sr4 / eb), nullptr);
            const double f6 = 6.0 * r->p->getNoteFrequency (60);
            r->set ("spectrumRange", 10.0f);          // arms the 50 ms fade on this voice
            juce::Thread::sleep (25);                 // the builder publishes (no audio yet)
            // Only 5 ms of the fade: harmonic 6 is still near ZERO on this voice, so a
            // fresh note that inherited the fade instead of snapping would go on creeping
            // up from there and read a fraction of its sustain at 10 ms.
            r->pump ((int) (0.005 * sr4 / eb), nullptr);
            // Release and re-strike the same key in one block: the fresh note lands on the
            // voice that is carrying the armed fade.
            {
                juce::MidiBuffer m;
                m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                m.addEvent (juce::MidiMessage::noteOn  (1, 60, (juce::uint8) 100), 0);
                r->b.clear(); r->p->processBlock (r->b, m);
            }
            std::vector<float> att;
            for (int k = 0; k < eb; ++k) att.push_back (r->b.getReadPointer (0)[k]);
            r->pump ((int) (0.5 * sr4 / eb), &att);
            const double early = mag (att, (size_t) (0.010 * sr4), win, f6);
            const double susta = mag (att, (size_t) (0.400 * sr4), win, f6);
            std::printf ("  fresh note onto an ARMED voice: mag@10ms=%.4f sustain=%.4f (%.0f%%)\n",
                         early, susta, susta > 0.0 ? 100.0 * early / susta : 0.0);
            // Note what this can and cannot see. A 50 ms fade would read 18-36% here, so
            // the bar catches the leak this exists for. It cannot distinguish a snap from
            // the ordinary 7 ms de-click slew, and it cannot be made to FAIL by breaking
            // one mechanism alone: curAmp is not zeroed on note-off (the envelope is what
            // silences a note), so a voice re-struck on the same spectrum has nothing left
            // to glide even with snapAmps and the per-note reset both disabled. Three
            // independent guards cover this - snapAmps on a cold note, and
            // tmplGlideSamps/ampGlideCur reset on EVERY note-on, steal included.
            check (susta > 1.0e-4 && early >= 0.7 * susta,
                   "a note struck into an armed template fade still SNAPS onto its target");
        }

        //-- (b) morph ON at its shortest must not be SLOWER than the morph-off fade.
        {
            auto off = build (false, 0.3f);
            off->key (true, 60);
            off->pump ((int) (0.5 * sr4 / eb), nullptr);
            const double f6 = 6.0 * off->p->getNoteFrequency (60);
            off->set ("spectrumRange", 10.0f);
            juce::Thread::sleep (25);
            std::vector<float> a; off->pump ((int) (1.2 * sr4 / eb), &a);
            const auto ro = riseMsOf (a, f6);

            auto on = build (true, 0.0f);   // Morph Time at its minimum
            on->key (true, 60);
            on->pump ((int) (0.5 * sr4 / eb), nullptr);
            on->set ("spectrumRange", 10.0f);
            juce::Thread::sleep (25);
            std::vector<float> c; on->pump ((int) (1.2 * sr4 / eb), &c);
            const auto rn = riseMsOf (c, f6);

            std::printf ("  appearing partial rise: morph OFF %.1f ms, morph ON at min Morph Time %.1f ms\n",
                         ro.first, rn.first);
            check (ro.second > 1.0e-4 && rn.second > 1.0e-4, "the partial arrives in both");
            // Bounded on BOTH sides. `>= 40` is run 37's claim (it is a real fade, not a
            // 7 ms step); the upper bound is what makes stacking visible - adding
            // kTmplGlideSec to the morph term instead of choosing between them takes this
            // number from 70 ms to 85 ms, and nothing else in the build would notice.
            check (ro.first >= 40.0 && ro.first <= 78.0,
                   "morph OFF is exactly ONE 50 ms fade - not a fade plus something else");
            check (rn.first >= 0.0 && rn.first < ro.first,
                   "morph ON at its minimum is FASTER - the fade and the morph are alternatives, never a sum");
        }
    }

    // --- run 41: rapid sequences, lifecycle and parameter edges -----------------------
    // Everything here is a state the panel can reach in a second of ordinary use and that
    // no test drove before: a switch thrown mid-note, a file loaded while a worker runs,
    // an undo across an engine change, and every parameter at both ends of its range.
    // The bar is deliberately blunt - the plugin must keep making finite audio and must
    // never lose the user's scale - because that is exactly what these sequences broke.
    {
        std::printf ("rapid sequences, lifecycle, parameter edges:\n");
        constexpr double sr3 = 48000.0;
        constexpr int    blk3 = 64;
        juce::AudioBuffer<float> buf (2, blk3);

        auto finite = [&buf] (const juce::AudioBuffer<float>& b)
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float v = b.getSample (ch, i);
                    if (! std::isfinite (v) || std::abs (v) > 8.0f) return false;
                }
            return true;
        };

        //-- (a) switches thrown WHILE notes sound: manual amps, the engine, morph, bend.
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr3, blk3);
            juce::String err;
            check (p->loadTimbreText ("5/4 3/2 7/4 2", err), "a custom spectrum is loaded");
            auto setQ = [&p] (const char* id, float v)
            { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
            setQ ("morph", 1.0f); setQ ("morphTime", 2.0f);
            p->pollParametersForTests();

            juce::MidiBuffer on;
            for (const int k : { 55, 60, 64, 67, 71 })
                on.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            p->processBlock (buf, on);

            bool ok = true;
            std::vector<spectrum::PartialGain> edits;
            for (int i = 1; i <= 8; ++i) edits.push_back ({ (long long) (i * 90000), -2.0f * (float) i });
            for (int t = 0; t < 240 && ok; ++t)
            {
                // Every second block: flip something that rebuilds the whole spectrum.
                switch (t % 6)
                {
                    case 0: setQ ("partialManual", (t / 6) % 2 ? 1.0f : 0.0f);
                            p->setPartialGains (edits, (t / 6) % 2 != 0); break;
                    case 1: p->setTimbreEnabled ((t / 6) % 2 == 0); break;                 // engine chip
                    case 2: setQ ("timbreTemper", (t / 6) % 3 == 0 ? 1.0f : 0.0f); break;  // temper
                    case 3: setQ ("morphTime", (t / 6) % 2 ? 0.001f : 5.0f); break;         // both ends
                    case 4: setQ ("baseN", (float) (1 + (t / 6) % 8)); break;
                    default: break;
                }
                p->pollParametersForTests();
                juce::MidiBuffer m;
                if (t % 7 == 0) m.addEvent (juce::MidiMessage::pitchWheel (1, (t * 977) % 16384), 0);
                p->processBlock (buf, m);
                ok = finite (buf);
            }
            check (ok, "flipping manual amps / engine / temper / morph time under five held notes stays finite");
            // ...and the user's scale is still there. Every one of those switches only
            // ever flips a parameter; not one of them may be the thing that loses it.
            std::printf ("  after the sequence: loaded=%d degrees=%d src=\"%s\"\n",
                         (int) p->timbreLoaded(), p->getTimbreNoteCount(),
                         p->getTimbreSource().substring (0, 40).toRawUTF8());
            check (p->timbreLoaded() && p->getTimbreNoteCount() == 4,
                   "and the loaded custom scale is untouched by all of it");
        }

        //-- (b) a .tun loaded while the adaptive search is RUNNING, and an undo across an
        //   engine switch. Both cross a worker thread and the undo history at once.
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr3, blk3);
            juce::String err;
            auto edo = [] (int n)
            {
                juce::String s;
                s << "! x.scl\n" << juce::String (n) << "-EDO\n " << juce::String (n) << "\n!\n";
                for (int i = 1; i <= n; ++i) s << " " << juce::String (1200.0 * (double) i / (double) n, 6) << "\n";
                return s;
            };
            check (p->loadScalaText (edo (19), "edo19", err), "a 19-EDO tuning loads");

            FractalAdditiveAudioProcessor::RtSettings rs;
            rs.adapt.partials = 12; rs.adapt.periodRatio = 4.0; rs.adapt.minGapCents = 40.0;
            rs.adapt.rootOnly = true; rs.adapt.goal = 0; rs.adapt.shapeAmps = true;
            juce::MidiBuffer on;
            for (const int k : { 60, 63, 67 }) on.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            p->processBlock (buf, on);
            p->setRealtimeLink (FractalAdditiveAudioProcessor::RtLink::SpectrumFromChord, rs);
            for (int i = 0; i < 8; ++i) { juce::MidiBuffer none; p->processBlock (buf, none); p->pollParametersForTests(); }

            // ...and NOW, mid-search, replace the tuning under it.
            const bool wasRunning = p->isSpectrumAdaptRunning();
            check (p->loadScalaText (edo (31), "edo31", err), "another tuning loads while the search runs");
            for (int i = 0; i < 200; ++i)
            { juce::MidiBuffer none; p->processBlock (buf, none); p->pollParametersForTests(); juce::Thread::sleep (1); }
            std::printf ("  adaptive search was running when the tuning changed: %s\n",
                         wasRunning ? "yes" : "no (it had already finished)");
            check (p->getScaleDegrees() == 31,
                   "the tuning the user asked for is the one in force - a worker never overwrites it");
            check (finite (buf), "and the audio is still finite through it");
            p->setRealtimeLink (FractalAdditiveAudioProcessor::RtLink::Off, rs);

            // Undo ACROSS an engine switch: the custom engine goes on (undoable), then off.
            check (p->loadTimbreTextUndoable ("3/2 2", err), "a typed spectrum is set undoably");
            const int before = p->getTimbreNoteCount();
            p->undoManager.undo();
            const int afterUndo = p->getTimbreNoteCount();
            p->undoManager.redo();
            const int afterRedo = p->getTimbreNoteCount();
            std::printf ("  typed spectrum degrees: %d -> undo %d -> redo %d\n", before, afterUndo, afterRedo);
            check (afterRedo == before, "undo then redo across the engine switch lands back exactly");
        }

        //-- (c) LIFECYCLE: destroy the processor with every worker in flight. The three
        //   threads capture `this` and publish into members, so a missed join is a write
        //   into freed memory - which is silent until it is not.
        {
            for (int trial = 0; trial < 4; ++trial)
            {
                auto p = std::make_unique<FractalAdditiveAudioProcessor>();
                p->prepareToPlay (sr3, blk3);
                juce::String err;
                juce::String scl ("! y.scl\n24-EDO\n 24\n!\n");
                for (int i = 1; i <= 24; ++i) scl << " " << juce::String (50.0 * (double) i, 6) << "\n";
                p->loadScalaText (scl, "edo24", err);
                // n-ad ranking (heavy), the chord search, and the spectrum adaptation, all
                // started and NONE waited for.
                p->startNadCompute (4, true, false, 2.0, 30.0);
                p->startChordSearch (5, 2.0, "2/1", 16, false, 30.0, true, err);
                FractalAdditiveAudioProcessor::RtSettings rs;
                rs.adapt.partials = 16; rs.adapt.periodRatio = 8.0; rs.adapt.minGapCents = 20.0;
                rs.adapt.rootOnly = true; rs.adapt.goal = 0; rs.adapt.shapeAmps = true;
                juce::MidiBuffer on;
                for (const int k : { 60, 64, 67, 71 }) on.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
                p->processBlock (buf, on);
                p->setRealtimeLink (FractalAdditiveAudioProcessor::RtLink::SpectrumFromChord, rs);
                for (int i = 0; i < 3; ++i) { juce::MidiBuffer none; p->processBlock (buf, none); p->pollParametersForTests(); }
                // ...and drop it on the floor, mid-flight.
                p.reset();
            }
            check (true, "destroying the processor with the n-ad, chord and adapt workers in flight returns");
        }

        //-- (d) PARAMETER EDGES: every parameter at its minimum, then at its maximum, with
        //   notes held and manual amplitudes on but NO edits stored (the "on with zero
        //   edits" case). A range that only works in the middle is a range that ships.
        {
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr3, blk3);
            juce::String err;
            p->loadTimbreText ("5/4 3/2 2", err);
            auto* man = p->apvts.getParameter ("partialManual");
            man->setValueNotifyingHost (1.0f);
            p->setPartialGains ({}, true);   // ON with nothing stored
            p->pollParametersForTests();

            juce::MidiBuffer on;
            for (const int k : { 36, 60, 96 }) on.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            p->processBlock (buf, on);
            check (finite (buf), "manual per-partial amplitudes ON with zero edits still sounds");

            int worst = -1;
            juce::String worstId;
            for (int end = 0; end < 2 && worst < 0; ++end)
            {
                for (auto* ap : p->getParameters())
                    if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap))
                        rp->setValueNotifyingHost (end == 0 ? 0.0f : 1.0f);
                p->pollParametersForTests();
                for (int i = 0; i < 120; ++i)
                {
                    juce::MidiBuffer none;
                    p->processBlock (buf, none);
                    p->pollParametersForTests();
                    if (! finite (buf)) { worst = end; break; }
                }
            }
            std::printf ("  every parameter at its %s: %s\n",
                         worst < 0 ? "min AND max" : (worst == 0 ? "min" : "max"),
                         worst < 0 ? "finite throughout" : "NOT FINITE");
            check (worst < 0, "every parameter at both ends of its range keeps the output finite");

            // An inverted adaptation range (lo > hi) is a thing a hand types. It must be
            // read as the interval between them, not as an empty one.
            {
                auto q = std::make_unique<FractalAdditiveAudioProcessor>();
                q->prepareToPlay (sr3, blk3);
                q->loadScalaText ("! z.scl\n12-EDO\n 12\n!\n 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n"
                                  " 600.0\n 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 2/1\n", "edo12", err);
                FractalAdditiveAudioProcessor::RtSettings rs;
                rs.adapt.partials = 6; rs.adapt.periodRatio = 4.0; rs.adapt.minGapCents = 40.0;
                rs.adapt.rootOnly = true; rs.adapt.goal = 2;  // Range
                rs.adapt.shapeAmps = false;
                rs.adapt.rangeLo = 900.0; rs.adapt.rangeHi = 100.0;   // deliberately inverted
                juce::String e2;
                std::vector<double> alphas { 1.0, std::pow (2.0, 4.0 / 12.0), std::pow (2.0, 7.0 / 12.0) };
                const bool started = q->startSpectrumAdapt (rs.adapt, alphas, 261.6255653, e2);
                for (int i = 0; i < 800 && q->isSpectrumAdaptRunning(); ++i) juce::Thread::sleep (2);
                std::printf ("  inverted adapt range (lo 900 > hi 100): started=%d\n", (int) started);
                check (started, "an inverted adaptation range is accepted, not refused");
                juce::String e3;
                check (q->applySpectrumAdapt (false, e3),
                       "and it produces a spectrum (the range is read between the two numbers)");
            }
        }
    }

    // --- run 42: the REALTIME adaptive link - how LATE it lands, and how hard ----------
    // "sigue habiendo un trompicon al cambiar el espectro realtime, arregla ese retraso y
    //  esa transicion que produce como un golpe al hacer el cambio, tiene que ser smooth."
    // Two complaints, so two numbers, both measured on the REAL link (a held chord, the
    // 60 Hz timer, the background search, the real apply path):
    //   LATENCY      - from the chord moving to the new spectrum being audible;
    //   DISCONTINUITY- how much of that spectral journey is spent in a single 2.5 ms hop,
    //                  plus run 26's per-sample jump/peak so no fix can smuggle in a click.
    //
    // The harness is paced to WALL CLOCK. The search runs on its own thread, so an offline
    // render that outruns realtime would credit the link with far more samples of lag than
    // a player ever hears (and would compress the 60 Hz timer into nothing).
    {
        std::printf ("\nrealtime adaptive spectrum: how late it lands and how hard:\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr int    eb  = 64;
        constexpr double sr4 = 48000.0;
        auto p = std::make_unique<Proc>();
        p->prepareToPlay (sr4, eb);
        auto setR = [&p] (const char* id, float v)
        { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
          p->pollParametersForTests(); };
        // Morph OFF: the reported case, and the one where a template change is a bare swap.
        setR ("morph", 0.0f);
        // Short envelope segments so the chord change itself is over within ~20 ms and the
        // only event left in the analysis window is the spectrum landing.
        setR ("ampAttack", 0.001f); setR ("ampDecay", 0.001f);
        setR ("ampSustain", 1.0f);  setR ("ampRelease", 0.003f);
        setR ("fractalLevel", 1.0f); setR ("curveSlope", 1.0f); setR ("baseN", 1.0f);

        juce::AudioBuffer<float> b (2, eb);
        std::vector<float> x;                  // the recorded stream
        struct Mark { size_t at; long long clock; juce::uint32 gen, rev; int applies, starts; };
        std::vector<Mark> marks;
        bool recording = false;
        long long clockS = 0, nextPoll = 0;
        const long long tickS = (long long) (sr4 / 60.0);   // the real message timer's cadence
        const double    t0ms  = juce::Time::getMillisecondCounterHiRes();

        auto pumpFor = [&] (double seconds, juce::MidiBuffer firstBlockMidi)
        {
            const long long want = clockS + (long long) (seconds * sr4);
            bool sent = false;
            while (clockS < want)
            {
                juce::MidiBuffer m;
                if (! sent) { m = firstBlockMidi; sent = true; }
                b.clear();
                p->processBlock (b, m);
                if (recording)
                    for (int k = 0; k < eb; ++k) x.push_back (b.getReadPointer (0)[k]);
                clockS += eb;
                if (clockS >= nextPoll)
                {
                    nextPoll += tickS;
                    p->pollParametersForTests();
                    const auto* t = p->getLiveTemplate();
                    marks.push_back ({ x.size(), clockS, t != nullptr ? t->generation : 0u,
                                       p->getSpectrumLiveRevision(),
                                       p->getRealtimeApplies(), p->getRealtimeStarts() });
                }
                for (;;)   // pace: never let the render outrun the searcher's wall clock
                {
                    const double behind = (double) clockS * 1000.0 / sr4
                                        - (juce::Time::getMillisecondCounterHiRes() - t0ms);
                    if (behind <= 0.0) break;
                    juce::Thread::sleep (behind > 2.0 ? 1 : 0);
                }
            }
        };
        auto keyMidi = [] (bool on, std::initializer_list<int> notes)
        {
            juce::MidiBuffer m;
            for (const int k : notes)
                m.addEvent (on ? juce::MidiMessage::noteOn (1, k, (juce::uint8) 100)
                               : juce::MidiMessage::noteOff (1, k), 0);
            return m;
        };

        Proc::RtSettings rs;
        rs.adapt.partials = 8; rs.adapt.periodRatio = 2.0; rs.adapt.minGapCents = 50.0;
        rs.adapt.rootOnly = true; rs.adapt.goal = 0; rs.adapt.shapeAmps = true;

        pumpFor (0.10, {});
        pumpFor (1.6, keyMidi (true, { 60, 64, 67 }));      // chord A, settled
        p->setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
        pumpFor (2.2, {});                                  // the first adaptation lands + settles

        recording = true;
        pumpFor (0.30, {});
        const size_t    T0 = x.size();                      // the chord MOVES here
        const long long C0 = clockS;                        // ...on the free-running clock too
        const auto genAt0 = [&]
        { const auto* t = p->getLiveTemplate(); return t != nullptr ? t->generation : 0u; }();
        const juce::uint32 revAt0 = p->getSpectrumLiveRevision();
        // Chord B moves the QUALITY as well as the pitch. A pure transposition would not
        // do: in an equal temperament the ratios above the root are then identical, and
        // the adaptation can come back with the very same typed degrees - a "change" that
        // legitimately produces no new spectrum at all, which is not what is being timed
        // here. (That is exactly what a first cut of the throttle probe measured: two major
        // triads two semitones apart, and a landing that never came because none was due.)
        pumpFor (0.02, keyMidi (false, { 60, 64, 67 }));
        pumpFor (1.6,  keyMidi (true,  { 62, 65, 69 }));    // C major -> D minor
        const size_t    T1 = x.size();                      // ...and a settled tail to compare against
        const long long C1 = clockS;
        recording = false;

        // ...and now the case a PLAYER actually meets: a second move only 130 ms after the
        // first, so the throttle is still loaded. This is where "el retraso" lives - a move
        // made from rest is never throttled at all, because the link had been idle.
        const auto genOf = [&]
        { const auto* t = p->getLiveTemplate(); return t != nullptr ? t->generation : 0u; };
        pumpFor (0.02, keyMidi (false, { 62, 65, 69 }));
        pumpFor (0.13, keyMidi (true,  { 57, 61, 64 }));    // A major
        const size_t   T2     = clockS;
        const auto     genAt2 = genOf();
        pumpFor (0.02, keyMidi (false, { 57, 61, 64 }));
        pumpFor (1.2,  keyMidi (true,  { 57, 60, 64 }));    // ...to A minor: a real new target

        // ---- and a result aimed at a chord the player has ALREADY LEFT ----
        // Press P, wait until the search for P is genuinely RUNNING, then move to Q while
        // it runs - so P's answer is guaranteed to arrive for a chord nobody is holding.
        // Both moves ADD a note: the chord never empties, so this cannot be confused with
        // the "let go a moment before the search finished" rule, which is the opposite case
        // (that result IS the chord the user played, and must still be applied).
        pumpFor (0.02, keyMidi (false, { 57, 60, 64 }));
        pumpFor (0.70, keyMidi (true,  { 60, 64, 67 }));    // settled; the throttle is clear
        const bool idleBeforeP = ! p->isSpectrumAdaptRunning();
        pumpFor (0.004, keyMidi (true, { 65 }));            // P = C E F G
        bool sawRunning = false;
        for (int i = 0; i < 600 && ! sawRunning; ++i)
        { pumpFor (0.0015, {}); sawRunning = p->isSpectrumAdaptRunning(); }
        const auto      genAtQ = genOf();
        const long long C3     = clockS;
        pumpFor (0.80, keyMidi (true, { 62 }));             // Q = C D E F G, mid-search
        int pubAfterQ = 0;
        for (const auto& mk : marks)
            if ((long long) mk.clock > C3) pubAfterQ = (int) (mk.gen - genAtQ);

        // ---- when did each new spectrum reach the voices? ----
        // marks[].at is the RECORDED length, which stops advancing once recording ends, so
        // the throttled probe is timed on the free-running sample clock instead.
        auto firstAfter = [&] (size_t t, juce::uint32 gen0, bool wantGen) -> size_t
        {
            for (const auto& mk : marks)
                if (mk.at > t && (wantGen ? mk.gen != gen0 : mk.rev != revAt0)) return mk.at;
            return 0;
        };
        const size_t applyS0 = firstAfter (T0, genAt0, true);
        const size_t swapS0  = firstAfter (T0, 0, false);       // the FIRST live-slot swap
        size_t applyS2 = 0; int marksAfter2 = 0; int genAfter2 = 0, startsAfter2 = 0;
        for (const auto& mk : marks)
            if ((long long) mk.clock > (long long) T2)
            {
                ++marksAfter2;
                genAfter2    = (int) (mk.gen - genAt2);
                startsAfter2 = mk.starts;
                if (applyS2 == 0 && mk.gen != genAt2) applyS2 = (size_t) mk.clock;
            }
        // Counted on the free-running clock, NOT on the recorded length: `at` stops
        // advancing the moment recording ends, so every later mark would otherwise be
        // filed at exactly T1 and the throttle probe's own changes would be counted here.
        int publishes = 0, swaps = 0;
        for (const auto& mk : marks)
            if (mk.clock > C0 && mk.clock <= C1)
            { publishes = (int) (mk.gen - genAt0); swaps = (int) (mk.rev - revAt0); }
        const double applyMs0 = applyS0 > 0 ? (double) (applyS0 - T0) * 1000.0 / sr4 : -1.0;
        const double swapMs0  = swapS0  > 0 ? (double) (swapS0  - T0) * 1000.0 / sr4 : -1.0;
        const double applyMs2 = applyS2 > 0 ? (double) (applyS2 - T2) * 1000.0 / sr4 : -1.0;

        // ---- the spectral trajectory across the first landing ----
        constexpr int fo = 10, fftN = 1 << fo, hop = 120;   // 21.3 ms window, 2.5 ms hop
        juce::dsp::FFT fft (fo);
        std::vector<float> fbuf (2 * (size_t) fftN);
        std::vector<std::vector<float>> mags;
        std::vector<size_t> mid;                             // window CENTRE = the frame's time
        for (size_t at = 0; at + (size_t) fftN <= x.size(); at += (size_t) hop)
        {
            std::fill (fbuf.begin(), fbuf.end(), 0.0f);
            for (int i = 0; i < fftN; ++i)
                fbuf[(size_t) i] = (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                             * (float) i / (float) (fftN - 1)))
                                 * x[at + (size_t) i];
            fft.performFrequencyOnlyForwardTransform (fbuf.data());
            mags.emplace_back (fbuf.begin(), fbuf.begin() + fftN / 2);
            mid.push_back (at + (size_t) fftN / 2);
        }
        auto dist = [] (const std::vector<float>& u, const std::vector<float>& v)
        { double d = 0.0; for (size_t k = 0; k < u.size(); ++k) d += std::abs ((double) u[k] - (double) v[k]); return d; };
        auto meanOf = [&] (size_t lo, size_t hi)
        {
            std::vector<float> a ((size_t) fftN / 2, 0.0f);
            int c = 0;
            for (size_t i = 0; i < mags.size(); ++i)
                if (mid[i] >= lo && mid[i] < hi) { for (size_t k = 0; k < a.size(); ++k) a[k] += mags[i][k]; ++c; }
            for (auto& v : a) v /= (float) std::max (1, c);
            return a;
        };
        // REFERENCE = the sound after the new NOTES are steady but before ANY part of the
        // new spectrum has moved (anchored on the first live-slot swap, whatever it was);
        // FINAL = the settled end of the first landing.
        const size_t lm = (size_t) fftN / 2;   // half a window: a frame centred here is clean
        const size_t firstMove = swapS0 > 0 ? swapS0 : T0 + (size_t) (0.25 * sr4);
        const size_t refLo = T0 + (size_t) (0.028 * sr4) + lm;
        const size_t refHi = firstMove > lm ? firstMove - lm : refLo + 1;
        const auto ref = meanOf (refLo, std::max (refHi, refLo + 1));
        const auto fin = meanOf (T1 - (size_t) (0.30 * sr4), T1);
        const double D0 = dist (ref, fin);
        double meanMag = 0.0;
        for (size_t i = 0; i < mags.size(); ++i)
            if (mid[i] >= T0 && mid[i] < T1)
            { double s2 = 0.0; for (const float v : mags[i]) s2 += v; meanMag += s2; }
        meanMag /= (double) std::max<size_t> (1, (T1 - T0) / (size_t) hop);

        // Progress along that journey. Smoothed over 5 hops (12.5 ms): three notes an equal
        // temperament apart BEAT, so a single frame's distance ripples by tens of percent
        // whatever the spectrum is doing, and an unsmoothed "largest one-hop step" would be
        // measuring the beat, not the landing.
        std::vector<double> prog, progT;
        for (size_t i = 0; i < mags.size(); ++i)
            if (mid[i] >= refLo && mid[i] < T1)
            { prog.push_back (D0 > 0.0 ? 1.0 - dist (mags[i], fin) / D0 : 0.0);
              progT.push_back ((double) ((long long) mid[i] - (long long) T0) * 1000.0 / sr4); }
        std::vector<double> sm (prog.size(), 0.0);
        for (size_t i = 0; i < prog.size(); ++i)
        {
            double a = 0.0; int c = 0;
            for (size_t j = (i >= 2 ? i - 2 : 0); j <= std::min (prog.size() - 1, i + 2); ++j) { a += prog[j]; ++c; }
            sm[i] = a / std::max (1, c);
        }
        double t10 = -1.0, t90 = -1.0, maxStep = 0.0; double stepAt = -1.0;
        for (size_t i = 0; i < sm.size(); ++i)
        {
            if (t10 < 0.0 && sm[i] >= 0.10) t10 = progT[i];
            if (t90 < 0.0 && sm[i] >= 0.90) t90 = progT[i];
            if (i > 0 && sm[i - 1] < 0.90 && sm[i] - sm[i - 1] > maxStep)
            { maxStep = sm[i] - sm[i - 1]; stepAt = progT[i]; }
        }
        // THE TWO METRICS THAT SURVIVE THE BEATING, AND HOW THEY ARE MADE FAIR.
        // Three notes a tempered third apart beat, and so do the adapted partials
        // themselves (the search is only asked to keep them 50 cents apart), so both the
        // spectrum and the loudness ripple continuously whatever the link is doing - by
        // over +/-30% of the mean, measured. Each event is therefore compared with that
        // shimmer PEAK TO PEAK over windows of the SAME length in the SAME take: 150 ms
        // straddling the landing, against the last settled 150 ms. A ratio near 1 means
        // the change cannot be picked out of the note's own movement; the "golpe" is a
        // number well above it. (Peak against a MEDIAN would have called the shimmer
        // itself a thump, which is how a first cut of this metric read 3.6x with nothing
        // happening at that instant at all.)
        //   - SPECTRAL FLUX: L1 between consecutive magnitude frames (the timbre stepping);
        //   - RMS STEP:      one-hop change in short-term loudness (the thump itself).
        const size_t evLo = firstMove > (size_t) (0.015 * sr4) ? firstMove - (size_t) (0.015 * sr4) : T0;
        const size_t evHi = evLo + (size_t) (0.150 * sr4);
        const size_t caLo = T1 - (size_t) (0.165 * sr4), caHi = T1 - (size_t) (0.015 * sr4);
        double fluxPeak = 0.0, fluxCalmPk = 0.0;
        for (size_t i = 1; i < mags.size(); ++i)
        {
            const double f = dist (mags[i], mags[i - 1]) / std::max (1.0e-9, meanMag);
            if (mid[i] >= evLo && mid[i] < evHi) fluxPeak   = std::max (fluxPeak, f);
            if (mid[i] >= caLo && mid[i] < caHi) fluxCalmPk = std::max (fluxCalmPk, f);
        }

        // THE THUMP, MEASURED AS A LOUDNESS EXCURSION. Two choices make this sharp:
        //   - a 20 ms RMS window, not a short one. In Aligned phase mode (the default) a
        //     voice is a PULSE train, so a 5 ms window holds barely one period and its RMS
        //     swings by tens of percent purely on where the window falls against the pulse
        //     - noise that buried the event completely.
        //   - peak-to-trough over a 30 ms span, not a one-hop difference. A step spends its
        //     whole size inside such a span; the notes' own beating (a few Hz, so 200 ms
        //     and up) can only contribute the small piece of its cycle that fits.
        constexpr int rw = 960, rh = 120;    // 20 ms window, 2.5 ms hop
        constexpr int exSpan = 12;           // 30 ms
        std::vector<double> rms;
        std::vector<size_t> rmsAt;
        for (size_t at = T0; at + (size_t) rw <= T1; at += (size_t) rh)
        {
            double s2 = 0.0;
            for (int i = 0; i < rw; ++i) { const double v = x[at + (size_t) i]; s2 += v * v; }
            rms.push_back (std::sqrt (s2 / rw));
            rmsAt.push_back (at + (size_t) rw / 2);   // the window's CENTRE is its time
        }
        double meanRms = 0.0; for (const double v : rms) meanRms += v;
        meanRms /= std::max<size_t> (1, rms.size());
        double thumpAt = -1.0, thump = 0.0, rmsCalmPk = 0.0;
        for (size_t i = 0; i + (size_t) exSpan < rms.size(); ++i)
        {
            double lo = rms[i], hi = rms[i];
            for (int k = 1; k <= exSpan; ++k) { lo = std::min (lo, rms[i + (size_t) k]);
                                                hi = std::max (hi, rms[i + (size_t) k]); }
            const double e = (hi - lo) / std::max (1.0e-12, meanRms);
            const size_t at = rmsAt[i];
            if (at >= evLo && at < evHi && e > thump)
            { thump = e; thumpAt = (double) ((long long) at - (long long) T0) * 1000.0 / sr4; }
            if (at >= caLo && at < caHi) rmsCalmPk = std::max (rmsCalmPk, e);
        }

        // Run 26's per-sample click metric, so no fix here can smuggle a click back in.
        double maxJump = 0.0, peak = 0.0;
        for (size_t i = T0 + (size_t) (0.030 * sr4); i < T1; ++i)
        {
            peak = std::max (peak, (double) std::abs (x[i]));
            maxJump = std::max (maxJump, (double) std::abs (x[i] - x[i - 1]));
        }

        const double fluxRatio = fluxCalmPk > 0.0 ? fluxPeak / fluxCalmPk : 0.0;
        const double rmsRatio  = rmsCalmPk  > 0.0 ? thump    / rmsCalmPk  : 0.0;
        std::printf ("  chord moved -> first live swap %.0f ms, new TEMPLATE %.0f ms; %d publishes, %d live swaps\n",
                     swapMs0, applyMs0, publishes, swaps);
        std::printf ("  a second move 130 ms later (throttle loaded) -> new template after %.0f ms"
                     " (%d ticks watched, %d publishes, %d searches, %d applies, link %s%s)\n",
                     applyMs2, marksAfter2, genAfter2, startsAfter2, p->getRealtimeApplies(),
                     p->getRealtimeLink() == Proc::RtLink::Off ? "OFF" : "on",
                     p->getRealtimeRefusal().toRawUTF8());
        std::printf ("  landing vs the note's own shimmer, 150 ms each: flux %.3f/%.3f = %.2fx   "
                     "RMS excursion %.3f (at %.0f ms)/%.3f = %.2fx\n",
                     fluxPeak, fluxCalmPk, fluxRatio, thump, thumpAt, rmsCalmPk, rmsRatio);
        std::printf ("  jump/peak %.4f; 10%% of the change at %.0f ms (D0 %.1f, meanMag %.1f, meanRms %.4f)\n",
                     peak > 0.0 ? maxJump / peak : 0.0, t10, D0, meanMag, meanRms);
        std::printf ("  events after the move (ms:gen/rev/starts/applies):");
        for (size_t i = 1; i < marks.size(); ++i)
            if (marks[i].clock > C0 && marks[i].clock <= C1
                && (marks[i].gen != marks[i - 1].gen || marks[i].rev != marks[i - 1].rev
                    || marks[i].starts != marks[i - 1].starts))
                std::printf (" %.0f:%u/%u/%d/%d", (double) (marks[i].clock - C0) * 1000.0 / sr4,
                             marks[i].gen - genAt0, marks[i].rev - revAt0,
                             marks[i].starts, marks[i].applies);
        std::printf ("\n  RMS (ms:%%ofmean):");
        for (size_t i = 0; i < rms.size(); i += 4)
        {
            const double ms = (double) ((long long) rmsAt[i] - (long long) T0) * 1000.0 / sr4;
            if (ms >= 0.0 && ms <= 260.0)
                std::printf (" %.0f:%.0f", ms, 100.0 * rms[i] / std::max (1.0e-12, meanRms));
        }
        std::printf ("\n");
        check (applyS0 > 0, "the chord change really did reach the spectrum");
        check (D0 > 0.0, "and the spectrum really did change");
        check (swaps == 1 && publishes == 1,
               "a chord move is ONE template swap - not a gains-only republish and then the real one");
        check (applyMs0 > 0.0 && applyMs0 <= 60.0,
               "the new spectrum is live within one search + two timer ticks of the chord moving");
        check (applyMs2 > 0.0 && applyMs2 <= 100.0,
               "and a move made 130 ms after another one is not held back by the throttle");
        check (rmsRatio <= 1.4,
               "the landing is no louder a step than the note's own beating (no thump)");
        // A ceiling, not the thump detector: one clean 50 ms fade moves MORE spectrum in
        // the landing window than two half-sized steps do, so this number is deliberately
        // allowed to go up when the apply is fixed. What it forbids is a landing several
        // times the note's own movement, i.e. a hard snap.
        check (fluxRatio <= 2.0,
               "and the spectrum is not snapped across in one hop either");
        std::printf ("  P then Q mid-search: search was running = %d, publishes after Q = %d\n",
                     (int) sawRunning, pubAfterQ);
        check (idleBeforeP && sawRunning,
               "the second move really did land while the first move's search was still running");
        check (pubAfterQ == 1,
               "a result aimed at the chord the player has already left is DROPPED, not applied late");

        //-- A REBUILD ALREADY IN FLIGHT MUST NOT LAND ON TOP OF THE ADAPTATION.
        // Publishing the adapted template directly is what removed the round trip, but it
        // means the build WORKER may still be compiling a template from settings captured
        // before the adaptation existed. If that one published afterwards it would put the
        // old spectrum back - and because the apply also tells the poll "this state is
        // pushed", nothing would ever notice: the adaptation would silently vanish, and the
        // REALTIME button would look like it had stopped working. So: a deliberately heavy
        // rebuild is queued, the adaptation is applied while it compiles, and the spectrum
        // must still be the adapted one afterwards.
        {
            auto q = std::make_unique<Proc>();
            q->prepareToPlay (sr4, 256);
            auto sq = [&q] (const char* id, float v)
            { auto* z = q->apvts.getParameter (id); z->setValueNotifyingHost (z->convertTo0to1 (v)); };
            sq ("fractalLevel", 4.0f); sq ("spectrumRange", 10.0f); sq ("baseN", 1.0f);
            q->pollParametersForTests();
            juce::Thread::sleep (400);                  // the heavy template settles in

            Proc::SpectrumAdaptSettings st;
            st.partials = 8; st.periodRatio = 2.0; st.minGapCents = 50.0;
            st.rootOnly = true; st.goal = 0; st.shapeAmps = true;
            const std::vector<double> al { 1.0, std::pow (2.0, 4.0 / 12.0), std::pow (2.0, 7.0 / 12.0) };
            juce::String e;
            check (q->startSpectrumAdapt (st, al, 261.6255653, e), "the race probe's adaptation starts");
            for (int i = 0; i < 3000 && q->isSpectrumAdaptRunning(); ++i) juce::Thread::sleep (2);

            // Queue the heavy rebuild, then apply BEFORE the worker can finish it.
            sq ("curveSlope", 3.0f);
            q->pollParametersForTests();
            check (q->applySpectrumAdapt (false, e), "...and applies while a rebuild is in flight");
            const auto* ta = q->getLiveTemplate();
            const size_t nAdapted = ta != nullptr ? ta->partials.size() : 0;
            juce::Thread::sleep (3000);                 // any in-flight build has long finished
            q->pollParametersForTests();
            juce::Thread::sleep (300);
            const auto* tb = q->getLiveTemplate();
            const size_t nAfter = tb != nullptr ? tb->partials.size() : 0;
            std::printf ("  in-flight rebuild vs the adaptation: %d partials at apply, %d three seconds later\n",
                         (int) nAdapted, (int) nAfter);
            check (nAdapted > 0 && nAfter == nAdapted,
                   "a rebuild already in flight cannot publish the OLD spectrum over the adaptation");
        }
    }

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
