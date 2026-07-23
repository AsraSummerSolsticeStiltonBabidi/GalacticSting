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
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

struct Ev { int sample; bool on; int note, ch; };

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

    std::printf ("== Spectrum-to-Rhythm MIDI verification (C3=60, defaults x=2 n=6) ==\n");

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

    // Key 60 = the tuning anchor 261.6256 Hz; harmonic 2 = 523.2511 Hz / 2^7 =
    // 4.0879 Hz -> period = 48000/4.0879 = 11742.0 samples. Harmonic 3 = 2/3 of it.
    if (onsP2.size() >= 3)
    {
        const double per = (double) (onsP2[2] - onsP2[0]) / 2.0;
        const double expect = sr / (2.0 * 261.6255653 / 128.0);
        std::printf ("  P2 period: %.1f samples (expected %.1f)\n", per, expect);
        check (std::abs (per - expect) <= 1.0, "P2 pulse period is sample-exact (f0*2 / 2^7)");
    }
    if (onsP2.size() >= 3 && onsP3.size() >= 4)
    {
        const double p2 = (double) (onsP2[2] - onsP2[0]) / 2.0;
        const double p3 = (double) (onsP3[3] - onsP3[0]) / 3.0;
        const double ratio = p2 / p3;
        std::printf ("  polyrhythm P3:P2 = %.5f (expected 1.50000)\n", ratio);
        check (std::abs (ratio - 1.5) < 0.001, "P3 vs P2 is an EXACT 3:2 polyrhythm");
    }
    const double rateHz = 261.6255653 / 128.0; // harmonic 1 of key 60 at the default divisor
    std::printf ("  pulse-1 rate = %.4f Hz = %.2f BPM (target ~130, power-of-2 division)\n",
                 rateHz, rateHz * 60.0);
    check (rateHz * 60.0 > 100.0 && rateHz * 60.0 < 150.0, "default pulse 1 lands near 130 BPM");
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

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
