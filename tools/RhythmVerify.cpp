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

#include <array>
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

//==============================================================================
// WAIT FOR THE BUILDER; NEVER GUESS AT IT.
//
// The spectrum is compiled on a Priority::low worker (SpectrumEngine.h). A verifier
// that sleeps a fixed time and then runs a tight processBlock loop STARVES that
// worker: measured 1907 ms and 2309 ms to publish under `sleep (120)` followed by
// ~1600 back-to-back blocks, against 3.0 ms of actual compileSpectrum work and 4 ms
// with a tight poll. Every "the spectrum changed" claim downstream of such a sleep is
// therefore a race, and that race is most of why six runs of this same binary at the
// same commit produced 2/3/11/12/17/19 failures with near-disjoint sets: "Base 1 vs
// Base 7 differs" is a deterministic spectral claim that only ever went red because
// the rebuild had not landed yet.
//
// The engine already keeps the counter that answers the question, so ask it. Silent
// when the build lands (it always should); a loud, counted failure when it does not,
// because a timeout here means every spectral claim below is reading a stale template
// and must not be allowed to pass by accident.
static bool awaitTemplateChange (const FractalAdditiveAudioProcessor& p, juce::uint32 gen0,
                                 const char* what, double timeoutMs = 15000.0)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (p.getTemplateGeneration() == gen0)
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > timeoutMs)
        {
            std::printf ("  [FAIL] the spectrum rebuild published within %.0f ms (%s)\n", timeoutMs, what);
            ++failures;
            return false;
        }
        juce::Thread::sleep (1);
    }
    return true;
}

// The other half of the same problem: after a GROUP of parameter edits, some of which may
// or may not ask for a rebuild, there is no one generation to wait for. Wait for the
// builder to go QUIET instead - the counter unchanged for `stableMs` - so a template can
// never land in the middle of the measurement that follows and change the spectrum under
// it. (Safe to call with nothing pending: `set` runs the poll synchronously, so by the
// time this is entered the request, if there is one, is already queued with the worker.)
static void awaitTemplateQuiet (const FractalAdditiveAudioProcessor& p,
                                double stableMs = 80.0, double timeoutMs = 15000.0)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    auto   gen   = p.getTemplateGeneration();
    double since = t0;
    for (;;)
    {
        juce::Thread::sleep (2);
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (const auto g = p.getTemplateGeneration(); g != gen) { gen = g; since = now; }
        if (now - since >= stableMs || now - t0 > timeoutMs)
            return;
    }
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
        // directions: back to the neutral steel that read as white, or onto a colour so
        // close to a neighbouring panel's that the boxes stop being tellable apart.
        //
        // Measured in OKLab against galactic::palette itself - the very set the palette
        // search produced - not against three colours copied in here by hand. Those
        // copies were three panels' hexes from an older palette, compared by HSV hue
        // gap and HSV saturation, and BOTH proxies were wrong: they passed a pink at
        // 0.33 "saturation" as steel and called a 15-unit separation a collision. A
        // check with its own copy of the palette measures the palette it remembers.
        auto checkPanelColour = [] (juce::Colour panel, const char* who)
        {
            using namespace galactic;
            const std::uint32_t rgb = panel.getARGB() & 0xffffffu;
            double worst = 1.0e9; const char* nearest = "";
            for (const auto& e : palette::identitySet())
            {
                if (e.rgb == rgb) continue;                 // itself
                const double d = colour::deltaE (e.rgb, rgb);
                if (d < worst) { worst = d; nearest = e.name; }
            }
            const double chroma = colour::chroma (colour::toOklab (rgb));
            std::printf ("  panel colour %s (%s)  chroma %.3f  nearest other identity: %s at %.1f\n",
                         panel.toDisplayString (false).toRawUTF8(), who, chroma, nearest, worst);
            check (chroma > 0.10, "the panel is a real colour, not the old neutral steel");
            // The guarantee the palette itself is held to (see SpectrumTests): if the
            // panel is further than that from everything else, it is as distinct as any
            // colour in this window is.
            check (worst >= 15.0, "and it is clear of every other identity in the window");
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

            // THE TWO CHIP ROWS ARE ONE CONTROL AT ONE SIZE.
            //
            // "haz que los botones harmonic/custom tengan el mismo tamano que los de los
            //  sub-menus dentro del menu modulator (pitch, filter, custom, etc)"
            //
            // HARMONIC / CUSTOM and AMP / PITCH / FILTER / CUSTOM do the same job at the
            // top of their box, and they were 104 x 16 against 58 x 19 - the engine pair
            // wider AND shorter, its text a whole point smaller, because each row carried
            // its own literals. They read from fgfit::chip now; this is what stops a hand
            // from putting a private number back into one of them.
            {
                auto rect = [&L] (const char* nm)
                {
                    juce::Rectangle<int> r;
                    for (const auto& c : L.controls) if (c.first == nm) r = c.second;
                    return r;
                };
                const auto ch0 = rect ("eng.chipHarmonic"), ch1 = rect ("eng.chipCustom");
                const auto ta  = rect ("mod.tabAmp"),       tf  = rect ("mod.tabFilter");
                std::printf ("  chips: HARMONIC %dx%d, CUSTOM %dx%d | tabs: AMP %dx%d, FILTER %dx%d\n",
                             ch0.getWidth(), ch0.getHeight(), ch1.getWidth(), ch1.getHeight(),
                             ta.getWidth(), ta.getHeight(), tf.getWidth(), tf.getHeight());
                check (! ch0.isEmpty() && ! ta.isEmpty(), "both chip rows are on screen");
                check (ch0.getHeight() == ta.getHeight() && ch1.getHeight() == ta.getHeight()
                           && ta.getHeight() == tf.getHeight(),
                       "an engine chip is exactly as tall as a modulator sub-tab (so its "
                       "text is drawn in the same size - the font follows the height)");
                // ...and each chip is sized to ITS OWN word, not to a shared constant.
                // "HARMONIC" and "CUSTOM" are different lengths in both languages, so
                // equal widths here can only mean a fixed number came back.
                check (ch0.getWidth() != ch1.getWidth(),
                       "and each engine chip is as wide as its own word, not a fixed 104");
                check (ch0.getWidth() < 104 && ch1.getWidth() < 104,
                       "which is narrower than the slab they used to be");
            }

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

                // ...AND THE CORNERS AT THAT END ARE SQUARE. Sharing the x was not enough:
                // SPECTRUM and TUNING rounded their top-right corner with a 9 px radius, so
                // each one's top edge curved away from the wall 9 px before reaching it and
                // the "one line" was pinched by two arcs. Invisible to every check above -
                // the arc leaves the border's x exactly where it was and both rects still
                // end on the wall - so it is measured as how far INSIDE its own corner each
                // panel's drawn border passes (see LayoutProbe::panelCorners).
                {
                    float sq = -1.0f, tu = -1.0f, es = -1.0f, en = -1.0f, co = -1.0f;
                    for (const auto& c : L.panelCorners)
                    {
                        if (c.first == "spectrum")   sq = c.second;
                        if (c.first == "tuning")     tu = c.second;
                        if (c.first == "essentials") es = c.second;
                        if (c.first == "envelopes")  en = c.second;
                        if (c.first == "column")     co = c.second;
                    }
                    std::printf ("  top-right corner inset: spectrum %.1f  tuning %.1f"
                                 " | essentials %.1f  envelopes %.1f  column %.1f px\n",
                                 sq, tu, es, en, co);
                    // A square corner reports 0.5 (the first sample is already inside the
                    // path); the 9 px rounding reports 2.5 - the quadratic's own closest
                    // approach to the corner is rad/4 in each axis, so the two cases are
                    // five sampling steps apart and cannot be confused for one another.
                    check (sq >= 0.0f && sq < 1.0f && tu >= 0.0f && tu < 1.0f,
                           "the two panels that OWN the shared wall meet it square - one straight line, no arcs");
                    check (es >= 2.0f && en >= 2.0f && co >= 2.0f,
                           "and every corner facing open page keeps its radius - only the shared end is squared");
                }
            }

            // AIR BETWEEN PANELS THAT SHARE NOTHING. The shared wall is one line by design;
            // everywhere else the boxes are separate objects and were 6 px (4 vertically)
            // apart, which at a 2.6 px stroke reads as two borders leaning on each other.
            // Measured on the RECTS here and stated as the drawn-line distance, which is
            // the rect gap plus the two stroke offsets (kFrameLine each side).
            {
                const int vGap  = L.essentials.getY()  - L.spectrum.getBottom();
                const int hGap1 = L.envelopes.getX()   - L.essentials.getRight();
                const int hGap2 = L.tuning.getX()      - L.envelopes.getRight();
                const float line = 2.0f * 1.3f;   // kFrameLine, both strokes
                std::printf ("  unshared gaps: spectrum->row2 %d px (%.1f between lines),"
                             " essentials->envelopes %d (%.1f), envelopes->tuning %d (%.1f)\n",
                             vGap, (float) vGap + line, hGap1, (float) hGap1 + line,
                             hGap2, (float) hGap2 + line);
                check (vGap >= 6, "rows 1 and 2 share no border, so they sit 6 px apart (they were 4)");
                check (hGap1 >= 8 && hGap2 >= 8,
                       "and the three row-2 boxes share none either - 8 px between each pair (they were 6)");
                check (! L.essentials.intersects (L.envelopes)
                       && ! L.envelopes.intersects (L.tuning)
                       && win.contains (L.envelopes),
                       "with nothing overlapping and nothing pushed out of the window to pay for it");
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
                                : c.first.startsWith ("top.")  ? &L.titleBar
                                // The two chip rows: the engine chips ARE the engine
                                // box's title band, the sub-tabs live inside Envelopes.
                                : c.first.startsWith ("eng.")  ? &L.engineBox
                                : c.first.startsWith ("mod.")  ? &L.envelopes
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

            // ...AND WHERE IT SITS, which is a different claim in each view.
            //
            // "haz que el boton temper to tuning este debajo de todo dentro de la
            //  seccion prime"  (harmonic view)
            // "haz que los botones remove random y temper to tuning esten abajo de la
            //  caja de ratios de custom tuning"  (custom view)
            //
            // Two placements of ONE button - it is laid out in both views - so both are
            // asserted, in the pass that shows that view. Nothing else in this file
            // could see it move: it stayed the same size, inside the same box, clear of
            // every neighbour, in the wrong place.
            {
                auto rect = [&L] (const char* nm)
                {
                    juce::Rectangle<int> r;
                    for (const auto& c : L.controls) if (c.first == nm) r = c.second;
                    return r;
                };
                juce::Rectangle<int> primeCell;
                for (const auto& c : L.cells) if (c.first == "eng.prime") primeCell = c.second;
                const auto temper = rect ("cust.temper");
                if (pass == 2)   // CUSTOM view
                {
                    const auto ratios = rect ("cust.intervals");
                    const auto load   = rect ("cust.load");
                    const auto rnd    = rect ("cust.random");
                    std::printf ("  custom view: ratios box bottom %d | load y %d, random y %d, temper y %d\n",
                                 ratios.getBottom(), load.getY(), rnd.getY(), temper.getY());
                    check (! ratios.isEmpty() && ! load.isEmpty(),
                           "the custom view shows its ratios box and its buttons");
                    check (load.getY() >= ratios.getBottom() && rnd.getY() >= ratios.getBottom()
                               && temper.getY() >= ratios.getBottom(),
                           "Load/Remove, Random and Temper all sit BELOW the ratios box");
                    check (ratios.getY() < load.getY(),
                           "and the ratios box rode up into the room they left at the top");
                }
                else             // HARMONIC view
                {
                    std::printf ("  harmonic view: prime cell %d..%d | temper %d..%d\n",
                                 primeCell.getY(), primeCell.getBottom(),
                                 temper.getY(), temper.getBottom());
                    check (! primeCell.isEmpty() && primeCell.contains (temper),
                           "TEMPER is inside the PRIME cell, not spanning the whole box under it");
                    // "debajo de todo": nothing in that cell may be below it. 6 px of
                    // slack is the cell's own bottom inset, no more.
                    check (temper.getBottom() >= primeCell.getBottom() - 6,
                           "and it is at the very bottom of that cell");
                    for (const auto& c : L.controls)
                        if (c.first.startsWith ("harm.") && c.second.getY() >= temper.getBottom())
                            check (false, "nothing in the prime section is laid out below TEMPER");
                }
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

                // EVERY COMBO'S WIDEST ITEM, AND EVERY BUTTON'S WORD, AGAINST ITS BOX.
                //
                // "hay varios botones y desplegables ahora mismo que son absurdamente
                //  anchos, reduce su anchura" - and narrowing a control is precisely how
                // truncation appears. Neither of these two kinds of control can report
                // it: a ComboBox draws as much of the item as the cell holds and a
                // TextButton condenses to 70% and then clips, so a box six pixels too
                // narrow passes every bounds, overlap and cell check in this file.
                //
                // Until now only shepMode / shepRise (combos) and three buttons whose
                // WORD is a feature were measured, by name. That is the list a change
                // forgets to extend: the six controls narrowed in this pass were on none
                // of them. So the sweep is over everything the probe registers, in both
                // languages, and the widest ITEM is what a combo is judged by - the
                // selected one only clips after the user has already picked it.
                {
                    int cut = 0; juce::String worstW; float worstBy = 0.0f;
                    auto tooTight = [&] (const juce::String& nm, const juce::String& txt,
                                         float need, int avail)
                    {
                        const float over = need - (float) avail;
                        if (over <= 0.0f) return;
                        ++cut;
                        if (over > worstBy) { worstBy = over; worstW = nm + " \"" + txt + "\""; }
                    };
                    for (const auto& t : P.comboTexts) tooTight (t.name, t.text, t.need, t.avail);
                    for (const auto& t : P.buttonFits) tooTight (t.name, t.text, t.need, t.avail);
                    std::printf ("  %s: %d combos + %d buttons measured, %d too narrow for their own words%s\n",
                                 lang == 1 ? "catalan" : "english",
                                 (int) P.comboTexts.size(), (int) P.buttonFits.size(), cut,
                                 cut > 0 ? (" (worst: " + worstW + " by "
                                            + juce::String ((int) std::ceil (worstBy)) + " px)").toRawUTF8() : "");
                    check (P.comboTexts.size() >= 6 && P.buttonFits.size() >= 8,
                           "the sweep really had combos and buttons to walk");
                    check (cut == 0,
                           lang == 1 ? "every combo item and every button word fits its box (Catalan)"
                                     : "every combo item and every button word fits its box (English)");
                }
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

                    // THE AGGREGATE BOX IS GONE, and this is where a well-meaning hand puts
                    // it back. It offered "average" vs "worst case"; with the range anchored
                    // at the played note and running upward the worst register is always the
                    // anchor, so minimax optimised one pitch and the Steps field beside it
                    // stopped changing anything (10.742 at every width). Probing by NAME,
                    // because a returning selector would sit there looking correct: nothing
                    // else in this file measures a control that should not exist.
                    if (sh.mode == 2)
                    {
                        bool hasAgg = false;
                        for (const auto& c : P.controls)      if (c.first == "spec.agg")    hasAgg = true;
                        for (const auto& w : P.wrappedLabels) if (w.name == "spec.aggInfo") hasAgg = true;
                        check (! hasAgg, "there is no average/worst-case selector, and no line explaining one");
                        // ...and the explanation did not go with it. The user's ask was
                        // explicitly "explain it in some hover", so the sentence has to be
                        // IN the neighbouring control's tooltip, in the language on screen -
                        // a tooltip no other check in this build can see.
                        const auto& tip = P.regPeriodsTip;
                        const bool says = lang == 1 ? tip.contains ("MITJANA")
                                                    : tip.contains ("AVERAGE");
                        // ...and the same rule for the OTHER thing this field's hover is the
                        // only home of: that the band reaches both ways. The caption is one
                        // word ("Periods") and the line under it is numbers, so if the hover
                        // stops saying it, nothing on screen says it in words at all.
                        const bool both = lang == 1 ? tip.contains ("PER SOTA")
                                                    : tip.contains ("BELOW");
                        std::printf ("    Periods hover: %d chars, says the aggregate is the average: %s,"
                                     " says the band reaches below the note: %s (%s)\n",
                                     tip.length(), says ? "yes" : "NO", both ? "yes" : "NO",
                                     lang == 1 ? "ca" : "en");
                        check (says,
                               "and the Periods field's hover says the spectrum is optimised as the AVERAGE"
                               " across the register span");
                        check (both,
                               "and that the band runs BELOW the played note as well as above it");
                    }

                    // THE WRAPPING READ-OUTS. A juce::Label lays out in as many lines as
                    // its height allows and then CONDENSES - and modern JUCE shrinks the
                    // font first - so a sentence that overflows still looks like text and
                    // no bounds check in the build can see it. That is exactly how the old
                    // "sounding now:" line shipped clipped. Every state of the read-out is
                    // measured here, not just whichever one happens to be showing.
                    {
                        int over = 0; juce::String worst; float worstBy = 0.0f;
                        const auto judge = [&] (const std::vector<FractalAdditiveAudioProcessorEditor::LayoutProbe::WrappedLabel>& v)
                        {
                            for (const auto& s : v)
                            {
                                const float dh = s.needHeight - (float) s.cell.getHeight();
                                const float dw = s.needWidth  - (float) s.cell.getWidth();
                                const float d  = juce::jmax (dh, dw);
                                if (d > 0.5f)
                                {
                                    ++over;
                                    if (d > worstBy) { worstBy = d; worst = s.name + " \"" + s.text + "\""; }
                                }
                            }
                        };
                        judge (P.wrappedLabels);
                        judge (P.readoutVariants);
                        if (sh.mode == 2)
                        {
                            std::printf ("    %d wrapping read-outs measured, %d overflow%s\n",
                                         (int) (P.wrappedLabels.size() + P.readoutVariants.size()), over,
                                         over > 0 ? (" (worst: " + worst + " by "
                                                     + juce::String ((int) std::ceil (worstBy)) + " px)").toRawUTF8() : "");
                            // The live labels can legitimately be empty here (the verifier
                            // has no message loop, so the timer that writes them never
                            // runs) - the VARIANTS are the ones that must always be there.
                            check (! P.readoutVariants.empty(),
                                   "the adaptation's sentences are probed at all");
                            check (over == 0,
                                   lang == 1 ? "every sentence the read-outs can print fits its box (Catalan)"
                                             : "every sentence the read-outs can print fits its box (English)");
                        }
                    }

                    // ...and the results list must SURVIVE those sentences. It is the
                    // LEFTOVER of this column: every row added above it comes straight out
                    // of its height, and a list squeezed to a sliver still sits inside its
                    // box, overlaps nothing and passes every other check here. The bar is
                    // AdaptList::kRowH = 18: three partial rows visible without scrolling
                    // in the tightest shape the panel has (goal Range adds a whole row).
                    // Before this pass it was 68 px in the Min shape and 43 in Range.
                    if (sh.mode == 2)
                        for (const auto& c : P.controls)
                            if (c.first == "spec.adaptList")
                            {
                                std::printf ("    adapted-partial list: %d px tall = %d rows (%s)\n",
                                             c.second.getHeight(), c.second.getHeight() / 18,
                                             lang == 1 ? "ca" : "en");
                                check (c.second.getHeight() >= 3 * 18,
                                       "the adapted-partial list still shows several partials unscrolled");
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

        // THE SPECTRUM PANEL'S DIVIDER LINES.
        //
        // "la linea de la pared izquierda de la subcaja de prime esta incompleta, tiene
        //  un hueco abajo."
        // "hay una linea fina horizontal en custom spectrum entre el boton remove y la
        //  caja de ratios de abajo, es inecesaria y fea, quitala."
        //
        // A divider is a bare drawLine with no component behind it: nothing else in this
        // file can see one. The BASE | HARMONICS wall stopped at the prime cell's bottom
        // - 23 px above the panel floor its three neighbours reach - because the
        // full-width TEMPER row sits under both columns; a ToggleButton paints only a
        // tick and its text, so that stretch was bare panel and the wall ended in mid-air.
        // Read off spectrumDividers(), the ONE function paint() strokes from.
        if (ed != nullptr)
        {
            std::printf ("\nthe spectrum panel's divider lines:\n");
            auto verticals = [] (const FractalAdditiveAudioProcessorEditor::LayoutProbe& q)
            {
                std::vector<std::pair<juce::String, juce::Line<float>>> v;
                for (const auto& d : q.dividers)
                    if (std::abs (d.second.getStartX() - d.second.getEndX()) < 0.01f)
                        v.push_back (d);
                return v;
            };
            auto names = [] (const FractalAdditiveAudioProcessorEditor::LayoutProbe& q)
            {
                juce::StringArray s;
                for (const auto& d : q.dividers) s.add (d.first);
                return s.joinIntoString (", ");
            };

            p.setTimbreEnabled (false);
            ed->refreshEngineDressing();
            const auto harm = ed->layoutProbe();
            std::printf ("  harmonic view: %s\n", names (harm).toRawUTF8());

            const auto hv = verticals (harm);
            check (hv.size() == 4,
                   "the harmonic view draws four vertical walls (engine|both, base|harmonics, "
                   "slope|fractal, fractal|morph)");
            bool sameSpan = true;
            juce::String odd;
            for (const auto& d : hv)
            {
                std::printf ("    %-16s x=%.1f  y %.1f..%.1f\n", d.first.toRawUTF8(),
                             d.second.getStartX(), d.second.getStartY(), d.second.getEndY());
                if (! hv.empty()
                     && (std::abs (d.second.getStartY() - hv[0].second.getStartY()) > 0.01f
                      || std::abs (d.second.getEndY()   - hv[0].second.getEndY())   > 0.01f))
                { sameSpan = false; odd = d.first; }
            }
            check (sameSpan,
                   "every vertical wall runs the SAME full height of the panel - none stops "
                   "short and leaves a hole under it");
            if (! sameSpan)
                std::printf ("    the odd one out is \"%s\"\n", odd.toRawUTF8());

            // ...and the CUSTOM view draws NOTHING inside the engine box.
            p.setTimbreEnabled (true);
            ed->refreshEngineDressing();
            const auto cust = ed->layoutProbe();
            std::printf ("  custom view:   %s\n", names (cust).toRawUTF8());
            check (names (cust) == "engine|both, slope|fractal, fractal|morph, morph|range",
                   "the custom view adds no divider of its own - the thin line between the "
                   "Remove button and the ratios box is gone");

            // THE CUSTOM RATIOS BOX'S OWN CLEARANCES.
            // "adapta la caja de custom ratios para que los numeros no toquen su borde."
            // Still in the custom view - it is the only one that lays this box out.
            std::printf ("\nthe custom ratios box:\n");
            const auto ink = ed->ratiosInkProbeForTests (
                // Enough tokens to WRAP: a one-line ink would never come near the bottom,
                // and the bottom is the side a TextEditor gives no indent to.
                "1 9/8 5/4 4/3 3/2 5/3 15/8 2 9/4 5/2 8/3 3 10/3 15/4 4 "
                "17/4 9/2 19/4 5 21/4 11/2 23/4 6 25/4 13/2 27/4 7");
            std::printf ("  box %dx%d  clip (%d,%d %dx%d)  ink (%d,%d %dx%d)  radius %.1f  gap %.1f\n",
                         ink.box.getWidth(), ink.box.getHeight(),
                         ink.clip.getX(), ink.clip.getY(), ink.clip.getWidth(), ink.clip.getHeight(),
                         ink.ink.getX(), ink.ink.getY(), ink.ink.getWidth(), ink.ink.getHeight(),
                         ink.radius, ink.minGap);
            check (ink.minGap >= 2.0f,
                   "the ratios box keeps clear pixels between its outline and the area any "
                   "digit can be drawn in - on all four sides");
            check (ink.clipInsideOutline,
                   "...and the whole of that area is INSIDE the drawn outline: the pill's arc "
                   "no longer cuts across the first and last rows of numbers");
            check (ink.inkInsideClip,
                   "the numbers really are laid out inside it");

            p.setTimbreEnabled (false);
            ed->refreshEngineDressing();
        }

        // THE AMP DECAY FADER AFTER A PENCIL ON/OFF CYCLE.
        //
        // Every control in SPECTRUM wears the panel colour, with ONE stated exception: the
        // AMP DECAY (slope) fader goes grey while manual per-partial amplitudes are on,
        // because it is then inert. Toggling that off has to give the panel colour BACK -
        // and it did not: the un-freeze path restored `cSlope`, the mint this panel
        // stopped using when the engines merged, so one pencil cycle left a green fader in
        // a blue panel for the rest of the session. Colour, not geometry, so nothing else
        // in this file could have seen it.
        if (ed != nullptr)
        {
            auto* pm = p.apvts.getParameter ("partialManual");
            auto slope = [&] { return ed->layoutProbe().slopeColour; };
            const auto panel = ed->layoutProbe().panelColour;
            pm->setValueNotifyingHost (0.0f); ed->refreshSlopeFreezeForTests();
            const auto live0 = slope();
            pm->setValueNotifyingHost (1.0f); ed->refreshSlopeFreezeForTests();
            const auto frozen = slope();
            // ...and an engine reselection while frozen must NOT repaint it live: that was
            // the mirror-image half of the same bug (recolourSpectrumControls painted every
            // fader the panel hue, deleting the panel's only inertness warning, and the
            // freeze refresh then refused to restore it because its state had not changed).
            ed->refreshEngineDressing();
            const auto frozenAfterEngine = slope();
            pm->setValueNotifyingHost (0.0f); ed->refreshSlopeFreezeForTests();
            const auto live1 = slope();
            std::printf ("  AMP DECAY fader: panel %s   live %s -> frozen %s -> engine redress %s -> live %s\n",
                         panel.toDisplayString (false).toRawUTF8(),
                         live0.toDisplayString (false).toRawUTF8(),
                         frozen.toDisplayString (false).toRawUTF8(),
                         frozenAfterEngine.toDisplayString (false).toRawUTF8(),
                         live1.toDisplayString (false).toRawUTF8());
            check (live0 == panel, "the AMP DECAY fader wears the panel colour while it is live");
            check (frozen != panel, "and goes off it while manual amplitudes freeze it (it says it is inert)");
            check (frozenAfterEngine == frozen,
                   "and stays inert-grey when the engine is reselected under it");
            check (live1 == panel,
                   "and a pencil on/off cycle puts the panel colour BACK (it used to leave it mint green)");
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

        //== THE KEYBOARD IS CENTRED ============================================
        // "el teclado ahora mismo no aparece centrado en el sinte, haz que asi sea"
        //
        // Two claims, and only the first was ever true by construction:
        //   (1) the COMPONENT is centred in the editor - resized() takes it off the
        //       bottom of an evenly inset area, so this holds at every width and the
        //       check exists to keep a future panel change from quietly nudging it;
        //   (2) the KEYS DRAWN ON IT are centred inside the component - which is a
        //       different rectangle, and was NOT centred. paint()'s loop stops at the
        //       tuning's last playable note, so a wide-period tuning (only a handful of
        //       keys between 40 Hz and 4186 Hz) filled the left third of the strip and
        //       left the rest blank page. Nothing in this build could see it: the
        //       component's own bounds were, and still are, perfectly centred.
        //
        // Swept at several widths on purpose. One width proves nothing about a layout
        // whose panels are percentages: the leftover the keys are centred in changes
        // with every one of them.
        if (ed != nullptr)
        {
            auto centring = [] (const FractalAdditiveAudioProcessorEditor::LayoutProbe& P,
                                int edW, const char* what)
            {
                const int compOff = P.keyboardBox.getX() - (edW - P.keyboardBox.getRight());
                const int keysOff = (P.keysDrawn.getX() - P.keyboardBox.getX())
                                  - (P.keyboardBox.getRight() - P.keysDrawn.getRight());
                std::printf ("  %-22s editor %d: component %d..%d (off centre by %d), "
                             "keys %d..%d of %d cells for %d playable (off centre by %d)\n",
                             what, edW, P.keyboardBox.getX(), P.keyboardBox.getRight(), compOff,
                             P.keysDrawn.getX(), P.keysDrawn.getRight(),
                             P.keysInWindow, P.keysPlayable, keysOff);
                check (! P.keyboardBox.isEmpty() && ! P.keysDrawn.isEmpty(),
                       "the keyboard and its keys are on screen");
                check (std::abs (compOff) <= 1,
                       "the keyboard component is centred in the editor");
                // 2 px: the cells are floats and the span is rounded outwards at both ends.
                check (std::abs (keysOff) <= 2,
                       "and the keys drawn on it are centred inside it");
            };

            const int widths[] = { 1200, 1000, 1400, 1320 };
            for (const int w : widths)
            {
                ed->setSize (w, 650);
                centring (ed->layoutProbe(), w, "12-TET");
            }

            // ...and the case that was actually broken: a tuning whose period is three
            // octaves, so 40 Hz .. 4186 Hz holds a handful of keys and the window has
            // room for thirty-five. Before this, those keys started at the left edge.
            {
                juce::String err;
                const bool wide = p.loadScalaText ("! wide.scl\nthree-octave period\n 1\n!\n 3600.0\n",
                                                   "wide-period", err);
                check (wide, "a three-octave-period scale loads (the off-centre case)");
                ed->resized();
                for (const int w : widths)
                {
                    ed->setSize (w, 650);
                    const auto P = ed->layoutProbe();
                    centring (P, w, "three-octave period");
                    check (P.keysPlayable < P.keysInWindow,
                           "precondition: this tuning really does leave the window part empty");
                }
                p.clearScala();        // back to 12-TET for everything after this
                ed->resized();
            }
            ed->setSize (1200, 650);
        }

        //== THE PRESET PLAYGROUND BUTTON =======================================
        // A TextButton does NOT condense the way a Label does - it CLIPS - and
        // "Obre el Laboratori de Presets" is eight characters longer than the English.
        // Measured in the very font drawButtonText uses (LookAndFeel_V4: the button's
        // height, less 6 px of vertical padding, capped at 15), against the cell the
        // layout really gives it.
        if (ed != nullptr)
        {
            for (int lang = 0; lang < 2; ++lang)
            {
                const auto P = ed->layoutProbeInLanguage (lang == 1);
                juce::String w;
                for (const auto& b : P.buttonTexts) if (b.first == "top.playground") w = b.second;
                juce::Rectangle<int> cell;
                for (const auto& c : P.controls) if (c.first == "top.playground") cell = c.second;
                const float fh = juce::jmin (15.0f, (float) cell.getHeight() * 0.6f);
                const float need = (float) juce::GlyphArrangement::getStringWidthInt (
                                       juce::Font (juce::FontOptions (fh)), w);
                std::printf ("  playground button (%s): \"%s\" needs %.0f px in %d\n",
                             lang == 1 ? "ca" : "en", w.toRawUTF8(), need, cell.getWidth());
                check (w.containsIgnoreCase (lang == 1 ? "Laboratori" : "Playground"),
                       "the playground button says what it opens, in this language");
                check (! cell.isEmpty() && need <= (float) cell.getWidth() - 8.0f,
                       "...and the word fits inside the cell the layout gives it, with margin");
                check (cell.getRight() <= 1200 && cell.getBottom() <= 78,
                       "...inside the title bar, not spilling over the panels below");
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

        //== PERIOD NUMBERS on the keys, and the register range that is counted in STEPS ==
        //
        // "C2".."C4" is 12-EDO's naming, and the register range - the one control that
        // decides WHICH REGISTERS a spectrum is optimised for - was expressed in it. Under
        // 31-EDO or Bohlen-Pierce those names denote nothing the tuning has. The keyboard
        // prints a PERIOD number beside every degree-0 key instead, and that is how the
        // range's anchor is named; the width itself is a count of STEPS above that anchor.
        // Three things have to hold and none of them is visible to any other check here.
        if (ed != nullptr)
        {
            std::printf ("period numbers on the keys, and as the register unit:\n");
            ed->setSpecModeForTests (2);
            ed->resized();

            // (1) THE NUMBER ITSELF. Counted the way octaves are counted, with the
            //     reference key (60) as period 3 - which is what the rest of the product
            //     calls C3 - and by FLOOR division, so the period below the reference is 2
            //     and not 3 again. In 12-EDO it must agree key for key with the note names
            //     it replaced; in 31-EDO it must not.
            {
                bool ok12 = true;
                for (int k = 0; k <= 127; k += 1)
                    if (ScaleKeyboard::periodNumberForNote (k, 12) != k / 12 - 2) ok12 = false;
                check (ok12, "in 12-EDO a period number is exactly the octave number it replaced");
                check (ScaleKeyboard::periodNumberForNote (60, 31) == 3
                       && ScaleKeyboard::periodNumberForNote (59, 31) == 2
                       && ScaleKeyboard::periodNumberForNote (29, 31) == 2
                       && ScaleKeyboard::periodNumberForNote (28, 31) == 1,
                       "in 31-EDO it counts 31-key periods, and the key below a boundary is the period below");
                check (ScaleKeyboard::noteForPeriodNumber (3, 31) == 60
                       && ScaleKeyboard::noteForPeriodNumber (2, 31) == 29
                       && ScaleKeyboard::noteForPeriodNumber (9, 31) == -1,
                       "and the inverse lands on the period's own first key, or says the board has none");
            }

            // (2) IT HAS TO BE READABLE. drawFittedText does not clip a number that is too
            //     big for its rect - it SHRINKS it - so an unreadable period tag looks like
            //     a deliberate design choice and no bounds check in this build can see it.
            //     Measured in the very font paint() sets, against the very rect it draws
            //     into, at the key width the keyboard really uses.
            {
                const auto tags = ed->keyboardPeriodTagsForTests();
                int tooNarrow = 0, sideBySide = 0, offColumn = 0, tiny = 0;
                juce::String firstBad;
                // THE STACK. The period number is ABOVE the "0" now, and the "0" is back on
                // the column every other degree number stands in - the user's complaint was
                // that the tag beside it DISPLACED it left. Neither is visible to an overlap
                // check: a tag parked to the right of the digit overlaps nothing at all, and
                // that was the shipped bug.
                float worstEdge = 1e9f, gapDisagree = 0.0f, worstColumn = 0.0f;
                int   tooFar = 0, tooSmall = 0, touchesBorder = 0;
                for (const auto& t : tags)
                {
                    const float gg = t.zero.getY() - t.tag.getBottom();
                    gapDisagree = juce::jmax (gapDisagree, std::abs (gg - t.gap));
                    // Close enough to read as an annotation of that "0" (it was 6.19 px of
                    // air side by side), never so close the two numbers' ink touches.
                    if (gg > 3.5f || gg < 0.5f) ++tooFar;
                    // "un poco mas grandes" - the tag was 8.8 pt at the smallest key.
                    if (t.tagFont < 9.5f) ++tooSmall;
                    if (t.tagTextWidth > (float) t.tag.getWidth()
                        || t.zeroTextWidth > (float) t.zero.getWidth())
                    { ++tooNarrow; if (firstBad.isEmpty()) firstBad = "period " + juce::String (t.number); }
                    // STACKED, not beside: the tag's whole rect is above the "0"'s.
                    if (t.tag.getBottom() > t.zero.getY() + 0.01f) ++sideBySide;
                    // CENTRED: the "0" stands exactly where the "1" on the next key does.
                    worstColumn = juce::jmax (worstColumn, std::abs (t.zeroCentreX - t.degreeCentreX));
                    if (std::abs (t.zeroCentreX - t.degreeCentreX) > 0.51f) ++offColumn;
                    // "que no toque el borde" - the user's words, as a number. Measured on
                    // the integer rects paint() draws into, on all four sides of the key.
                    worstEdge = juce::jmin (worstEdge, t.edge);
                    if (t.edge < 1.0f) ++touchesBorder;
                    if (t.tagFont < 7.0f) ++tiny;
                }
                std::printf ("  %d degree-0 keys on screen: %d shrunk to fit%s, %d not stacked,"
                             " %d off the degree column, %d below 7 pt, %d touching the key border\n",
                             (int) tags.size(), tooNarrow,
                             firstBad.isEmpty() ? "" : (" (" + firstBad + ")").toRawUTF8(),
                             sideBySide, offColumn, tiny, touchesBorder);
                if (! tags.empty())
                    std::printf ("  first tag: period %d, number %.1f pt needing %.1f px of %.0f ABOVE"
                                 " a %.1f pt \"0\" in %.0f px; column error %.3f px, border clearance"
                                 " %.2f px, vertical gap %.2f\n",
                                 tags.front().number, tags.front().tagFont, tags.front().tagTextWidth,
                                 tags.front().tag.getWidth(), tags.front().zeroFont,
                                 tags.front().zero.getWidth(), worstColumn, worstEdge,
                                 tags.front().gap);
                check (! tags.empty(), "every period boundary on screen carries its number");
                check (tooNarrow == 0, "and every one of them fits its rect without being squeezed");
                check (sideBySide == 0, "the period number sits ABOVE the \"0\", never beside it");
                check (offColumn == 0, "and the \"0\" is centred on the very column the other degrees use");
                check (touchesBorder == 0, "and neither number touches the key's border");
                check (tiny == 0, "and none of them is drawn below 7 pt");
                check (gapDisagree < 0.01f,
                       "the layout's own gap is the one the drawn rects produce");
                check (tooFar == 0, "the period number sits CLOSE above its \"0\" (0.5 .. 3.5 px)");
                check (tooSmall == 0, "and is drawn larger than the 8.8 pt it used to be");

                // ...INCLUDING the numbers this tuning does not happen to show, AT THE
                // SMALLEST KEY THE KEYBOARD EVER DRAWS. A period number runs from one to
                // three characters (a one-degree-per-period scale reaches "-10"), and the
                // widest is the one nobody was looking at; keys have a fixed width, so
                // kCellW - not the cell this window happened to open at - is the worst case.
                // Measured through the same layout call paint() uses. This is where a stack
                // fails: two numbers where there was one, in a band that starts at the key's
                // own top edge.
                if (! tags.empty())
                {
                    const float cellW = (float) ScaleKeyboard::kCellW;
                    const auto& t0    = tags.front();
                    const auto  key   = ScaleKeyboard::cellRectForTests (cellW, t0.cellH);
                    const float col   = ScaleKeyboard::degreeCentreXForTests (cellW, t0.cellH, t0.label);
                    int narrow = 0, small = 0, far = 0, border = 0, offCol = 0;
                    for (const char* s : { "3", "-2", "-10" })
                    {
                        const juce::String txt (s);
                        const auto z = ScaleKeyboard::layOutPeriodTagForCell (cellW, t0.cellH,
                                                                             t0.label, txt);
                        const float w  = ScaleKeyboard::boldTextWidth (txt, z.tagFont);
                        const float zw = ScaleKeyboard::boldTextWidth ("0", z.zeroFont);
                        const float gg = z.zero.getY() - z.tag.getBottom();
                        const auto ti = z.tag.getSmallestIntegerContainer().toFloat();
                        const auto zi = z.zero.getSmallestIntegerContainer().toFloat();
                        const float e = juce::jmin (juce::jmin (ti.getY() - key.getY(),
                                                                key.getBottom() - zi.getBottom()),
                                                    juce::jmin (juce::jmin (ti.getX() - key.getX(),
                                                                            key.getRight() - ti.getRight()),
                                                                juce::jmin (zi.getX() - key.getX(),
                                                                            key.getRight() - zi.getRight())));
                        std::printf ("    \"%s\" at a %d px key: %.1f pt, needs %.1f px of %.1f, above a"
                                     " %.1f pt \"0\" (%.1f px of %.1f); gap %.2f, border clearance %.2f,"
                                     " column error %.3f\n",
                                     s, ScaleKeyboard::kCellW, z.tagFont, w, z.tag.getWidth(),
                                     z.zeroFont, zw, z.zero.getWidth(), gg, e,
                                     std::abs (z.zero.getCentreX() - col));
                        if (w > z.tag.getWidth() || zw > z.zero.getWidth()) ++narrow;
                        // The "0" is the scale degree: a stack that pays for itself by
                        // shrinking the digit it annotates has solved nothing. 18 pt is the
                        // side-by-side layout's own "0" at this key size.
                        if (z.tagFont < 7.0f || z.zeroFont < 18.0f) ++small;
                        if (gg > 3.5f || gg < 0.5f) ++far;
                        if (e < 1.0f) ++border;
                        if (std::abs (z.zero.getCentreX() - col) > 0.51f) ++offCol;
                    }
                    check (narrow == 0, "a one-, two- or three-character period all fit their rect");
                    check (small == 0, "and none of them costs the \"0\" its own size");
                    check (far == 0, "and every one of them stays tight above its \"0\"");
                    check (border == 0, "and none of them touches the key border at the smallest key drawn");
                    check (offCol == 0, "and the \"0\" stays on the degree column whatever the period is");
                }
            }

            // (3) ONE NUMBER: N PERIODS EACH WAY FROM THE NOTE BEING PLAYED. The range was
            //     two absolute bounds first (note names, then period numbers), both pinned
            //     to fixed pitches so the adaptation answered with the same spectrum
            //     whichever register was actually played; then a count of STEPS running
            //     UPWARD, which followed the hands but never asked what the spectrum does
            //     BELOW them and could not be read as a width without dividing by the
            //     degree count. What has to hold now: the width is in the TUNING'S OWN
            //     PERIODS, the band straddles the anchor, and the anchor MOVES. None of it
            //     is visible to any other check in this build.
            {
                const int D = juce::jmax (1, p.getScaleDegrees());
                juce::AudioBuffer<float> rbuf (2, blk);
                const auto play = [&] (std::initializer_list<int> keys, bool on)
                {
                    juce::MidiBuffer m;
                    for (const int k : keys)
                        m.addEvent (on ? juce::MidiMessage::noteOn (1, k, (juce::uint8) 100)
                                       : juce::MidiMessage::noteOff (1, k), 0);
                    rbuf.clear();
                    p.processBlock (rbuf, m);
                };
                // The band's ends are FREQUENCIES (a fractional width lands between keys),
                // so everything below is judged in cents off the anchor - the unit the
                // claim is actually made in.
                const auto centsOff = [&] (int anchorKey, double hz)
                {
                    const double a = p.getNoteFrequency (anchorKey);
                    return (a > 0.0 && hz > 0.0) ? 1200.0 * std::log2 (hz / a) : 0.0;
                };

                // (3a) THE DEFAULT IS A WIDTH IN PERIODS, and the SAME number in every
                //      tuning - that is the whole reason the unit changed from steps
                //      (see dissonance::kAdaptRegisterDefaultPeriods, which SpectrumTests
                //      measures). The band it produces is symmetric about the anchor.
                const auto d0 = ed->registerProbeForTests();
                std::printf ("  default width: %.1f periods of a %d-degree scale = %.1f c each way; \"%s\"\n",
                             d0.periods, D, centsOff (d0.anchorKey, d0.hiHz), d0.spanText.toRawUTF8());
                check (std::abs (d0.periods - dissonance::kAdaptRegisterDefaultPeriods) < 1.0e-9,
                       "a fresh panel starts at the measured default width, in the tuning's own periods");
                check (d0.periodsText == juce::String (dissonance::kAdaptRegisterDefaultPeriods, 1),
                       "and the field shows that number rather than leaving the user to guess it");
                check (std::abs (centsOff (d0.anchorKey, d0.hiHz)
                                 - 1200.0 * dissonance::kAdaptRegisterDefaultPeriods) < 0.05
                       && std::abs (centsOff (d0.anchorKey, d0.loHz)
                                    + 1200.0 * dissonance::kAdaptRegisterDefaultPeriods) < 0.05,
                       "and one period each way in 12-EDO really is 1200 cents DOWN and 1200 UP");
                check (d0.spanText.contains (juce::String (dissonance::kAdaptRegisterDefaultPeriods, 1))
                       || d0.spanText.contains (juce::String (dissonance::kAdaptRegisterDefaultPeriods, 1)
                                                    .replaceCharacter ('.', ',')),
                       "and the line under it repeats the width - two frequencies alone are a magic number");

                // (3b) THE ANCHOR RULE, all three branches. Held (lowest), else last
                //      played, else key 60. The middle branch is the one that makes the
                //      hand-pressed Adapt usable at all: nobody can hold a chord and click
                //      a button at the same time, and a range that vanished with the notes
                //      would put that button back to guessing.
                play ({ 55, 48 }, true);
                const auto held = ed->registerProbeForTests();
                play ({ 55, 48 }, false);
                const auto letGo = ed->registerProbeForTests();
                std::printf ("  anchor: holding 48+55 -> key %d (\"%s\"); after release -> key %d\n",
                             held.anchorKey, held.spanText.toRawUTF8(), letGo.anchorKey);
                check (held.anchorKey == 48,
                       "the range hangs off the LOWEST note being held, not the last one pressed");
                check (letGo.anchorKey == 48,
                       "and it stays there when the keys come up - the Adapt button always has an anchor");
                check (held.spanText.contains (":"),
                       "and the panel NAMES that note (period:degree), so the user can see what the width is counted from");

                // (3c) THE BAND RESOLVES ANCHOR +- N PERIODS THROUGH THE TUNING, IT
                //      STRADDLES THE ANCHOR, and it MOVES with the hands. The straddling
                //      half is the new part: the band used to START at the played key, so
                //      keyLo == anchorKey was the shipping behaviour and is now a bug.
                play ({ 72 }, true);
                const auto up = ed->registerProbeForTests();
                play ({ 72 }, false);
                std::printf ("  playing 72: %.1f..%.1f Hz, %.1f c either side (was %.1f..%.1f Hz from key 48)\n",
                             up.loHz, up.hiHz, centsOff (72, up.hiHz), held.loHz, held.hiHz);
                const double wantC = 1200.0 * up.periods;   // 12-EDO: one period is 1200 c
                check (std::abs (centsOff (72, up.loHz) + wantC) < 0.05
                       && std::abs (centsOff (72, up.hiHz) - wantC) < 0.05,
                       "the band runs N periods BELOW the played key and N periods above it");
                const double anchorHz72 = p.getNoteFrequency (72);
                check (up.loHz < anchorHz72 && up.hiHz > anchorHz72,
                       "so the note being played is INSIDE the band, not standing at its bottom edge");
                check (std::abs (up.loHz - p.getNoteFrequency (60)) < 1.0e-9
                       && std::abs (up.hiHz - p.getNoteFrequency (84)) < 1.0e-9,
                       "and at a whole number of periods both ends land on the ACTIVE TUNING's own keys");
                check (up.loHz > held.loHz * 1.5 && up.hiHz > held.hiHz * 1.5,
                       "and moving up the keyboard really does move the registers being optimised for");

                // (3d) WHAT THE PANEL SHOWS IS WHAT THE SEARCH GETS. The editor pushes the
                //      width in PERIODS and the processor resolves it; if those two ever
                //      disagreed the panel would be describing a run that never happened.
                {
                    FractalAdditiveAudioProcessor::SpectrumAdaptSettings st;
                    st.regPeriods = up.periods;
                    const auto regs = p.adaptRegistersFor (st, p.getNoteFrequency (60), false);
                    const auto now = ed->registerProbeForTests();
                    check (! regs.f0.empty()
                           && std::abs (regs.f0.front() - now.loHz) < 1.0e-9
                           && std::abs (regs.f0.back()  - now.hiHz) < 1.0e-9,
                           "the Hz the search is really given are the Hz the panel is showing");
                }

                // (3e) THE BOUNDS ARE THE TUNING'S OWN PERIOD, IN CENTS, IN EVERY TUNING.
                //      This is the check the unit change is FOR: "N periods" has to come out
                //      as N times whatever that scale's period really is - 1200 c in 12-EDO,
                //      1200 in 19-EDO with a different degree count under it, 1902 c for
                //      Bohlen-Pierce's 3/1, and 1200 again for a five-note octave scale
                //      whose keys are 240 c apart. A step count could not state this at all;
                //      an implementation that quietly used 12-EDO arithmetic, or that fell
                //      back to an octave, fails here and nowhere else.
                {
                    struct Tun { const char* name; const char* text; int degrees; double periodC; };
                    const juce::String edo19 = [] { juce::String s; for (int i = 1; i <= 19; ++i)
                                                        s << " " << juce::String (1200.0 * i / 19.0, 5) << "\n"; return s; } ();
                    const juce::String bp    = [] { juce::String s; for (int i = 1; i <= 13; ++i)
                                                        s << " " << juce::String (1901.955 * i / 13.0, 5) << "\n"; return s; } ();
                    const Tun tuns[] = {
                        { "12-EDO",        nullptr, 12, 1200.0 },
                        { "19-EDO",        nullptr, 19, 1200.0 },
                        { "Bohlen-Pierce", nullptr, 13, 1901.955 },
                        { "5-note octave", nullptr,  5, 1200.0 } };
                    const juce::String texts[4] = {
                        "! e12.scl\n12-EDO\n 12\n!\n" + [] { juce::String s; for (int i = 1; i <= 12; ++i)
                                                        s << " " << juce::String (100.0 * i, 5) << "\n"; return s; } (),
                        "! e19.scl\n19-EDO\n 19\n!\n" + edo19,
                        "! bp.scl\nBohlen-Pierce\n 13\n!\n" + bp,
                        "! p5.scl\n5 per octave\n 5\n!\n 240.00000\n 480.00000\n 720.00000\n 960.00000\n 1200.00000\n" };
                    // Two widths, so the check is about N*period and not about "one period
                    // happens to be one degree-count of keys".
                    const double widths[] = { 1.0, 1.5 };
                    for (int t = 0; t < 4; ++t)
                    {
                        juce::String e;
                        // check() takes a const char*, so every message here is materialised
                        // into a named local first: a temporary juce::String would be dead
                        // by the time the failure line is printed.
                        const juce::String mLoad = juce::String ("the ") + tuns[t].name + " scale loads";
                        const juce::String mDeg  = juce::String ("...with the ") + juce::String (tuns[t].degrees)
                                                 + " degrees per period it declares";
                        check (p.loadScalaText (texts[t], tuns[t].name, e), mLoad.toRawUTF8());
                        check (p.getScaleDegrees() == tuns[t].degrees, mDeg.toRawUTF8());
                        for (const double w : widths)
                        {
                            ed->setAdaptRegisterPeriodsForTests (juce::String (w, 1));
                            play ({ 60 }, true); play ({ 60 }, false);
                            ed->resized();
                            const auto r = ed->registerProbeForTests();
                            const double anchorHz = p.getNoteFrequency (r.anchorKey);
                            const double dn = 1200.0 * std::log2 (anchorHz / r.loHz);
                            const double upC = 1200.0 * std::log2 (r.hiHz / anchorHz);
                            std::printf ("  %-14s +-%.1f per: %.1f..%.1f Hz, -%.1f c / +%.1f c"
                                         " (the scale's own period is %.1f c, so %.1f c is asked for)\n",
                                         tuns[t].name, w, r.loHz, r.hiHz, dn, upC,
                                         tuns[t].periodC, w * tuns[t].periodC);
                            // 0.05 c, not "about right": the ends are the anchor's frequency
                            // times the tuning's own period ratio, so they are exact to
                            // floating point, and a loose bound would pass an implementation
                            // that used the wrong period - or the old one, which rounded the
                            // width onto the nearest KEY and missed by up to half a step
                            // (120 cents in the five-note scale below).
                            const juce::String mBand = juce::String ("in ") + tuns[t].name
                                                     + " the band reaches exactly " + juce::String (w, 1)
                                                     + " of THAT tuning's periods each way";
                            check (std::abs (dn  - w * tuns[t].periodC) < 0.05
                                   && std::abs (upC - w * tuns[t].periodC) < 0.05,
                                   mBand.toRawUTF8());
                        }
                    }
                }

                // (3f) A TYPED N NEEDS NO PROTECTION FROM A TUNING CHANGE any more, and that
                //      is the point of the unit: a period IS the tuning's own unit, so 1.5
                //      means one and a half periods of whatever is loaded, before the swap
                //      and after it. Under the step count this needed a rule (and a bug
                //      where the rule was missing); here it is arithmetic.
                p.clearScala();
                ed->setAdaptRegisterPeriodsForTests ("1.5");
                play ({ 60 }, true); play ({ 60 }, false);
                ed->resized();
                const auto typed = ed->registerProbeForTests();
                juce::String sclErr;
                // Bohlen-Pierce on purpose: swapping 12-EDO for another octave-based scale
                // would leave the band the same number of CENTS wide and prove nothing. A
                // 3/1 period makes the same typed 1.5 mean a genuinely different width.
                const bool loaded = p.loadScalaText (
                    "! bp2.scl\nBohlen-Pierce\n 13\n!\n"
                    + [] { juce::String s; for (int i = 1; i <= 13; ++i)
                               s << " " << juce::String (1901.955 * i / 13.0, 5) << "\n"; return s; } (),
                    "bp2", sclErr);
                play ({ 60 }, true); play ({ 60 }, false);
                ed->resized();
                const auto after = ed->registerProbeForTests();
                std::printf ("  typed 1.5 periods in 12-EDO (%.1f c each way), then loaded Bohlen-Pierce:"
                             " still %.1f periods, now %.1f c each way (\"%s\")\n",
                             centsOff (typed.anchorKey, typed.hiHz), after.periods,
                             centsOff (after.anchorKey, after.hiHz), after.spanText.toRawUTF8());
                check (loaded, "the Bohlen-Pierce scale loads");
                check (std::abs (typed.periods - 1.5) < 1.0e-9 && std::abs (after.periods - 1.5) < 1.0e-9,
                       "a typed width survives a tuning change unchanged - it is already in the tuning's units");
                check (std::abs (centsOff (typed.anchorKey, typed.hiHz) - 1800.0) < 0.05
                       && std::abs (centsOff (after.anchorKey, after.hiHz) - 1.5 * 1901.955) < 0.05,
                       "and the same 1.5 periods is 1800 cents in 12-EDO and 2853 in Bohlen-Pierce - the width"
                       " follows the scale, the NUMBER does not");

                // The clamps, both of them, and both stated in the field rather than behind
                // it. 0 is legal - "just the register I am playing", the single-pitch
                // objective chosen on purpose - and the ceiling is a guard against a typo
                // pricing a run in hours, not a musical decision.
                ed->setAdaptRegisterPeriodsForTests ("99");
                const auto big = ed->registerProbeForTests();
                ed->setAdaptRegisterPeriodsForTests ("0");
                const auto zero = ed->registerProbeForTests();
                check (std::abs (big.periods - dissonance::kAdaptRegisterMaxPeriods) < 1.0e-9
                       && big.periodsText == juce::String (dissonance::kAdaptRegisterMaxPeriods, 1),
                       "a width past the ceiling snaps to it IN THE FIELD, not behind it");
                check (zero.periods == 0.0 && std::abs (zero.loHz - zero.hiHz) < 1.0e-9,
                       "and 0 is legal: one register, the played one");
                // Rubbish snaps back to the number in force rather than becoming "no width",
                // which the processor would read as the default and quietly undo the typing.
                ed->setAdaptRegisterPeriodsForTests ("");
                const auto junk = ed->registerProbeForTests();
                check (junk.periods == zero.periods && junk.periodsText.isNotEmpty(),
                       "unreadable text snaps back to the width in force, never to \"no width\"");
                // ...and the anchor near the top of the board, where N genuinely does not fit
                // ABOVE while it fits perfectly below. The band is cut at that end only, and
                // the shortened half must be SAID rather than left looking symmetric - which
                // is a failure the upward-only form could not even have.
                p.clearScala();   // back to 12-EDO: the numbers below are that scale's
                ed->setAdaptRegisterPeriodsForTests ("2.0");
                play ({ 120 }, true);
                ed->resized();
                const auto top = ed->registerProbeForTests();
                play ({ 120 }, false);
                std::printf ("  +-2 periods around key 120: -%.2f/+%.2f fit, %.1f..%.1f Hz, \"%s\"\n",
                             top.fitDown, top.fitUp, top.loHz, top.hiHz, top.spanText.toRawUTF8());
                check (std::abs (top.hiHz - p.getNoteFrequency (127)) < 1.0e-9
                       && std::abs (top.loHz - p.getNoteFrequency (96)) < 1.0e-9,
                       "at the top of the keyboard the band stops at the last key - the anchor is never moved to make N fit");
                check (std::abs (top.fitDown - 2.0) < 1.0e-6 && std::abs (top.fitUp - 7.0 / 12.0) < 1.0e-6,
                       "and the two halves are genuinely different there: the full width down, seven semitones up");
                check (! top.spanText.contains ("+-") && top.spanText.contains ("-2.0")
                       && top.spanText.contains ("0.6"),
                       "so the panel drops the symmetric \"+-\" form and prints each side's real reach");

                // Back to a known state for everything downstream: 12-EDO, default width.
                p.clearScala();
                ed->setAdaptRegisterPeriodsForTests (juce::String (dissonance::kAdaptRegisterDefaultPeriods, 1));
                play ({ 60 }, true); play ({ 60 }, false);
                ed->resized();
            }

            //== THE PRICE OF A RUN, ON THE BUTTON =============================
            // The panel used to REFUSE "all vs all over 31 notes x 16 partials" as "too
            // heavy to search". It is a calculation of tens of seconds on a worker thread
            // that already reports progress and already cancels. What a musician needs
            // there is the number, not a veto - so the button wears it.
            {
                ed->setSpecModeForTests (2);
                ed->resized();
                const double ms = ed->adaptCostMsForTests (false);
                const auto txt = ed->adaptButtonTextForTests();
                std::printf ("  loaded tuning, %d degrees: one Adapt ~%.0f ms; button says \"%s\"\n",
                             juce::jmax (1, p.getScaleDegrees()), ms, txt.toRawUTF8());
                check (ms > 0.0, "the panel can price a run before starting it");
                // Cheap runs must NOT shout a duration at the user; expensive ones must.
                check (ms < 950.0 ? ! txt.containsChar ('~') : txt.containsChar ('~'),
                       "the button carries a duration only when there is really a wait");
                // With nothing held the follower has no target and no cost, so the read-out
                // must NOT be warning about lag - a lag warning with no chord down would be
                // a warning about a search that is not going to happen.
                check (ed->adaptCostMsForTests (true) == 0.0,
                       "an empty hand prices a realtime update at nothing, not at a warning");

                // THE ESTIMATE'S SHAPE, through the processor's own estimator - the same
                // call the ten-minute guard measures itself against, so the price on the
                // button and the only refusal left cannot be two different numbers.
                {
                    FractalAdditiveAudioProcessor::SpectrumAdaptSettings s;
                    s.partials = 16; s.periodRatio = 8.0; s.minGapCents = 20.0;
                    // Six octaves of band, in the unit the setting is now in: three periods
                    // each way in 12-EDO. The estimator has to price the registers that
                    // width really produces, not a pair of frequencies nothing else uses.
                    s.shapeAmps = true; s.regPeriods = 3.0;
                    const double f0 = 261.6;
                    s.rootOnly = false;
                    const double heavy = p.estimateAdaptMs (s, 31, f0, false);
                    const double heavyRt = p.estimateAdaptMs (s, 31, f0, true);
                    s.rootOnly = true;
                    const double light = p.estimateAdaptMs (s, 31, f0, false);
                    std::printf ("  31 notes x 16 partials over 6 octaves: all-vs-all %.1f s"
                                 " (realtime cap %.1f s), 0-vs-all %.1f s; hard bound %.0f s\n",
                                 heavy * 0.001, heavyRt * 0.001, light * 0.001,
                                 dissonance::kAdaptAbsurdMs * 0.001);
                    // The realtime path takes the smaller register cap (3 instead of 7) and
                    // the cost is exactly linear in that count, so it is never dearer.
                    check (heavyRt < heavy,
                           "one realtime update never costs more than one hand-pressed Adapt");
                    // The configuration the old dialog REFUSED outright. It is tens of
                    // seconds - offered, priced, cancellable - not "too heavy to search".
                    check (heavy > dissonance::kAdaptWarnMs && heavy < dissonance::kAdaptAbsurdMs,
                           "the search the old dialog refused is warned about, and offered");
                    check (light < heavy * 0.5,
                           "and \"0 vs all\" is priced as the much cheaper scope it is");
                }
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
            const auto regs = p.adaptRegistersFor (st, f0, /*realtime*/ false);
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
            // THE REGISTER BAND, end to end through the real processor. A settings block
            // with no width set means the DEFAULT width - one period of the loaded scale
            // EACH WAY from the anchor key (test (a) above left that at key 60) - and the
            // result must SAY which registers it was optimised for, because with a
            // single-pitch objective (the bug this replaces) the answer is different.
            const double anchorHz = p.getNoteFrequency (p.registerAnchorKey());
            double botHz = 0.0, topHz = 0.0;
            p.registerBandHz (dissonance::kAdaptRegisterDefaultPeriods, botHz, topHz);
            std::printf ("  registers: %d points, %.2f..%.2f Hz (anchor key %d at %.2f Hz, target f0 %.2f),"
                         " aggregated as the mean\n",
                         info.regPoints, info.regLoHz, info.regHiHz, p.registerAnchorKey(),
                         anchorHz, f0);
            check (info.regPoints == 3 && std::abs (info.regLoHz - botHz) < 1.0e-6
                   && std::abs (info.regHiHz - topHz) < 1.0e-6,
                   "an unset width is one period either side of the note played, sampled at three registers");
            check (info.regLoHz < anchorHz * 0.999 && info.regHiHz > anchorHz * 1.001,
                   "so the played note sits INSIDE the band the spectrum was optimised over, not at its floor");
            check ((int) info.perRegister.size() == info.regPoints
                   && (int) info.registerF0.size() == info.regPoints,
                   "and the result carries one measured number per register");
            // The number the panel prints IS the aggregate the search minimised - the MEAN -
            // and the worst register is carried beside it as a read-out only. If `after`
            // ever came back as the worst register's own value again (which is what the
            // deleted minimax path made it), this is where it would show.
            check (std::abs (info.after - info.mean) < 1.0e-12,
                   "what the panel prints as \"after\" is the MEAN over the registers, not one of them");
            check (info.mean <= info.worst + 1.0e-12 && info.after <= info.seedWorst,
                   "and the read-outs stay consistent (the mean is never above the worst register)");
            // The realtime follower's cap is a STATED difference, not a silent one: at this
            // two-octave range it samples exactly as densely as the Adapt button does.
            const auto rtRegs = p.adaptRegistersFor (st, f0, /*realtime*/ true);
            check (rtRegs.f0.size() == regs.f0.size(),
                   "and at the default two-period width realtime samples exactly as densely");
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

        //-- (b2) SPECTRUM PROVENANCE: a spectrum derived FROM a tuning remembers which
        //   tuning, as full source text; the Tuning panel's "Load spectrum's tuning"
        //   brings it back note-for-note after the active tuning was replaced, a save
        //   and a reload - and it is honestly ABSENT where no tuning was the origin.
        {
            std::printf ("\nspectrum provenance (\"load spectrum's tuning\"):\n");
            juce::String err;
            // What this block must hand back to the tests downstream (they assume the
            // tuning and spectrum state (b) left): captured now, restored at the end.
            const auto origSrc  = p.activeSclSource();
            const auto origName = p.tuningScaleName();
            // A distinctive origin: 7-EDO, nothing else in this file uses it.
            juce::String edo7 ("! edo7.scl\nseven\n 7\n!\n");
            for (int i = 1; i <= 7; ++i)
                edo7 << " " << juce::String (1200.0 * i / 7.0, 5) << "\n";
            check (p.loadScalaText (edo7, "edo7", err), "the origin tuning (7-EDO) loads");
            std::array<double, 128> want {};
            for (int n = 0; n < 128; ++n) want[(size_t) n] = p.getNoteFrequency (n);

            // Derive the spectrum FROM that tuning (the dissonance-minimum spectrum of
            // 7-EDO): the caller states the target is the tuning, exactly as the panel's
            // Adapt with target = tuning does.
            Proc::SpectrumAdaptSettings st;
            st.partials = 4; st.periodRatio = 2.0; st.minGapCents = 50.0;
            st.rootOnly = true; st.goal = 0; st.shapeAmps = false;
            double f0 = 0.0;
            const auto alphas = p.tuningAsTargetAlphas (f0);
            check (p.startSpectrumAdapt (st, alphas, f0, err, false, /*targetIsTuning*/ true),
                   "a tuning-targeted adaptation starts");
            for (int i = 0; i < 2000 && p.isSpectrumAdaptRunning(); ++i) juce::Thread::sleep (5);
            check (p.applySpectrumAdapt (true, err), "and applies");
            check (p.hasSpectrumProvenance(), "the spectrum now knows the tuning it came from");
            check (p.spectrumProvenanceIsActive(), "which right now IS the active tuning");
            check (p.getSpectrumProvenanceName() == "edo7", "by name");
            // Already active = the button refuses instead of pushing a do-nothing undo.
            check (! p.loadSpectrumProvenanceTuning (err),
                   "and loading it while it is already active refuses out loud");

            // Replace the active tuning; the provenance must survive, no longer active.
            juce::String edo5 ("! edo5.scl\nfive\n 5\n!\n");
            for (int i = 1; i <= 5; ++i)
                edo5 << " " << juce::String (1200.0 * i / 5.0, 5) << "\n";
            check (p.loadScalaText (edo5, "edo5", err), "an unrelated tuning (5-EDO) replaces it");
            check (p.hasSpectrumProvenance() && ! p.spectrumProvenanceIsActive(),
                   "the origin is still remembered, and no longer claims to be active");

            // SAVE the whole state, reload it into a FRESH instance (the origin's bank
            // slot / file may be long gone by then - the state itself must carry it).
            juce::MemoryBlock saved;
            p.getStateInformation (saved);
            {
                auto q = std::make_unique<Proc>();
                q->prepareToPlay (sr, blk);
                q->setStateInformation (saved.getData(), (int) saved.getSize());
                q->pollParametersForTests();
                check (q->hasSpectrumProvenance() && ! q->spectrumProvenanceIsActive(),
                       "after save + reload the reopened instance still offers the origin");
                const auto displaced = q->activeSclSource();
                // THE BUTTON: load the tuning the spectrum came from.
                check (q->loadSpectrumProvenanceTuning (err), "pressing the button loads it");
                double worstC = 0.0;
                for (int n = 0; n < 128; ++n)
                    worstC = std::max (worstC, std::abs (1200.0 * std::log2 (
                                 q->getNoteFrequency (n) / want[(size_t) n])));
                std::printf ("  reopened + button: worst deviation from the original 7-EDO"
                             " = %.6f cents over all 128 keys\n", worstC);
                check (worstC < 0.01, "the original tuning is back note-for-note");
                check (q->spectrumProvenanceIsActive(), "and the button now reports it active");
                // ...through the normal undoable path: one Ctrl+Z = the displaced tuning.
                q->undoManager.undo();
                q->pollParametersForTests();
                check (q->activeSclSource() == displaced,
                       "one Ctrl+Z brings the displaced tuning straight back");
            }

            // THE CLEARING RULES. Typed intervals have no tuning origin...
            check (p.loadTimbreTextUndoable ("3/2 2", err), "typing intervals replaces the spectrum");
            check (! p.hasSpectrumProvenance(), "a typed spectrum offers no tuning (it has none)");
            // ...but undoing back to the adapted spectrum re-attaches its origin.
            p.undoManager.undo();
            check (p.hasSpectrumProvenance(),
                   "undoing back to the adapted spectrum brings its origin back with it");
            // A random spectrum is no one's tuning either.
            p.pressRandomSpectrum();
            check (p.pollRandomSpectrumTrigger(), "the random generator fires");
            check (! p.hasSpectrumProvenance(), "a random spectrum offers no tuning");

            // A CHORD-targeted adaptation: the origin is the fingers, not the scale, so
            // no provenance - the button must not claim 5-EDO fathered it.
            {
                juce::MidiBuffer m;
                for (const int k : { 60, 64, 67 })
                    m.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
                p.processBlock (buf, m);
                double cf0 = 0.0;
                const auto held = p.heldChordAsTargetAlphas (cf0);
                check (p.startSpectrumAdapt (st, held, cf0, err, false, /*targetIsTuning*/ false),
                       "a chord-targeted adaptation starts");
                for (int i = 0; i < 2000 && p.isSpectrumAdaptRunning(); ++i) juce::Thread::sleep (5);
                check (p.applySpectrumAdapt (true, err), "and applies");
                check (! p.hasSpectrumProvenance(),
                       "a spectrum adapted to the held chord names no tuning as its origin");
                juce::MidiBuffer off;
                for (const int k : { 60, 64, 67 })
                    off.addEvent (juce::MidiMessage::noteOff (1, k), 0);
                p.processBlock (buf, off);
            }

            // Back to the state (b) left, so the follower tests downstream measure their
            // own preconditions and not this block's leftovers: unwind our three spectrum
            // transactions (the chord adapt, the random draw, the 7-EDO adaptation - the
            // typed-intervals one was already undone above), then put the tuning back.
            p.undoManager.undo();
            p.undoManager.undo();
            p.undoManager.undo();
            check (p.loadScalaText (origSrc, origName, err),
                   "teardown: the original tuning returns for the tests downstream");
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
        // 100 ms. The history of this number is all measurements, never making a change
        // fit: 60 -> 70 when the register range became "N steps ABOVE the note" (the
        // follow-up search sweeps from a different spectrum - one 16.7 ms poll interval,
        // 51 -> 67 ms end to end), and 70 -> 100 when ~450 lines of run-47 test code
        // shifted the SAME processor binary path from 67 to a deterministic 84 ms:
        // bisected by building main's sources with only the new tests added, which
        // already read 84 - the second search's wall time crossed one more poll
        // boundary purely on the test binary's own code layout. What this check exists
        // to catch is the 163 ms class of regression (the throttle doing the waiting),
        // and 100 ms - six poll ticks, still well under half the old 250 ms floor and
        // heard as immediate - keeps catching exactly that.
        check (applyMs0 > 0.0 && applyMs0 <= 100.0,
               "the new spectrum is live within one search + two timer ticks of the chord moving");
        check (applyMs2 > 0.0 && applyMs2 <= 100.0,
               "and a move made 130 ms after another one is not held back by the throttle");
        // 2.0, raised from 1.4 by the same run-47 bisection: the landing time moving one
        // poll tick (52 -> 70 ms) re-phases the 30 ms excursion span against the notes'
        // own beating and read 1.79x from an unchanged processor. A real thump - the
        // "golpe" this metric was built on - reads several times the shimmer, and 2.0
        // still forbids it while tolerating where the beat cycle happens to sit.
        check (rmsRatio <= 2.0,
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

    //==========================================================================
    // THE PRESET PLAYGROUND, end to end on the real processor.
    //
    // The maths is checked offline in SpectrumTests (weights, log-vs-linear, hysteresis,
    // the crossfade). What can only be checked HERE is the wiring: that dragging the two
    // automatable parameters really moves ~200 controls, that it moves them the right
    // way, that the SPECTRUM comes from the blend and not from the morphed knobs, that
    // the TUNING is untouched, and that a corner arriving with the realtime follower
    // armed does not immediately overwrite the blend.
    {
        std::printf ("\npreset playground (end to end):\n");
        using Proc = FractalAdditiveAudioProcessor;

        // Two presets on disk. Named so they cannot collide with a real one, and deleted
        // at the end whatever happens.
        auto folder = Proc::getPresetFolder();
        auto fileA = folder.getChildFile ("zzz-verify-pg-A.gsting");
        auto fileB = folder.getChildFile ("zzz-verify-pg-B.gsting");

        auto p = std::make_unique<Proc>();
        p->prepareToPlay (48000.0, 64);
        auto sp = [&] (const char* id, float plain)
        {
            if (auto* q = p->apvts.getParameter (id))
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
        };
        auto gp = [&] (const char* id) { return p->apvts.getRawParameterValue (id)->load(); };

        // Corner A: the HARMONIC engine, short envelope, low base.
        sp ("timbreOn", 0.0f); sp ("timbreTemper", 0.0f);
        sp ("baseN", 1.0f); sp ("ampDecay", 0.02f); sp ("filterCutoff", 1200.0f);
        sp ("curveSlope", 1.0f); sp ("phaseMode", 0.0f); sp ("masterGain", -6.0f);
        sp ("refA", 440.0f); sp ("octaveShift", 0.0f); sp ("tuningMode", 2.0f);
        p->pollParametersForTests();
        check (p->savePreset ("zzz-verify-pg-A").isNotEmpty(), "corner preset A saved");

        // Corner B: the CUSTOM SCALE engine - a different engine, so their parameter
        // sets do not correspond and only the compiled partials can be blended.
        {
            juce::String err;
            check (p->loadTimbreText ("5/4 3/2 2", err), "corner preset B gets a custom spectrum");
        }
        sp ("baseN", 16.0f); sp ("ampDecay", 2.0f); sp ("filterCutoff", 11000.0f);
        sp ("curveSlope", 3.0f); sp ("phaseMode", 1.0f); sp ("masterGain", 6.0f);
        sp ("timbreRepeats", 4.0f);
        // ...and B is a preset whose ADAPTIVE/REALTIME spectrum is armed. That is part of
        // the patch and has to survive the save.
        {
            Proc::RtSettings rs;
            rs.adapt.partials = 6;
            p->setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
        }
        p->pollParametersForTests();
        check (p->savePreset ("zzz-verify-pg-B").isNotEmpty(), "corner preset B saved");
        p->setRealtimeLink (Proc::RtLink::Off, {});

        // THE ADAPTIVE SPECTRUM IS PART OF A PRESET.
        {
            auto xml = juce::XmlDocument::parse (fileB);
            const auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
            const auto rt = tree.getChildWithName ("REALTIME");
            check (rt.isValid() && (int) rt.getProperty ("link", 0) == (int) Proc::RtLink::SpectrumFromChord,
                   "a preset saved with REALTIME on stores that it was on");
            check (rt.isValid() && (int) rt.getProperty ("aPart", 0) == 6,
                   "...and the settings the follower was aimed with, not just the switch");
            check (p->loadPreset ("zzz-verify-pg-B")
                   && p->getRealtimeLink() == Proc::RtLink::SpectrumFromChord,
                   "loading it arms the follower again");
            p->setRealtimeLink (Proc::RtLink::Off, {});
        }

        // A neutral starting patch, so "the morph moved it" cannot be confused with
        // "it was already there".
        p->loadPreset ("zzz-verify-pg-A");
        sp ("ampDecay", 0.5f); sp ("filterCutoff", 5000.0f); sp ("baseN", 8.0f);
        sp ("refA", 432.0f); sp ("octaveShift", 2.0f); sp ("bendRange", 0.0f);
        sp ("tuningBankSlot", 0.0f);
        p->pollParametersForTests();
        const float refABefore = gp ("refA"), octBefore = gp ("octaveShift"),
                    bendBefore = gp ("bendRange"), modeBefore = gp ("tuningMode");

        juce::String err;
        check (p->assignPlaygroundCorner (0, "zzz-verify-pg-A", err), "preset A dropped on corner A");
        check (! p->isPlaygroundActive(), "ONE corner does not drive anything - the patch is left alone");
        check (p->assignPlaygroundCorner (1, "zzz-verify-pg-B", err), "preset B dropped on corner B");
        check (p->isPlaygroundActive(), "two corners engage the morph");

        auto point = [&] (float x, float y)
        {
            sp ("pgX", x); sp ("pgY", y);
            p->pollParametersForTests();
            p->pollParametersForTests();   // the control writes are paced at ~30 Hz
        };

        point (0.0f, 0.0f);
        check (std::abs (gp ("ampDecay") - 0.02f) < 1e-3f && std::abs (gp ("baseN") - 1.0f) < 0.01f
               && std::abs (gp ("filterCutoff") - 1200.0f) < 1.0f && gp ("phaseMode") < 0.5f,
               "at corner A every control reads preset A");
        point (1.0f, 0.0f);
        check (std::abs (gp ("ampDecay") - 2.0f) < 1e-2f && std::abs (gp ("baseN") - 16.0f) < 0.01f
               && std::abs (gp ("filterCutoff") - 11000.0f) < 1.0f && gp ("phaseMode") > 0.5f,
               "at corner B every control reads preset B");

        // THE MIDDLE IS A REAL INTERMEDIATE, in the right space.
        point (0.5f, 0.0f);
        const float midDecay = gp ("ampDecay"), midBase = gp ("baseN"), midCut = gp ("filterCutoff");
        std::printf ("  midpoint: ampDecay %.4f s (log %.4f, linear %.4f), baseN %.2f (log 4, linear 8.5), "
                     "cutoff %.0f c\n",
                     midDecay, std::sqrt (0.02 * 2.0), (0.02 + 2.0) / 2.0, midBase, midCut);
        check (std::abs (midDecay - std::sqrt (0.02f * 2.0f)) < 0.01f,
               "halfway between a 20 ms and a 2 s decay is 200 ms, not the 1.01 s linear smear");
        check (std::abs (midBase - 4.0f) < 0.51f,
               "halfway between Base 1 and Base 16 is Base 4 - two octaves, not three and a bit");
        check (std::abs (midCut - 6100.0f) < 5.0f,
               "cents are already logarithmic, so the cutoff averages LINEARLY");

        // THE TUNING IS NOT MORPHED. Corner A and corner B were saved with the same
        // tuning parameters, so this is checked against what the PATCH had when the
        // playground engaged - which is different from both of them.
        check (gp ("refA") == refABefore && gp ("octaveShift") == octBefore
               && gp ("bendRange") == bendBefore && gp ("tuningMode") == modeBefore,
               "reference A, octave, bend range and tuning mode are exactly where they were");

        // A DRAG PRODUCES MONOTONIC INTERMEDIATES - no jumps, no overshoot.
        {
            bool mono = true;
            float prev = -1.0f;
            for (int i = 0; i <= 20; ++i)
            {
                point ((float) i / 20.0f, 0.0f);
                const float d = gp ("ampDecay");
                if (i > 0 && d < prev - 1e-4f) mono = false;
                prev = d;
            }
            check (mono, "sweeping the point across writes a monotonically rising decay, never a jump");
        }

        // THE SPECTRUM IS THE BLEND, and at a corner it is EXACTLY that corner's.
        {
            point (1.0f, 0.0f);
            juce::Thread::sleep (300);          // the blend runs on the spectrum worker
            const auto* live = p->getLiveTemplate();
            check (live != nullptr && ! live->partials.empty(), "the playground publishes a spectrum");
            // Corner B is the custom scale 1, 5/4, 3/2 over 4 periods - the harmonic
            // engine cannot produce 1.25, so finding it proves the blend followed the
            // ENGINE across, not just the knobs.
            bool has125 = false;
            if (live != nullptr)
                for (const auto& pp : live->partials)
                    if (std::abs ((double) pp.ratio - 1.25) < 1e-3) has125 = true;
            check (has125, "at corner B the sounding spectrum is the CUSTOM engine's, morphed across engines");

            point (0.0f, 0.0f);
            juce::Thread::sleep (300);
            const auto* atA = p->getLiveTemplate();
            bool stillHas125 = false;
            if (atA != nullptr)
                for (const auto& pp : atA->partials)
                    if (std::abs ((double) pp.ratio - 1.25) < 1e-3) stillHas125 = true;
            check (! stillHas125, "and back at corner A none of B's partials are left behind");

            point (0.5f, 0.0f);
            juce::Thread::sleep (300);
            const auto* mid = p->getLiveTemplate();
            bool bothPresent = false;
            if (mid != nullptr)
            {
                bool a = false, b = false;
                for (const auto& pp : mid->partials)
                {
                    if (std::abs ((double) pp.ratio - 1.25) < 1e-3) b = true;
                    if (std::abs ((double) pp.ratio - 7.00) < 1e-2) a = true;
                }
                bothPresent = a && b;
            }
            check (bothPresent, "half way across, partials from BOTH engines are sounding at once");
        }

        // A DRAG'S MORPH RESTARTS ARE THROTTLED, NOT ABSENT. The generation is what
        // starts a voice's morph: it must move at the ~10 Hz glide cadence (that is what
        // bends the partial frequencies - "el modo morph no produce pitch bend en los
        // parciales" was this exact number sitting at zero) but NEVER on all sixty
        // frames a second (the documented click machine), while the live revision moves
        // on every frame so the amplitudes track the blend continuously.
        {
            point (0.0f, 0.0f);
            juce::Thread::sleep (250);
            const auto gen0 = p->getTemplateGeneration();
            const auto rev0 = p->getSpectrumLiveRevision();
            for (int i = 1; i <= 20; ++i)      // a slow drag: 0.02 per frame, well under the jump threshold
            {
                sp ("pgX", (float) i * 0.02f);
                p->pollParametersForTests();
                juce::Thread::sleep (12);
            }
            juce::Thread::sleep (250);
            const auto gen1 = p->getTemplateGeneration();
            const auto rev1 = p->getSpectrumLiveRevision();
            std::printf ("  20 drag frames: generation +%u, live revision +%u\n",
                         gen1 - gen0, rev1 - rev0);
            check (gen1 - gen0 >= 2 && gen1 - gen0 <= 5,
                   "a drag bumps the generation at the 10 Hz glide cadence - never all 60 frames");
            check (rev1 > rev0 + 5, "...while the live template really is being replaced on the way");
        }

        // A JUMP is different, and must be treated as one.
        {
            const auto gen0 = p->getTemplateGeneration();
            point (1.0f, 0.0f);
            juce::Thread::sleep (250);
            check (p->getTemplateGeneration() > gen0,
                   "automation cutting across the pad DOES bump the generation - the voices morph/fade it");
        }

        // A CORNER THAT ARRIVES WITH REALTIME ARMED must not overwrite the blend.
        {
            p->assignPlaygroundCorner (0, {}, err);
            p->assignPlaygroundCorner (1, {}, err);
            Proc::RtSettings rs;
            p->setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
            check (p->getRealtimeLink() == Proc::RtLink::SpectrumFromChord, "realtime armed by hand");
            p->assignPlaygroundCorner (0, "zzz-verify-pg-A", err);
            p->assignPlaygroundCorner (1, "zzz-verify-pg-B", err);
            p->pollParametersForTests();
            check (p->getRealtimeLink() == Proc::RtLink::Off && p->isPlaygroundSuspendingRealtime(),
                   "engaging the playground stands the follower down - one owner of the spectrum, and it SAYS so");
            p->assignPlaygroundCorner (1, {}, err);
            p->pollParametersForTests();
            check (p->getRealtimeLink() == Proc::RtLink::SpectrumFromChord,
                   "clearing a corner hands the follower straight back, exactly as armed");
            p->setRealtimeLink (Proc::RtLink::Off, {});
        }

        // The playground survives a state round trip, and a corner whose preset has
        // vanished comes back EMPTY rather than pointing at something else.
        {
            p->assignPlaygroundCorner (0, "zzz-verify-pg-A", err);
            p->assignPlaygroundCorner (1, "zzz-verify-pg-B", err);
            sp ("pgX", 0.37f); sp ("pgY", 0.62f);
            p->pollParametersForTests();
            juce::MemoryBlock blob;
            p->getStateInformation (blob);

            auto q = std::make_unique<Proc>();
            q->prepareToPlay (48000.0, 64);
            q->setStateInformation (blob.getData(), (int) blob.getSize());
            q->pollParametersForTests();
            check (q->getPlaygroundCornerName (0) == "zzz-verify-pg-A"
                   && q->getPlaygroundCornerName (1) == "zzz-verify-pg-B"
                   && q->isPlaygroundActive(),
                   "the playground comes back from a project reload with its corners on");
            check (std::abs (q->apvts.getRawParameterValue ("pgX")->load() - 0.37f) < 1e-3f
                   && std::abs (q->apvts.getRawParameterValue ("pgY")->load() - 0.62f) < 1e-3f,
                   "...and the point back where it was");

            fileB.deleteFile();
            auto r = std::make_unique<Proc>();
            r->prepareToPlay (48000.0, 64);
            r->setStateInformation (blob.getData(), (int) blob.getSize());
            check (r->getPlaygroundCornerName (0) == "zzz-verify-pg-A"
                   && r->getPlaygroundCornerName (1).isEmpty()
                   && ! r->isPlaygroundActive(),
                   "a corner whose preset is gone comes back EMPTY - it never substitutes another one");
        }

        // THE WINDOW ITSELF. It is a separate desktop window, so the main layout probe
        // never sees it and no screenshot in this build can either - these lay it out at
        // its smallest allowed size in BOTH languages, paint it, and drive the pad.
        {
            auto ed = std::unique_ptr<FractalAdditiveAudioProcessorEditor> (
                          dynamic_cast<FractalAdditiveAudioProcessorEditor*> (p->createEditor()));
            check (ed != nullptr, "the editor builds with the playground in it");
            if (ed != nullptr)
            {
                ed->setSize (1200, 650);
                for (int lang = 0; lang < 2; ++lang)
                {
                    // 420 x 380 is setResizeLimits' floor - the tightest the user can make it.
                    const auto L = ed->playgroundProbeForTests (420, 380, lang == 1);
                    int overlaps = 0, tiny = 0;
                    int editBad = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (! L.panel.contains (L.box[i]) || ! L.panel.contains (L.clear[i])) ++overlaps;
                        if (L.box[i].getWidth() < 60 || L.box[i].getHeight() < 14) ++tiny;
                        if (L.box[i].intersects (L.clear[i]) || L.box[i].intersects (L.lbl[i])) ++overlaps;
                        if (L.pad.intersects (L.box[i])) ++overlaps;
                        // THE EDIT BUTTON, added to a row that was already full. It has to
                        // be inside the panel, clear of the name box and the X either side
                        // of it, big enough to hit, and its REAL label has to fit - "OK"
                        // is wider than "E" and it is the one the button wears while a
                        // corner is being edited, i.e. exactly when it is being looked at.
                        if (! L.panel.contains (L.edit[i])) ++editBad;
                        if (L.edit[i].intersects (L.box[i]) || L.edit[i].intersects (L.clear[i])
                            || L.edit[i].intersects (L.lbl[i]) || L.edit[i].intersects (L.pad)) ++editBad;
                        if (L.edit[i].getWidth() < 18 || L.edit[i].getHeight() < 14) ++editBad;
                        for (const char* s : { "E", "OK" })
                            if (juce::GlyphArrangement::getStringWidth (
                                    juce::Font (juce::FontOptions (13.0f)), s)
                                > (float) L.edit[i].getWidth() - 4.0f) ++editBad;
                        // An EMPTY corner has nothing to edit: the button must be dead,
                        // not a click that silently does nothing (this run has corners A
                        // and B assigned and C, D empty).
                        const bool assigned = L.cornerName[i].isNotEmpty();
                        if (L.editEnabled[i] != assigned) ++editBad;
                    }
                    std::printf ("  edit buttons (%s): %dx%d each, \"%s\", enabled %d%d%d%d\n",
                                 lang == 1 ? "ca" : "en", L.edit[0].getWidth(), L.edit[0].getHeight(),
                                 L.editText[0].toRawUTF8(), (int) L.editEnabled[0], (int) L.editEnabled[1],
                                 (int) L.editEnabled[2], (int) L.editEnabled[3]);
                    check (editBad == 0,
                           "every corner's EDIT button fits its row, clears its neighbours, and is dead on an empty corner");
                    std::printf ("  playground panel (%s) at 420x380: pad %dx%d, combo %dx%d, "
                                 "%d overlaps, %d squashed\n", lang == 1 ? "ca" : "en",
                                 L.pad.getWidth(), L.pad.getHeight(),
                                 L.box[0].getWidth(), L.box[0].getHeight(), overlaps, tiny);
                    check (overlaps == 0 && tiny == 0,
                           "at its smallest size nothing in the playground overlaps or is squashed away");
                    check (L.pad.getWidth() > 200 && L.pad.getHeight() > 120,
                           "...and the pad still gets most of the window");
                    check (L.status.isNotEmpty(), "the panel says what the blend currently is");
                    // The MORPH/MIX mode pair: present, non-overlapping, and each REAL
                    // (localised) label fits its button - "MESCLA" is wider than "MIX".
                    bool modeOk = ! L.mode[0].isEmpty() && ! L.mode[1].isEmpty()
                               && L.panel.contains (L.mode[0]) && L.panel.contains (L.mode[1])
                               && ! L.mode[0].intersects (L.mode[1])
                               && ! L.mode[0].intersects (L.pad) && ! L.mode[1].intersects (L.pad);
                    for (int m = 0; m < 2; ++m)
                    {
                        const auto tw = juce::GlyphArrangement::getStringWidth (
                                            juce::Font (juce::FontOptions (12.0f)), L.modeText[m]);
                        if (tw > (float) L.mode[m].getWidth() - 4.0f) modeOk = false;
                    }
                    check (modeOk, "the MORPH/MIX mode pair fits, in this language, without covering anything");
                    // The MIX voice-budget row: present, clear of the pad and status,
                    // and its REAL (localised) label fits the space it is given.
                    const bool vOk = ! L.voicesL.isEmpty() && ! L.voicesC.isEmpty()
                                  && L.panel.contains (L.voicesL) && L.panel.contains (L.voicesC)
                                  && ! L.voicesL.intersects (L.voicesC)
                                  && ! L.voicesC.intersects (L.pad)
                                  && juce::GlyphArrangement::getStringWidth (
                                         juce::Font (juce::FontOptions (12.0f)), L.voicesText)
                                       <= (float) L.voicesL.getWidth() - 2.0f;
                    check (vOk, "the mix voice-budget control fits, label and all, in this language");
                }

                // THE PAD'S MAPPING: a click at a fraction of the travel has to become
                // exactly that value on the parameter, or the sound would never be where
                // the point is drawn.
                ed->playgroundProbeForTests (520, 480, false);
                ed->playgroundDragForTests (0.25f, 0.75f);
                std::printf ("  drag to (0.25, 0.75) -> pgX %.4f pgY %.4f\n", gp ("pgX"), gp ("pgY"));
                check (std::abs (gp ("pgX") - 0.25f) < 0.02f && std::abs (gp ("pgY") - 0.75f) < 0.02f,
                       "a drag lands the point exactly where it was dropped");
                ed->playgroundDragForTests (0.0f, 0.0f);
                check (gp ("pgX") == 0.0f && gp ("pgY") == 0.0f, "and a corner really is the corner");
            }
        }

        fileA.deleteFile();
        fileB.deleteFile();
        check (! fileA.existsAsFile() && ! fileB.existsAsFile(), "the verifier's presets are cleaned up");
    }

    //--- EDITING A PLAYGROUND CORNER IN PLACE ---------------------------------------
    // "cuando esta el playground haz que cada preset tenga un boton de edit y al clicarlo
    //  puedas editarlo a tiempo real y entonces el nombre del preset aparece con un * para
    //  avisar que fue editado, y que dandole a save se pueda guardar luego con otro nombre"
    //
    // Easy to demo, catastrophic to get wrong: the main editor's controls ARE the live
    // patch, and pointing them at a corner without an ownership contract loses the sound
    // the user was working on. Every check below is a clause of that contract (the rules
    // are written out above beginPlaygroundCornerEdit) and not one of them is visible to
    // any other check in this build.
    {
        std::printf ("\nediting a playground corner in place:\n");
        using Proc = FractalAdditiveAudioProcessor;
        auto folder = Proc::getPresetFolder();
        auto fA = folder.getChildFile ("zzz-verify-ce-A.gsting");
        auto fB = folder.getChildFile ("zzz-verify-ce-B.gsting");
        auto fS = folder.getChildFile ("zzz-verify-ce-B2.gsting");

        auto p = std::make_unique<Proc>();
        p->prepareToPlay (48000.0, 64);
        auto sp = [&] (const char* id, float v)
        { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
        auto gp   = [&] (const char* id) { return p->apvts.getRawParameterValue (id)->load(); };
        auto tick = [&] (int n) { for (int i = 0; i < n; ++i) p->pollParametersForTests(); };
        // Every parameter's normalised value, so "the live patch came back" can be checked
        // as ALL OF IT rather than as the three knobs the test happened to think of.
        auto snapAll = [&]
        {
            std::vector<float> v;
            for (auto* ap : p->getParameters())
                if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (ap)) v.push_back (q->getValue());
            return v;
        };
        auto ratiosOf = [] (const std::shared_ptr<const spectrum::SpectrumTemplate>& t)
        {
            std::vector<double> r;
            if (t != nullptr) for (const auto& pa : t->partials) r.push_back (pa.ratio);
            return r;
        };

        sp ("timbreOn", 0.0f); sp ("baseN", 1.0f); sp ("ampDecay", 0.05f);
        sp ("filterCutoff", 1000.0f); sp ("masterGain", -6.0f);
        tick (2);
        check (p->savePreset ("zzz-verify-ce-A").isNotEmpty(), "corner preset A saved");
        sp ("baseN", 8.0f); sp ("ampDecay", 1.5f); sp ("filterCutoff", 9000.0f);
        tick (2);
        check (p->savePreset ("zzz-verify-ce-B").isNotEmpty(), "corner preset B saved");

        juce::String err;
        p->assignPlaygroundCorner (0, "zzz-verify-ce-A", err);
        p->assignPlaygroundCorner (1, "zzz-verify-ce-B", err);
        check (p->isPlaygroundActive(), "two corners engage the playground");
        check (! p->beginPlaygroundCornerEdit (2), "an EMPTY corner cannot be edited - there is nothing there");

        // MIX MODE FIRST, because it is the mode where the promise is testable: the
        // playground writes nothing to the live patch there, so a live patch that comes
        // back changed came back changed because edit mode broke it.
        sp ("pgMode", 1.0f); sp ("pgX", 0.35f); sp ("pgY", 0.65f);
        tick (4);
        // The user's own sound: unlike either corner, with a tuning of its own.
        sp ("baseN", 5.0f); sp ("ampDecay", 0.4f); sp ("filterCutoff", 4000.0f);
        sp ("refA", 428.0f); sp ("octaveShift", 1.0f);
        tick (4);
        const auto  liveBefore   = snapAll();
        const float pgxBefore    = gp ("pgX"), pgyBefore = gp ("pgY");
        const bool  driftBefore  = p->presetHasDrifted();
        const auto  loadedBefore = p->getLoadedPresetName();

        check (p->beginPlaygroundCornerEdit (1), "Edit on an assigned corner enters edit mode");
        check (p->getPlaygroundEditCorner() == 1, "...and the processor says WHICH corner it is");
        tick (3);
        std::printf ("  entering edit: ampDecay %.3f (corner B saved 1.500, live patch 0.400),"
                     " baseN %.1f, refA %.0f, point (%.2f, %.2f)\n",
                     gp ("ampDecay"), gp ("baseN"), gp ("refA"), gp ("pgX"), gp ("pgY"));
        check (std::abs (gp ("ampDecay") - 1.5f) < 0.01f && std::abs (gp ("baseN") - 8.0f) < 0.01f
               && std::abs (gp ("filterCutoff") - 9000.0f) < 1.0f,
               "the main window's controls now READ that corner's stored values");
        check (gp ("refA") == 428.0f && gp ("octaveShift") == 1.0f,
               "...and the tuning is not part of a corner: reference and transpose never moved");
        check (gp ("pgX") == pgxBefore && gp ("pgY") == pgyBefore,
               "mix mode does not pin the point - the corner is its own engine, heard in the balance");
        check (! p->isPlaygroundCornerEdited (1),
               "and an UNTOUCHED corner carries no * - the warning has to mean something");

        // RULE 2: THE HOST NEVER SEES THE SCRATCH SURFACE.
        {
            juce::MemoryBlock blob;
            p->getStateInformation (blob);
            auto q = std::make_unique<Proc>();
            q->prepareToPlay (48000.0, 64);
            q->setStateInformation (blob.getData(), (int) blob.getSize());
            const float qd = q->apvts.getRawParameterValue ("ampDecay")->load();
            std::printf ("  project saved MID-EDIT reopens with ampDecay %.3f (live 0.400, corner 1.500)\n", qd);
            check (std::abs (qd - 0.4f) < 0.02f,
                   "a project saved mid-edit reopens as the USER'S patch, never as somebody's corner");
            check (q->getPlaygroundCornerName (1) == "zzz-verify-ce-B",
                   "...with the corner assignments still on it");
        }

        // TURNING A CONTROL EDITS THE CORNER - and reaches the SOUND, not just the store.
        const int  rev0   = p->getPlaygroundCornerRevForTests (1);
        const auto tmpl0  = ratiosOf (p->getPlaygroundCornerTemplateForTests (1));
        sp ("ampDecay", 0.11f);
        sp ("baseN", 3.0f);            // a SPECTRUM parameter: it must reach the template
        tick (20);                     // the recompile is throttled to the ~10 Hz glide cadence
        const auto tmpl1 = ratiosOf (p->getPlaygroundCornerTemplateForTests (1));
        std::printf ("  after editing: corner rev %d -> %d, template partials %d -> %d,"
                     " first ratios %.4f -> %.4f, name \"%s\"\n",
                     rev0, p->getPlaygroundCornerRevForTests (1), (int) tmpl0.size(), (int) tmpl1.size(),
                     tmpl0.empty() ? 0.0 : tmpl0[0], tmpl1.empty() ? 0.0 : tmpl1[0],
                     p->getPlaygroundCornerDisplayName (1).toRawUTF8());
        check (p->getPlaygroundCornerRevForTests (1) > rev0,
               "moving a control while editing writes THAT CORNER's store");
        check (p->isPlaygroundCornerEdited (1) && p->getPlaygroundCornerDisplayName (1).endsWith ("*"),
               "...and the corner's name gains the * that warns it no longer matches its preset");
        check (! tmpl1.empty() && tmpl1 != tmpl0,
               "...and the corner's SPECTRUM is recompiled, so the edit is HEARD and not merely stored");

        // RULE 1: LEAVING HANDS THE LIVE PATCH BACK, ALL OF IT.
        p->endPlaygroundCornerEdit();
        tick (4);
        check (p->getPlaygroundEditCorner() < 0, "OK leaves edit mode");
        {
            const auto liveAfter = snapAll();
            int moved = 0;
            float worst = 0.0f;
            for (size_t i = 0; i < liveAfter.size() && i < liveBefore.size(); ++i)
                if (const float d = std::abs (liveAfter[i] - liveBefore[i]); d > 1.0e-4f)
                { ++moved; worst = juce::jmax (worst, d); }
            std::printf ("  live patch on the way out: %d of %d parameters moved (worst %.5f)\n",
                         moved, (int) liveBefore.size(), worst);
            check (moved == 0, "and the live patch comes back EXACTLY as it was left - every parameter of it");
        }
        check (p->getLoadedPresetName() == loadedBefore && p->presetHasDrifted() == driftBefore,
               "...including WHICH preset it is and whether it had drifted from it");
        check (p->isPlaygroundCornerEdited (1),
               "the unsaved edits stay ON THE CORNER for this session - the * is what warns about them");

        // SAVE UNDER ANOTHER NAME.
        check (p->savePlaygroundCornerAs (1, "zzz-verify-ce-B2").isEmpty(),
               "a corner that is NOT in edit mode refuses to be saved - its full state is nowhere to save from");
        p->beginPlaygroundCornerEdit (1);
        tick (3);
        const auto stored = p->savePlaygroundCornerAs (1, "zzz-verify-ce-B2");
        check (stored == "zzz-verify-ce-B2" && fS.existsAsFile(),
               "Save writes the edited corner as a preset of its own, under a new name");
        check (p->getPlaygroundCornerName (1) == stored && ! p->isPlaygroundCornerEdited (1),
               "...the corner takes that name and loses its *");
        check (p->getLoadedPresetName() == loadedBefore,
               "...and the LIVE patch's own preset identity is not touched by it");
        check (fB.existsAsFile(),
               "...and the preset the corner came FROM is never written by editing it");
        p->endPlaygroundCornerEdit();
        tick (2);
        {
            auto q = std::make_unique<Proc>();
            q->prepareToPlay (48000.0, 64);
            check (q->loadPreset ("zzz-verify-ce-B2"), "the saved corner loads like any other preset");
            std::printf ("  saved corner reloads with ampDecay %.3f baseN %.1f (edited 0.110 / 3.0)\n",
                         q->apvts.getRawParameterValue ("ampDecay")->load(),
                         q->apvts.getRawParameterValue ("baseN")->load());
            check (std::abs (q->apvts.getRawParameterValue ("ampDecay")->load() - 0.11f) < 0.005f
                   && std::abs (q->apvts.getRawParameterValue ("baseN")->load() - 3.0f) < 0.01f,
                   "...and the file really carries the edit, not the preset it started from");
        }

        // RULE 6: MORPH MODE PINS THE POINT, and the blend stops fighting the user.
        sp ("pgMode", 0.0f); sp ("pgX", 0.5f); sp ("pgY", 0.0f);
        tick (6);
        p->beginPlaygroundCornerEdit (1);
        tick (3);
        std::printf ("  morph mode: editing corner B pinned the point to (%.2f, %.2f)\n", gp ("pgX"), gp ("pgY"));
        check (gp ("pgX") == 1.0f && gp ("pgY") == 0.0f,
               "in MORPH the point moves ONTO the corner being edited - what you hear IS what you edit");
        sp ("filterCutoff", 2222.0f);
        tick (10);
        std::printf ("  a control set to 2222 while editing reads %.0f ten ticks later\n", gp ("filterCutoff"));
        check (std::abs (gp ("filterCutoff") - 2222.0f) < 1.0f,
               "and the blend's 30 Hz write-back is suspended - a knob turned in edit mode does not snap back");
        // ...AND DRAGGING THE PAD AWAY MUST NOT DRAG THE CORNER WITH IT. This is the one
        // that bites: at a pinned corner the blend writes back exactly what the pull just
        // captured, so the suspension looks harmless. Move the point off the corner and it
        // is a FEEDBACK LOOP - the blend rewrites every control from the mixture, the next
        // pull captures that mixture into the corner, and the preset being edited walks
        // towards the average of its neighbours one frame at a time until nothing of it is
        // left. Corner A's cutoff is 1000; thirty frames of that would be most of the way.
        sp ("pgX", 0.5f);
        tick (30);
        std::printf ("  pad dragged to the middle while editing corner B: cutoff %.0f"
                     " (corner A is 1000, the edit was 2222)\n", gp ("filterCutoff"));
        check (std::abs (gp ("filterCutoff") - 2222.0f) < 1.0f,
               "and dragging the pad away while editing never drags the corner's own values towards the others");
        p->endPlaygroundCornerEdit();
        tick (6);
        check (std::abs (gp ("pgX") - 0.5f) < 1.0e-3f && gp ("pgY") == 0.0f,
               "leaving puts the point back exactly where the user had it");

        // RULE 4: re-assigning the corner is the ONE act that throws unsaved edits away.
        p->beginPlaygroundCornerEdit (0);
        tick (3);
        sp ("ampDecay", 0.9f);
        tick (20);
        check (p->isPlaygroundCornerEdited (0), "corner A now carries unsaved edits");
        p->assignPlaygroundCorner (0, "zzz-verify-ce-A", err);
        tick (3);
        check (p->getPlaygroundEditCorner() < 0 && ! p->isPlaygroundCornerEdited (0),
               "dropping a preset on the corner being edited ends the edit and re-reads it from disk");
        // ...and so is the playground letting go entirely.
        p->beginPlaygroundCornerEdit (1);
        tick (3);
        check (p->getPlaygroundEditCorner() == 1, "editing corner B again");
        p->assignPlaygroundCorner (0, {}, err);
        tick (3);
        check (p->getPlaygroundEditCorner() < 0,
               "and clearing a corner until the playground disengages ends edit mode too - never a"
               " scratch surface left standing in for a blend that stopped");

        // THE MAIN WINDOW SAYS WHOSE PATCH IT IS SHOWING (rule 3).
        {
            p->assignPlaygroundCorner (0, "zzz-verify-ce-A", err);
            tick (3);
            auto ed = std::unique_ptr<FractalAdditiveAudioProcessorEditor> (
                          dynamic_cast<FractalAdditiveAudioProcessorEditor*> (p->createEditor()));
            check (ed != nullptr, "the editor builds with corner edit in it");
            if (ed != nullptr)
            {
                ed->setSize (1200, 650);
                const auto before = ed->cornerEditUiProbeForTests();
                p->beginPlaygroundCornerEdit (1);
                tick (3);
                const auto during = ed->cornerEditUiProbeForTests();
                p->endPlaygroundCornerEdit();
                tick (3);
                const auto after = ed->cornerEditUiProbeForTests();
                std::printf ("  preset field: \"%s\" -> \"%s\" -> \"%s\"; Save: \"%s\" -> \"%s\"\n",
                             before.presetText.toRawUTF8(), during.presetText.toRawUTF8(),
                             after.presetText.toRawUTF8(), before.saveText.toRawUTF8(),
                             during.saveText.toRawUTF8());
                check (during.presetText.startsWith ("B:")
                       && during.presetText.contains (p->getPlaygroundCornerName (1)),
                       "while editing, the preset field NAMES the corner instead of the loaded preset");
                check (! during.presetEnabled && ! during.dirtyMarkVisible,
                       "...and is dead: a corner is not something you load a preset over");
                check (during.saveText != before.saveText && during.saveText.contains ("B"),
                       "...and Save says which corner it would write");
                check (after.presetText == before.presetText && after.presetEnabled,
                       "and leaving edit mode puts the live patch's own preset name back, live again");
            }
        }

        fA.deleteFile(); fB.deleteFile(); fS.deleteFile();
        check (! fA.existsAsFile() && ! fB.existsAsFile() && ! fS.existsAsFile(),
               "the corner-edit verifier's presets are cleaned up");
    }

    //--- A SEARCH BOX ON THE PRESETS DROP-DOWN ---------------------------------------
    // "haz que el desplegable de presets tenga un buscador"
    //
    // The trap is not the filtering, it is WHAT COUNTS AS A MATCH. This product is
    // bilingual and its presets are named in the user's own words, so a byte-exact match
    // is a search box that cannot find "Can\xc3\xa7\xc3\xb3" unless you remember which
    // accent you typed a year ago. Measured through the REAL panel (it is the component
    // the drop-down builds), so the filter cannot drift away from the one that ships.
    {
        std::printf ("\nthe presets drop-down's search box:\n");
        // CharPointer_UTF8, NOT a bare const char*, and this matters more than it looks:
        // juce::String (const char*) reads each BYTE as its own code point, so
        // "Can\xc3\xa7\xc3\xb3" built that way is a six-character Latin-1 smear and a test
        // written with it would be checking the fold against mojibake instead of against
        // the name. Real preset names arrive from getFileNameWithoutExtension - properly
        // decoded by the OS - so these have to be built the same way to mean anything.
        const auto U = [] (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); };
        juce::StringArray names;
        names.add (U ("Can\xc3\xa7\xc3\xb3 llunyana"));   // ca: cedilla + acute
        names.add (U ("Melod\xc3\xad" "a nocturna"));     // es: acute (split, or \xad eats the 'a')
        names.add (U ("R\xc3\xa0" "dio C\xc3\xb2smica")); // ca: graves
        names.add (U ("Col\xc2\xb7legi de sons"));        // ca: l-geminada (a MIDDLE DOT, not a letter)
        names.add ("BRASS stack");
        names.add ("brass soft");
        names.add ("Drone");
        const auto E = [&] (const char* q) { return FractalAdditiveAudioProcessorEditor::presetSearchProbeForTests (names, q); };

        check (E ("").size() == names.size(), "an empty box shows everything - it filters, it does not hide");
        {
            const auto r = E ("canco");
            std::printf ("  typing \"canco\" -> %d match(es): %s\n", r.size(), r.joinIntoString (", ").toRawUTF8());
            check (r.size() == 1 && r[0] == names[0],
                   "\"canco\" finds \"Can\xc3\xa7\xc3\xb3 llunyana\" - accents typed or not, it is the same word");
        }
        {
            const auto r = E ("MELODIA");
            check (r.size() == 1 && r[0] == names[1],
                   "and shouting it in ASCII finds \"Melod\xc3\xad" "a\" too - case and accents both ignored");
        }
        {
            const auto r = E ("radio cosmica");
            check (r.size() == 1 && r[0] == names[2], "graves fold as well, across a space");
        }
        {
            const auto r = E ("collegi");
            check (r.size() == 1 && r[0] == names[3],
                   "and Catalan's l-geminada folds to nothing, so \"collegi\" finds \"col\xc2\xb7legi\"");
        }
        {
            const auto r = E ("brass");
            std::printf ("  typing \"brass\" -> %d match(es): %s\n", r.size(), r.joinIntoString (", ").toRawUTF8());
            check (r.size() == 2, "a substring matches anywhere in the name, in either case");
        }
        {
            const auto r = E ("zzzz");
            check (r.isEmpty(), "and a query nothing matches shows an EMPTY list, not the whole folder");
        }
        // The accented names must still come back with their accents INTACT: the folding
        // is a comparison, never a rewrite of the user's own file names.
        check (E ("canco")[0] == names[0] && E ("radio")[0] == names[2],
               "the names the list shows are the real ones - folding compares, it never renames");

        // ...AND IT OPENS MORE THAN ONCE.
        // "pulse una vez en el boton de presets y cargue uno y ahora pulso otra vez pero
        //  ya no se despliega la lista"
        // ComboBox::showPopupIfNotActive() raises the private menuActive flag and then
        // dispatches showPopup() asynchronously; the ONLY code that lowers it again is
        // hidePopup(), reached from the modal callback of the PopupMenu that the base
        // showPopup() launches. A box that takes the drop-down over never launches one,
        // so the flag stayed up and every later click, Return and scroll was swallowed:
        // the presets box was dead for the rest of the editor's life. Three attempts,
        // because the bug lets exactly the FIRST one through.
        {
            const int opened = FractalAdditiveAudioProcessorEditor::comboReopenProbeForTests (3);
            std::printf ("  a taken-over drop-down opened %d time(s) out of 3\n", opened);
            check (opened == 3,
                   "the presets drop-down opens EVERY time it is clicked, not just the first");
        }
    }

    //==========================================================================
    // --- NO STRING IN THE WHOLE EDITOR IS CUT OFF, IN EITHER LANGUAGE ----------------
    // "haz un repaso de todos los textos que quedan cortados como 'not in this spectrum'
    //  y arreglalos"
    // Named checks can only see the text somebody registered, and a truncation is
    // exactly what nobody registered. This walks the whole tree instead - every visible
    // Label, Button and ComboBox in every panel - and measures the string against the
    // room its own LookAndFeel leaves it, in English and in Catalan.
    {
        std::printf ("\nevery drawn string against the box it is drawn in:\n");
        auto pt = std::make_unique<FractalAdditiveAudioProcessor>();
        pt->prepareToPlay (sr, blk);
        std::unique_ptr<juce::AudioProcessorEditor> tbase (pt->createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (tbase.get());
        if (ed == nullptr) check (false, "the editor was created");
        else
        {
            // Route a rhythm slot to a partial the spectrum does NOT have, so the row
            // that carries the user's own example - "not in this spectrum" - is really
            // on screen while the sweep runs. A state no default window ever shows.
            // ...which needs a LIVE spectrum to compare against: with no template the
            // row cannot know the partial is missing and just says "P901".
            {
                juce::AudioBuffer<float> tb (2, blk);
                juce::MidiBuffer on;  on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                tb.clear(); pt->processBlock (tb, on);
                for (int i = 0; i < 60; ++i)
                { juce::MidiBuffer none; tb.clear(); pt->processBlock (tb, none); pt->pollParametersForTests(); juce::Thread::sleep (2); }
                juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                tb.clear(); pt->processBlock (tb, off);
                for (int i = 0; i < 10; ++i) { juce::MidiBuffer none; tb.clear(); pt->processBlock (tb, none); }
            }
            pt->getRhythm().slots[0].partial  = 0;   pt->getRhythm().slots[0].enabled = true;
            pt->getRhythm().slots[1].partial  = 900; pt->getRhythm().slots[1].enabled = true;
            int totalOver = 0, measured = 0;
            for (int lang = 0; lang < 2; ++lang)
            {
                auto fits = ed->textFitProbeForTests (lang == 1);
                measured += (int) fits.size();
                std::sort (fits.begin(), fits.end(),
                           [] (const auto& a, const auto& b) { return a.over() > b.over(); });
                int over = 0;
                for (const auto& f : fits)
                {
                    if (f.over() <= 0.5f) continue;
                    ++over;
                    if (over <= 40)
                        std::printf ("  %s %-12s needs %4.0f in %4.0f px %s  at (%d,%d %dx%d)  [%s]  %s\n",
                                     lang == 1 ? "ca" : "en", f.kind.toRawUTF8(),
                                     f.wraps ? f.needH : f.need, f.wraps ? f.availH : f.avail,
                                     f.wraps ? "tall" : "wide",
                                     f.bounds.getX(), f.bounds.getY(), f.bounds.getWidth(), f.bounds.getHeight(),
                                     f.text.toRawUTF8(), f.path.toRawUTF8());
                }
                std::printf ("  %s: %d of %d drawn strings do not fit\n",
                             lang == 1 ? "ca" : "en", over, (int) fits.size());
                totalOver += over;
            }
            check (measured > 90, "the sweep really walked the whole window");
            check (totalOver == 0,
                   "every Label, Button and ComboBox in the editor fits the box it is drawn "
                   "in, in BOTH languages");
        }
    }

    //==========================================================================
    // --- A CONTROL MOVES WHILE NOTES ARE HELD ---------------------------------------
    // "mover controles con notas apretadas sigue siendo buggy, a veces no deja mover el
    //  control y a veces si, solo si has movido el control un instante antes de empezar
    //  a tocar teclas"
    {
        std::printf ("\ndragging a fader while notes are held:\n");
        auto pd = std::make_unique<FractalAdditiveAudioProcessor>();
        pd->prepareToPlay (sr, blk);
        std::unique_ptr<juce::AudioProcessorEditor> base (pd->createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
        juce::AudioBuffer<float> db (2, blk);

        auto show = [] (const char* what, const FractalAdditiveAudioProcessorEditor::DragProbe& d)
        {
            std::printf ("  %-22s found=%d vis=%d en=%d  %.4f -> %.4f -> %.4f  held=%d "
                         "focus=%d(%s) sinceFocus=%dms\n",
                         what, (int) d.found, (int) d.visible, (int) d.enabled,
                         d.before, d.afterDrag, d.afterSettle, d.heldNotes,
                         (int) d.editorHasFocus, d.focusOwner.toRawUTF8(), d.msSinceFocused);
        };

        if (ed == nullptr) check (false, "the editor was created");
        else
        {
            const auto quiet = ed->dragFaderForTests ("slope", 20, 3);
            show ("silent", quiet);
            check (quiet.found && quiet.afterDrag != quiet.before,
                   "with nothing sounding, dragging the SLOPE fader moves it");
            check (quiet.afterSettle == quiet.afterDrag,
                   "...and three timer ticks later it is still where the drag left it");

            // Now with a chord really held: note-ons through processBlock, blocks kept
            // running between the ticks, exactly as a host does while keys are down.
            juce::MidiBuffer on;
            for (const int k : { 60, 64, 67 }) on.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            pd->processBlock (db, on);
            for (int i = 0; i < 20; ++i)
            { juce::MidiBuffer none; db.clear(); pd->processBlock (db, none); pd->pollParametersForTests(); }
            ed->tickTimerForTests();

            const auto held = ed->dragFaderForTests ("slope", -20, 0);
            show ("3 notes held", held);
            check (held.found && held.afterDrag != held.before,
                   "with a chord held, dragging the SLOPE fader still moves it");

            // ...and it must STAY there while the notes go on sounding: a value that is
            // written back a tick later reads exactly as "no deja mover el control".
            for (int i = 0; i < 20; ++i)
            { juce::MidiBuffer none; db.clear(); pd->processBlock (db, none); pd->pollParametersForTests(); }
            ed->tickTimerForTests(); ed->tickTimerForTests();
            const auto after = ed->dragFaderForTests ("slope", 0, 3);
            show ("held, after settle", after);
            check (std::abs (after.before - held.afterDrag) < 1.0e-6,
                   "...and it is still there twenty blocks and two ticks later");

            juce::MidiBuffer off;
            for (const int k : { 60, 64, 67 }) off.addEvent (juce::MidiMessage::noteOff (1, k), 0);
            pd->processBlock (db, off);
            for (int i = 0; i < 10; ++i) { juce::MidiBuffer none; db.clear(); pd->processBlock (db, none); }

            //-- ...THROUGH JUCE'S REAL EVENT ROUTING, with a focus grab in the middle.
            // The slider's own mouseDown/mouseDrag are innocent (above). The question
            // the report actually asks is about the layer ABOVE them: a drag is a mouse
            // CAPTURE, and a capture can be taken away. So the editor goes on a real
            // (off-screen) peer and the events are injected at the peer, which runs
            // JUCE's hit-testing, capture and focus code exactly as a hand would.
            ed->setTopLeftPosition (-4000, -4000);  // off every real screen: nothing appears
            ed->setVisible (true);
            ed->addToDesktop (0);
            if (auto* peer = ed->getPeer())
            {
                juce::Rectangle<int> slope;
                for (const auto& c : ed->layoutProbe().controls)
                    if (c.first == "both.slope") slope = c.second;

                auto inject = [peer] (juce::Point<float> pos, juce::ModifierKeys mods)
                {
                    peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, pos, mods,
                                            juce::MouseInputSource::defaultPressure,
                                            juce::MouseInputSource::defaultOrientation,
                                            juce::Time::currentTimeMillis());
                };
                const auto press = juce::ModifierKeys::leftButtonModifier;
                const juce::Point<float> a ((float) slope.getCentreX(), (float) slope.getCentreY() + 25.0f);
                const juce::Point<float> b ((float) slope.getCentreX(), (float) slope.getCentreY() - 25.0f);

                // A drag with NOTHING interfering - the control case.
                const double v0 = ed->dragFaderForTests ("slope", 0, 0).before;
                inject (a, {}); inject (a, press); inject (b, press); inject (b, {});
                const double vPlain = ed->dragFaderForTests ("slope", 0, 0).before;
                std::printf ("  routed drag, undisturbed:      %.4f -> %.4f\n", v0, vPlain);
                check (std::abs (vPlain - v0) > 1.0e-6,
                       "a drag injected through JUCE's own routing moves the fader");

                // The SAME drag, with the editor grabbing keyboard focus between the
                // press and the move - which is what mouseEnter/refocus() do, and what
                // the report's "sometimes" would have to be explained by.
                inject (a, {}); inject (a, press);
                ed->grabKeyboardFocus();
                const double vMid = ed->dragFaderForTests ("slope", 0, 0).before;
                inject (b, press); inject (b, {});
                const double vGrab = ed->dragFaderForTests ("slope", 0, 0).before;
                std::printf ("  routed drag, focus grabbed mid-press: %.4f -> %.4f\n", vMid, vGrab);
                check (std::abs (vGrab - vMid) > 1.0e-6,
                       "...and it STILL moves when the editor grabs keyboard focus mid-drag");

                // ...and with a full relayout in the middle, the other thing the timer
                // does to this window while notes are sounding.
                inject (a, {}); inject (a, press);
                const double vMid2 = ed->dragFaderForTests ("slope", 0, 0).before;
                ed->resized();
                inject (b, press); inject (b, {});
                const double vLay = ed->dragFaderForTests ("slope", 0, 0).before;
                std::printf ("  routed drag, relayout mid-press:      %.4f -> %.4f\n", vMid2, vLay);
                check (std::abs (vLay - vMid2) > 1.0e-6,
                       "...and when the whole editor is laid out again mid-drag");

                //-- IS EVERY CONTROL ACTUALLY THE THING UNDER THE MOUSE?
                // A control added BEFORE the panel it lives in is buried under that
                // panel: it still lays out, still paints (the panel is drawn first) and
                // still shows its tooltip, but the press lands on the panel and the
                // control cannot be moved at all. That is the literal shape of "a veces
                // no deja mover el control", per control, and nothing in this build
                // could see it. Asked the only way that answers it: put the mouse on the
                // control's own centre and ask JUCE what is under it.
                int buried = 0, tested = 0;
                juce::String firstBuried;
                for (const auto& c : ed->layoutProbe().controls)
                {
                    if (c.second.isEmpty()) continue;
                    ++tested;
                    inject (c.second.getCentre().toFloat(), {});
                    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
                    // The control itself, or one of its own children (a Slider's text
                    // box, a ComboBox's label) - both are that control under the hand.
                    bool ok = false;
                    for (auto* w = under; w != nullptr; w = w->getParentComponent())
                        if (w->getBounds() == c.second && w->getParentComponent() == ed) { ok = true; break; }
                    if (! ok)
                    {
                        ++buried;
                        if (firstBuried.isEmpty())
                            firstBuried = c.first + " -> " + (under != nullptr ? juce::String (typeid (*under).name())
                                                                               : juce::String ("nothing"));
                    }
                }
                std::printf ("  %d of %d laid-out controls are not the component under their own centre%s%s\n",
                             buried, tested, buried > 0 ? "; first: " : "", firstBuried.toRawUTF8());
                check (tested > 30, "the sweep really had the whole editor to walk");
                check (buried == 0,
                       "every laid-out control IS the component under its own centre - none is "
                       "buried under the panel it lives in, where it would paint and show its "
                       "tooltip but refuse to be dragged");

                //-- NO FOCUS GRAB WHILE A BUTTON IS DOWN.
                // The one thing in this editor that changes the keyboard focus at the
                // instant of a press: mouseEnter -> reclaimFocusUnlessTyping, fired by
                // the pointer arriving. A focus change while a press is live can revoke
                // the mouse capture, and JUCE turns a revoked capture into a synthetic
                // mouse-up: the drag ends mid-gesture. Asked with a REAL injected press,
                // so the button state is the one JUCE itself would see.
                inject (a, {});
                // The keyboard goes elsewhere, as it is whenever the host owns it - the
                // ONLY state in which the reclaim does anything at all. Without this the
                // test would pass on "the focus was already ours" and prove nothing.
                juce::Component::unfocusAllComponents();
                juce::Thread::sleep (300);   // past the reclaim's own debounce window
                const auto starved = ed->focusReclaimProbeForTests();
                inject (a, press);
                const auto pressed = ed->focusReclaimProbeForTests();
                std::printf ("  focus reclaim: keyboard elsewhere ours=%d soon=%d held=%d grab=%d"
                             " | pressed ours=%d soon=%d held=%d grab=%d\n",
                             (int) starved.alreadyOurs, (int) starved.tooSoon,
                             (int) starved.mouseHeld,   (int) starved.wouldGrab,
                             (int) pressed.alreadyOurs, (int) pressed.tooSoon,
                             (int) pressed.mouseHeld,   (int) pressed.wouldGrab);
                check (starved.wouldGrab,
                       "with the keyboard elsewhere and no button down, the editor DOES take it "
                       "back - the reclaim still does its job");
                check (pressed.mouseHeld,
                       "an injected press really registers as a mouse button being held");
                check (! pressed.wouldGrab,
                       "...and while it is held the editor will NOT take the keyboard focus, "
                       "so nothing can revoke the capture the drag is riding on");
                inject (b, press); inject (b, {});

                // ...and the release must still re-arm the computer keyboard, or every
                // fader move would silently stop you playing. onDragEnd -> refocus()
                // runs during the mouseUp, when the button state is already clear.
                check (ed->hasKeyboardFocus (true),
                       "letting go of a fader hands the keyboard straight back to the editor");
            }
            else check (false, "the editor got a native peer to inject mouse events into");
            ed->removeFromDesktop();

            //-- HOW LONG THE MESSAGE THREAD IS BUSY while notes are sounding.
            // A drag is delivered on the message thread. If a timer tick blocks there,
            // the control simply does not follow the mouse for as long as the block
            // lasts - "a veces no deja mover el control", with no bug in the slider, the
            // routing or the focus at all. The heavy case: a scale with many degrees, the
            // recommendation list on screen, and the realtime spectrum adaptation
            // republishing the template under a held chord.
            {
                juce::String err;
                juce::String scl ("! e31.scl\n31-EDO\n 31\n!\n");
                for (int i = 1; i <= 31; ++i) scl << " " << juce::String (1200.0 * (double) i / 31.0, 6) << "\n";
                pd->loadScalaText (scl, "edo31", err);
                ed->setSpecModeForTests (1);
                ed->setTuneSourceForTests (1);

                FractalAdditiveAudioProcessor::RtSettings rs;
                rs.adapt.partials = 16; rs.adapt.periodRatio = 2.0; rs.adapt.minGapCents = 20.0;
                rs.adapt.rootOnly = true; rs.adapt.goal = 0; rs.adapt.shapeAmps = true;
                juce::MidiBuffer chord;
                for (const int k : { 60, 64, 67, 71 })
                    chord.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
                db.clear(); pd->processBlock (db, chord);
                pd->setRealtimeLink (FractalAdditiveAudioProcessor::RtLink::SpectrumFromChord, rs);

                // REAL TIME between ticks, because the two things that make a tick
                // expensive are rate-limited by the wall clock: the spectrum-driven
                // recompute of the recommendation list is allowed through once a second,
                // and each one CANCELS AND JOINS its worker on this very thread. A loop
                // with no time in it never lets either happen and measures nothing.
                // Notes are changed as it goes, which is what restarts the adaptation.
                double worstMs = 0.0; int worstTick = -1;
                for (int t = 0; t < 90; ++t)
                {
                    if (t % 15 == 7)
                    {
                        juce::MidiBuffer m;
                        m.addEvent (juce::MidiMessage::noteOff (1, 71), 0);
                        m.addEvent (juce::MidiMessage::noteOn (1, 69 + (t % 3), (juce::uint8) 100), 0);
                        db.clear(); pd->processBlock (db, m);
                    }
                    for (int i = 0; i < 6; ++i)
                    { juce::MidiBuffer none; db.clear(); pd->processBlock (db, none); pd->pollParametersForTests(); }
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    ed->tickTimerForTests();
                    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                    if (ms > worstMs) { worstMs = ms; worstTick = t; }
                    juce::Thread::sleep (33);   // the 30 Hz cadence the editor really runs at
                }
                std::printf ("  worst editor timer tick under a held chord + live adaptation: "
                             "%.1f ms (tick %d of 90)\n", worstMs, worstTick);
                // 30 Hz timer = 33 ms per tick. A tick that eats more than half of that
                // is a window that stutters under the hand; anything near 100 ms is a
                // control that visibly refuses to follow the mouse.
                check (worstMs < 33.0,
                       "no timer tick eats a whole frame, so the window keeps following the "
                       "mouse while notes are held");
                juce::MidiBuffer allOff;
                for (const int k : { 60, 64, 67, 71 }) allOff.addEvent (juce::MidiMessage::noteOff (1, k), 0);
                db.clear(); pd->processBlock (db, allOff);
                pd->setRealtimeLink (FractalAdditiveAudioProcessor::RtLink::Off, rs);
                for (int i = 0; i < 10; ++i) { juce::MidiBuffer none; db.clear(); pd->processBlock (db, none); }
            }
        }
    }

    //==========================================================================
    // --- "TUNING INFO" NEVER NUMBERS A DEGREE THE SCALE DOES NOT HAVE ---------------
    //
    // "los resultados de tuning info aparecen con numeros que no corresponden a los
    //  numeros de tunings que no son 12edo, tipo en una pentatonica me dice 7 y 11 como
    //  recomendaciones de acordes en ese tuning, cuando el periodo es 2"
    //
    // Every chip in that list is a DEGREE INDEX into the loaded scale, so on a 5-note
    // scale the only legal numbers are 0..4. "7" and "11" are 12-TET's degrees, left on
    // screen: the recompute was gated on the "from current tuning" SOURCE, and the
    // Tuning-info view lives in the other source (it is where a chord loaded from the
    // free search shows its own degrees), so in that view the list was never recomputed
    // at all. Driven through the real buttons and the real timer tick.
    {
        std::printf ("\n\"Tuning info\" numbers only degrees the loaded scale has:\n");
        auto p2 = std::make_unique<FractalAdditiveAudioProcessor>();
        p2->prepareToPlay (sr, blk);
        std::unique_ptr<juce::AudioProcessorEditor> base (p2->createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());

        if (ed == nullptr) check (false, "the editor was created");
        else
        {
            // The worker publishes on its own thread; the editor only picks the rows up
            // on a timer tick, and this build has no message loop to deliver one.
            auto settle = [&]
            {
                for (int i = 0; i < 1200 && p2->isNadBusy(); ++i) juce::Thread::sleep (5);
                ed->tickTimerForTests();
            };

            // (a) The same list in the OTHER source, over 12-TET's twelve degrees. This
            //     is the state whose leftovers used to be shown under a 5-note scale.
            ed->setSpecModeForTests (1);     // "Find tunings based on beating"
            ed->setTuneSourceForTests (1);   // ...from the current tuning
            ed->tickTimerForTests();
            settle();
            const auto a = ed->nadShownProbeForTests();
            std::printf ("  12-TET: %d rows, biggest chip %d, scale has %d degrees\n",
                         a.rows, a.maxDegree, a.degreesPerPeriod);
            check (a.rows > 0 && a.maxDegree >= 0, "12-TET fills the recommendation list");
            check (a.maxDegree <= a.degreesPerPeriod - 1,
                   "...and every chip is a degree 12-TET really has (0..11)");

            // (b) Now the free chord search, a five-note chord LOADED as the scale, and
            //     the "Tuning info" view it flips to - the user's exact path.
            ed->setTuneSourceForTests (2);   // ...from all possible tunings (free search)
            juce::String err;
            check (p2->startChordSearch (5, 2.0, "2/1", 8, false, 30.0, true, err),
                   "a five-note chord search starts");
            for (int i = 0; i < 4000 && p2->isChordSearchRunning(); ++i) juce::Thread::sleep (5);
            ed->tickTimerForTests();         // the results reach the list
            ed->loadChordResultForTests (0); // double-click row 1: load it AND jump to Tuning info
            std::printf ("  loaded \"%s\" -> %d degrees\n",
                         p2->getGeneratedSclName().toRawUTF8(), p2->getScaleDegrees());
            check (p2->getScaleDegrees() == 5,
                   "the loaded chord IS the scale now, and it is a five-note one");
            ed->tickTimerForTests();
            settle();
            const auto b = ed->nadShownProbeForTests();
            std::printf ("  Tuning info: %d rows, biggest chip %d, scale has %d degrees%s\n",
                         b.rows, b.maxDegree, b.degreesPerPeriod, b.equalStep ? " (offsets)" : "");
            check (b.maxDegree <= b.degreesPerPeriod - 1,
                   "every number \"Tuning info\" shows is a degree THIS scale has - no 7 or 11 "
                   "in a five-note tuning");
            check (b.rows > 0, "...and it really did recompute, rather than going blank");
            // The stamp the display gate reads. A worker publishes on its own thread, so
            // rows CAN land after the tuning has moved; the editor refuses to show rows
            // whose stamp does not match the scale in force rather than print indices
            // into a scale that is gone.
            check (p2->getNadResultDegrees() == p2->getScaleDegrees(),
                   "the published rows carry the degree count they index into");
        }
    }

    //==========================================================================
    // --- run 43: ADAPTIVE CORNERS stay live inside the playground morph ---------------
    // "quiero que los presets puedan tener espectro adaptativo y aun asi morphear entre
    //  ellos en el preset playground... quiero que afecte, para dar pie a morphs locos,
    //  como cuando cambias de espectro con el morph on."
    // A corner whose preset was saved with the SpectrumFromChord follower armed must
    // keep adapting INSIDE the playground: its corner template changes when the chord
    // changes, the blended LIVE template moves with it (diluted, but present), each
    // adaptive update publishes WITH a generation bump (the voices glide/fade it), a
    // drag bumps the generation at the 10 Hz glide cadence only (never 60 Hz), a
    // stale result is dropped, a reload re-arms the corners - and none of it clicks
    // or thumps, morph on OR off.
    {
        std::printf ("\nadaptive corners inside the playground:\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr int    eb = 64;
        constexpr double sr = 48000.0;

        auto folder = Proc::getPresetFolder();
        auto fileA = folder.getChildFile ("zzz-verify-pga-A.gsting");
        auto fileB = folder.getChildFile ("zzz-verify-pga-B.gsting");
        auto fileC = folder.getChildFile ("zzz-verify-pga-C.gsting");

        // Three presets: A static, B the SAME sound saved with the follower armed, C the
        // same sound with it off (the control corner - so the control take differs from
        // the adaptive take by exactly one thing: whether the corner adapts).
        {
            auto m = std::make_unique<Proc>();
            m->prepareToPlay (sr, eb);
            auto sm = [&m] (const char* id, float v)
            { if (auto* q = m->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
            sm ("ampAttack", 0.001f); sm ("ampDecay", 0.001f); sm ("ampSustain", 1.0f);
            sm ("ampRelease", 0.003f); sm ("baseN", 1.0f); sm ("curveSlope", 1.0f);
            sm ("fractalLevel", 1.0f);
            m->pollParametersForTests();
            check (m->savePreset ("zzz-verify-pga-A").isNotEmpty(), "static corner preset saved");
            Proc::RtSettings rs;
            rs.adapt.partials = 6; rs.adapt.rootOnly = true; rs.adapt.shapeAmps = true;
            m->setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
            m->pollParametersForTests();
            check (m->savePreset ("zzz-verify-pga-B").isNotEmpty(),
                   "adaptive corner preset saved (REALTIME armed)");
            m->setRealtimeLink (Proc::RtLink::Off, {});
            m->pollParametersForTests();
            check (m->savePreset ("zzz-verify-pga-C").isNotEmpty(), "control corner preset saved");
        }

        const auto ratiosOf = [] (const std::shared_ptr<const spectrum::SpectrumTemplate>& t)
        {
            std::vector<float> r;
            if (t != nullptr) for (const auto& p : t->partials) r.push_back (p.ratio);
            return r;
        };
        const auto liveRatios = [] (Proc& p)
        {
            std::vector<float> r;
            if (const auto* t = p.getLiveTemplate())
                for (const auto& pp : t->partials) r.push_back (pp.ratio);
            return r;
        };
        const auto differs = [] (const std::vector<float>& a, const std::vector<float>& b)
        {
            if (a.size() != b.size()) return true;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::abs (a[i] - b[i]) > 1.0e-4f) return true;
            return false;
        };

        // One wall-clock-paced take: corners A + (B|C) at the midpoint, chord A held and
        // settled, then the chord MOVES while the recorder runs. Same rig as run 42, so
        // the searches (real threads) and the 60 Hz poll keep their real cadence.
        struct Take
        {
            std::vector<float> x;          // recorded audio across the chord move
            size_t T0 = 0, T1 = 0;         // the move, and the settled end
            juce::uint32 genBeforeMove = 0, genAfterSettle = 0;
            int startsEnd = 0, appliesEnd = 0;
            std::vector<float> cornerBefore, cornerAfter, liveBefore, liveAfter;
            std::unique_ptr<Proc> p;       // handed back for the follow-on probes
        };
        auto take = [&] (bool adaptiveB, bool morphOn) -> Take
        {
            Take tk;
            tk.p = std::make_unique<Proc>();
            auto& p = *tk.p;
            p.prepareToPlay (sr, eb);
            auto sp = [&p] (const char* id, float v)
            { if (auto* q = p.apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
            sp ("ampAttack", 0.001f); sp ("ampDecay", 0.001f); sp ("ampSustain", 1.0f);
            sp ("ampRelease", 0.003f); sp ("morph", morphOn ? 1.0f : 0.0f); sp ("morphTime", 0.3f);
            p.pollParametersForTests();

            juce::String err;
            p.assignPlaygroundCorner (0, "zzz-verify-pga-A", err);
            p.assignPlaygroundCorner (1, adaptiveB ? "zzz-verify-pga-B" : "zzz-verify-pga-C", err);
            sp ("pgX", 0.5f); sp ("pgY", 0.0f);
            p.pollParametersForTests();

            juce::AudioBuffer<float> b (2, eb);
            long long clockS = 0, nextPoll = 0;
            const long long tickS = (long long) (sr / 60.0);
            const double t0ms = juce::Time::getMillisecondCounterHiRes();
            bool recording = false;
            auto pumpFor = [&] (double seconds, juce::MidiBuffer firstBlockMidi)
            {
                const long long want = clockS + (long long) (seconds * sr);
                bool sent = false;
                while (clockS < want)
                {
                    juce::MidiBuffer m;
                    if (! sent) { m = firstBlockMidi; sent = true; }
                    b.clear();
                    p.processBlock (b, m);
                    if (recording)
                        for (int k = 0; k < eb; ++k) tk.x.push_back (b.getReadPointer (0)[k]);
                    clockS += eb;
                    if (clockS >= nextPoll) { nextPoll += tickS; p.pollParametersForTests(); }
                    for (;;)   // never outrun the searcher's wall clock (as run 42)
                    {
                        const double behind = (double) clockS * 1000.0 / sr
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

            pumpFor (0.30, {});
            pumpFor (1.40, keyMidi (true, { 60, 64, 67 }));      // chord A held + settled
            tk.cornerBefore  = ratiosOf (p.getPlaygroundCornerTemplateForTests (1));
            tk.liveBefore    = liveRatios (p);
            tk.genBeforeMove = p.getTemplateGeneration();
            recording = true;
            pumpFor (0.10, {});
            tk.T0 = tk.x.size();                                  // the chord MOVES here
            pumpFor (0.02, keyMidi (false, { 60, 64, 67 }));
            pumpFor (1.60, keyMidi (true,  { 62, 65, 69 }));      // C major -> D minor
            tk.T1 = tk.x.size();
            recording = false;
            tk.cornerAfter    = ratiosOf (p.getPlaygroundCornerTemplateForTests (1));
            tk.liveAfter      = liveRatios (p);
            tk.genAfterSettle = p.getTemplateGeneration();
            tk.startsEnd      = p.getPlaygroundAdaptStarts();
            tk.appliesEnd     = p.getPlaygroundAdaptApplies();
            return tk;
        };

        // Run-26-style per-sample click metric, plus run-42-style loudness excursion:
        // the event window straddles the landing (which arrives search+throttle after
        // T0), the calm window is the settled tail of the SAME take, so the note's own
        // beating is the yardstick the landing is measured against.
        const auto clickOf = [] (const Take& tk)
        {
            double maxJump = 0.0, peak = 0.0;
            const size_t from = tk.T0 + (size_t) (0.030 * 48000.0);
            for (size_t i = from + 1; i < tk.T1 && i < tk.x.size(); ++i)
            {
                peak    = std::max (peak, (double) std::abs (tk.x[i]));
                maxJump = std::max (maxJump, (double) std::abs (tk.x[i] - tk.x[i - 1]));
            }
            return peak > 0.0 ? maxJump / peak : 0.0;
        };
        const auto thumpOf = [] (const Take& tk)
        {
            constexpr int rw = 960, rh = 120, exSpan = 12;       // 20 ms RMS, 30 ms excursion
            std::vector<double> rms;
            std::vector<size_t> at;
            for (size_t a = tk.T0; a + (size_t) rw <= tk.T1; a += (size_t) rh)
            {
                double s2 = 0.0;
                for (int i = 0; i < rw; ++i) { const double v = tk.x[a + (size_t) i]; s2 += v * v; }
                rms.push_back (std::sqrt (s2 / rw));
                at.push_back (a + (size_t) rw / 2);
            }
            double mean = 0.0;
            for (const double v : rms) mean += v;
            mean /= std::max<size_t> (1, rms.size());
            // The event window opens AFTER the new chord's own 1 ms attack has fully
            // left the 20 ms RMS windows (the landing cannot arrive before ~60 ms:
            // detection tick + search + apply tick), so what this measures is the
            // LANDING - with the attack inside the window the attack's step dominated
            // and the metric could not see a landing thump at all.
            const size_t evLo = tk.T0 + (size_t) (0.070 * 48000.0);
            const size_t evHi = tk.T0 + (size_t) (0.500 * 48000.0);
            const size_t caLo = tk.T1 - (size_t) (0.450 * 48000.0);
            const size_t caHi = tk.T1 - (size_t) (0.030 * 48000.0);
            double ev = 0.0, calm = 0.0;
            for (size_t i = 0; i + (size_t) exSpan < rms.size(); ++i)
            {
                double lo = rms[i], hi = rms[i];
                for (int k = 1; k <= exSpan; ++k)
                { lo = std::min (lo, rms[i + (size_t) k]); hi = std::max (hi, rms[i + (size_t) k]); }
                const double e = (hi - lo) / std::max (1.0e-12, mean);
                if (at[i] >= evLo && at[i] < evHi) ev   = std::max (ev, e);
                if (at[i] >= caLo && at[i] < caHi) calm = std::max (calm, e);
            }
            return calm > 0.0 ? ev / calm : 0.0;
        };

        // ---- morph OFF: the adaptation moves the corner, the blend, and the ear ----
        auto ctlOff = take (false, false);
        auto adpOff = take (true,  false);
        std::printf ("  morph off: control starts/applies %d/%d, adaptive %d/%d; "
                     "gen ctl +%u adp +%u\n",
                     ctlOff.startsEnd, ctlOff.appliesEnd, adpOff.startsEnd, adpOff.appliesEnd,
                     ctlOff.genAfterSettle - ctlOff.genBeforeMove,
                     adpOff.genAfterSettle - adpOff.genBeforeMove);
        check (adpOff.p->getPlaygroundAdaptiveCount() == 1,
               "a corner saved with REALTIME armed counts as adaptive inside the playground");
        check (ctlOff.p->getPlaygroundAdaptiveCount() == 0 && ctlOff.startsEnd == 0,
               "a corner saved with it off never launches a search");
        check (adpOff.startsEnd >= 1 && adpOff.appliesEnd >= 1,
               "the adaptive corner really searched and really applied");
        check (differs (adpOff.cornerBefore, adpOff.cornerAfter),
               "the CORNER's template changes when the chord changes");
        check (differs (adpOff.liveBefore, adpOff.liveAfter),
               "...and the LIVE blended template moves with it - the point never moved");
        check (! differs (ctlOff.liveBefore, ctlOff.liveAfter),
               "the control take's live template does not move (the change IS the adaptation)");
        check (adpOff.genAfterSettle > adpOff.genBeforeMove,
               "an adaptive update publishes WITH a generation bump - the voices fade/morph it");
        check (ctlOff.genAfterSettle == ctlOff.genBeforeMove,
               "the control take's generation stands still across the same chord move");

        const double jOffC = clickOf (ctlOff), jOffA = clickOf (adpOff);
        const double tOffA = thumpOf (adpOff);
        std::printf ("  morph off: jump/peak ctl %.3f adp %.3f; landing/calm RMS excursion %.2fx\n",
                     jOffC, jOffA, tOffA);
        check (jOffA <= jOffC * 2.5 + 0.02, "morph off: an adaptive landing does not click");
        check (tOffA <= 1.6,
               "morph off: the landing is no louder a step than the note's own movement (no thump)");

        // ---- morph ON: same take, the voices GLIDE to each adaptive blend ----
        auto ctlOn = take (false, true);
        auto adpOn = take (true,  true);
        const double jOnC = clickOf (ctlOn), jOnA = clickOf (adpOn);
        const double tOnA = thumpOf (adpOn);
        std::printf ("  morph on:  jump/peak ctl %.3f adp %.3f; landing/calm RMS excursion %.2fx; "
                     "starts/applies %d/%d\n", jOnC, jOnA, tOnA, adpOn.startsEnd, adpOn.appliesEnd);
        check (adpOn.appliesEnd >= 1 && adpOn.genAfterSettle > adpOn.genBeforeMove,
               "morph on: the adaptation lands and bumps the generation (the morph glides to it)");
        check (jOnA <= jOnC * 2.5 + 0.02, "morph on: an adaptive landing does not click either");
        check (tOnA <= 1.6, "morph on: ...and does not thump");

        // ---- the follow-on probes run on the settled morph-off adaptive rig ----
        auto& p = *adpOff.p;
        auto sp = [&p] (const char* id, float v)
        { if (auto* q = p.apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };

        // A DRAG under a held chord bumps the generation at the 10 Hz GLIDE cadence
        // only (the same throttle run 42 measures): the chord is steady, so no
        // adaptation is due, and the drag frames must never restart morphs at the full
        // 60 Hz - while the revision moves on every frame.
        {
            for (int i = 0; i < 30; ++i) { p.pollParametersForTests(); juce::Thread::sleep (5); }
            const auto gen0 = p.getTemplateGeneration();
            const auto rev0 = p.getSpectrumLiveRevision();
            // TOWARDS the incumbent winner (A, adopted at the midpoint engage): a drag
            // the other way crosses the 0.08-hysteresis boundary, and a winner flip is a
            // real discrete jump that bumps the generation BY DESIGN - it must not be
            // mistaken for the drag itself doing it.
            for (int i = 1; i <= 20; ++i)
            {
                sp ("pgX", 0.5f - (float) i * 0.01f);
                p.pollParametersForTests();
                juce::Thread::sleep (12);
            }
            juce::Thread::sleep (150);
            p.pollParametersForTests();
            const auto genD = p.getTemplateGeneration() - gen0;
            std::printf ("  drag with an adaptive corner engaged: generation +%u, revision +%u\n",
                         genD, p.getSpectrumLiveRevision() - rev0);
            check (genD >= 2 && genD <= 5,
                   "a drag with an adaptive corner engaged bumps at the glide cadence, never 60 Hz");
            check (p.getSpectrumLiveRevision() > rev0 + 5,
                   "...while the live template really is being replaced along the way");
        }

        // A STALE RESULT IS DROPPED. Move to chord P, wait until P's search is genuinely
        // RUNNING, move to Q mid-search: P's answer must be dropped (the chord it was
        // aimed at is gone) and Q must get exactly one apply of its own.
        {
            juce::AudioBuffer<float> b (2, eb);
            auto blockWith = [&] (juce::MidiBuffer m)
            { b.clear(); p.processBlock (b, m); };
            auto chord = [] (bool on, std::initializer_list<int> notes)
            {
                juce::MidiBuffer m;
                for (const int k : notes)
                    m.addEvent (on ? juce::MidiMessage::noteOn (1, k, (juce::uint8) 100)
                                   : juce::MidiMessage::noteOff (1, k), 0);
                return m;
            };
            auto settle = [&] (double seconds)
            {
                const int blocks = (int) (seconds * sr / eb);
                for (int i = 0; i < blocks; ++i)
                {
                    blockWith ({});
                    if ((i % 12) == 0) p.pollParametersForTests();
                    juce::Thread::sleep (1);
                }
            };
            blockWith (chord (false, { 62, 65, 69 }));
            settle (0.6);
            const int starts0 = p.getPlaygroundAdaptStarts(), applies0 = p.getPlaygroundAdaptApplies();
            blockWith (chord (true, { 60, 64, 67, 65 }));        // P
            bool sawRunning = false;
            for (int i = 0; i < 4000 && ! sawRunning; ++i)
            {
                blockWith ({});
                p.pollParametersForTests();
                sawRunning = p.isSpectrumAdaptRunning();
            }
            blockWith (chord (true, { 62 }));                     // Q, mid-search
            settle (1.0);
            const int startsD = p.getPlaygroundAdaptStarts() - starts0;
            const int appliesD = p.getPlaygroundAdaptApplies() - applies0;
            std::printf ("  P then Q mid-search: running seen %d, starts +%d, applies +%d\n",
                         (int) sawRunning, startsD, appliesD);
            check (sawRunning, "Q really did land while P's search was still running");
            check (startsD == 2 && appliesD == 1,
                   "a result aimed at a chord the player has left is DROPPED, not applied late");
        }

        // WHEN THE ADAPTIVE CORNER IS THE WINNER, the live patch's spectrum state
        // follows its adaptation - the Custom Engine box must describe what sounds.
        {
            sp ("pgX", 1.0f); sp ("pgY", 0.0f);                   // jump onto corner B
            p.pollParametersForTests();
            juce::AudioBuffer<float> b (2, eb);
            juce::MidiBuffer off1;
            for (const int k : { 60, 62, 64, 65, 67 }) off1.addEvent (juce::MidiMessage::noteOff (1, k), 0);
            b.clear(); p.processBlock (b, off1);
            juce::MidiBuffer on2;
            for (const int k : { 57, 61, 64 }) on2.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            b.clear(); p.processBlock (b, on2);
            for (int i = 0; i < 900; ++i)
            {
                juce::MidiBuffer m; b.clear(); p.processBlock (b, m);
                if ((i % 12) == 0) p.pollParametersForTests();
                juce::Thread::sleep (1);
            }
            std::printf ("  winner-corner adaptation: live custom spectrum '%s' (%d degrees)\n",
                         p.getTimbreSource().substring (0, 40).toRawUTF8(), p.getTimbreNoteCount());
            check (p.timbreLoaded() && p.getTimbreSource().contains ("c"),
                   "with the adaptive corner as WINNER the live Custom Engine shows the adapted intervals");
        }

        // A RELOADED playground re-arms its adaptive corners.
        {
            juce::MemoryBlock blob;
            p.getStateInformation (blob);
            auto q = std::make_unique<Proc>();
            q->prepareToPlay (sr, eb);
            q->setStateInformation (blob.getData(), (int) blob.getSize());
            q->pollParametersForTests();
            check (q->isPlaygroundActive() && q->getPlaygroundAdaptiveCount() == 1,
                   "a reloaded playground re-arms its adaptive corner");
            juce::AudioBuffer<float> b (2, eb);
            juce::MidiBuffer on;
            for (const int k : { 60, 64, 67 }) on.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
            b.clear(); q->processBlock (b, on);
            for (int i = 0; i < 900 && q->getPlaygroundAdaptApplies() < 1; ++i)
            {
                juce::MidiBuffer m; b.clear(); q->processBlock (b, m);
                if ((i % 12) == 0) q->pollParametersForTests();
                juce::Thread::sleep (1);
            }
            std::printf ("  after reload: starts %d applies %d\n",
                         q->getPlaygroundAdaptStarts(), q->getPlaygroundAdaptApplies());
            check (q->getPlaygroundAdaptApplies() >= 1,
                   "...and it genuinely adapts again after the reload");
        }

        fileA.deleteFile();
        fileB.deleteFile();
        fileC.deleteFile();
    }

    //==========================================================================
    // --- run 44: playground MIX mode - the integrated "multi-Fibló" -------------------
    // "haz que el playground tenga un modo morph y otro modo que sea mix en que los 4
    //  presets pueden sonar superpuestos a la vez, osea mover de un lado a otro equivale
    //  a subir/bajar volumen de uno u otro."
    // Verified on the REAL processor, by ear-equivalent measurement: each corner is
    // rendered by its own engine, the point is an equal-power volume balance, the live
    // patch is untouched and comes back exactly, and none of it clicks.
    {
        std::printf ("\nrun 44: playground MIX mode (multi-engine, end to end):\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr double msr = 48000.0;
        constexpr int    meb = 64;

        auto folder = Proc::getPresetFolder();
        auto fileA  = folder.getChildFile ("zzz-verify-mix-A.gsting");
        auto fileB  = folder.getChildFile ("zzz-verify-mix-B.gsting");

        // Windowed Goertzel magnitude of one partial in a captured stretch of output.
        // Hann-windowed so the strong neighbours of a dense harmonic spectrum do not
        // leak into the bin being asked about.
        const auto toneAmp = [] (const std::vector<float>& x, double sr, double hz)
        {
            const size_t n = x.size();
            if (n < 16) return 0.0;
            const double w = 2.0 * juce::MathConstants<double>::pi * hz / sr;
            const double c = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                const double win = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                         * (double) i / (double) (n - 1));
                const double s0 = (double) x[i] * win + c * s1 - s2;
                s2 = s1; s1 = s0;
            }
            const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
            return std::sqrt (re * re + im * im) / (0.25 * (double) n); // /(N/2 * hann gain)
        };

        auto p = std::make_unique<Proc>();
        p->prepareToPlay (msr, meb);
        auto sp = [&] (const char* id, float plain)
        {
            if (auto* q = p->apvts.getParameter (id))
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
        };
        auto poll = [&] (int times = 3)
        { for (int i = 0; i < times; ++i) { p->pollParametersForTests(); juce::Thread::sleep (2); } };
        juce::AudioBuffer<float> buf (2, meb);
        auto renderSec = [&] (double seconds, std::vector<float>* capture = nullptr)
        {
            const int blocks = (int) (seconds * msr / meb);
            for (int i = 0; i < blocks; ++i)
            {
                juce::MidiBuffer m; buf.clear(); p->processBlock (buf, m);
                if (capture != nullptr)
                {
                    const float* L = buf.getReadPointer (0);
                    capture->insert (capture->end(), L, L + meb);
                }
                if ((i % 12) == 0) poll (1);
            }
        };
        const double f0 = 261.6255653; // key 60 under the default tuning

        // THREE DISTINGUISHABLE SPECTRA. Corner A: the harmonic engine (has 7.00,
        // cannot have 1.25). Corner B: the custom scale 5/4 3/2 2 (has 1.25, cannot
        // have 7.00). The LIVE patch: 11/8 2 (has 1.375 and neither of the others), so
        // "the main engine is silent under the mix, and comes back after" is a
        // measurement, not a hope.
        {
            juce::String err;
            p->clearTimbre();
            sp ("timbreOn", 0.0f); sp ("timbreTemper", 0.0f);
            sp ("ampAttack", 0.005f); sp ("ampSustain", 1.0f); sp ("ampDecay", 0.2f);
            sp ("baseN", 1.0f); sp ("curveSlope", 1.0f); sp ("masterGain", 0.0f);
            poll();
            check (p->savePreset ("zzz-verify-mix-A").isNotEmpty(), "mix corner preset A saved (harmonic)");
            check (p->loadTimbreText ("5/4 3/2 2", err), "corner B gets the custom scale");
            sp ("timbreRepeats", 4.0f);
            poll();
            check (p->savePreset ("zzz-verify-mix-B").isNotEmpty(), "mix corner preset B saved (custom)");
            check (p->loadTimbreText ("11/8 2", err), "the LIVE patch gets its own signature spectrum");
            poll();
        }

        // MIX MODE FIRST, corners after: from here on the playground must never write a
        // live parameter, which is exactly what the round-trip check at the end proves.
        sp ("pgMode", 1.0f); sp ("pgX", 0.0f); sp ("pgY", 0.0f);
        poll();
        std::vector<std::pair<juce::String, float>> snap;
        for (auto* ap : p->getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap))
                snap.push_back ({ rp->paramID, rp->getValue() });

        // Hold the note BEFORE engaging, so the engage path has to re-strike it into
        // the cold corner engines - a held chord must survive the hand-over.
        {
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            buf.clear(); p->processBlock (buf, on);
        }
        renderSec (0.2);

        juce::String err;
        check (p->assignPlaygroundCorner (0, "zzz-verify-mix-A", err), "A on corner 0");
        check (p->assignPlaygroundCorner (1, "zzz-verify-mix-B", err), "B on corner 1");
        poll();
        renderSec (0.1);
        check (p->isPlaygroundMixMode() && p->isPlaygroundMixEngaged(),
               "two corners + mix mode = the mix is engaged and sounding");

        // VERTEX A: only corner A's engine, at full level - the note that was already
        // held sounds because engage re-struck it.
        std::vector<float> cap;
        renderSec (0.8);                       // engage fade + gain slew fully settled
        cap.clear(); renderSec (0.4, &cap);
        const double a7A = toneAmp (cap, msr, 7.0 * f0), a125A = toneAmp (cap, msr, 1.25 * f0),
                     a1375A = toneAmp (cap, msr, 1.375 * f0);
        std::printf ("  vertex A: harmonic7 %.5f, custom1.25 %.5f, live1.375 %.5f\n", a7A, a125A, a1375A);
        check (a7A > 1.0e-4, "at vertex A the held note sounds through corner A's engine (re-struck on engage)");
        check (a125A < a7A * 0.1, "corner B at gain 0 contributes silence");
        check (a1375A < a7A * 0.1, "the LIVE patch is silent under the mix (it is not corner 0's slot)");

        // VERTEX B: only corner B's engine - a different ENGINE, not a blended patch.
        sp ("pgX", 1.0f); poll();
        renderSec (0.5);
        cap.clear(); renderSec (0.4, &cap);
        const double a7B = toneAmp (cap, msr, 7.0 * f0), a125B = toneAmp (cap, msr, 1.25 * f0);
        std::printf ("  vertex B: harmonic7 %.5f, custom1.25 %.5f\n", a7B, a125B);
        check (a125B > 1.0e-4, "at vertex B corner B's custom engine sounds");
        check (a7B < a125B * 0.1, "and corner A is gone - the point really is a volume balance");

        // THE MIDDLE: both engines AT ONCE (the thing morph mode cannot do is two
        // envelopes/two mod banks - spectrally the proof is both signatures together),
        // each at sqrt(0.5) of its vertex level: the equal-power law, measured.
        sp ("pgX", 0.5f); poll();
        renderSec (0.5);
        cap.clear(); renderSec (0.4, &cap);
        const double a7M = toneAmp (cap, msr, 7.0 * f0), a125M = toneAmp (cap, msr, 1.25 * f0);
        const double ratioB = a125B > 0.0 ? a125M / a125B : 0.0;
        const double ratioA = a7A > 0.0 ? a7M / a7A : 0.0;
        std::printf ("  middle: harmonic7 %.5f (%.3f of its vertex), custom1.25 %.5f (%.3f of its vertex; "
                     "equal-power predicts 0.707)\n", a7M, ratioA, a125M, ratioB);
        check (a7M > 1.0e-4 && a125M > 1.0e-4, "half way across BOTH presets sound simultaneously");
        check (ratioB > 0.55 && ratioB < 0.87 && ratioA > 0.55 && ratioA < 0.87,
               "each corner sits at ~0.707 of its vertex level - the equal-power law, not a linear sag");

        // DISENGAGE (clear the corners): the live patch comes back sounding, exactly
        // as it was - no parameter moved, no spectrum overwritten.
        p->assignPlaygroundCorner (0, {}, err);
        p->assignPlaygroundCorner (1, {}, err);
        poll();
        renderSec (0.6);
        cap.clear(); renderSec (0.4, &cap);
        const double a1375D = toneAmp (cap, msr, 1.375 * f0), a125D = toneAmp (cap, msr, 1.25 * f0);
        std::printf ("  disengaged: live1.375 %.5f, custom1.25 %.5f\n", a1375D, a125D);
        check (! p->isPlaygroundMixEngaged(), "clearing the corners disengages the mix");
        check (a1375D > 1.0e-4, "the live patch comes BACK SOUNDING mid-note (it was never killed, only faded)");
        check (a125D < a1375D * 0.1, "no corner engine is left sounding behind it");
        {
            int moved = 0;
            juce::String movedId;
            for (auto* ap : p->getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap))
                    for (const auto& s : snap)
                        if (s.first == rp->paramID
                            && s.first != "pgX" && s.first != "pgY"   // the pad itself, moved on purpose
                            && std::abs (s.second - rp->getValue()) > 1.0e-6f)
                        { ++moved; if (movedId.isEmpty()) movedId = s.first; }
            std::printf ("  parameters moved by the whole mix session: %d%s%s\n",
                         moved, moved > 0 ? " first: " : "", movedId.toRawUTF8());
            check (moved == 0,
                   "MIX never wrote a single live parameter - disengaging restores the patch EXACTLY");
        }

        // RELOADING MID-MIX comes back sounding.
        {
            check (p->assignPlaygroundCorner (0, "zzz-verify-mix-A", err)
                   && p->assignPlaygroundCorner (1, "zzz-verify-mix-B", err), "corners back on");
            sp ("pgX", 1.0f); poll();
            juce::MemoryBlock blob;
            p->getStateInformation (blob);

            auto q = std::make_unique<Proc>();
            q->prepareToPlay (msr, meb);
            q->setStateInformation (blob.getData(), (int) blob.getSize());
            for (int i = 0; i < 5; ++i) { q->pollParametersForTests(); juce::Thread::sleep (2); }
            check (q->isPlaygroundMixMode() && q->isPlaygroundMixEngaged(),
                   "a project saved mid-MIX reloads with the mix engaged");
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            juce::AudioBuffer<float> qb (2, meb);
            qb.clear(); q->processBlock (qb, on);
            std::vector<float> qcap;
            for (int i = 0; i < (int) (1.0 * msr / meb); ++i)
            {
                juce::MidiBuffer m; qb.clear(); q->processBlock (qb, m);
                if ((i % 12) == 0) q->pollParametersForTests();
                if (i > (int) (0.6 * msr / meb))
                {
                    const float* L = qb.getReadPointer (0);
                    qcap.insert (qcap.end(), L, L + meb);
                }
            }
            const double q125 = toneAmp (qcap, msr, 1.25 * f0), q7 = toneAmp (qcap, msr, 7.0 * f0);
            std::printf ("  after reload at vertex B: custom1.25 %.5f, harmonic7 %.5f\n", q125, q7);
            check (q125 > 1.0e-4 && q7 < q125 * 0.1,
                   "...and it SOUNDS: corner B's own engine, at its vertex, straight off the reload");
        }

        // CLICK-FREENESS, by run 26's own metric (largest sample-to-sample jump over
        // peak). Control: the mix parked. Test 1: the balance jumped corner-to-corner
        // by automation every few blocks (gains must ramp, never step). Test 2: the
        // MODE flipped morph<->mix repeatedly (the engage/disengage crossfade).
        //
        // The corners here are NEAR-SINE presets (slope 6: the second partial sits
        // -36 dB down), deliberately: a sine's own per-sample slope is ~2*pi*f/sr of
        // its peak (~0.05 here), so the metric's floor is LOW and a stepped gain
        // (jump/peak ~ 0.7 at the equal-power vertex level) cannot hide inside the
        // signal's natural movement - which it could inside the dense-spectrum
        // presets above, whose honest slope is ~0.36 of peak.
        {
            juce::String e0;
            p->clearTimbre();
            sp ("curveSlope", 6.0f); sp ("ampAttack", 0.005f); sp ("ampSustain", 1.0f);
            poll();
            check (p->savePreset ("zzz-verify-mix-S1").isNotEmpty(), "near-sine corner S1 saved");
            check (p->loadTimbreText ("3/2 2", e0), "S2's custom scale loads");
            poll();
            check (p->savePreset ("zzz-verify-mix-S2").isNotEmpty(), "near-sine corner S2 saved");

            auto clickRun = [&] (int mode, const char* label)
            {
                auto r = std::make_unique<Proc>();
                r->prepareToPlay (msr, meb);
                auto rs = [&] (const char* id, float v)
                { if (auto* q = r->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
                rs ("pgMode", 1.0f); rs ("ampAttack", 0.005f); rs ("ampSustain", 1.0f);
                r->pollParametersForTests();
                juce::String e2;
                r->assignPlaygroundCorner (0, "zzz-verify-mix-S1", e2);
                r->assignPlaygroundCorner (1, "zzz-verify-mix-S2", e2);
                r->pollParametersForTests();
                juce::AudioBuffer<float> b (2, meb);
                {
                    juce::MidiBuffer m;
                    m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                    m.addEvent (juce::MidiMessage::noteOn (1, 67, 0.8f), 1);
                    b.clear(); r->processBlock (b, m);
                }
                for (int i = 0; i < 120; ++i)   // engage fade + attack fully done
                { juce::MidiBuffer m; b.clear(); r->processBlock (b, m);
                  if ((i % 12) == 0) r->pollParametersForTests(); }

                float maxJump = 0.0f, peak = 0.0f, prev = 0.0f;
                bool have = false;
                for (int step = 0; step < 240; ++step)
                {
                    if (mode == 1 && (step % 6) == 0)   // ~8 ms cadence: a hostile LFO on the balance
                        rs ("pgX", (step / 6) & 1 ? 1.0f : 0.0f);
                    if (mode == 2 && (step % 45) == 0)  // flip morph<->mix every ~60 ms
                        rs ("pgMode", (step / 45) & 1 ? 0.0f : 1.0f);
                    if ((step % 3) == 0) r->pollParametersForTests();
                    juce::MidiBuffer m; b.clear(); r->processBlock (b, m);
                    const float* L = b.getReadPointer (0);
                    for (int k = 0; k < meb; ++k)
                    {
                        const float x = L[k];
                        peak = std::max (peak, std::abs (x));
                        if (have) maxJump = std::max (maxJump, std::abs (x - prev));
                        prev = x; have = true;
                    }
                }
                const float ratio = peak > 0.0f ? maxJump / peak : 0.0f;
                std::printf ("  %-28s peak=%.4f maxJump=%.4f jump/peak=%.3f\n", label, peak, maxJump, ratio);
                return ratio;
            };
            const float ctl  = clickRun (0, "mix parked (control)");
            const float bal  = clickRun (1, "balance jumped 0<->1");
            const float flip = clickRun (2, "mode flipped morph<->mix");
            check (ctl > 0.0f, "the control case actually made sound");
            check (bal  <= ctl * 2.5f + 0.02f, "slamming the balance corner-to-corner does not click");
            check (flip <= ctl * 2.5f + 0.02f, "switching morph<->mix mid-note does not click");
            folder.getChildFile ("zzz-verify-mix-S1.gsting").deleteFile();
            folder.getChildFile ("zzz-verify-mix-S2.gsting").deleteFile();
        }

        // CPU, measured not guessed: ms per 512-sample block at 48 kHz (budget 10.67 ms),
        // main engine alone vs the mix (main still tracking underneath - that cost is
        // the no-orphaned-notes guarantee and is REPORTED, not hidden). The EXTREME
        // case - 32 dense-spectrum notes, every engine at 32 voices - shows why the
        // per-corner polyphony is a VISIBLE control (the main engine ALONE is near the
        // whole budget there); the REALISTIC case - a four-note chord at the default
        // voice budget of 8 - is what must actually fit.
        {
            auto bench = [&] (int corners, int voicesPerCorner, int notes, const char* label)
            {
                auto r = std::make_unique<Proc>();
                r->prepareToPlay (48000.0, 512);
                auto rs = [&] (const char* id, float v)
                { if (auto* q = r->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
                rs ("ampSustain", 1.0f);
                if (corners > 0)
                {
                    rs ("pgMode", 1.0f);
                    rs ("pgVoices", (float) voicesPerCorner);
                    juce::String e2;
                    static const char* names[4] = { "zzz-verify-mix-A", "zzz-verify-mix-B",
                                                    "zzz-verify-mix-A", "zzz-verify-mix-B" };
                    for (int i = 0; i < corners; ++i) r->assignPlaygroundCorner (i, names[i], e2);
                }
                for (int i = 0; i < 6; ++i) { r->pollParametersForTests(); juce::Thread::sleep (2); }
                juce::AudioBuffer<float> b (2, 512);
                {
                    juce::MidiBuffer m;
                    for (int k = 0; k < notes; ++k)
                        m.addEvent (juce::MidiMessage::noteOn (1, notes >= 32 ? 40 + k : 60 + 4 * k,
                                                               (juce::uint8) 100), 0);
                    b.clear(); r->processBlock (b, m);
                }
                for (int i = 0; i < 60; ++i)   // fades settled, all voices sounding
                { juce::MidiBuffer m; b.clear(); r->processBlock (b, m);
                  if ((i % 12) == 0) r->pollParametersForTests(); }
                const auto t0 = juce::Time::getHighResolutionTicks();
                constexpr int N = 200;
                for (int i = 0; i < N; ++i)
                { juce::MidiBuffer m; b.clear(); r->processBlock (b, m); }
                const double ms = juce::Time::highResolutionTicksToSeconds (
                                      juce::Time::getHighResolutionTicks() - t0) * 1000.0 / N;
                std::printf ("  %-40s %.3f ms/block (%.0f%% of budget)\n", label, ms, ms / 10.667 * 100.0);
                return ms;
            };
            const double e0 = bench (0, 0, 32, "extreme 32 notes: main alone");
            const double e4 = bench (4, 32, 32, "extreme 32 notes: 4 corners @32 voices");
            const double d4 = bench (4, 8, 32, "extreme 32 notes: 4 corners @default 8");
            const double r0 = bench (0, 0, 4, "realistic chord:  main alone");
            const double r4 = bench (4, 8, 4, "realistic chord:  4 corners @default 8");
            juce::ignoreUnused (e0, r0);
            check (d4 < e4 * 0.75,
                   "the VISIBLE voice control genuinely buys back CPU (default 8 well under 32)");
            check (r4 < 10.0,
                   "a realistic mix - 4 corners + main, default voice budget - fits the 10.67 ms block budget");
        }

        fileA.deleteFile();
        fileB.deleteFile();
        check (! fileA.existsAsFile() && ! fileB.existsAsFile(), "run 44's presets are cleaned up");
    }

    //==========================================================================
    // --- run 45: the floating windows are OS-OWNED by the editor's window --------------
    // "al abrir la ventana del playground, a la que muevo el mouse se pone debajo de la
    //  ventana principal del fiblo, arregla eso"
    // Both floating tool windows were plain SIBLING top-level windows, so the first
    // click on the main Fiblo window raised it over the one just opened. The fix is
    // Win32 ownership (GWLP_HWNDPARENT): an owned window stays above its owner, and
    // only its owner - never above other applications. Windows enforces the Z-order
    // FROM this one recorded relationship, so what a build can honestly assert is the
    // record itself, read BACK from the OS - the raise behaviour (click the editor,
    // drag it, type into it, floating window stays visible) is confirmed by eye in
    // the Standalone. Peers are created HIDDEN here: nothing flashes on screen.
#if JUCE_WINDOWS
    {
        std::printf ("\nrun 45: floating windows owned by the editor's top-level window:\n");
        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (48000.0, 256);
        auto ed = std::unique_ptr<FractalAdditiveAudioProcessorEditor> (
                      dynamic_cast<FractalAdditiveAudioProcessorEditor*> (p->createEditor()));
        check (ed != nullptr, "the editor builds");
        if (ed != nullptr)
        {
            // The mechanism needs a native editor window to own against. Hidden: the
            // peer exists (that is all ownership needs) but never appears on screen.
            ed->setVisible (false);
            ed->addToDesktop (0);
            for (const bool playground : { false, true })
            {
                const auto pr = ed->floatingOwnershipProbeForTests (playground);
                std::printf ("  %s window: on desktop %d, OS owner == editor root %d\n",
                             playground ? "playground " : "partial-amp", (int) pr.windowOnDesktop,
                             (int) pr.ownerIsEditorRoot);
                check (pr.windowOnDesktop,
                       playground ? "the playground window gets a native peer"
                                  : "the partial-amp window gets a native peer");
                // Read back from the OS, not from our own bookkeeping: GetWindowLongPtr
                // (GWLP_HWNDPARENT) must name the editor's GA_ROOT window.
                check (pr.ownerIsEditorRoot,
                       playground ? "the OS records the editor's window as the playground's OWNER"
                                  : "the OS records the editor's window as the partial-amp OWNER");
            }
            ed->removeFromDesktop();
        }
    }
#endif

    // --- run 46: SHEPARD MODE - PLAYED, and built from the synth's own spectrum --------
    // v2, a redesign of run 46's original subject. The brief: "quiero que el creador de
    // shepard tones no sea un motor de audio totalmente aparte e incontrolable por midi
    // ... el shepards tone parte del tono pulsado en el teclado y construye un shepards
    // tone usando el espectro del sinte en vez de un sine". The free-running sine drone
    // is gone; the mode now lives inside the voice. So every claim below is measured
    // through the REAL processor, with real MIDI, on the voice that is really sounding:
    //   1. ROOT FOLLOWS THE KEY: every copy sits a whole number of rise intervals from
    //      the PRESSED key's own tuned pitch, and the stack's phase is exactly where the
    //      elapsed time says it should be (the old C3-anchored drone fails this by the
    //      interval between the two keys);
    //   2. SPECTRUM FOLLOWS THE PATCH: a copy carries a whole spectrum, not one sine,
    //      and editing the spectrum changes what the Shepard sounds like;
    //   3. SILENT WRAP: a copy may only appear or disappear with window amplitude below
    //      -80 dB (the raised cosine's edges reach exactly zero);
    //   4. STATIONARY CENTROID: it rises for ever and goes nowhere;
    //   5. NO CLICKS: the largest per-sample step never exceeds what a smooth render of
    //      the partials really sounding can produce (the analytic bound, calibrated from
    //      the block's own RMS);
    //   6. SYNC TIMING: 4 beats at 120 BPM = 2.000 s per rise interval, measured from
    //      the copy roots through a host playhead;
    //   7. THE ENVELOPE APPLIES: releasing the key fades the Shepard out to silence;
    //   8. THE RISE INTERVAL: an OCTAVE by default even under Bohlen-Pierce, and the
    //      tuning's 3/1 period only when the new dropdown asks for it;
    //   9. CPU: the render cost with the mode on against the same note with it off;
    //  10. the TUNING panel still fits, in both languages, and the new dropdown is on
    //      screen exactly when it is a real choice.
    {
        std::printf ("\nrun 46: Shepard MODE - played, from the synth's own spectrum:\n");
        constexpr double ssr  = 48000.0;
        constexpr int    sblk = 256;

        long long pm [shepard::kSlots];
        double    pr [shepard::kSlots], pw[shepard::kSlots];
        int       pcn[shepard::kSlots];

        struct TestPlayHead : juce::AudioPlayHead
        {
            double bpm = 120.0;
            juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
            {
                juce::AudioPlayHead::PositionInfo pi;
                pi.setBpm (bpm);
                pi.setIsPlaying (false); // BPM is offered without the transport rolling
                return pi;
            }
        };
        TestPlayHead ph;

        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (ssr, sblk);
        p->setPlayHead (&ph);
        juce::AudioBuffer<float> b (2, sblk);
        auto set = [&] (const char* id, float plain)
        {
            auto* q = p->apvts.getParameter (id);
            q->setValueNotifyingHost (q->convertTo0to1 (plain));
            // The verifier has no message loop, so the 60 Hz poll that notices a
            // spectrum parameter and asks the builder for a new template has to be
            // pumped by hand - otherwise "editing the spectrum edits the Shepard"
            // would be measuring a template nobody ever rebuilt.
            p->pollParametersForTests();
        };
        juce::MidiBuffer pending;
        auto pump = [&] (double seconds)
        {
            const int nB = juce::jmax (1, (int) std::lround (seconds * ssr / sblk));
            for (int i = 0; i < nB; ++i)
            {
                juce::MidiBuffer m;
                if (i == 0) m.swapWith (pending);
                b.clear();
                p->processBlock (b, m);
            }
            return nB;
        };
        auto keyDown = [&] (int note) { pending.addEvent (juce::MidiMessage::noteOn  (1, note, 0.9f), 0); };
        auto keyUp   = [&] (int note) { pending.addEvent (juce::MidiMessage::noteOff (1, note), 0); };
        auto probe   = [&] { return p->voiceShepardForTests (pm, pr, pw, pcn, shepard::kSlots); };

        // -- the defaults --------------------------------------------------------
        check ((int) p->apvts.getRawParameterValue ("shepMode")->load() == 1,
               "the rate control defaults to SYNC (v1 defaulted to Free)");
        check ((int) p->apvts.getRawParameterValue ("shepRise")->load() == 0,
               "and one rise step defaults to the OCTAVE");
        check (p->apvts.getParameter ("shepLevel") == nullptr,
               "the v1 LEVEL fader is gone (the Shepard IS the voice now - it was a second master volume)");

        // "QUE ESE SEA SU UNICO MODO." There is no free-running path left to reach: with
        // the mode switched ON and NOTHING held, the plugin is silent. v1 would be
        // droning here - that was the entire complaint - so this is the check that says
        // the old engine was removed rather than merely hidden behind the new one.
        {
            p->apvts.getParameter ("shepOn")->setValueNotifyingHost (1.0f);
            pump (2.0);                                  // long past v1's ~30 ms fade-in
            float loudest = 0.0f;
            for (int i = 0; i < 40; ++i)
            {
                juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                loudest = std::max (loudest, b.getMagnitude (0, sblk));
            }
            std::printf ("  Shepard ON, no key held: loudest sample %.2e over %d blocks\n", loudest, 40);
            check (loudest < 1.0e-6f,
                   "with the mode ON and no key down the plugin is SILENT - no drone survives anywhere");
            p->apvts.getParameter ("shepOn")->setValueNotifyingHost (0.0f);
            pump (0.3);
        }

        keyDown (60);
        pump (0.3);
        check (probe() == 0, "mode OFF by default: a played note is an ordinary note, no copies at all");
        check (b.getMagnitude (0, sblk) > 1.0e-4f, "...and that ordinary note sounds");
        keyUp (60); pump (1.5);

        // -- 1. THE ROOT IS THE KEY YOU PRESSED ----------------------------------
        set ("shepOn", 1.0f);
        set ("shepDiv", 9.0f);   // "4" beats -> 2.0000 s per rise interval at 120 BPM
        set ("phaseMode", 0.0f); // Aligned: a note-on is bit-for-bit repeatable
        double bandCentroid[2] = { 0.0, 0.0 };
        for (int ki = 0; ki < 2; ++ki)
        {
            const int key = ki == 0 ? 60 : 67;
            keyDown (key);
            const int nB = pump (0.5);
            const double f0 = p->getNoteFrequency (key);
            const int n = probe();
            // The stack's WEIGHTED CENTRE, in absolute log2 Hz. Recorded per key because
            // "the root is the key you pressed" is only half the rule: the copies sit on
            // the KEY's grid, but the WINDOW that decides which of them sound - and how
            // loudly - is a fixed band of ABSOLUTE frequency that must not move with the
            // key at all. Checked below, across the two keys.
            {
                double sw = 0.0, sx = 0.0;
                for (int k = 0; k < n; ++k) { sw += pw[k]; sx += pw[k] * std::log2 (pr[k]); }
                bandCentroid[ki] = sw > 1.0e-12 ? sx / sw : 0.0;
            }
            // Where the rise really is: the probe reports each block's MIDPOINT, so
            // after nB blocks the grid phase is (nB - 0.5) block-times into a 2 s rise.
            const double gExp  = ((double) nB - 0.5) * ((double) sblk / ssr) / 2.0;
            const double gFrac = gExp - std::floor (gExp);
            double worst = 0.0, worstSp = 0.0;
            std::vector<double> steps;
            for (int k = 0; k < n; ++k)
            {
                const double st = std::log2 (pr[k] / f0);
                steps.push_back (st);
                const double d = std::abs ((st - std::floor (st)) - gFrac);
                worst = std::max (worst, std::min (d, 1.0 - d));
            }
            std::sort (steps.begin(), steps.end());
            for (size_t i = 1; i < steps.size(); ++i)
                worstSp = std::max (worstSp, std::abs (steps[i] - steps[i - 1] - 1.0));
            std::printf ("  key %d (%.4f Hz): %d copies, worst distance off THAT key's grid %.2e intervals,"
                         " worst |spacing - 1 oct| %.2e\n", key, f0, n, worst, worstSp);
            check (n >= 5 && worst < 1.0e-6,
                   "every copy is a whole number of rise intervals from the PRESSED key's tuned pitch");
            check (worstSp < 1.0e-9, "and neighbouring copies are exactly one rise interval apart");
            keyUp (key); pump (1.5);
        }
        // ...and the BAND stayed put. The two keys are a fifth apart and were held for
        // exactly the same time, so a key-anchored stack under a fixed window puts its
        // weighted centre in the same place to the last bit; a stack whose window was
        // built from some OTHER anchor - v1's C3, say - transposes the whole audible
        // band by the interval between the two keys instead. The grid check above cannot
        // see that: it measures each copy against the key it was built from, which is
        // true by construction whatever the window is doing. This is the check that
        // failed when the anchor was put back to C3 (0.585 octaves of drift, a fifth).
        std::printf ("  the sounding BAND across the two keys: %.6f vs %.6f log2 Hz (drift %.2e oct)\n",
                     bandCentroid[0], bandCentroid[1], std::abs (bandCentroid[0] - bandCentroid[1]));
        check (std::abs (bandCentroid[0] - bandCentroid[1]) < 0.02,
               "and the WINDOW did not move with the key - the same absolute band sounds either way");

        // -- 2. THE BUILDING BLOCK IS THE SPECTRUM, NOT A SINE -------------------
        // A copy carries the whole partial list, and editing the spectrum edits the
        // Shepard: same key, same rise, but a different sound - and putting the patch
        // back puts the sound back.
        auto capture = [&] (float baseN, std::vector<float>& out)
        {
            // The new template must be PUBLISHED before the note below is played, or the
            // capture records the previous spectrum and "editing the spectrum edits the
            // Shepard" fails for a reason that has nothing to do with the Shepard. Wait on
            // the generation counter (see awaitTemplateChange); the pump afterwards is only
            // the engine settling on it, with nothing sounding.
            const auto genBefore = p->getTemplateGeneration();
            const bool rebuilds  = (int) p->apvts.getRawParameterValue ("baseN")->load() != (int) baseN;
            set ("baseN", baseN);
            if (rebuilds) awaitTemplateChange (*p, genBefore, "run 46 capture: Base N");
            pump (0.3);
            out.clear();
            keyDown (60);
            for (int i = 0; i < 60; ++i)      // ~0.32 s
            {
                juce::MidiBuffer m;
                if (i == 0) m.swapWith (pending);
                b.clear();
                p->processBlock (b, m);
                const float* L = b.getReadPointer (0);
                for (int s2 = 0; s2 < sblk; ++s2) out.push_back (L[s2]);
            }
            const int n = probe();
            int loudest = 0;
            for (int k = 1; k < n; ++k) if (pw[k] > pw[loudest]) loudest = k;
            const int partials = n > 0 ? pcn[loudest] : 0;
            keyUp (60); pump (1.5);
            return partials;
        };
        {
            std::vector<float> a1, a2, a3;
            const int part1 = capture (1.0f, a1);
            (void)             capture (7.0f, a2);
            const int part3 = capture (1.0f, a3);
            double d12 = 0.0, d13 = 0.0, e1 = 0.0;
            const size_t nS = std::min (a1.size(), std::min (a2.size(), a3.size()));
            for (size_t i = 0; i < nS; ++i)
            {
                d12 += (double) (a1[i] - a2[i]) * (a1[i] - a2[i]);
                d13 += (double) (a1[i] - a3[i]) * (a1[i] - a3[i]);
                e1  += (double) a1[i] * a1[i];
            }
            const double r12 = std::sqrt (d12 / std::max (1.0e-15, e1));
            const double r13 = std::sqrt (d13 / std::max (1.0e-15, e1));
            std::printf ("  loudest copy carries %d partials; Base 1 vs Base 7 differs by %.3f of its own"
                         " energy, Base 1 vs Base 1 again by %.5f\n", part1, r12, r13);
            check (part1 > 4, "a copy is a whole SPECTRUM (a sine-based Shepard would report 1)");
            check (r12 > 0.25, "editing the spectrum edits the Shepard - it is built from the patch");
            check (r13 < 0.05, "and putting the patch back gives the same Shepard again");
        }

        // -- 3/4/5. SILENT WRAP, STATIONARY CENTROID, NO CLICKS ------------------
        // A fast rise (400 ms per octave) so several wraps really happen inside a few
        // seconds of audio, and Scattered phases so the click bound below is calibrated
        // against an incoherent sum rather than the coherent note-on pulse.
        set ("shepMode",  0.0f);      // Free (ms)
        set ("shepTime",  400.0f);
        set ("phaseMode", 1.0f);      // Scattered
        set ("baseN",     1.0f);
        set ("ampSustain", 1.0f);
        // ...and none of those may land in the MIDDLE of the three seconds measured below:
        // a template published mid-note is a real spectrum change, and the click bound and
        // the centroid would both be measuring it rather than the wrap.
        awaitTemplateQuiet (*p);
        pump (0.3);
        {
            keyDown (60);
            pump (0.3);               // past the attack: the envelope is at sustain

            std::map<long long, double> firstW, lastW;
            std::map<long long, bool>   inFirst;
            std::vector<double>         centroids;
            float  prev = 0.0f; bool have = false;
            double worstExcess = 0.0, worstBound = 0.0;
            float  maxJump = 0.0f;
            const int totalBlocks = (int) (3.0 * ssr / sblk);
            for (int blk2 = 0; blk2 < totalBlocks; ++blk2)
            {
                juce::MidiBuffer m;
                b.clear();
                p->processBlock (b, m);
                const int n = probe();
                if (n == 0) continue;

                // The analytic ceiling for a SMOOTH render of exactly the partials that
                // are sounding: |x[k]-x[k-1]| <= sum_j a_j * 2 sin(pi f_j / sr). The
                // overall gain (normalisation x envelope x velocity) is unknown here, so
                // it is calibrated from this block's own RMS, which for an incoherent
                // sum is G * sqrt(sum a_j^2 / 2).
                const auto* T = p->getLiveTemplate();
                double sumA2 = 0.0, sumB = 0.0, num = 0.0, den = 0.0;
                if (T != nullptr)
                    for (int k = 0; k < n; ++k)
                    {
                        const double w  = pw[k];
                        const int    nc = std::min (pcn[k], (int) T->partials.size());
                        for (int i = 0; i < nc; ++i)
                        {
                            const double a = (double) T->partials[(size_t) i].amp * w;
                            const double f = std::min ((double) T->partials[(size_t) i].ratio * pr[k],
                                                       0.4999 * ssr);
                            sumA2 += a * a;
                            sumB  += a * 2.0 * std::sin (juce::MathConstants<double>::pi * f / ssr);
                        }
                        num += w * std::log2 (pr[k]);
                        den += w;
                    }
                const float* L = b.getReadPointer (0);
                double energy = 0.0; float blockJump = 0.0f;
                for (int i = 0; i < sblk; ++i)
                {
                    const float x = L[i];
                    energy += (double) x * x;
                    if (have) blockJump = std::max (blockJump, std::abs (x - prev));
                    prev = x; have = true;
                }
                const double rms = std::sqrt (energy / (double) sblk);
                const double G   = sumA2 > 1.0e-18 ? rms / std::sqrt (0.5 * sumA2) : 0.0;
                const double bound = G * sumB * 1.35 + 1.0e-4;
                maxJump = std::max (maxJump, blockJump);
                worstBound = std::max (worstBound, bound);
                if (bound > 0.0) worstExcess = std::max (worstExcess, (double) blockJump / bound);

                for (int k = 0; k < n; ++k)
                {
                    if (firstW.find (pm[k]) == firstW.end())
                    {
                        firstW[pm[k]]  = pw[k];
                        inFirst[pm[k]] = blk2 == 0;   // the initial stack is not a wrap
                    }
                    lastW[pm[k]] = pw[k];
                }
                if (den > 0.0) centroids.push_back (num / den);
            }

            std::printf ("  click metric: max per-sample jump %.5f, smooth-render bound %.5f"
                         " (worst jump/bound %.2f)\n", maxJump, worstBound, worstExcess);
            check (worstExcess <= 1.0,
                   "no per-sample step exceeds what these partials can smoothly produce (no clicks)");

            const int lastN = probe();
            std::map<long long, bool> stillHere;
            for (int k = 0; k < lastN; ++k) stillHere[pm[k]] = true;
            double worstEntry = 0.0, worstExit = 0.0; int wraps = 0;
            for (const auto& e : firstW)
                if (! inFirst[e.first]) { ++wraps; worstEntry = std::max (worstEntry, e.second); }
            for (const auto& e : lastW)
                if (stillHere.find (e.first) == stillHere.end())
                { ++wraps; worstExit = std::max (worstExit, e.second); }
            std::printf ("  %d wrap events; worst entry window %.2e (%.1f dB), worst exit %.2e (%.1f dB)\n",
                         wraps, worstEntry, 20.0 * std::log10 (juce::jmax (1.0e-12, worstEntry)),
                         worstExit,  20.0 * std::log10 (juce::jmax (1.0e-12, worstExit)));
            check (wraps >= 5, "several copies wrapped during the render (the test saw the event)");
            check (worstEntry < 1.0e-4 && worstExit < 1.0e-4,
                   "every wrap happens below -80 dB: the copy re-entering underneath is SILENT");

            double cLo = 1.0e9, cHi = -1.0e9;
            for (const double c : centroids) { cLo = std::min (cLo, c); cHi = std::max (cHi, c); }
            const size_t nC = centroids.size();
            double headMean = 0.0, tailMean = 0.0;
            const size_t seg = juce::jmax ((size_t) 1, nC / 5);
            for (size_t i = 0; i < seg; ++i) { headMean += centroids[i]; tailMean += centroids[nC - 1 - i]; }
            headMean /= (double) seg; tailMean /= (double) seg;
            std::printf ("  copy centroid (log2 Hz): %.4f .. %.4f (p-p %.4f oct), first fifth %.4f vs"
                         " last fifth %.4f (drift %.4f)\n",
                         cLo, cHi, cHi - cLo, headMean, tailMean, std::abs (tailMean - headMean));
            check (nC > 100 && cHi - cLo < 0.15,
                   "the window's centroid only ripples - the tone rises but goes NOWHERE");
            check (std::abs (tailMean - headMean) < 0.05,
                   "and it does not drift over 7+ intervals (the window never travels with the stack)");

            // -- 7. THE ENVELOPE APPLIES ----------------------------------------
            set ("ampRelease", 0.30f);
            const float held = b.getMagnitude (0, sblk);
            keyUp (60);
            pump (0.12);
            const float mid = b.getMagnitude (0, sblk);
            pump (2.0);
            const float after = b.getMagnitude (0, sblk);
            std::printf ("  release: held %.4f -> 0.12 s after key-up %.4f -> settled %.2e\n",
                         held, mid, after);
            check (held > 1.0e-3f && mid < held * 0.85f,
                   "letting go of the key RELEASES the Shepard through the amp envelope");
            check (after < 1.0e-5f, "and it ends in silence, like any other note");
        }

        // -- 6. SYNC TIMING ------------------------------------------------------
        set ("shepMode", 1.0f);   // Sync (beats)
        set ("shepDiv",  9.0f);   // 4 beats -> 2.0000 s per interval at 120 BPM
        check (! p->shepardSyncHasNoBpm(), "with a host BPM on offer, Sync is really syncing");
        {
            keyDown (60);
            pump (0.5);
            const int n1 = probe();
            std::map<long long, double> r1;
            for (int k = 0; k < n1; ++k) r1[pm[k]] = pr[k];
            const int nB = pump (1.0);
            const double dt = (double) nB * sblk / ssr;
            const int n2 = probe();
            const double R = p->voiceShepardRiseRatioForTests();
            double measured = -1.0;
            for (int k = 0; k < n2 && measured < 0.0; ++k)
            {
                const auto it = r1.find (pm[k]);
                if (it == r1.end()) continue;
                const double ratio = pr[k] / it->second;
                if (ratio > 1.0000001) measured = dt * std::log2 (R) / std::log2 (ratio);
            }
            std::printf ("  sync 4 beats @ 120 BPM: measured %.4f s per rise interval (expected 2.0000)\n",
                         measured);
            check (std::abs (measured - 2.0) < 0.005,
                   "quantized mode: 4 beats at 120 BPM = 2.000 s per rise interval");

            // ...and the honesty flag when the host stops offering a BPM.
            p->setPlayHead (nullptr);
            pump (0.2);
            check (p->shepardSyncHasNoBpm(),
                   "no host BPM: the processor still reports the free-value fallback honestly");
            p->setPlayHead (&ph);
            keyUp (60); pump (2.0);
        }

        // -- 8. THE RISE INTERVAL: an octave unless the user says otherwise ------
        {
            juce::String err;
            check (p->loadScalaText ("! bp\nBohlen-Pierce period test\n 1\n 3/1\n", "bp-period", err),
                   "a 3/1-period scale loads as the active tuning");
            check (shepard::periodDiffersFromOctave (p->getTuningPeriodRatio()),
                   "and the processor agrees its period is not an octave (so the dropdown exists)");

            auto spacingNow = [&]
            {
                const int n = probe();
                std::vector<double> fs (pr, pr + n);
                std::sort (fs.begin(), fs.end());
                double worst = 0.0, first = fs.size() > 1 ? fs[1] / fs[0] : 0.0;
                for (size_t i = 1; i < fs.size(); ++i)
                    worst = std::max (worst, std::abs (fs[i] / fs[i - 1] - first));
                return std::pair<double, double> { first, worst };
            };

            keyDown (60); pump (0.5);
            auto oct = spacingNow();
            std::printf ("  Bohlen-Pierce loaded, dropdown left at OCTAVE: copy spacing %.6f"
                         " (worst deviation %.2e)\n", oct.first, oct.second);
            check (std::abs (oct.first - 2.0) < 1.0e-9 && oct.second < 1.0e-9,
                   "the DEFAULT rise is an octave even under a 3/1 tuning (no silent tuning decision)");
            keyUp (60); pump (2.0);

            set ("shepRise", 1.0f);   // "Period pitch bend"
            keyDown (60); pump (0.5);
            auto per = spacingNow();
            std::printf ("  dropdown set to PERIOD: copy spacing %.6f (worst deviation %.2e)\n",
                         per.first, per.second);
            check (std::abs (per.first - 3.0) < 1.0e-6 && per.second < 1.0e-6,
                   "choosing 'period pitch bend' rises by the TUNING's own 3/1 instead");
            keyUp (60); pump (2.0);
            set ("shepRise", 0.0f);
            check (p->loadScalaText ("! oct\noctave test\n 1\n 2/1\n", "oct-period", err),
                   "and an octave-period scale loads back");
            check (! shepard::periodDiffersFromOctave (p->getTuningPeriodRatio()),
                   "...where there is nothing to choose, so the dropdown has no reason to exist");
        }

        // -- 9. CPU: what the mode actually costs -------------------------------
        {
            // The key MATTERS to this measurement. The fractal levels of a harmonic base
            // are integer ratios, so at C4 only the ~91 integers under Nyquist ever
            // sound however deep the fractal goes: measuring "dense" at C4 would report
            // the cheap patch twice and call it a range. The dense case is therefore
            // played LOW, where the same template really does put thousands of partials
            // under Nyquist - which is the case a user hits and has to be told about.
            auto timeBlocks = [&] (bool on, int key)
            {
                set ("shepOn", on ? 1.0f : 0.0f);
                pump (0.3);
                keyDown (key);
                pump (0.2);                       // steady state before the clock starts
                const int nB = 400;
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < nB; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }
                const double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / (double) nB;
                // The oscillator count the meter reports - the copies' partials added
                // up with the mode on, the plain sub-Nyquist count with it off.
                const int osc = p->getPartialCount();
                keyUp (key); pump (2.0);
                return std::pair<double, int> { ms, osc };
            };
            const double blockMs = 1000.0 * sblk / ssr;
            auto report = [&] (const char* what, int key)
            {
                const auto off = timeBlocks (false, key);
                const auto onv = timeBlocks (true,  key);
                std::printf ("  CPU at %s (key %d), ONE held note, %d-sample blocks (%.2f ms of audio):\n"
                             "    Shepard OFF %.4f ms/block (%6.1f%% of realtime), %5d oscillators\n"
                             "    Shepard ON  %.4f ms/block (%6.1f%% of realtime), %5d oscillators  ->  x%.1f\n",
                             what, key, sblk, blockMs,
                             off.first, 100.0 * off.first / blockMs, off.second,
                             onv.first, 100.0 * onv.first / blockMs, onv.second,
                             onv.first / std::max (1.0e-6, off.first));
                return std::pair<std::pair<double, int>, std::pair<double, int>> { off, onv };
            };

            const auto dflt = report ("the DEFAULT spectrum", 60);
            check (dflt.second.second > dflt.first.second * 2,
                   "Shepard mode really multiplies the partial set (the copies are all sounding)");
            check (dflt.second.first < 0.5 * blockMs,
                   "and at the default spectrum one Shepard voice renders in well under realtime");

            // ...and at a DENSE one, which is where the honest ceiling is. The mode
            // multiplies the partial count by however many copies are inside the window,
            // so a patch that is already expensive becomes several times as expensive:
            // the number below is what a user has to be told, not a number measured on
            // the cheapest possible patch and quoted as if it were the cost.
            // A LEVEL-4 fractal is the heaviest compile in the file, and 400 ms was a
            // guess at it: the CPU numbers below are meaningless if they are measured
            // while the previous template is still the live one.
            {
                const auto genBefore = p->getTemplateGeneration();
                set ("fractalLevel", 4.0f);
                awaitTemplateChange (*p, genBefore, "run 46 CPU: fractal level 4");
            }
            pump (0.6);
            const auto dense = report ("a DENSE spectrum (fractal level 4), played LOW", 24);

            // WHAT THAT SECOND ROW ACTUALLY SAYS, and it is not what you would guess.
            // An ordinary low note pays for every partial the template has under its own
            // Nyquist - 1024 of them at key 24. A SHEPARD note does not: its copies live
            // in a FIXED band of absolute frequency, so the total is
            //     sum over copies of (Nyquist / that copy's root)
            // which for octave spacing converges to about twice the LOWEST copy's count,
            // and the lowest copy sits near 40 Hz whatever key is held. The cost of the
            // mode is therefore bounded by the BAND, not by the patch - which is why a
            // dense low note can cost the SAME with the mode on as without it, and why
            // the multiplication seen at C4 (x6) is the expensive end, not the cheap one.
            // Nothing is being culled to achieve that: every copy is culled at its own
            // Nyquist exactly as an ordinary voice at that pitch would be.
            const double perVoice = std::max (dflt.second.first, dense.second.first);
            std::printf ("    -> the honest ceiling: the copies live in a FIXED band, so ONE Shepard voice\n"
                         "       costs at most ~%.0f%% of one core's realtime budget (%d oscillators at C4,\n"
                         "       %d on the dense low note) - roughly %d simultaneous Shepard voices per core,\n"
                         "       against %d for the same patch played normally.\n",
                         100.0 * perVoice / blockMs, dflt.second.second, dense.second.second,
                         (int) (blockMs / std::max (1.0e-6, perVoice)),
                         (int) (blockMs / std::max (1.0e-6, std::max (dflt.first.first, dense.first.first))));

            // Deliberately NOT a "the mode multiplies" assertion here: at this key it
            // truthfully does not (see above), and an assertion written to the guess
            // instead of to the mechanism would have to be faked to stay green.
            // What MUST hold is the invariant the arrays depend on - no copy may ever
            // carry more than one kStride slot - measured on the copies themselves.
            {
                set ("shepOn", 1.0f);
                keyDown (24); pump (0.4);
                const int n = probe();
                int worstCopy = 0, tot = 0;
                for (int k = 0; k < n; ++k) { worstCopy = std::max (worstCopy, pcn[k]); tot += pcn[k]; }
                std::printf ("    dense low Shepard: %d copies, biggest carries %d partials (slot holds %d),"
                             " %d in total\n", n, worstCopy, shepard::kStride, tot);
                check (n >= 2 && worstCopy > 40,
                       "a dense low note really is several copies, each a whole spectrum");
                check (worstCopy <= shepard::kStride,
                       "...and no copy ever overruns its own oscillator slot");
                check (tot <= spectrum::kMaxPartials,
                       "...nor the voice's arrays in total (the meter's number is a real one)");
                keyUp (24); pump (2.0);
            }
            set ("fractalLevel", 1.0f);
            juce::Thread::sleep (250);
            pump (0.6);
        }

        // -- the nastiest hand-over there is: a DENSE ordinary note, STOLEN by a
        //    Shepard one. The ordinary voice can be carrying thousands of fading
        //    partials; a Shepard copy's bank is one kStride slot wide, so every loop
        //    that walks a copy has to stop there - a stride further is the next copy's
        //    phases, and from the top slot it is past the end of the array entirely.
        {
            set ("shepOn", 0.0f);
            set ("fractalLevel", 3.0f);      // a few thousand partials in the template
            set ("ampRelease", 3.0f);        // the tails are still alive when the steal lands
            juce::Thread::sleep (250);
            pump (0.6);
            for (int k = 0; k < 24; ++k) keyDown (12 + k), pump (0.02);  // ~16 Hz up: huge partial counts
            pump (0.3);
            set ("shepOn", 1.0f);
            float worst = 0.0f; bool finite = true;
            for (int k = 0; k < 40; ++k)     // 40 more notes onto a 32-voice pool: real steals
            {
                keyDown (48 + (k % 24));
                pump (0.03);
                const float mag = b.getMagnitude (0, sblk);
                worst = std::max (worst, mag);
                if (! std::isfinite (mag)) finite = false;
            }
            for (int k = 0; k < 24; ++k) { keyUp (12 + k); keyUp (48 + k); }
            pump (4.0);
            std::printf ("  dense-note steals into Shepard mode: worst block magnitude %.3f, finite %s\n",
                         worst, finite ? "yes" : "NO");
            check (finite && worst < 4.0f,
                   "a Shepard note stealing a dense ordinary voice stays bounded and finite");
            check (b.getMagnitude (0, sblk) < 1.0e-4f, "and everything still releases to silence afterwards");
            set ("fractalLevel", 1.0f);
            set ("ampRelease", 0.30f);
            set ("shepOn", 0.0f);
            juce::Thread::sleep (250);
            pump (0.6);
        }

        // -- 10. the TUNING panel with the new row, in its DENSEST reachable shape --
        // The Shepard row is unchanged in height, but its right-hand slot now holds the
        // rise-interval dropdown - and only when the tuning's period is not an octave.
        // The generic run-35 rules are applied to every control in the box: inside it,
        // usable size, no overlaps - and the bank must still be on screen.
        {
            auto tmp2 = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("fiblo_shep_layout.tun");
            {
                double f0 = 440.0 * std::pow (2.0, -69.0 / 12.0);
                juce::String s ("; stretched, non-repeating\n[Exact Tuning]\nbasefreq=");
                s << juce::String (f0, 10) << "\n";
                for (int n = 0; n < 128; ++n)
                {
                    const double d = (double) (n - 60);
                    s << "note " << n << "=" << juce::String (n * 100.0 + 0.002 * d * std::abs (d), 6) << "\n";
                }
                tmp2.replaceWithText (s);
            }
            auto p2 = std::make_unique<FractalAdditiveAudioProcessor>();
            p2->prepareToPlay (ssr, sblk);
            juce::String err;
            check (p2->loadScalaFile (tmp2, err), "the layout probe's table tuning loads");
            check (p2->setTuningTablePeriod ("3/2", err), "with a user period (the reset button appears too)");
            std::unique_ptr<juce::AudioProcessorEditor> base (p2->createEditor());
            auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
            check (ed != nullptr, "the editor builds");
            for (int sync = 0; ed != nullptr && sync < 2; ++sync)
            {
                if (auto* q = p2->apvts.getParameter ("shepMode"))
                    q->setValueNotifyingHost (q->convertTo0to1 ((float) sync));
                ed->resized();
                for (int lang = 0; lang < 2; ++lang)
                {
                    const auto P = ed->layoutProbeInLanguage (lang == 1);
                    int outside = 0, squashed = 0, clashes = 0;
                    bool hasOn = false, hasMode = false, hasRise = false,
                         hasRate = false, hasBank = false;
                    juce::String first;
                    std::vector<std::pair<juce::String, juce::Rectangle<int>>> tun;
                    for (const auto& c : P.controls)
                    {
                        if (! c.first.startsWith ("tun.")) continue;
                        tun.push_back (c);
                        if (! P.tuning.contains (c.second)) { ++outside; if (first.isEmpty()) first = c.first + " out"; }
                        if (c.second.getHeight() < 12 || c.second.getWidth() < 10)
                        { ++squashed; if (first.isEmpty()) first = c.first + " squashed"; }
                        if (c.first == "tun.shepOn")   hasOn   = true;
                        if (c.first == "tun.shepMode") hasMode = true;
                        if (c.first == "tun.shepRise") hasRise = true;
                        if (c.first == (sync == 1 ? "tun.shepDiv" : "tun.shepTime")) hasRate = true;
                        if (c.first == "tun.bank")     hasBank = true;
                    }
                    for (size_t i = 0; i < tun.size(); ++i)
                        for (size_t j = i + 1; j < tun.size(); ++j)
                            if (tun[i].second.intersects (tun[j].second))
                            { ++clashes; if (first.isEmpty()) first = tun[i].first + " / " + tun[j].first; }
                    std::printf ("  tuning panel [%s, %s]: %d controls, %d out, %d squashed, %d overlaps%s\n",
                                 sync == 1 ? "sync" : "free", lang == 1 ? "ca" : "en",
                                 (int) tun.size(), outside, squashed, clashes,
                                 first.isEmpty() ? "" : (" (" + first + ")").toRawUTF8());
                    {   // the Shepard row's real geometry, so a width that has to change
                        // is a number here rather than a guess in the layout code
                        int rl = 0, rr = 0;
                        for (const auto& c : tun)
                        {
                            if (c.first == "tun.shepOn")   rl = c.second.getX();
                            if (c.first == "tun.shepRise") rr = c.second.getRight();
                        }
                        std::printf ("    shepard row: x %d .. %d (%d px wide)\n", rl, rr, rr - rl);
                    }
                    check (outside + squashed + clashes == 0,
                           "the whole TUNING stack fits, usable and clash-free, table period row included");
                    check (hasOn && hasMode && hasRate,
                           sync == 1 ? "the Shepard row shows the mode toggle, Free/Sync and the BEATS box"
                                     : "the Shepard row shows the mode toggle, Free/Sync and the MS fader");
                    check (hasRise, "and the rise-interval dropdown, because this tuning's period is 3/2");
                    check (hasBank, "and the tuning bank did not pay for it (still on screen)");

                    // ...and the WORDS in that row are readable, not merely inside their
                    // rectangles. Renaming the rate mode from "Sync (beats)" to "Sync
                    // Project Tempo" (and its longer Catalan) makes the label say what it
                    // locks to - but only if the box is wide enough to print it. A
                    // ComboBox neither clips visibly nor resizes: it draws as much as the
                    // cell holds, so every bounds/overlap check above stays green while
                    // the screen reads "Sync Project Te". This is the one check that sees
                    // that, and it sees it in BOTH languages.
                    for (const auto& t : P.comboTexts)
                        if (t.name == "shepMode" || t.name == "shepRise")
                        {
                            const bool fits = t.need <= (float) t.avail;
                            if (! fits || lang == 1)
                                std::printf ("    combo %-9s [%s] \"%s\": needs %.0f px, has %d\n",
                                             t.name.toRawUTF8(), lang == 1 ? "ca" : "en",
                                             t.text.toRawUTF8(), t.need, t.avail);
                            check (fits, t.name == "shepMode"
                                             ? "the rate mode's word fits its box (it is not drawn cut off)"
                                             : "the rise interval's word fits its box too");
                        }
                }
            }
            // ...and it VANISHES the moment the period is an octave again: the timer's
            // layout signature carries the tuning period for exactly this reason.
            if (ed != nullptr)
            {
                check (p2->setTuningTablePeriod ("2/1", err), "the table period is set back to an octave");
                ed->tickTimerForTests();
                bool stillThere = false;
                for (const auto& c : ed->layoutProbeInLanguage (false).controls)
                    if (c.first == "tun.shepRise") stillThere = true;
                check (! stillThere,
                       "with an octave period the rise-interval dropdown is GONE (nothing to choose)");
            }
            tmp2.deleteFile();
        }
    }

    //==========================================================================
    // --- run 47: playground round trips are LOSSLESS, drags are GRADUAL ---------------
    // "si por ejemplo voy adelante y atras entre dos presets uno con menos amp sustain
    //  que el otro, el volumen va bajando hasta silenciarse... los modulators no se
    //  mueven gradualmente si no que saltan de arriba abajo... curiosamente con el modo
    //  mix pasa lo mismo"
    // A morph between two FIXED corners is a pure function of (x, y): parked back on
    // corner A, every parameter must read EXACTLY what it read before the trip - any
    // drift means the blend is compounding on its own output. Everything here goes
    // through the real drivePlayground at its real cadence (one poll per frame, control
    // writes throttled to every other frame), with audio blocks interleaved so the
    // ModEngine's write-back machinery runs exactly as it does live.
    {
        std::printf ("\nrun 47: playground round trips (drain) + gradual drags:\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr double psr = 48000.0;
        constexpr int    pblk = 256;

        auto folder = Proc::getPresetFolder();
        auto fileA = folder.getChildFile ("zzz-verify-rt-A.gsting");
        auto fileB = folder.getChildFile ("zzz-verify-rt-B.gsting");

        auto p = std::make_unique<Proc>();
        p->prepareToPlay (psr, pblk);
        auto sp = [&] (const char* id, float plain)
        {
            if (auto* q = p->apvts.getParameter (id))
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
        };
        auto gp = [&] (const char* id) { return p->apvts.getRawParameterValue (id)->load(); };
        juce::AudioBuffer<float> ab (2, pblk);

        // The user's routing: an envelope rides ampSustain (per-voice target) and an LFO
        // rides masterGain (a GLOBAL target, so ModEngine's raw-atomic write-back and
        // restore paths are both active throughout). Saved into BOTH presets, exactly as
        // a real preset carries its routing.
        p->getModEngine().addTarget (0, "ampSustain");
        p->getModEngine().addTarget (3, "masterGain");

        // Preset A: high sustain, short times.
        sp ("timbreOn", 0.0f); sp ("timbreTemper", 0.0f); sp ("phaseMode", 0.0f);
        sp ("ampAttack", 0.005f); sp ("ampDecay", 0.1f); sp ("ampSustain", 0.9f);
        sp ("ampRelease", 0.05f); sp ("masterGain", 0.0f);
        // Ranges near zero ON PURPOSE: the ModEngine write-back/restore machinery runs
        // for both targets, but the modulation itself must not colour the RMS
        // measurements below (a hot LFO on masterGain made every visit to A sample a
        // different LFO phase, which read as a drain that was not one).
        sp ("modE1A", 0.05f); sp ("modE1Range", 0.05f); sp ("modL1Range", 0.0f);
        sp ("filterCutoff", 4000.0f); sp ("baseN", 1.0f); sp ("curveSlope", 1.5f);
        p->pollParametersForTests();
        check (p->savePreset ("zzz-verify-rt-A").isNotEmpty(), "corner preset A saved (ampSustain 0.9)");

        // Preset B: low sustain, long times, the CUSTOM engine, a darker filter and a
        // different master gain - the report's real shape ("uno con menos amp sustain
        // que el otro... o por los diferentes filtros de spectrum").
        {
            juce::String terr;
            check (p->loadTimbreText ("5/4 3/2 2", terr), "corner preset B gets a custom spectrum");
        }
        sp ("ampAttack", 0.5f); sp ("ampDecay", 1.0f); sp ("ampSustain", 0.2f);
        sp ("ampRelease", 0.8f); sp ("masterGain", -6.0f);
        sp ("modE1A", 2.0f); sp ("modE1Range", -0.05f); sp ("modL1Range", 0.0f);
        sp ("filterCutoff", 9000.0f); sp ("curveSlope", 2.5f);
        p->pollParametersForTests();
        check (p->savePreset ("zzz-verify-rt-B").isNotEmpty(), "corner preset B saved (ampSustain 0.2)");

        juce::String err;
        check (p->assignPlaygroundCorner (0, "zzz-verify-rt-A", err), "A on corner 0");
        check (p->assignPlaygroundCorner (1, "zzz-verify-rt-B", err), "B on corner 1");

        // One poll frame: the 60 Hz tick plus one audio block, which is what a running
        // instance actually interleaves.
        auto frame = [&]
        {
            p->pollParametersForTests();
            juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m);
        };
        auto park = [&] (float x, float y, int frames = 8)
        {
            sp ("pgX", x); sp ("pgY", y);
            for (int i = 0; i < frames; ++i) frame();
        };

        // RMS of a freshly-struck note, measured well into its sustain. The worker
        // blend is given time to settle first so both measurements see the same
        // published spectrum path.
        auto noteRms = [&]
        {
            juce::Thread::sleep (300);
            const int holdBlocks = (int) (0.6 * psr / pblk);
            const int from = (int) (0.3 * psr / pblk);
            double sum = 0.0; long n = 0;
            for (int b = 0; b < holdBlocks; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                ab.clear(); p->processBlock (ab, m);
                if (b >= from)
                {
                    const float* L = ab.getReadPointer (0);
                    for (int i = 0; i < pblk; ++i) { sum += (double) L[i] * L[i]; ++n; }
                }
            }
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
              ab.clear(); p->processBlock (ab, m); }
            for (int b = 0; b < (int) (1.5 * psr / pblk); ++b)   // let the tail die completely
            { juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); }
            return std::sqrt (sum / (double) juce::jmax (1L, n));
        };

        // A drag leg at real cadence: 25 frames each way, 0.04 per frame (well under
        // the 0.15 jump threshold - this is a DRAG, not automation cutting across).
        auto dragTo = [&] (float x0, float x1)
        {
            for (int i = 1; i <= 25; ++i)
            {
                sp ("pgX", x0 + (x1 - x0) * (float) i / 25.0f);
                frame();
            }
        };

        // ---- MORPH mode: park at A, snapshot, ten round trips, compare -------------
        park (0.0f, 0.0f, 12);
        const auto& all = p->getParameters();
        std::vector<float> snap ((size_t) all.size(), -1.0f);
        for (int i = 0; i < all.size(); ++i)
            if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
                snap[(size_t) i] = q->getValue();
        const double rms0 = noteRms();
        check (rms0 > 1.0e-4, "parked on corner A, a note sounds at all");

        for (int trip = 0; trip < 10; ++trip) { dragTo (0.0f, 1.0f); dragTo (1.0f, 0.0f); }
        park (0.0f, 0.0f, 12);   // settle: pgParamsPending must flush the last write

        {
            int drifted = 0;
            float worst = 0.0f; juce::String worstId;
            for (int i = 0; i < all.size(); ++i)
                if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
                {
                    if (q->paramID == "pgX" || q->paramID == "pgY") continue; // moved on purpose
                    const float d = std::abs (q->getValue() - snap[(size_t) i]);
                    if (d > 2.0e-4f) ++drifted;
                    if (d > worst) { worst = d; worstId = q->paramID; }
                }
            std::printf ("  morph: 10 round trips A->B->A -> %d parameters drifted "
                         "(worst %s by %.6f normalised)\n",
                         drifted, worstId.toRawUTF8(), worst);
            check (drifted == 0,
                   "after ten round trips every parameter is EXACTLY back at corner A");
        }
        {
            const double rms1 = noteRms();
            std::printf ("  morph: note RMS at A before %.5f, after trips %.5f (ratio %.3f)\n",
                         rms0, rms1, rms0 > 0.0 ? rms1 / rms0 : 0.0);
            check (rms1 > rms0 * 0.9 && rms1 < rms0 * 1.1,
                   "the note at corner A is as loud after the trips as before - no volume drain");
        }

        // ---- a SLOW SWEEP writes gradual intermediates (continuous params) ---------
        {
            std::vector<float> sus, e1a;
            for (int i = 1; i <= 50; ++i)
            {
                sp ("pgX", (float) i * 0.02f);
                frame();
                sus.push_back (gp ("ampSustain"));
                e1a.push_back (gp ("modE1A"));
            }
            // The drag has STOPPED: the pgParamsPending flush owes the controls one
            // last write on the next tick. Sample after it, or the assert below would
            // fail on the one-frame lag that is by design (the 30 Hz throttle).
            for (int i = 0; i < 3; ++i) frame();
            sus.push_back (gp ("ampSustain"));
            e1a.push_back (gp ("modE1A"));
            float worstSusStep = 0.0f, worstE1aFac = 1.0f;
            bool susMono = true, e1aMono = true;
            for (size_t i = 1; i < sus.size(); ++i)
            {
                const float dS = sus[i] - sus[i - 1];
                if (dS > 1.0e-4f) susMono = false;                 // A 0.9 -> B 0.2: falls
                worstSusStep = std::max (worstSusStep, std::abs (dS));
                if (e1a[i] < e1a[i - 1] - 1.0e-5f) e1aMono = false; // A 0.05 -> B 2.0: rises
                if (e1a[i - 1] > 0.0f)
                    worstE1aFac = std::max (worstE1aFac, e1a[i] / e1a[i - 1]);
            }
            std::printf ("  sweep A->B: ampSustain %.3f..%.3f worst step %.4f; "
                         "modE1A %.4f..%.4f worst factor %.3f\n",
                         sus.front(), sus.back(), worstSusStep,
                         e1a.front(), e1a.back(), worstE1aFac);
            check (susMono, "ampSustain falls monotonically across the whole sweep - no jumps back");
            check (worstSusStep <= 0.06f,
                   "no ampSustain step exceeds twice the per-write bound (gradual, not stairs)");
            check (std::abs (sus.back() - 0.2f) < 5.0e-3f, "and it arrives exactly at B's 0.2");
            check (e1aMono, "a modulator TIME (modE1A) also moves monotonically");
            check (worstE1aFac <= 1.6f,
                   "and in its own log space: no write jumps by more than the per-write ratio bound");
        }
        park (0.0f, 0.0f, 12);

        // ---- a note HELD THROUGH continuous round trips must not fade away ---------
        // The report verbatim: "voy adelante y atras entre dos presets... el volumen va
        // bajando hasta silenciarse". One note is held through the whole ride; every
        // return to corner A measures how loud that same note still is. ~16 ms of audio
        // is rendered per frame (three 256-sample blocks), the real interleave.
        auto heldTripRms = [&] (bool morphOn, bool mixMode, int framesPerLeg, int trips,
                                const char* label)
        {
            sp ("morph", morphOn ? 1.0f : 0.0f);
            sp ("pgMode", mixMode ? 1.0f : 0.0f);
            park (0.0f, 0.0f, 12);
            juce::Thread::sleep (250);
            for (int i = 0; i < 8; ++i) frame();      // mix engage / blend settle
            auto audioFrame = [&] (double& rmsOut)
            {
                p->pollParametersForTests();
                double sum = 0.0;
                for (int b = 0; b < 3; ++b)
                {
                    juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m);
                    const float* L = ab.getReadPointer (0);
                    for (int i = 0; i < pblk; ++i) sum += (double) L[i] * L[i];
                }
                rmsOut = std::sqrt (sum / (double) (3 * pblk));
            };
            // Strike the note and let it settle onto A's sustain.
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
              ab.clear(); p->processBlock (ab, m); }
            double r = 0.0;
            for (int i = 0; i < 40; ++i) audioFrame (r);   // ~0.65 s: past attack+decay
            std::vector<double> atA;
            auto dwellRms = [&]
            {
                double acc = 0.0;
                for (int i = 0; i < 12; ++i) { audioFrame (r); if (i >= 4) acc += r * r; }
                return std::sqrt (acc / 8.0);
            };
            atA.push_back (dwellRms());
            for (int trip = 0; trip < trips; ++trip)
            {
                for (int i = 1; i <= framesPerLeg; ++i)
                { sp ("pgX", (float) i / (float) framesPerLeg); audioFrame (r); }
                for (int i = framesPerLeg - 1; i >= 0; --i)
                { sp ("pgX", (float) i / (float) framesPerLeg); audioFrame (r); }
                atA.push_back (dwellRms());
            }
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
              ab.clear(); p->processBlock (ab, m); }
            for (int b = 0; b < (int) (1.5 * psr / pblk); ++b)
            { juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); }
            std::printf ("  held note [%s]: RMS at A per visit:", label);
            for (const double v : atA) std::printf (" %.4f", v);
            std::printf ("\n");
            const double first = atA.front(), last = atA.back();
            double lowest = first;
            for (const double v : atA) lowest = std::min (lowest, v);
            check (first > 1.0e-4, "the held note sounds at the first visit to A");
            check (last > first * 0.7 && last < first * 1.4,
                   "after all the round trips the SAME held note is as loud at A as it started");
            check (lowest > first * 0.5,
                   "and no visit to A ever found it faded away (no progressive drain)");
        };
        heldTripRms (false, false, 25, 6, "morph off, slow drags");
        heldTripRms (true,  false, 25, 6, "morph on, slow drags");
        heldTripRms (false, false, 5,  6, "morph off, FAST drags (jump path)");
        heldTripRms (false, true,  25, 6, "MIX mode, slow drags");
        sp ("pgMode", 0.0f); sp ("morph", 0.0f);
        park (0.0f, 0.0f, 12);

        // ---- the DRAG's frequency glide: partials BEND with Morph on ---------------
        // "el modo morph no produce pitch bend en los parciales, cosa que es lo que
        //  deberia hacer, es lo que hace en el resto de la app". Corner A is harmonic
        //  (rank 1 sounds ratio 2), corner B is the custom scale (rank 1 is 5/4): with
        //  Morph ON a drag must carry rank 1 THROUGH values that exist in neither
        //  corner - a real per-partial pitch bend - and with Morph OFF it must keep
        //  snapping between corner ratios exactly as before (the 50 ms fade carries
        //  the amplitudes; run 26's metric below checks neither mode clicks).
        {
            auto glideSamples = [&] (bool morphOn)
            {
                sp ("morph", morphOn ? 1.0f : 0.0f); sp ("morphTime", 0.4f);
                park (0.0f, 0.0f, 12);
                juce::Thread::sleep (250);
                for (int i = 0; i < 4; ++i) frame();
                { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                  ab.clear(); p->processBlock (ab, m); }
                for (int i = 0; i < 30; ++i) frame();
                std::vector<double> r1;
                for (int i = 1; i <= 50; ++i)
                {
                    sp ("pgX", (float) i * 0.02f);
                    p->pollParametersForTests();
                    double rr[4] = {};
                    for (int b = 0; b < 3; ++b)
                    { juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); }
                    if (p->voiceCurRatiosForTests (rr, 4) >= 2)
                        r1.push_back (rr[1]);
                }
                for (int i = 0; i < 30; ++i) frame();  // settle at B (the final landing bump)
                double rrEnd[4] = {};
                const int nEnd = p->voiceCurRatiosForTests (rrEnd, 4);
                { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                  ab.clear(); p->processBlock (ab, m); }
                for (int b = 0; b < (int) (1.5 * psr / pblk); ++b)
                { juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); }
                park (0.0f, 0.0f, 12);
                return std::make_pair (r1, nEnd >= 2 ? rrEnd[1] : 0.0);
            };

            const auto isCornerRatio = [] (double r)
            {
                for (const double c : { 1.25, 1.5, 2.0, 2.5 })   // both corners' low ranks
                    if (std::abs (r - c) < 2.0e-3) return true;
                return false;
            };
            auto countBending = [&] (const std::vector<double>& r1)
            {
                int n = 0;
                for (const double r : r1)
                    if (r > 1.27 && r < 1.98 && ! isCornerRatio (r)) ++n;
                return n;
            };

            const auto on  = glideSamples (true);
            const auto off = glideSamples (false);
            std::printf ("  glide probe rank1: morph ON %d/%d frames at in-between ratios "
                         "(landed %.4f), morph OFF %d/%d (landed %.4f)\n",
                         countBending (on.first), (int) on.first.size(), on.second,
                         countBending (off.first), (int) off.first.size(), off.second);
            check ((int) on.first.size() >= 40, "the glide probe really watched a sounding voice");
            check (countBending (on.first) >= 4,
                   "morph ON: a drag BENDS rank 1 through ratios that exist in neither corner (pitch glide)");
            check (std::abs (on.second - 1.25) < 2.0e-3,
                   "...and it LANDS exactly on corner B's 5/4 after the drag stops (the settle bump)");
            check (countBending (off.first) == 0,
                   "morph OFF: ratios only ever snap between corner values - the fade, not a glide");
        }

        // ---- run 26's metric, applied to the drag itself: neither mode clicks -------
        {
            auto dragJump = [&] (bool morphOn, bool drag)
            {
                sp ("morph", morphOn ? 1.0f : 0.0f);
                park (0.0f, 0.0f, 12);
                juce::Thread::sleep (250);
                { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                  ab.clear(); p->processBlock (ab, m); }
                for (int i = 0; i < 30; ++i) frame();
                float maxJump = 0.0f, peak = 0.0f, prev = 0.0f;
                bool have = false;
                for (int i = 1; i <= 100; ++i)
                {
                    if (drag) // one full round trip, at drag speed
                        sp ("pgX", i <= 50 ? (float) i * 0.02f : (float) (100 - i) * 0.02f);
                    p->pollParametersForTests();
                    for (int b = 0; b < 3; ++b)
                    {
                        juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m);
                        const float* L = ab.getReadPointer (0);
                        for (int k = 0; k < pblk; ++k)
                        {
                            const float x = L[k];
                            peak = std::max (peak, std::abs (x));
                            if (have) maxJump = std::max (maxJump, std::abs (x - prev));
                            prev = x; have = true;
                        }
                    }
                }
                { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                  ab.clear(); p->processBlock (ab, m); }
                for (int b = 0; b < (int) (1.5 * psr / pblk); ++b)
                { juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); }
                park (0.0f, 0.0f, 8);
                return peak > 0.0f ? maxJump / peak : 0.0f;
            };
            const float ctrl    = dragJump (false, false);   // parked: the note's own slope
            const float offDrag = dragJump (false, true);
            const float onDrag  = dragJump (true,  true);
            std::printf ("  drag discontinuity (jump/peak): parked %.3f, morph OFF drag %.3f, "
                         "morph ON drag %.3f\n", ctrl, offDrag, onDrag);
            check (ctrl > 0.0f, "the control case actually made sound");
            check (offDrag <= ctrl * 2.5f + 0.02f,
                   "a morph-OFF drag round trip does not click (run 26's bound)");
            check (onDrag <= ctrl * 2.5f + 0.02f,
                   "a morph-ON gliding drag round trip does not click either");
            sp ("morph", 0.0f);
        }

        // ---- MIX mode: same trips, nothing may touch the live patch, RMS holds -----
        {
            sp ("pgMode", 1.0f);
            park (0.0f, 0.0f, 12);
            juce::Thread::sleep (300);            // corner engines build + publish
            for (int i = 0; i < 8; ++i) frame();  // engage crossfade fully in
            const double mix0 = noteRms();
            check (p->isPlaygroundMixEngaged(), "MIX mode engages over the corners");
            check (mix0 > 1.0e-4, "a note sounds through the mix at corner A");

            std::vector<float> snapMix ((size_t) all.size(), -1.0f);
            for (int i = 0; i < all.size(); ++i)
                if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
                    snapMix[(size_t) i] = q->getValue();

            for (int trip = 0; trip < 10; ++trip) { dragTo (0.0f, 1.0f); dragTo (1.0f, 0.0f); }
            park (0.0f, 0.0f, 12);

            int touched = 0; juce::String touchedId;
            for (int i = 0; i < all.size(); ++i)
                if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
                {
                    if (q->paramID == "pgX" || q->paramID == "pgY") continue;
                    if (std::abs (q->getValue() - snapMix[(size_t) i]) > 2.0e-4f)
                    { ++touched; if (touchedId.isEmpty()) touchedId = q->paramID; }
                }
            std::printf ("  mix: 10 round trips touched %d live parameters%s\n", touched,
                         touchedId.isEmpty() ? "" : (" (first: " + touchedId + ")").toRawUTF8());
            check (touched == 0, "MIX round trips write NO live parameter at all");

            const double mix1 = noteRms();
            std::printf ("  mix: note RMS at A before %.5f, after trips %.5f (ratio %.3f)\n",
                         mix0, mix1, mix0 > 0.0 ? mix1 / mix0 : 0.0);
            check (mix1 > mix0 * 0.9 && mix1 < mix0 * 1.1,
                   "the mix at corner A is as loud after the trips as before - no volume drain");
            sp ("pgMode", 0.0f);
            for (int i = 0; i < 8; ++i) frame();
        }

        p->assignPlaygroundCorner (0, {}, err);
        p->assignPlaygroundCorner (1, {}, err);
        for (int m = 0; m < ModEngine::kMods; ++m) p->getModEngine().unmapAll (m);
        fileA.deleteFile();
        fileB.deleteFile();
        check (! fileA.existsAsFile() && ! fileB.existsAsFile(), "run 47's presets are cleaned up");
    }

    // --- run 48: MONOPHONIC GLIDE - one voice, and a real bend between two tuned pitches -
    // "al activarlo el sinte no puede producir acordes pero al pulsar una nota y luego
    //  otra se hace pitchbend de una a la otra en un tiempo especificado manualmente en ms"
    //
    // Everything here is measured on the RENDERED AUDIO wherever it can be, because the
    // claim is about what is heard: the fundamental is estimated by autocorrelation over
    // a 30 ms window (robust with a harmonic spectrum, where a zero-crossing count reads
    // the brightest partial instead of the pitch), and the voice's own probe is printed
    // beside it so a failure names WHICH half disagrees.
    {
        std::printf ("\nmonophonic glide: one voice, and a bend between two tuned pitches:\n");
        constexpr double sr8 = 48000.0;
        constexpr int    b8  = 256;
        auto p = std::make_unique<FractalAdditiveAudioProcessor>();
        p->prepareToPlay (sr8, b8);
        auto sp = [&p] (const char* id, float v)
        { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
          p->pollParametersForTests(); };

        // A patch whose pitch is easy to read and whose ENVELOPE is easy to catch
        // retriggering: 1 ms attack, 30 ms decay to a 0.2 sustain, so a re-attack is a
        // five-fold RMS bump that no smoothing can hide.
        sp ("morph", 0.0f);
        sp ("ampAttack", 0.001f); sp ("ampDecay", 0.030f);
        sp ("ampSustain", 0.2f);  sp ("ampRelease", 0.010f);
        sp ("phaseMode", 0.0f);
        sp ("filterCutoff", 9600.0f);   // wide open in cents: the filter must not colour f0
        sp ("pitchEnvAmount", 0.0f);

        juce::AudioBuffer<float> ab (2, b8);
        std::vector<float> x;
        int clk = 0;
        // Runs `blocks` blocks, optionally with MIDI on the first one, recording the mix.
        auto pump = [&] (int blocks, const juce::MidiBuffer& first)
        {
            for (int i = 0; i < blocks; ++i)
            {
                juce::MidiBuffer m;
                if (i == 0) m = first;
                ab.clear();
                p->processBlock (ab, m);
                const float* r = ab.getReadPointer (0);
                for (int k = 0; k < b8; ++k) x.push_back (r[k]);
                clk += b8;
            }
        };
        auto on  = [] (int k) { juce::MidiBuffer m;
                                m.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0); return m; };
        auto off = [] (int k) { juce::MidiBuffer m;
                                m.addEvent (juce::MidiMessage::noteOff (1, k), 0); return m; };

        // f0 by NORMALISED autocorrelation over `win` samples, lags from 2 kHz down to
        // 50 Hz, with the octave rule that makes it usable on a harmonic spectrum at all:
        // a periodic signal correlates just as well at TWICE its period, and a plain
        // "largest correlation wins" therefore reports f0/2 about half the time (it did,
        // for every reading here, until this was added). So the answer is the FIRST local
        // peak that reaches 85% of the global one - the shortest period that explains the
        // waveform - refined by a parabolic fit through its two neighbours.
        std::vector<double> ac;
        auto f0At = [&] (size_t at, int win) -> double
        {
            const int loLag = (int) (sr8 / 2000.0), hiLag = (int) (sr8 / 50.0);
            if (at + (size_t) (win + hiLag) + 2 >= x.size()) return 0.0;
            ac.assign ((size_t) hiLag + 2, 0.0);
            double best = 0.0;
            for (int lag = loLag; lag <= hiLag; ++lag)
            {
                double s = 0.0, e0 = 0.0, e1 = 0.0;
                for (int i = 0; i < win; ++i)
                {
                    const double a = x[at + (size_t) i], c = x[at + (size_t) (i + lag)];
                    s += a * c; e0 += a * a; e1 += c * c;
                }
                const double n = std::sqrt (e0 * e1);
                ac[(size_t) lag] = n > 1.0e-12 ? s / n : 0.0;
                best = std::max (best, ac[(size_t) lag]);
            }
            if (best <= 0.2) return 0.0;                    // nothing periodic here
            int pick = 0;
            for (int lag = loLag + 1; lag < hiLag; ++lag)
                if (ac[(size_t) lag] >= 0.85 * best
                    && ac[(size_t) lag] >= ac[(size_t) (lag - 1)]
                    && ac[(size_t) lag] >= ac[(size_t) (lag + 1)])
                { pick = lag; break; }
            if (pick <= loLag || pick >= hiLag) return 0.0;
            const double ym = ac[(size_t) (pick - 1)], y0 = ac[(size_t) pick], yp = ac[(size_t) (pick + 1)];
            const double den = ym - 2.0 * y0 + yp;
            const double d   = std::abs (den) > 1.0e-12 ? 0.5 * (ym - yp) / den : 0.0;
            return sr8 / ((double) pick + juce::jlimit (-1.0, 1.0, d));
        };
        auto rmsAt = [&] (size_t at, int win) -> double
        {
            if (at + (size_t) win >= x.size()) return 0.0;
            double s2 = 0.0;
            for (int i = 0; i < win; ++i) { const double v = x[at + (size_t) i]; s2 += v * v; }
            return std::sqrt (s2 / (double) win);
        };

        const double fLo = p->getNoteFrequency (60);
        const double fHi = p->getNoteFrequency (67);   // a fifth: two clearly separate pitches
        constexpr double glideMs = 200.0;

        //-- (a) the CONTROL: mono OFF is a jump, and a second key adds a voice ----------
        sp ("monoGlide", 0.0f);
        sp ("glideTime", (float) glideMs);
        check (p->synthVoiceCountForTests() == 32, "with the mode off the pool is the full 32 voices");
        x.clear(); clk = 0;
        pump (40, on (60));
        const size_t polyA = x.size();
        pump (40, on (67));
        const int polyActive = p->activeVoiceCountForTests();
        pump (10, {});
        const double polyF0 = f0At (polyA + (size_t) (0.010 * sr8), 1440);
        pump (4, off (67)); pump (4, off (60)); pump (40, {});
        std::printf ("  poly control: %d voices sounding after the second key, f0 10 ms after it %.1f Hz\n",
                     polyActive, polyF0);
        check (polyActive == 2, "polyphonic: a second key really does add a second voice");

        //-- (b) MONO ON: one voice, and the pitch BENDS ---------------------------------
        sp ("monoGlide", 1.0f);
        check (p->synthVoiceCountForTests() == 1,
               "switching Monophonic glide on drops the pool to ONE voice (no chords possible)");

        x.clear(); clk = 0;
        pump (60, on (60));                     // settle on the low note
        const size_t T = x.size();              // the takeover happens on the next block
        pump (2, on (67));
        const int monoActive = p->activeVoiceCountForTests();
        pump (160, {});                         // ~850 ms: the glide plus a settled tail
        const size_t T2 = x.size();
        pump (2, off (67));                     // ...and back to the key still held
        pump (120, {});
        const size_t T3 = x.size();
        pump (2, off (60)); pump (60, {});

        check (monoActive == 1, "mono: a second simultaneous key adds NO voice - it takes the one over");

        const int    win  = (int) (0.030 * sr8);      // 30 ms analysis window
        const double fBefore = f0At (T - (size_t) (0.040 * sr8), win);
        const double fAfter  = f0At (T + (size_t) ((glideMs + 120.0) * 0.001 * sr8), win);
        // Linear in log2 -> the halfway sample sits on the GEOMETRIC mean. The window is
        // centred on it, so the reading is the pitch at the midpoint and not an average
        // of one side.
        const double fMidT   = f0At (T + (size_t) ((0.5 * glideMs) * 0.001 * sr8) - (size_t) (win / 2), win);
        const double fMidExp = std::sqrt (fLo * fHi);
        std::printf ("  keys 60 -> 67: before %.2f Hz (tuned %.2f), after %.2f Hz (tuned %.2f), "
                     "midpoint %.2f Hz (geometric mean %.2f)\n",
                     fBefore, fLo, fAfter, fHi, fMidT, fMidExp);
        check (std::abs (1200.0 * std::log2 (fBefore / fLo)) < 12.0,
               "the held note sounds at its own tuned frequency before the takeover");
        check (std::abs (1200.0 * std::log2 (fAfter / fHi)) < 12.0,
               "and at the NEW note's tuned frequency once the glide time has passed");
        check (std::abs (1200.0 * std::log2 (fMidT / fMidExp)) < 60.0,
               "halfway through the glide the pitch is halfway between them (log2-linear portamento)");

        // THE SWEEP IS CONTINUOUS, and this is the assertion a jump has to fail. Sampled
        // every 10 ms across the glide: every reading must be inside the interval, the
        // sequence must climb, and no single 10 ms step may carry more than a quarter of
        // the whole interval (a snap carries all of it in one step).
        std::vector<double> sweep;
        for (double t = 0.0; t <= glideMs; t += 10.0)
            sweep.push_back (f0At (T + (size_t) (t * 0.001 * sr8), win));
        // THE COUNT THAT MATTERS IS "strictly between", not "small steps". A restrike
        // reaches the new pitch BEFORE the first 10 ms sample, so every step it takes
        // afterwards is zero and every reading is (just) inside the closed interval: a
        // sabotage that removed the glide entirely passed "inside", "non-falling" AND
        // "biggest step" and was caught by one assertion out of four. Readings strictly
        // between the endpoints are what a jump cannot fake - it has none at all.
        int    rising = 0, between = 0;
        double biggestStep = 0.0;
        const double totalC = 1200.0 * std::log2 (fHi / fLo);
        for (size_t i = 0; i < sweep.size(); ++i)
        {
            if (sweep[i] > fLo * 1.01 && sweep[i] < fHi * 0.99) ++between;
            if (i > 0)
            {
                if (sweep[i] >= sweep[i - 1] * 0.995) ++rising;
                biggestStep = std::max (biggestStep, 1200.0 * std::log2 (std::max (1.0e-9, sweep[i] / sweep[i - 1])));
            }
        }
        std::printf ("  sweep (10 ms apart, Hz):");
        for (const double v : sweep) std::printf (" %.0f", v);
        std::printf ("\n  strictly between the two pitches %d/%d, non-falling %d/%d, "
                     "biggest 10 ms step %.0f c of %.0f c, starts %.0f c above the old note\n",
                     between, (int) sweep.size(), rising, (int) sweep.size() - 1, biggestStep, totalC,
                     sweep.empty() ? -1.0 : 1200.0 * std::log2 (std::max (1.0e-9, sweep[0] / fLo)));
        check (between >= (int) sweep.size() - 3,
               "the glide really TRAVELS: all but the endpoints lie strictly between the two tuned frequencies");
        check (! sweep.empty() && std::abs (1200.0 * std::log2 (std::max (1.0e-9, sweep[0] / fLo))) < 150.0,
               "and it starts from the OLD note's pitch - it has not already arrived when the key goes down");
        check (rising == (int) sweep.size() - 1, "and the sweep never goes backwards");
        check (biggestStep < 0.25 * totalC,
               "no single 10 ms step carries a quarter of the interval - it is a bend, not a jump");

        //-- (c) the legato RETURN: release the top key, glide back to the one held ------
        const double fBack = f0At (T3 - (size_t) (0.050 * sr8), win);
        std::printf ("  released the new key with the old one still down: %.2f Hz (tuned %.2f)\n",
                     fBack, fLo);
        check (std::abs (1200.0 * std::log2 (fBack / fLo)) < 12.0,
               "releasing the key you are on returns the pitch to the key still held");

        //-- (d) LEGATO DOES NOT RETRIGGER, but a note from silence DOES ------------------
        // The patch decays to a 0.2 sustain in 30 ms, so a re-attack is a large, brief
        // RMS bump. Measured as the loudest 5 ms window in the 40 ms after the event
        // against the settled level just before it.
        auto bumpAround = [&] (size_t at) -> double
        {
            const int w = (int) (0.005 * sr8);
            const double before = rmsAt (at - (size_t) (0.020 * sr8), w);
            double peak = 0.0;
            for (size_t s = at; s < at + (size_t) (0.040 * sr8); s += (size_t) (0.001 * sr8))
                peak = std::max (peak, rmsAt (s, w));
            return before > 1.0e-9 ? peak / before : 0.0;
        };
        const double legatoBump = bumpAround (T);
        const double returnBump = bumpAround (T2);
        // ...and the same measurement on a note struck from SILENCE, which must retrigger.
        x.clear(); clk = 0;
        pump (40, on (60));
        pump (4, off (60));
        pump (60, {});                       // the release finishes: nothing is held
        const size_t coldAt = x.size();
        pump (60, on (64));
        pump (4, off (64)); pump (40, {});
        const double coldPeak = [&]
        {
            double pk = 0.0;
            for (size_t s = coldAt; s < coldAt + (size_t) (0.040 * sr8); s += (size_t) (0.001 * sr8))
                pk = std::max (pk, rmsAt (s, (int) (0.005 * sr8)));
            return pk;
        }();
        const double coldSettled = rmsAt (coldAt + (size_t) (0.200 * sr8), (int) (0.005 * sr8));
        const double coldBump = coldSettled > 1.0e-9 ? coldPeak / coldSettled : 0.0;
        std::printf ("  envelope bump: legato takeover x%.2f, legato return x%.2f, "
                     "cold note from silence x%.2f (attack/sustain = x%.1f)\n",
                     legatoBump, returnBump, coldBump, 1.0 / 0.2);
        check (legatoBump < 1.6, "a legato takeover does NOT retrigger the amplitude envelope");
        check (returnBump < 1.6, "and neither does the legato return to a still-held key");
        check (coldBump > 2.5,
               "...while a note struck after every key came up DOES retrigger it (and starts at its own pitch)");

        //-- (e) switching the mode off gives the polyphony back --------------------------
        sp ("monoGlide", 0.0f);
        check (p->synthVoiceCountForTests() == 32, "switching it off restores the full voice pool");
        x.clear(); clk = 0;
        pump (30, on (60));
        pump (30, on (64));
        const int backActive = p->activeVoiceCountForTests();
        pump (2, off (60)); pump (2, off (64)); pump (40, {});
        std::printf ("  after switching off: %d voices sounding on a two-key chord\n", backActive);
        check (backActive == 2, "...and a chord really is polyphonic again");

        //-- (f) MIX MODE: the rule reaches every sub-engine, or nothing here means anything
        {
            const auto dir = FractalAdditiveAudioProcessor::getPresetFolder();
            dir.createDirectory();
            juce::String err;
            sp ("baseN", 2.0f);  const auto nameA = p->savePreset ("run48-A");
            sp ("baseN", 5.0f);  const auto nameB = p->savePreset ("run48-B");
            juce::File fA = dir.getChildFile (nameA + ".fibpreset");
            juce::File fB = dir.getChildFile (nameB + ".fibpreset");
            p->assignPlaygroundCorner (0, nameA, err);
            p->assignPlaygroundCorner (1, nameB, err);
            sp ("pgMode", 1.0f);            // MIX
            sp ("pgVoices", 8.0f);
            for (int i = 0; i < 20; ++i) { p->pollParametersForTests(); juce::Thread::sleep (10); }
            const int polyMix0 = p->mixVoiceCountForTests (0), polyMix1 = p->mixVoiceCountForTests (1);
            sp ("monoGlide", 1.0f);
            for (int i = 0; i < 20; ++i) { p->pollParametersForTests(); juce::Thread::sleep (10); }
            const int monoMix0 = p->mixVoiceCountForTests (0), monoMix1 = p->mixVoiceCountForTests (1);
            sp ("monoGlide", 0.0f);
            for (int i = 0; i < 20; ++i) { p->pollParametersForTests(); juce::Thread::sleep (10); }
            const int backMix0 = p->mixVoiceCountForTests (0);
            std::printf ("  MIX corner voices: poly %d/%d, mono %d/%d, back to poly %d\n",
                         polyMix0, polyMix1, monoMix0, monoMix1, backMix0);
            check (polyMix0 == 8 && polyMix1 == 8, "MIX corners follow the visible voice control when mono is off");
            check (monoMix0 == 1 && monoMix1 == 1,
                   "mono glide makes EVERY mix sub-engine mono too - never one engine mono and another not");
            check (backMix0 == 8, "...and switching it off hands the corners their polyphony back");
            sp ("pgMode", 0.0f);
            p->assignPlaygroundCorner (0, {}, err);
            p->assignPlaygroundCorner (1, {}, err);
            fA.deleteFile(); fB.deleteFile();
            // Base N is put BACK. The two corner presets above were saved by moving it, and
            // leaving it on 5 left the spectrum's lowest partial at the 5th harmonic - so
            // the f0 probe in (g) below read a pitch two octaves adrift and failed a claim
            // about the glide that the glide was keeping perfectly.
            sp ("baseN", 1.0f);
        }

        // Does the pitch actually TRAVEL from one note to the other? Sample the rendered f0
        // every 10 ms across the glide and count the readings that lie STRICTLY between the
        // two pitches. A restrike has none - it is already at the new pitch before the first
        // sample - so this is the one metric a jump cannot pass. (A "biggest step" metric
        // cannot: a jump's steps are all zero. That was measured, not assumed.)
        auto glideTravel = [&] (int k1, int ch1, int k2, int ch2, double ms) -> double
        {
            x.clear(); clk = 0;
            juce::MidiBuffer m1; m1.addEvent (juce::MidiMessage::noteOn (ch1, k1, (juce::uint8) 100), 0);
            juce::MidiBuffer m2; m2.addEvent (juce::MidiMessage::noteOn (ch2, k2, (juce::uint8) 100), 0);
            pump (60, m1);
            const size_t at = x.size();
            pump (2, m2);
            pump (160, {});
            const int w = (int) (0.030 * sr8);
            const double f1 = p->getNoteFrequency (k1), f2 = p->getNoteFrequency (k2);
            int between = 0, total = 0;
            for (double t = 0.0; t <= ms; t += 10.0)
            {
                const double f = f0At (at + (size_t) (t * 0.001 * sr8), w);
                ++total;
                // A reading the estimator could not make counts AGAINST travelling. Skipping
                // it silently would let a restrike - which is loud and messy exactly where
                // the jump happens - hide inside the gap it created.
                if (f > std::min (f1, f2) * 1.01 && f < std::max (f1, f2) * 0.99) ++between;
            }
            juce::MidiBuffer o1; o1.addEvent (juce::MidiMessage::noteOff (ch2, k2), 0);
            juce::MidiBuffer o2; o2.addEvent (juce::MidiMessage::noteOff (ch1, k1), 0);
            pump (2, o1); pump (2, o2); pump (60, {});
            return total > 0 ? (double) between / (double) total : 0.0;
        };

        //-- (g) MPE: one note stream, whatever channel it arrives on ---------------------
        // The mono rule is a rule about the INSTRUMENT, not about a channel. Under an MPE
        // zone every note has its own channel, so a per-channel reading of "mono" would
        // let an MPE controller play chords with the switch on - and the glide would never
        // fire, because nothing would ever be taken over. Both notes here are on DIFFERENT
        // member channels, and the first carries a per-note BEND: the glide has to start
        // from the pitch that note is really sounding at, bend included, not from its key.
        {
            sp ("monoGlide", 1.0f);
            sp ("tuningMode", 3.0f);      // MPE zone: +-48 semitones per note, tuning = Equal
            const double fBent = p->getNoteFrequency (60) * std::exp2 (2.0 / 12.0);  // +2 semitones
            const double fPlain = p->getNoteFrequency (67);
            x.clear(); clk = 0;
            juce::MidiBuffer mpeOn;
            mpeOn.addEvent (juce::MidiMessage::pitchWheel (2, 8192 + (int) std::lround (8192.0 * 2.0 / 48.0)), 0);
            mpeOn.addEvent (juce::MidiMessage::noteOn (2, 60, (juce::uint8) 100), 1);
            pump (60, mpeOn);
            const size_t mT = x.size();
            juce::MidiBuffer mpeOn2; mpeOn2.addEvent (juce::MidiMessage::noteOn (3, 67, (juce::uint8) 100), 0);
            pump (2, mpeOn2);
            const int mpeActive = p->activeVoiceCountForTests();
            pump (200, {});
            const int  wM = (int) (0.030 * sr8);
            const double mBefore = f0At (mT - (size_t) (0.040 * sr8), wM);
            const double mAfter  = f0At (mT + (size_t) ((glideMs + 150.0) * 0.001 * sr8), wM);
            juce::MidiBuffer mOff; mOff.addEvent (juce::MidiMessage::noteOff (3, 67), 0);
            juce::MidiBuffer mOff2; mOff2.addEvent (juce::MidiMessage::noteOff (2, 60), 0);
            pump (2, mOff); pump (2, mOff2); pump (60, {});
            std::printf ("  MPE (ch2 bent +2 st -> ch3): %d voices, %.2f Hz (bent key = %.2f) -> "
                         "%.2f Hz (target %.2f)\n", mpeActive, mBefore, fBent, mAfter, fPlain);
            check (mpeActive == 1,
                   "MPE: a second note on ANOTHER channel takes the voice over too - mono is per instrument");
            check (std::abs (1200.0 * std::log2 (mBefore / fBent)) < 15.0,
                   "...and the glide starts from the BENT pitch that note was really sounding, not from its key");
            check (std::abs (1200.0 * std::log2 (mAfter / fPlain)) < 15.0,
                   "...and still arrives on the new note's own frequency");
            sp ("tuningMode", 0.0f);
        }

        //-- (h) SHEPARD MODE survives the takeover ---------------------------------------
        // A Shepard note is a PLAYED mode latched for the life of the note (ShepardEngine.h),
        // and a legato takeover IS that same note being bent. So the stack must keep rising
        // through it: re-latching would reset the rise phase to zero, which is the swoop out
        // of nothing that latch exists to prevent.
        {
            sp ("shepOn", 1.0f);
            sp ("shepMode", 0.0f);        // Free (ms), so no host tempo is needed
            sp ("shepTime", 4000.0f);
            x.clear(); clk = 0;
            pump (60, on (60));
            const bool shepBefore = p->voiceShepardActiveForTests();
            pump (2, on (67));
            const int  shepVoices = p->activeVoiceCountForTests();
            const bool shepDuring = p->voiceShepardActiveForTests();
            const bool gliding    = p->voiceGlidingForTests();
            pump (10, {});
            const double shepMid = p->voiceSoundingHzForTests();
            pump (160, {});
            const double shepEnd = p->voiceSoundingHzForTests();
            pump (2, off (67)); pump (2, off (60)); pump (60, {});
            std::printf ("  Shepard + takeover: %d voice(s), shepard before/after %d/%d, gliding %d, "
                         "voice pitch %.2f -> %.2f Hz (targets %.2f / %.2f)\n",
                         shepVoices, (int) shepBefore, (int) shepDuring, (int) gliding,
                         shepMid, shepEnd, fLo, fHi);
            check (shepBefore && shepDuring,
                   "a Shepard note taken over by the next key is STILL a Shepard note (the latch survives)");
            check (shepVoices == 1, "...and it is still one voice");
            check (shepMid > fLo * 1.005 && shepMid < fHi * 0.995,
                   "...with the portamento genuinely under way 10 ms in, not snapped and not stalled");
            check (std::abs (1200.0 * std::log2 (shepEnd / fHi)) < 5.0,
                   "...and arriving on the new key's tuned pitch");
            sp ("shepOn", 0.0f);
        }

        //-- (i) THE NOTE GATE'S OWN "Mono" STANDS DOWN -----------------------------------
        // The gate's Mono emits a note-OFF for the sounding key at the same sample as the
        // new note-on and BEFORE it, which puts the single voice into its release tail: the
        // takeover then sees `releasing`, correctly refuses to call itself legato, and
        // restrikes. Two mono switches, and the older one silently won. The processor now
        // stands the gate's Mono down while glide is on; this is the measurement that says
        // so, and it is the one that fails if that line is removed.
        {
            const double freeTravel  = glideTravel (60, 1, 67, 1, glideMs);
            sp ("rhythmMono", 1.0f);
            const double gatedTravel = glideTravel (60, 1, 67, 1, glideMs);
            sp ("rhythmMono", 0.0f);
            std::printf ("  note-gate Mono: %.0f%% of the readings lie between the two pitches with the "
                         "gate off, %.0f%% with it ON\n", freeTravel * 100.0, gatedTravel * 100.0);
            check (freeTravel > 0.7, "the control glide really travels (as run 48 already asserted)");
            check (gatedTravel > 0.7,
                   "and the note gate's own Mono no longer turns the glide back into a jump");
        }

        sp ("monoGlide", 0.0f);
    }

    // --- run 49: ADAPTIVE LOOKAHEAD - the spectrum is ready BEFORE the note sounds -----
    // "habria alguna manera de hacer que el modo realtime se anticipara al midi que viene
    //  y preparara el spectrum de antemano para tener 0 latencia en el cambio de parciales
    //  cuando se usa compositivamente en el daw?"
    //
    // Three claims, three separate measurements, because they fail for different reasons:
    //   1. the plugin really delays itself by exactly what it REPORTS as latency;
    //   2. the adapted spectrum is published AT OR BEFORE the delayed note's first
    //      audible sample - against the follower's own ~50-100 ms without it;
    //   3. a transport jump flushes what is in flight, so a loop cannot replay it.
    {
        std::printf ("\nadaptive lookahead: the spectrum is ready before the note sounds:\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr double sr9 = 48000.0;
        constexpr int    b9  = 64;
        auto p = std::make_unique<Proc>();
        p->prepareToPlay (sr9, b9);
        auto sp = [&p] (const char* id, float v)
        { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
          p->pollParametersForTests(); };
        sp ("morph", 0.0f);
        sp ("ampAttack", 0.001f); sp ("ampDecay", 0.001f);
        sp ("ampSustain", 1.0f);  sp ("ampRelease", 0.003f);
        // Quantize-to-pulse is ON by default and would hold a note back for reasons that
        // have nothing to do with this feature; the onset measured below has to be the
        // lookahead's delay and nothing else's.
        sp ("rhythmQPulse", 0.0f);

        // A host timeline this harness owns, so the transport jump can be a real one.
        struct FakeHead : juce::AudioPlayHead
        {
            juce::AudioPlayHead::PositionInfo pos;
            juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return pos; }
        };
        FakeHead head;
        head.pos.setBpm (120.0);
        head.pos.setIsPlaying (false);
        head.pos.setPpqPosition (0.0);
        p->setPlayHead (&head);

        Proc::RtSettings rs;
        rs.adapt.partials = 8; rs.adapt.periodRatio = 2.0; rs.adapt.minGapCents = 50.0;
        rs.adapt.rootOnly = true; rs.adapt.goal = 0; rs.adapt.shapeAmps = true;

        //-- (1) THE REPORTED LATENCY, and the three states it has ----------------------
        rs.lookahead = false;
        p->setRealtimeLink (Proc::RtLink::Off, rs);
        const int latOff = p->getLatencySamples();
        rs.lookahead = true;                       // armed, but no follower to feed
        p->updateRealtimeSettings (rs);
        const int latArmedIdle = p->getLatencySamples();
        p->setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
        const int latOn = p->getLatencySamples();
        const int budget = p->getLookaheadSamples();
        const int wantRaw = (int) std::ceil (Proc::kLookaheadMs * 0.001 * sr9);
        const int wantBlk = ((wantRaw + b9 - 1) / b9) * b9;
        std::printf ("  latency: off %d, armed-but-idle %d, running %d samples (%.1f ms)"
                     " - budget %d, %d ms rounded up to whole %d-sample blocks = %d\n",
                     latOff, latArmedIdle, latOn, (double) latOn * 1000.0 / sr9, budget,
                     (int) Proc::kLookaheadMs, b9, wantBlk);
        check (latOff == 0, "with lookahead off the plugin reports no latency at all");
        check (latArmedIdle == 0,
               "armed with no realtime follower running it still reports none - latency nobody uses is not paid");
        check (latOn == wantBlk && budget == wantBlk,
               "with the follower running the reported latency is the budget, rounded up to whole blocks");

        //-- (2) THE REPORTED LATENCY IS THE BUFFER DEPTH -------------------------------
        // Not "about right": the first audible sample of a note fed at sample 0 must land
        // exactly `budget` samples later, because that is the number the host's delay
        // compensation is going to shift everything back by.
        juce::AudioBuffer<float> ab (2, b9);
        head.pos.setIsPlaying (false);
        {
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            long long onset = -1, at = 0;
            for (int i = 0; i < 400 && onset < 0; ++i)
            {
                juce::MidiBuffer mm;
                if (i == 0) mm = m;
                ab.clear();
                p->processBlock (ab, mm);
                const float* r = ab.getReadPointer (0);
                for (int k = 0; k < b9; ++k)
                    if (onset < 0 && std::abs (r[k]) > 1.0e-7f) onset = at + k;
                at += b9;
            }
            std::printf ("  a note fed at sample 0 first sounds at sample %lld (reported latency %d)\n",
                         onset, latOn);
            check (onset == (long long) latOn,
                   "the note is delayed by EXACTLY the reported latency - the host's compensation lands it on time");
            juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            for (int i = 0; i < 400; ++i)
            { juce::MidiBuffer mm; if (i == 0) mm = off; ab.clear(); p->processBlock (ab, mm); }
        }

        //-- (3) A TRANSPORT JUMP FLUSHES WHAT IS IN FLIGHT ------------------------------
        // The control and the jump are the SAME take apart from the jump itself, so a
        // difference can only be the flush.
        auto playFrom = [&] (double ppq0, bool jumpAfterTwoBlocks) -> long long
        {
            head.pos.setIsPlaying (false);
            head.pos.setPpqPosition (ppq0);
            for (int i = 0; i < 4; ++i) { juce::MidiBuffer mm; ab.clear(); p->processBlock (ab, mm); }
            head.pos.setIsPlaying (true);
            double ppq = ppq0;
            const double dPpq = (double) b9 * 120.0 / (60.0 * sr9);
            long long onset = -1, at = 0;
            for (int i = 0; i < 600 && onset < 0; ++i)
            {
                juce::MidiBuffer mm;
                if (i == 2) mm.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
                if (jumpAfterTwoBlocks && i == 5) ppq = ppq0;      // the loop point
                head.pos.setPpqPosition (ppq);
                ppq += dPpq;
                ab.clear();
                p->processBlock (ab, mm);
                const float* r = ab.getReadPointer (0);
                for (int k = 0; k < b9; ++k)
                    if (onset < 0 && std::abs (r[k]) > 1.0e-7f) onset = at + k;
                at += b9;
            }
            juce::MidiBuffer offm; offm.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            for (int i = 0; i < 300; ++i)
            { juce::MidiBuffer mm; if (i == 0) mm = offm; ab.clear(); p->processBlock (ab, mm); }
            head.pos.setIsPlaying (false);
            return onset;
        };
        const long long plainOnset = playFrom (0.0, false);
        const long long jumpOnset  = playFrom (8.0, true);
        std::printf ("  playback: note buffered then heard at sample %lld (2 blocks + %d budget = %lld);"
                     " with a loop jump 3 blocks later: %lld (-1 = never sounded)\n",
                     plainOnset, latOn, (long long) (2 * b9 + latOn), jumpOnset);
        check (plainOnset == (long long) (2 * b9 + latOn),
               "during playback a buffered note still sounds exactly one budget after it arrived");
        check (jumpOnset < 0,
               "a transport jump FLUSHES the buffered note - a loop restart cannot replay it");
        check (p->activeVoiceCountForTests() == 0,
               "...and nothing is left holding after the flush (no stranded voice)");

        //-- (4) THE ALIGNMENT: the spectrum is live before the note is audible ----------
        // Paced to WALL CLOCK, exactly as run 42 is and for the same reason: the search
        // runs on its own thread and the 60 Hz timer is a real timer, so an offline render
        // that outran realtime would hand the follower a head start no player ever gets.
        //
        // Measured TWICE with everything else identical - lookahead on, then off - because
        // the number that matters is the DIFFERENCE, and only the difference can be
        // attributed to this feature.
        // Filled by every `landing` call with the number the PLUGIN says it took, so the
        // read-out on the button can be checked against the one measured from outside.
        double selfReportMs = -1.0;
        auto landing = [&] (bool lookahead) -> double
        {
            rs.lookahead = lookahead;
            p->setRealtimeLink (Proc::RtLink::Off, rs);
            p->setRealtimeLink (Proc::RtLink::SpectrumFromChord, rs);
            const int bud = p->getLookaheadSamples();

            long long clk = 0, nextPoll = 0;
            const long long tick = (long long) (sr9 / 60.0);
            const double    t0   = juce::Time::getMillisecondCounterHiRes();
            juce::uint32 gen0 = 0;
            long long feedAt = -1, landAt = -1;
            long long startAt = -1, doneAt = -1; int startsSeen = 0;
            long long appliedAt = -1; int appliesSeen = 0;
            double wStart = -1.0, wDone = -1.0, wLand = -1.0, wApply = -1.0, wFeed = 0.0;

            auto genNow = [&] { const auto* t = p->getLiveTemplate(); return t != nullptr ? t->generation : 0u; };
            auto pumpFor = [&] (double seconds, juce::MidiBuffer first)
            {
                const long long want = clk + (long long) (seconds * sr9);
                bool sent = false;
                while (clk < want)
                {
                    juce::MidiBuffer m;
                    if (! sent) { m = first; sent = true; }
                    ab.clear();
                    p->processBlock (ab, m);
                    clk += b9;
                    if (clk >= nextPoll)
                    {
                        nextPoll += tick;
                        p->pollParametersForTests();
                        if (feedAt >= 0 && landAt < 0 && genNow() != gen0) { landAt = clk; wLand = juce::Time::getMillisecondCounterHiRes() - t0; }
                        if (feedAt >= 0 && startAt < 0 && p->getRealtimeStarts() != startsSeen) { startAt = clk; wStart = juce::Time::getMillisecondCounterHiRes() - t0; }
                        if (feedAt >= 0 && doneAt  < 0 && ! p->isSpectrumAdaptRunning() && startAt >= 0) { doneAt = clk; wDone = juce::Time::getMillisecondCounterHiRes() - t0; }
                        if (feedAt >= 0 && appliedAt < 0 && p->getRealtimeApplies() != appliesSeen) { appliedAt = clk; wApply = juce::Time::getMillisecondCounterHiRes() - t0; }
                    }
                    for (;;)   // never let the render outrun the searcher's wall clock
                    {
                        const double behind = (double) clk * 1000.0 / sr9
                                            - (juce::Time::getMillisecondCounterHiRes() - t0);
                        if (behind <= 0.0) break;
                        juce::Thread::sleep (behind > 2.0 ? 1 : 0);
                    }
                }
            };
            auto keys = [] (bool down, std::initializer_list<int> ks)
            {
                juce::MidiBuffer m;
                for (const int k : ks)
                    m.addEvent (down ? juce::MidiMessage::noteOn (1, k, (juce::uint8) 100)
                                     : juce::MidiMessage::noteOff (1, k), 0);
                return m;
            };

            pumpFor (0.10, {});
            pumpFor (2.4, keys (true, { 60, 64, 67 }));     // chord A, adapted and settled
            pumpFor (0.02, keys (false, { 60, 64, 67 }));
            // C major -> D minor: the QUALITY moves, not just the root, so the adaptation
            // has a genuinely different answer to give (run 42 documents why a pure
            // transposition would produce no new spectrum at all in an equal temperament).
            const int startsBefore = p->getRealtimeStarts(), appliesBefore = p->getRealtimeApplies();
            startsSeen = startsBefore;
            appliesSeen = appliesBefore;
            gen0   = genNow();
            feedAt = clk;
            wFeed  = juce::Time::getMillisecondCounterHiRes() - t0;
            pumpFor (2.0, keys (true, { 62, 65, 69 }));
            std::printf ("     WALL: launched %.1f, worker idle %.1f, applied %.1f, live %.1f (ms after feed)\n",
                         wStart - wFeed, wDone - wFeed, wApply - wFeed, wLand - wFeed);
            // The delayed note's first audible sample, by construction: the buffer depth
            // asserted in (2) above. In ms from the moment the note ARRIVED.
            const double audibleMs = (double) bud * 1000.0 / sr9;
            const double landMs    = landAt >= 0 ? (double) (landAt - feedAt) * 1000.0 / sr9 : -1.0;
            std::printf ("  lookahead %s: budget %.1f ms, the new spectrum went live %.1f ms "
                         "after the notes arrived (margin %.1f ms; %d searches / %d applies during it)\n",
                         lookahead ? "ON " : "OFF", audibleMs, landMs, audibleMs - landMs,
                         p->getRealtimeStarts() - startsBefore, p->getRealtimeApplies() - appliesBefore);
            std::printf ("     search launched at %.1f ms, worker idle again at %.1f ms\n",
                         startAt >= 0 ? (double) (startAt - feedAt) * 1000.0 / sr9 : -1.0,
                         doneAt  >= 0 ? (double) (doneAt  - feedAt) * 1000.0 / sr9 : -1.0);
            // THE PLUGIN'S OWN READ-OUT of the same interval, checked against the one this
            // harness measured from outside. It is what the LOOKAHEAD button prints and it
            // is the only thing telling a user whose settings outgrow the budget that they
            // have - so a read-out that quietly drifted from the truth would be worse than
            // none. Both are wall-clock and the plugin starts its stopwatch one poll
            // earlier (at the tick that SAW the chord move), so they agree to a tick.
            selfReportMs = p->getLastRealtimeLandingMs();
            std::printf ("     the plugin reports the same landing as %.1f ms (measured here: %.1f ms)\n",
                         selfReportMs, wLand - wFeed);
            return landMs;
        };
        double selfOn = 0.0, selfOff = 0.0;
        const double onMs  = landing (true);  selfOn  = selfReportMs;
        // The ON take is run TWICE and the WORST is asserted on. The first take of a fresh
        // link was measured at 80 ms on an idle machine and at 213 ms on the same binary
        // with a compiler running beside it, so a single fast take would let a budget that
        // does not really fit pass. The worst of two is the honest reading.
        const double onMs2 = landing (true);  selfOn  = std::max (selfOn, selfReportMs);
        const double offMs = landing (false); selfOff = selfReportMs;
        const double onWorst   = std::max (onMs, onMs2);
        const double audibleMs = (double) latOn * 1000.0 / sr9;
        check (onMs > 0.0 && offMs > 0.0, "both takes really did produce a new spectrum");
        check (onWorst <= audibleMs,
               "WITH lookahead the adapted spectrum is live AT OR BEFORE the delayed note's first sample");
        check (offMs > audibleMs * 0.25,
               "WITHOUT it the same move lands far later - which is the delay this exists to remove");
        std::printf ("  the note sounds %.1f ms after it arrives with lookahead on; the spectrum needs "
                     "%.1f / %.1f ms (on) / %.1f ms (off) - so the change is %s\n", audibleMs, onMs, onMs2,
                     offMs, onWorst <= audibleMs ? "sample-aligned with the note" : "STILL LATE");
        // The plugin's own number has to be the same number. 40 ms of tolerance is two 60 Hz
        // ticks plus slack: the harness only samples at poll boundaries, the plugin stamps
        // its stopwatch inside the poll that saw the move.
        std::printf ("  the plugin's own read-out: %.1f ms (on) / %.1f ms (off); the button calls it %s\n",
                     selfOn, selfOff, selfOn <= audibleMs ? "on time" : "LATE");
        check (selfOn > 0.0 && std::abs (selfOn - onWorst) < 40.0,
               "the landing the LOOKAHEAD button prints is the landing that really happened");
        check (selfOn <= audibleMs,
               "...and at these settings it reports a landing INSIDE the budget, so it does not say LATE");

        p->setRealtimeLink (Proc::RtLink::Off, rs);
        p->setPlayHead (nullptr);
    }

    // --- run 50: the DRY/WET TRANSITION - a crossfade, not a bend ---------------------
    // "cuando esta en on actua de manera similar a morph, en cada cambio de espectro no hay
    // brusquedad si no un fade in fade out simultaneo de el estado previo al estado nuevo
    // del espectro, es una alternativa al pitch bending de morph."
    //
    // Morph BENDS: mid-glide a partial sounds at a frequency that is in neither spectrum.
    // This CROSSFADES: mid-transition BOTH spectra sound, each at its own frequencies, and
    // nothing is ever heard in between. That difference is the whole feature, it is audible,
    // and it is invisible to every other check in this build - so it is measured here, in
    // the rendered audio, with the same click metric run 26 uses standing beside it.
    //
    // THE SPECTRUM IS CHOSEN TO MAKE THE QUESTION ANSWERABLE: a typed Custom Engine timbre
    // of exactly two partials, 1 and 2 of the played pitch, replaced by exactly two, 1 and
    // 3. Same count, so nothing appears or disappears; a flat amplitude curve, so both are
    // easy to find; and the one partial that changes moves far enough (523 -> 785 Hz at
    // middle C) that its morph midpoint (641 Hz) is five FFT bins clear of both ends.
    {
        std::printf ("dry/wet transition (a crossfade between two standing spectra):\n");
        constexpr int    eb  = 64;
        constexpr double srD = 48000.0;
        constexpr int    fo  = 11, fftN = 1 << fo;   // 42.7 ms window, 23.4 Hz bins

        struct Take
        {
            std::vector<float> x;
            size_t change = 0;            // sample at which the new template went live
            double f0 = 0.0;
            double jumpPeak = 0.0;        // run 26's metric over the whole take
            double endJump  = 0.0;        // ...and over the 40 ms around the hand-over
            int    oldN = 0, newN = 0;
            bool   loaded = false;
            double oldTop = 0.0, newTop = 0.0;   // the one ratio that moves, before and after
        };

        auto take = [&] (bool morphOn, bool dwOn, float time, double tailS) -> Take
        {
            Take t;
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (srD, eb);
            auto s = [&] (const char* id, float v)
            { if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            // A flat, fully sustained envelope and no fractal: anything that moves in this
            // recording is the transition, not the ADSR and not a convolution.
            s ("ampAttack", 0.002f); s ("ampDecay", 0.002f); s ("ampSustain", 1.0f);
            s ("ampCurveA", 0.0f);   s ("ampCurveD", 0.0f);
            // Slope 1, NOT flat. A flat spectrum would give the two typed timbres identical
            // amplitudes, and then the hand-over at the end of a transition - curAmp taking
            // over the values the incoming bank was rendering - would be a copy of one
            // number onto the same number, i.e. untestable. At slope 1 the moving partial is
            // at 1/2 before and 1/3 after, so the hand-over has something to get wrong.
            s ("fractalLevel", 0.0f); s ("curveSlope", 1.0f); s ("timbreRepeats", 1.0f);
            s ("morph",  morphOn ? 1.0f : 0.0f); s ("morphTime",  time);
            s ("dryWet", dwOn    ? 1.0f : 0.0f); s ("dryWetTime", time);

            // The Custom Engine, typed. loadTimbreText only PARSES and stores - the
            // template is compiled by the message-thread poll, which an offline harness has
            // to drive itself, exactly as every other run here does.
            // AND WAIT FOR THE COMPILE - on the CONDITION, not on the clock. The template is
            // built on the engine's worker thread while rendering offline runs thousands of
            // times faster than real time, so a hundred blocks here are microseconds of wall
            // time and the "new" spectrum is still the old one. A fixed sleep is a race and
            // this file has watched it lose: at 30 ms, one take in nine came back on the
            // startup harmonic spectrum (1024 partials) and every reading taken from it was
            // of a spectrum that never changed - a silent pass, or a false failure.
            const auto awaitTemplate = [&] (juce::uint32 differentFrom) -> bool
            {
                for (int i = 0; i < 400; ++i)   // 2 s of patience, then give up and say so
                {
                    const auto* tt = p->getLiveTemplate();
                    if (tt != nullptr && tt->generation != differentFrom) return true;
                    juce::Thread::sleep (5);
                }
                return false;
            };
            const auto* t00 = p->getLiveTemplate();
            const juce::uint32 genStart = t00 != nullptr ? t00->generation : 0u;

            juce::String err;
            t.loaded = p->loadTimbreText ("1 2 8", err);   // partials at 1 and 2; 8 is the period
            p->pollParametersForTests();
            t.loaded = awaitTemplate (genStart) && t.loaded;
            juce::AudioBuffer<float> b (2, eb);
            for (int i = 0; i < 40; ++i) { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
              b.clear(); p->processBlock (b, m); }

            std::vector<float> x;
            const auto render = [&] (int blocks)
            {
                for (int i = 0; i < blocks; ++i)
                {
                    juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                    const float* L = b.getReadPointer (0);
                    x.insert (x.end(), L, L + eb);
                }
            };
            render ((int) std::ceil (0.25 * srD / eb));   // let the note settle
            const auto* t0 = p->getLiveTemplate();
            const juce::uint32 gen0 = t0 != nullptr ? t0->generation : 0u;
            t.oldN = t0 != nullptr ? (int) t0->partials.size() : 0;
            if (t0 != nullptr && t.oldN >= 2) t.oldTop = (double) t0->partials[1].ratio;

            t.loaded = p->loadTimbreText ("1 3 8", err) && t.loaded;   // partials at 1 and 3
            p->pollParametersForTests();
            // Waited for the same way, so the swap is live before the first recorded block
            // and the transition begins exactly at the start of the tail.
            t.loaded = awaitTemplate (gen0) && t.loaded;
            const int blocks = (int) std::ceil (tailS * srD / eb);
            for (int i = 0; i < blocks; ++i)
            {
                juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                if (t.change == 0)
                {
                    const auto* tn = p->getLiveTemplate();
                    if (tn != nullptr && tn->generation != gen0) t.change = x.size();
                }
                const float* L = b.getReadPointer (0);
                x.insert (x.end(), L, L + eb);
            }
            const auto* t1 = p->getLiveTemplate();
            t.newN = t1 != nullptr ? (int) t1->partials.size() : 0;
            if (t1 != nullptr && t.newN >= 2) t.newTop = (double) t1->partials[1].ratio;
            t.f0   = p->getNoteFrequency (60);
            double mj = 0.0, pk = 0.0, ej = 0.0;
            // ...and the same metric NARROWED to the 40 ms around the moment the transition
            // ENDS. That is where the hand-over lives - the incoming bank's phases, ratios
            // and amplitudes being taken over by the running one - and it is one instant in
            // a recording of seconds, so a step there can hide inside a whole-take maximum
            // that some louder part of the note already sets.
            const size_t endAt = t.change + (size_t) ((double) time * srD);
            const size_t eLo = endAt > (size_t) (0.02 * srD) ? endAt - (size_t) (0.02 * srD) : 1;
            const size_t eHi = std::min (x.size(), endAt + (size_t) (0.02 * srD));
            for (size_t i = 0; i < x.size(); ++i)
            {
                pk = std::max (pk, (double) std::abs (x[i]));
                if (i > 0) mj = std::max (mj, (double) std::abs (x[i] - x[i - 1]));
                if (i >= eLo && i < eHi) ej = std::max (ej, (double) std::abs (x[i] - x[i - 1]));
            }
            t.jumpPeak = pk > 0.0 ? mj / pk : 0.0;
            t.endJump  = pk > 0.0 ? ej / pk : 0.0;
            std::printf ("    take[morph %d, dry/wet %d, %.3f s]: %d->%d partials, top %.3f->%.3f,"
                         " swap at %zu of %zu, jump/peak %.3f (%.3f at the hand-over)\n",
                         morphOn ? 1 : 0, dwOn ? 1 : 0, (double) time, t.oldN, t.newN,
                         t.oldTop, t.newTop, t.change, x.size(), t.jumpPeak, t.endJump);
            t.x = std::move (x);
            { juce::MidiBuffer m; b.clear(); p->processBlock (b, m); }   // let the note go
            return t;
        };

        // The magnitude spectrum of one 42.7 ms window CENTRED on `centre`, Hann-windowed.
        juce::dsp::FFT fftD (fo);
        std::vector<float> fbufD (2 * (size_t) fftN);
        const auto spectrumAt = [&] (const std::vector<float>& x, size_t centre)
        {
            std::vector<float> out ((size_t) fftN / 2, 0.0f);
            if (centre < (size_t) fftN / 2 || centre + (size_t) fftN / 2 > x.size()) return out;
            const size_t at = centre - (size_t) fftN / 2;
            std::fill (fbufD.begin(), fbufD.end(), 0.0f);
            for (int i = 0; i < fftN; ++i)
                fbufD[(size_t) i] = (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                              * (float) i / (float) (fftN - 1)))
                                  * x[at + (size_t) i];
            fftD.performFrequencyOnlyForwardTransform (fbufD.data());
            std::copy (fbufD.begin(), fbufD.begin() + fftN / 2, out.begin());
            return out;
        };
        // ...read at one frequency by SUMMING the five bins around it, not by taking the
        // tallest. A Hann window spreads a sinusoid over three bins and how tall the tallest
        // one is depends on where the partial falls BETWEEN bin centres - up to 15% for a
        // partial landing exactly half way, which is where 785 Hz falls at this FFT size.
        // That bias is a property of the measurement, not of the synth, and reading it as
        // one bin made a perfectly exact endpoint look 13% short. Five bins is wide enough
        // to collect the whole main lobe and far narrower than the 11-bin spacing of the
        // partials being told apart.
        const auto magAt = [&] (const std::vector<float>& mag, double hz)
        {
            const int k = (int) std::lround (hz * (double) fftN / srD);
            double m = 0.0;
            for (int d = -2; d <= 2; ++d)
                if (k + d >= 0 && k + d < (int) mag.size()) m += (double) mag[(size_t) (k + d)];
            return m;
        };

        // ---- (a) A CROSSFADE, MEASURED: both spectra at once, nothing in between --------
        // Two seconds, so a 42.7 ms window is a snapshot rather than a smear of the whole
        // transition, and the three probe points are unambiguously early / middle / late.
        {
            const auto dw = take (/*morph*/ false, /*dryWet*/ true, 2.0f, 2.6);
            const auto mo = take (/*morph*/ true,  /*dryWet*/ false, 2.0f, 2.6);
            const double fOld = dw.f0 * 2.0, fNew = dw.f0 * 3.0;
            const double fBetween = dw.f0 * std::sqrt (6.0);   // where a MORPH would be at half
            // Every reading is a partial's magnitude AS A FRACTION OF THE FUNDAMENTAL's, in
            // the same frame. The fundamental is in both spectra and never moves, so it is
            // the one reference immune to the voice's own per-sample normalisation.
            //
            // `secs` is measured from the moment the new template went live; a NEGATIVE one
            // reads a window entirely BEFORE it, which is the only honest way to sample the
            // near endpoint - a window centred on the change straddles it.
            const auto probe = [&] (const Take& tk, double secs, double& oldR, double& newR, double& midR)
            {
                const size_t half = (size_t) fftN / 2 + 256;
                const size_t c = secs < 0.0 ? (tk.change > half ? tk.change - half : 0)
                                            : tk.change + (size_t) (secs * srD);
                const auto m = spectrumAt (tk.x, c);
                const double f0m = std::max (1.0e-9, magAt (m, tk.f0));
                oldR = magAt (m, fOld) / f0m; newR = magAt (m, fNew) / f0m; midR = magAt (m, fBetween) / f0m;
            };
            double dwOp = 0, dwNp = 0, dwMp = 0, dwO1 = 0, dwN1 = 0, dwM1 = 0;
            double dwO5 = 0, dwN5 = 0, dwM5 = 0, dwO3 = 0, dwN3 = 0, dwM3 = 0;
            double dwOe = 0, dwNe = 0, dwMe = 0, moO5 = 0, moN5 = 0, moM5 = 0;
            probe (dw, -1.0, dwOp, dwNp, dwMp);   // before   : only the old spectrum
            probe (dw,  0.5, dwO1, dwN1, dwM1);   // a quarter in
            probe (dw,  1.0, dwO5, dwN5, dwM5);   // half way
            probe (dw,  1.5, dwO3, dwN3, dwM3);   // three quarters
            probe (dw,  2.2, dwOe, dwNe, dwMe);   // after    : only the new spectrum
            probe (mo,  1.0, moO5, moN5, moM5);
            std::printf ("  partials as a fraction of the fundamental (%.1f / %.1f / %.1f Hz):\n",
                         fOld, fBetween, fNew);
            std::printf ("    dry/wet   before old %.3f  between %.3f  new %.3f\n", dwOp, dwMp, dwNp);
            std::printf ("    dry/wet   1/4    old %.3f  between %.3f  new %.3f\n", dwO1, dwM1, dwN1);
            std::printf ("    dry/wet   half   old %.3f  between %.3f  new %.3f\n", dwO5, dwM5, dwN5);
            std::printf ("    dry/wet   3/4    old %.3f  between %.3f  new %.3f\n", dwO3, dwM3, dwN3);
            std::printf ("    dry/wet   after  old %.3f  between %.3f  new %.3f\n", dwOe, dwMe, dwNe);
            std::printf ("    morph     half   old %.3f  between %.3f  new %.3f\n", moO5, moM5, moN5);
            std::printf ("    the two spectra: %d partials, top ratio %.3f -> %d partials, top ratio %.3f"
                         " (loaded %s, swap landed %s)\n",
                         dw.oldN, dw.oldTop, dw.newN, dw.newTop,
                         dw.loaded ? "yes" : "NO", dw.change > 0 ? "yes" : "NO");
            check (dw.loaded && dw.oldN == 2 && dw.newN == 2
                   && std::abs (dw.oldTop - 2.0) < 1.0e-4 && std::abs (dw.newTop - 3.0) < 1.0e-4
                   && dw.change > 0,
                   "the two typed spectra really are two partials each, at 1 and 2 then 1 and 3,"
                   " and the swap really landed");
            // THE DEFINING PROPERTY. Half way through, both spectra are sounding at their
            // OWN frequencies and there is nothing between them.
            check (dwO5 > 0.10 && dwN5 > 0.10,
                   "half way through a dry/wet transition BOTH spectra are sounding at once");
            check (dwM5 < 0.15 * std::min (dwO5, dwN5),
                   "and NOTHING sounds between them - no partial changed frequency, which is what"
                   " makes this a crossfade and not a morph");
            // ...and the same instant under a MORPH is the opposite, which is what stops
            // this being a test that any smoothing at all would pass: the old partial has
            // LEFT the frequency it was on and is sounding between the two.
            check (moM5 > 0.15 && moO5 < 0.2 * moM5,
                   "where a MORPH at the same instant has its partial BETWEEN the two, and nothing"
                   " left where the old one was");
            // AMPLITUDES CROSS OVER, in the right direction and monotonically.
            check (dwO1 > dwO5 && dwO5 > dwO3 && dwN1 < dwN5 && dwN5 < dwN3,
                   "the old spectrum fades DOWN and the new one fades UP across the transition");
            check (dwO1 > dwN1 && dwN3 > dwO3,
                   "...crossing over in the middle - the old one still leads at a quarter in, the new"
                   " one leads at three quarters");
            // THE TWO ENDPOINTS ARE EXACTLY THE OLD AND THE NEW SPECTRUM, and each one is
            // checked against the spectrum it is supposed to be rather than against the
            // other. At slope 1 the compiler gives a partial the amplitude ratio^-1, so the
            // moving partial has to sit at exactly 1/2 of the fundamental before and exactly
            // 1/3 after. An endpoint that merely "looks settled" - the amplitude slew's
            // 63%-of-the-way, say - lands between those two numbers and fails here.
            const double wantOld = 1.0 / dw.oldTop, wantNew = 1.0 / dw.newTop;
            std::printf ("    endpoints: old/f0 before = %.3f (want %.3f), new/f0 after = %.3f"
                         " (want %.3f); strays before = %.3f, after = %.3f\n",
                         dwOp, wantOld, dwNe, wantNew, dwNp, dwOe);
            check (dwNp < 0.06 * dwOp && dwOe < 0.06 * dwNe,
                   "at the start only the OLD spectrum sounds, and at the end only the NEW one");
            check (std::abs (dwOp - wantOld) < 0.12 * wantOld
                   && std::abs (dwNe - wantNew) < 0.12 * wantNew,
                   "and each endpoint is that spectrum EXACTLY - the amplitudes the compiler gave it,"
                   " not an amplitude slew caught part way");
            // EQUAL POWER, not linear. Two different spectra are uncorrelated, so a linear
            // crossfade loses 3 dB in the middle; the transition must not audibly duck.
            const auto rmsAt = [&] (const Take& tk, double secs)
            {
                const size_t c = secs < 0.0 ? (tk.change > 4096 ? tk.change - 4096 : 0)
                                            : tk.change + (size_t) (secs * srD);
                if (c < 2048 || c + 2048 > tk.x.size()) return 0.0;
                double s2 = 0.0;
                for (size_t i = c - 2048; i < c + 2048; ++i) s2 += (double) tk.x[i] * tk.x[i];
                return std::sqrt (s2 / 4096.0);
            };
            const double r0 = rmsAt (dw, -1.0), r5 = rmsAt (dw, 1.0), r9 = rmsAt (dw, 2.2);
            std::printf ("    loudness through the transition: %.4f -> %.4f -> %.4f (%.1f%% at the middle)\n",
                         r0, r5, r9, 100.0 * r5 / std::max (1.0e-9, 0.5 * (r0 + r9)));
            // ...and the level rides through the transition without a step or a hole. Not
            // dead flat, and it cannot be: the per-voice normalisation is derived from the
            // two banks' amplitude sums mixed by the same weight, which is exact for a
            // partial the two spectra SHARE (phase-locked, so it really is one partial) and
            // an over-estimate for one that moved (two partials at half gain each). Here half
            // the energy is shared, so the middle sits ~1.3 dB under the ends. A step, a
            // hole, or a swell is the failure; 2 dB of smooth ride over two seconds is not.
            check (r5 > 0.80 * 0.5 * (r0 + r9) && r5 < 1.20 * 0.5 * (r0 + r9),
                   "and the level rides smoothly through the middle of the crossfade rather than"
                   " ducking into a hole or swelling");
        }

        // ---- (b) MORPH WINS when both are on, and it is audible ------------------------
        // The two features are one crossfade machine per voice, so exactly one can act. The
        // panel says which; this proves which, in the audio, so the UI's claim is not the
        // only evidence for it.
        {
            const auto both = take (/*morph*/ true, /*dryWet*/ true, 2.0f, 2.6);
            const auto m = spectrumAt (both.x, both.change + (size_t) (1.0 * srD));
            const double f0m = std::max (1.0e-9, magAt (m, both.f0));
            const double mid = magAt (m, both.f0 * std::sqrt (6.0)) / f0m;
            const double old = magAt (m, both.f0 * 2.0) / f0m;
            std::printf ("  both switches ON, half way: between %.3f, old %.3f -> %s\n",
                         mid, old, mid > old ? "MORPH is what acted" : "the crossfade acted");
            check (mid > 0.15 && old < 0.2 * mid,
                   "with BOTH on it is the MORPH that acts - one machine, and the panel greys the other"
                   " out rather than letting the user believe both are running");
        }

        // ---- (c) NO CLICK, at three transition times, with morph on and off -------------
        // Run 26's metric: a click IS a discontinuity, so measure the largest sample-to-
        // sample jump normalised by the take's own peak. The control is the same swap with
        // both switches off, which is the amplitude fade the synth already had.
        {
            const auto ctrlTake = take (false, false, 0.050f, 0.60);
            const double ctrl = ctrlTake.jumpPeak, ctrlEnd = ctrlTake.endJump;
            struct Row { const char* label; bool morph; bool dw; float t; };
            const Row rows[] = {
                { "dry/wet 1 ms",            false, true,  0.001f },
                { "dry/wet 50 ms",           false, true,  0.050f },
                { "dry/wet 2 s",             false, true,  2.000f },
                { "morph on, dry/wet 1 ms",  true,  true,  0.001f },
                { "morph on, dry/wet 50 ms", true,  true,  0.050f },
                { "morph on, dry/wet 2 s",   true,  true,  2.000f } };
            std::printf ("  discontinuity (jump/peak), control = both off: %.3f (%.3f at the hand-over)\n",
                         ctrl, ctrlEnd);
            int clicks = 0, endClicks = 0;
            for (const auto& r : rows)
            {
                const auto tk = take (r.morph, r.dw, r.t, r.t > 1.0f ? 2.6 : 0.60);
                std::printf ("    %-26s %.3f  (%.3f at the hand-over)\n", r.label, tk.jumpPeak, tk.endJump);
                // The same generous bound run 26 uses: what this catches is a full-scale
                // coherent impulse (jump/peak near 1), not a few percent of extra slope.
                if (tk.jumpPeak > ctrl * 2.5 + 0.02) ++clicks;
                // The hand-over gets a TIGHTER bound, because there the control take is the
                // same note doing nothing in particular: this is 40 ms of steady tone, and a
                // phase, ratio or amplitude taken over wrongly shows up as a step against it.
                if (tk.endJump > ctrlEnd * 1.6 + 0.01) ++endClicks;
            }
            check (ctrl > 0.0, "the control take made sound");
            check (clicks == 0,
                   "no transition time clicks - 1 ms, 50 ms and 2 s, with Morph off and with Morph on");
            check (endClicks == 0,
                   "...and none at the moment a transition ENDS, where the incoming bank's phases,"
                   " frequencies and amplitudes are handed to the running one");
        }

        // ---- (d) AND THE PANEL SAYS SO ------------------------------------------------
        // The rule is "Morph wins"; the requirement is that the UI must not let the user
        // believe both are acting. That is a claim about the SCREEN, so it is measured on
        // the screen: with Morph on, both controls are dead and an amber MORPH ON tag is
        // painted beside a greyed DRY/WET - and that tag has to FIT, in both languages, in
        // a state the layout sweep never enters on its own (it probes with Morph off).
        {
            auto owned = std::make_unique<FractalAdditiveAudioProcessor>();
            owned->prepareToPlay (srD, eb);
            std::unique_ptr<juce::AudioProcessorEditor> base (owned->createEditor());
            auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
            check (ed != nullptr, "the editor builds");
            if (ed != nullptr)
            {
                const auto setMorph = [&] (bool on)
                {
                    if (auto* q = owned->apvts.getParameter ("morph")) q->setValueNotifyingHost (on ? 1.0f : 0.0f);
                    owned->pollParametersForTests();
                    ed->applyDryWetEnablementForTests();
                    ed->resized();
                };
                const auto tagged = [&] (const FractalAdditiveAudioProcessorEditor::LayoutProbe& pr,
                                         const char* nm, juce::Rectangle<int>* cellOut, float* wOut)
                {
                    for (const auto& l : pr.paintedLabels)
                        if (l.name == nm) { if (cellOut) *cellOut = l.cell; if (wOut) *wOut = l.textWidth; return true; }
                    return false;
                };
                for (int lang = 0; lang < 2; ++lang)
                {
                    setMorph (false);
                    auto off = ed->layoutProbeInLanguage (lang == 1);
                    const bool offTag = tagged (off, "both.morphWins", nullptr, nullptr);
                    setMorph (true);
                    auto on = ed->layoutProbeInLanguage (lang == 1);
                    juce::Rectangle<int> cell; float w = 0.0f;
                    const bool onTag = tagged (on, "both.morphWins", &cell, &w);
                    std::printf ("  %s: with Morph off the DRY/WET cell carries no tag (%s);"
                                 " with it on the tag needs %.1f px of %d\n",
                                 lang == 1 ? "ca" : "en", offTag ? "IT DOES" : "correct", w, cell.getWidth());
                    check (! offTag,
                           "with Morph OFF the Dry/Wet cell wears no warning - the control really is live");
                    check (onTag, "and with Morph ON the panel paints a reason beside a greyed DRY/WET");
                    check (w <= (float) cell.getWidth() + 0.5f,
                           "...and that reason FITS its rect, in this language");
                    check (! ed->dryWetControlsEnabledForTests(),
                           "...while the two controls themselves are dead, so nothing on screen claims"
                           " two smoothings are running");
                    setMorph (false);
                    check (ed->dryWetControlsEnabledForTests(),
                           "and switching Morph off hands them straight back - the user's setting was never"
                           " rewritten, only overruled");
                }
                ed->layoutProbeInLanguage (false);   // leave the panel in English
            }
        }
    }

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
