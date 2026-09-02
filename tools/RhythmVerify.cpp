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
#include <cstring>
#include <map>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    // A FAILURE IS ALSO NAMED ON STDERR, and stdout is flushed behind it. This run once
    // ended "FAILURES (1 failures)" with no [FAIL] line anywhere in the redirected output
    // - the string "FAIL" appeared exactly once in 1838 lines, in the summary - so the one
    // thing the log had to say was the one thing missing from it, and finding out that the
    // check was "and no two controls overlap" cost a rebuild with a trace bolted on. Two
    // lines here mean the next one names itself.
    if (! ok)
    {
        ++failures;
        std::fflush (stdout);
        std::fprintf (stderr, "  [FAIL] %s\n", what);
        std::fflush (stderr);
    }
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

// One scratch preset in the user's real folder, taken away again on the way out of the
// scope that made it - including the paths where a block returns early. It saves through
// the processor and then owns whatever the processor ACTUALLY wrote: savePreset()
// sanitises the name, and naming the file by hand a second time is the mistake that left
// run48-A.gsting behind (the save wrote ".gsting", the cleanup deleted ".fibpreset").
// ONE PREFIX for every scratch preset this suite writes, so the sweeps below can be a
// single rule rather than a list of names somebody has to remember to extend. run 48's
// files were called "run48-A/B" and so were invisible to a sweep looking for the others.
static constexpr const char* kScratchPrefix = "zzz-verify-";

struct ScopedPreset
{
    ScopedPreset() = default;
    // The destructor is where a leak is CHEAP to explain: it knows which preset this is
    // and which block made it. Discovered instead by the end-of-run audit, 3000 lines
    // later, all it can say is that some file appeared - a run that left run48-A/B behind
    // while the audit named zzz-verify-mix-A/B could not be pinned on any block at all.
    ~ScopedPreset()
    {
        if (file == juce::File()) return;
        file.deleteFile();
        if (file.existsAsFile())
        {
            std::printf ("  [FAIL] the scratch preset \"%s\" was removed from the user's folder\n",
                         name.toRawUTF8());
            ++failures;
        }
    }
    ScopedPreset (const ScopedPreset&) = delete;
    ScopedPreset& operator= (const ScopedPreset&) = delete;

    bool save (FractalAdditiveAudioProcessor& p, const juce::String& want)
    {
        // Every scratch preset carries the prefix, whatever the caller asked for.
        name = p.savePreset (want.startsWith (kScratchPrefix) ? want : kScratchPrefix + want);
        file = FractalAdditiveAudioProcessor::presetFile (name);
        return name.isNotEmpty() && file.existsAsFile();
    }

    juce::String name;
    juce::File   file;
};

// "Nothing is owed to the spectrum worker any more."
//
// The stronger of the two waits, and the right one wherever a test asks WHAT IS SOUNDING
// NOW straight after moving something: it also covers the publishes that deliberately
// leave the generation alone - the playground's blend and a gains-only republish - which
// the generation wait above cannot see at all, and which a fixed sleep against a
// Priority::low worker can only guess at. Loud, counted failure on timeout, because a
// timeout means the measurement below is reading the previous spectrum.
static bool awaitSpectrumIdle (const FractalAdditiveAudioProcessor& p, const char* what,
                               double timeoutMs = 15000.0)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (p.isSpectrumBuildPending())
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > timeoutMs)
        {
            std::printf ("  [FAIL] the spectrum worker went idle within %.0f ms (%s)\n", timeoutMs, what);
            ++failures;
            return false;
        }
        juce::Thread::sleep (1);
    }
    return true;
}

struct Ev { int sample; bool on; int note, ch; int vel = 0; };

// THE USER'S PRESET FOLDER IS NOT A SCRATCH DIRECTORY, so this suite does not write in it.
//
// Documents/Galactic Sting/Presets holds the player's own patches, and this suite used to
// write its presets there because the real save/load path knew no other folder. run 48
// saved through savePreset() - which writes ".gsting" - and then deleted
// "<name>.fibpreset", so its own two deleteFile() calls deleted nothing and run48-A.gsting
// and run48-B.gsting sat in the user's folder beside their own patches until somebody
// removed them by hand. zzz-verify-mix-A/B survived the same way when a run died before
// its cleanup - and, measured on 2026-08-29, ALSO when two RhythmVerify processes ran at
// once: the second process's files landed in the first one's end-of-run audit, because
// both were writing the same directory. Promising to tidy up cannot fix either.
//
// So the whole run is redirected to a scratch directory of its own, named after this
// PROCESS (setPresetFolderForTests). Two things are then checked at the end: that the
// user's real folder is byte-for-byte the list it was before anything ran, and that the
// scratch folder is empty - the second is the same per-site cleanup audit as before, kept
// because a leak nobody is told about is a leak that comes back.
static juce::StringArray presetFolderInventory (const juce::File& dir)
{
    juce::StringArray names;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false,
                                             juce::String ("*") + FractalAdditiveAudioProcessor::kPresetExt))
        names.add (f.getFileName());
    names.sort (true);
    return names;
}

//==============================================================================
// THE PHASE FADER UNDER A HELD NOTE (run 53), lifted out of main() so it can be
// re-run ON ITS OWN: `RhythmVerify --phase`.
//
// The whole suite takes minutes, and a check that is expensive to re-run is a check
// that gets re-run less often than it should. This block is the one that a change to
// the phase engine has to be measured against, and its sabotage pass - break the fix,
// watch the assertion go red, undo it, watch it go green - is only honest if running
// it four times is cheap. It was not, so this is now a function.
static void runPhaseFaderChecks()
{
    // --- run 53: THE PHASE FADER UNDER A HELD NOTE ----------------------------------
    //
    // "cuando muevo el fader de fase mientras el sonido suena el volumen aumenta pero no
    //  se produce el efecto o no del todo, es extrano. quiero que el aumento de volumen al
    //  desfasar los parciales sea solo por los parciales desfasandose y por la
    //  normalizacion no arbitrario."
    //
    // He is describing a consequence arriving without its cause. The normalisation law is
    // blended across the fader because the two ends of it are up to 20.70 dB apart, and it
    // used to be blended from the LIVE PARAMETER - while a held note's partials do not
    // re-phase until something moves them. So the gain moved and the crest factor it is
    // compensating did not, which is the definition of arbitrary loudness.
    //
    // Two claims here, and they are different claims:
    //   (A) with no transition selected the phases CANNOT move under a held note, so the
    //       LEVEL MUST NOT MOVE EITHER.
    //   (B) with Morph on the phases DO move, over Morph Time, and they must ARRIVE.
    //       Arrival is measured against a CONTROL voice played at the destination fader
    //       position from its own note-on: it advances at exactly the same increments, so
    //       once the move has landed the two phase arrays are the same array, partial for
    //       partial. Nothing else can produce that agreement.
    //
    // And the level claim is separable because of a fact about sums of sines: the RMS of a
    // partial bank does not depend on the partials' phases AT ALL. So every dB of level
    // change across the gesture is the normalisation law, and the phases show up in the
    // CREST factor instead. Both are printed, both are asserted.
    {
        std::printf ("\nrun 53: the Phase fader under a HELD note - the level follows the phases:\n");
        constexpr double sr53   = 48000.0;
        constexpr int    eb53   = 256;
        constexpr int    warm   = 60;   // blocks before the note: let the builder land
        constexpr int    hold   = 94;   // ~0.5 s of steady note before the fader moves
        constexpr int    after  = 190;  // ~1.0 s after it, so a 150 ms move has long landed
        constexpr int    kPh    = 2048;

        struct Take
        {
            std::vector<float>  x;            // the rendered left channel, whole run
            std::vector<double> ph;           // the running bank's phases at the end
            int    nph  = 0;
            double snd  = -1.0;               // where the SOUNDING phases sit on the fader
            int    mark = 0;                  // sample index at which the fader was moved
            double lawDb = 0.0;               // 20log10(rmsN / min(rmsN,peakN)) for this spectrum
            double cents = 0.0;               // worst detune the re-phase applied, in cents
            long long sm[shepard::kMaxCopies] = {};             // Shepard grid indices of the copies that were sounding
            int    sc[shepard::kMaxCopies] = {};                // ...and how many partials each of them renders
            double sw[shepard::kMaxCopies] = {};                // ...and how open each one's window is
            int    nsm = 0;
            int    probeSlot = 0;             // which oscillator slot the phase probe read
            long long probeM = 0;             // ...and which grid index was living in it
        };

        // One take. `from` is where the fader starts (and where the note-on seeds the
        // phases); `to` is where it is dragged to at the mark. from == to is a control.
        // `bump` also changes the SPECTRUM at the mark, so the fader move lands while a
        // real morph is in flight - the case where alignPhase[] is audible and where a
        // correction that reached only one bank would null a partial outright. The
        // rebuild is awaited BEFORE the next block renders, in every take, so the new
        // template lands on the same block number in the take and in its control and the
        // phase comparison stays an equality rather than a race.
        // `dw` selects the OTHER transition: Dry/Wet instead of Morph, which answers the
        // same gesture by crossfading the two phase sets rather than detuning anything.
        auto take = [&] (float from, float to, bool morphOn, float tSec,
                         bool bump = false, bool shep = false, bool dw = false) -> Take
        {
            Take r;
            auto p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr53, eb53);
            auto s = [&] (const char* id, float v)
            {
                if (auto* q = p->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v));
                p->pollParametersForTests();
            };
            // A perfectly flat, fully-sustained note: any level change we measure is the
            // normalisation and not an envelope still moving. The filter envelope is taken
            // out for the same reason (the filter itself may stay - it is linear and its
            // magnitudes do not depend on phase, so it cancels exactly in every ratio).
            s ("ampAttack", 0.002f); s ("ampDecay", 0.002f); s ("ampSustain", 1.0f);
            s ("ampCurveA", 0.0f);   s ("ampCurveD", 0.0f);
            s ("filterAttack", 0.002f); s ("filterSustain", 1.0f); s ("filterEnvAmount", 0.0f);
            s ("morph",  (morphOn && ! dw) ? 1.0f : 0.0f);
            s ("morphTime", tSec);
            s ("dryWet", dw ? 1.0f : 0.0f);
            s ("dryWetTime", tSec);
            s ("shepOn", shep ? 1.0f : 0.0f);
            // A FAST rise, so copies really are claimed and re-seeded DURING the gesture.
            // At the 8 s default a 150 ms move finishes long before any slot is recycled,
            // and the trap this case exists for - a re-entering copy that takes the
            // correction on top of its own re-seeding and so travels twice as far - would
            // never be reached at all. At 500 ms a slot turns over every ~62 ms.
            if (shep) s ("shepTime", 500.0f);
            s ("phaseMode", from);
            awaitSpectrumIdle (*p, "run 53 setup");

            juce::AudioBuffer<float> b (2, eb53);
            auto block = [&] (juce::MidiBuffer m)
            {
                b.clear(); p->processBlock (b, m);
                const float* L = b.getReadPointer (0);
                r.x.insert (r.x.end(), L, L + eb53);
            };
            for (int i = 0; i < warm; ++i) block ({});
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 45, 0.8f), 0); block (std::move (m)); }
            for (int i = 1; i < hold; ++i) block ({});
            r.mark = (int) r.x.size();
            if (bump) { s ("baseN", 5.0f); awaitSpectrumIdle (*p, "run 53 spectrum bump"); }
            if (to != from) s ("phaseMode", to);
            for (int i = 0; i < after; ++i) block ({});

            r.ph.assign (kPh, 0.0);
            r.snd = p->voiceSoundingScatterForTests();
            r.cents = p->voiceRephaseWorstCentsForTests();
            r.nsm   = p->voiceShepardForTests (r.sm, nullptr, r.sw, r.sc, shepard::kMaxCopies);
            // WHICH BANK, AND HOW MUCH OF IT ACTUALLY SOUNDS. With Shepard off there is one
            // bank at slot 0 and activeCount is its length. With it on, each copy has its
            // own slot and its own partial count (a copy that has risen carries fewer under
            // Nyquist), and the slots above that count are advanced by nothing at all - so
            // comparing them compares two banks that are both SILENT, in a test about what
            // is heard. Read the copy whose WINDOW IS WIDEST OPEN, and only as far as it
            // sounds. Widest-open and not most-partials, which was the first attempt: the
            // copy carrying the most partials is the LOWEST one, which is the NEWEST one,
            // which is the one sitting on the bottom edge at a window value of ~1e-5 -
            // i.e. the least audible copy in the stack. A phase claim measured there is a
            // claim about something nobody can hear, and it fails for a reason
            // (a copy takes a correction only for the samples it is actually rendered on,
            // and a copy crossing the edge is rendered for part of one buffer) that has no
            // audible consequence at all. The copy the listener IS hearing must be exact.
            int base = 0, cap = kPh;
            if (r.nsm > 0)
            {
                // Among the copies that are REALLY sounding (a window of 0.05 is -26 dB,
                // well clear of the ~1e-5 an edge-crossing copy sits at), take the one
                // carrying the MOST partials - the widest evidence available about
                // something the listener can actually hear.
                int best = -1;
                for (int i = 0; i < r.nsm; ++i)
                    if (r.sw[i] >= 0.05 && (best < 0 || r.sc[i] > r.sc[best])) best = i;
                for (int i = 0; best < 0 && i < r.nsm; ++i)      // nothing open: widest anyway
                    if (best < 0 || r.sw[i] > r.sw[best]) best = i;
                base = (int) (((r.sm[best] % shepard::kSlots) + shepard::kSlots) % shepard::kSlots) * shepard::kStride;
                cap  = r.sc[best];
                r.probeSlot = base / shepard::kStride;
                r.probeM    = r.sm[best];
            }
            r.nph = std::min (cap, p->voicePhasesForTests (r.ph.data(), kPh, base));

            // The two laws, from the template that is actually sounding. rmsN is the
            // scattered end, min(rmsN, peakN) the aligned end; their ratio is the ENTIRE
            // level change the fader is allowed to make, and it is what claim (c) is
            // compared against.
            if (const auto* t = p->getLiveTemplate())
            {
                const double f0 = 110.0;                      // note 45, 12-TET, A4 = 440
                const double maxR = 0.5 * sr53 / f0;
                double sum = 0.0, sq = 0.0;
                for (size_t i = 0; i < t->partials.size(); ++i)
                {
                    if ((double) t->partials[i].ratio > maxR) break;
                    double a = (double) t->partials[i].amp;
                    if (i < t->gain.size()) a *= (double) t->gain[i];
                    sum += a; sq += a * a;
                }
                const double rmsN  = sq  > 0.0 ? 0.25 / std::sqrt (sq) : 0.0;
                const double peakN = sum > 0.0 ? 0.5  / sum            : 0.0;
                const double cap   = std::min (rmsN, peakN);
                r.lawDb = cap > 0.0 ? 20.0 * std::log10 (rmsN / cap) : 0.0;
            }
            return r;
        };

        // RMS and peak over [a, b). 110 Hz into 48 kHz makes 0.2 s exactly 22 periods, so
        // a window of 9600 samples measures a harmonic bank's RMS with no leakage at all.
        auto rms = [] (const std::vector<float>& x, int a, int b)
        {
            double s = 0.0; int n = 0;
            for (int i = std::max (0, a); i < std::min ((int) x.size(), b); ++i) { s += (double) x[i] * x[i]; ++n; }
            return n > 0 ? std::sqrt (s / n) : 0.0;
        };
        auto pk = [] (const std::vector<float>& x, int a, int b)
        {
            double m = 0.0;
            for (int i = std::max (0, a); i < std::min ((int) x.size(), b); ++i) m = std::max (m, std::abs ((double) x[i]));
            return m;
        };
        // THE PER-SAMPLE METRIC, and it is per-sample on purpose. An RMS over 5 ms is
        // structurally blind to a one-sample step - it averages it away against 240 clean
        // samples - and several defects in this file shipped behind exactly that blindness.
        auto worstStep = [] (const std::vector<float>& x, int a, int b)
        {
            double m = 0.0;
            for (int i = std::max (1, a); i < std::min ((int) x.size(), b); ++i)
                m = std::max (m, std::abs ((double) x[i] - (double) x[i - 1]));
            return m;
        };
        // ...compared RAW against BOTH controls, not as a fraction of the window's own peak.
        // Normalising by the peak is how the first version of this check was defeated: the
        // naive re-phase (write the new phases and be done) was measured at a step of
        // 0.29993 against a control's 0.05608 - five times worse - and still passed,
        // because the same artefact had lifted the peak it was being divided by. And the
        // two controls are BOTH needed: the gesture starts on the aligned waveform and ends
        // on the scattered one, and those have very different natural slopes, so a bound
        // taken from either alone is a bound on the wrong signal for half the move.
        auto stepCheck = [&] (const Take& m, const Take& a, const Take& b,
                              int settle, const char* what)
        {
            const double sm = worstStep (m.x, m.mark - eb53, m.mark + settle);
            const double sa = worstStep (a.x, a.mark - eb53, a.mark + settle);
            const double sb = worstStep (b.x, b.mark - eb53, b.mark + settle);
            std::printf ("    worst PER-SAMPLE step %.5f; the two controls %.5f and %.5f\n",
                         sm, sa, sb);
            check (sm <= std::max (sa, sb) * 1.6 + 0.01, what);
        };
        auto dB = [] (double num, double den) { return (num > 0.0 && den > 0.0) ? 20.0 * std::log10 (num / den) : 0.0; };
        // How far apart two takes' phase banks are, worst case, in degrees - plus how MANY
        // slots disagree and which one first. The counts matter: a handful of bad slots is
        // a partial that took the wrong route, and ALL of them is a bank that never
        // participated at all, and the two have nothing to do with each other.
        auto worstPhaseDegN = [] (const Take& a, const Take& b, int* bad, int* first)
        {
            const int n = std::min (a.nph, b.nph);
            double w = 0.0;
            if (bad != nullptr) *bad = 0;
            if (first != nullptr) *first = -1;
            for (int i = 0; i < n; ++i)
            {
                double d = a.ph[(size_t) i] - b.ph[(size_t) i];
                d -= std::floor (d);
                if (d > 0.5) d -= 1.0;
                const double deg = std::abs (d) * 360.0;
                if (deg > 0.5)
                {
                    if (bad != nullptr) ++*bad;
                    if (first != nullptr && *first < 0) *first = i;
                }
                w = std::max (w, deg);
            }
            return w;
        };
        auto worstPhaseDeg = [&] (const Take& a, const Take& b)
        { return worstPhaseDegN (a, b, nullptr, nullptr); };

        // ================= (A) BOTH TRANSITIONS OFF ==================================
        // The phases cannot move, so nothing may. This is the half the user felt directly.
        {
            const auto swept = take (0.0f, 1.0f, false, 0.300f);   // the gesture he played
            const auto still = take (0.0f, 0.0f, false, 0.300f);   // the control: no gesture
            const int  w = 9600;                                   // 0.2 s = 22 periods at 110 Hz
            const double before = rms (swept.x, swept.mark - w, swept.mark);
            const double afterR = rms (swept.x, swept.mark + 6 * eb53, swept.mark + 6 * eb53 + w);
            const double ctrlB  = rms (still.x, still.mark - w, still.mark);
            const double ctrlA  = rms (still.x, still.mark + 6 * eb53, still.mark + 6 * eb53 + w);
            std::printf ("  transitions OFF: fader 0 -> 1 under a held note\n"
                         "    level across the move  %+.3f dB   (control, no move: %+.3f dB)\n"
                         "    sounding fader position after the move: %.3f  (the parameter says 1.000)\n"
                         "    the two laws are %.2f dB apart on this spectrum\n",
                         dB (afterR, before), dB (ctrlA, ctrlB), swept.snd, swept.lawDb);
            check (std::abs (dB (afterR, before)) <= 0.20,
                   "with no transition selected, moving the Phase fader under a held note does"
                   " NOT change its level - the phases cannot move, so the normalisation may not");
            check (swept.snd <= 0.001,
                   "...because the level is derived from where the SOUNDING phases are,"
                   " which is still 0, and not from where the fader is");
            check (swept.lawDb > 3.0,
                   "(and the two normalisation laws really are far enough apart on this patch"
                   " for the previous behaviour to have been audible)");
            check (worstPhaseDeg (swept, still) <= 1.0e-6,
                   "and the phases themselves are untouched, partial for partial");
        }

        // ================= (B) MORPH ON: THE PHASES REALLY MOVE ======================
        {
            const float T = 0.150f;
            const auto moved = take (0.0f, 1.0f, true, T);   // seeded aligned, dragged to scattered
            const auto dest  = take (1.0f, 1.0f, true, T);   // the CONTROL: born at the destination
            const auto orig  = take (0.0f, 0.0f, true, T);   // the CONTROL: never moved
            const int  w = 9600;
            const int  settle = (int) (0.4 * sr53);          // well past a 150 ms move

            const double worstDeg = worstPhaseDeg (moved, dest);
            std::printf ("  Morph ON (%.0f ms): fader 0 -> 1 under a held note\n"
                         "    sounding fader position after the move: %.6f  (parameter 1.000)\n"
                         "    worst phase error vs a voice BORN at the destination: %.6f deg over %d partials\n"
                         "    worst detune the move applied: %.1f cents (of the partial it was applied to)\n",
                         T * 1000.0, moved.snd, worstDeg, std::min (moved.nph, dest.nph), moved.cents);
            check (moved.nph > 4, "the probe actually read a partial bank back");
            check (moved.snd >= 0.999,
                   "the sounding fader position ARRIVES at the parameter - the phases really moved");
            check (worstDeg <= 0.5,
                   "and every partial arrives at the phase the fader's own interpolation asks for,"
                   " to under half a degree, measured against a voice born there");

            // (b) NO SAMPLE-TO-SAMPLE DISCONTINUITY, per sample, over the whole move.
            stepCheck (moved, orig, dest, settle,
                       "re-phasing a held note makes no sample-to-sample discontinuity: the"
                       " worst per-sample step over the whole move is one the tone already had");

            // (c) THE LEVEL, AND WHAT EXPLAINS IT.
            const double before = rms (moved.x, moved.mark - w, moved.mark);
            const double afterR = rms (moved.x, moved.mark + settle, moved.mark + settle + w);
            const double destR  = rms (dest.x,  dest.mark  + settle, dest.mark  + settle + w);
            const double origR  = rms (orig.x,  orig.mark  + settle, orig.mark  + settle + w);
            const double crestB = before > 0.0 ? pk (moved.x, moved.mark - w, moved.mark) / before : 0.0;
            const double crestA = afterR > 0.0 ? pk (moved.x, moved.mark + settle, moved.mark + settle + w) / afterR : 0.0;
            std::printf ("    level: %+.3f dB across the move; the normalisation law alone predicts %+.3f dB\n"
                         "    against the two controls: born-at-destination %+.3f dB, never-moved %+.3f dB\n"
                         "    crest factor (peak/rms): %.3f before -> %.3f after\n",
                         dB (afterR, before), moved.lawDb, dB (afterR, destR), dB (origR, before), crestB, crestA);
            // The law HELD FIXED is exactly the never-moved control: same phases moving or
            // not, the RMS of a partial bank is phase-independent, so `orig` is the level
            // this note would have kept if the normalisation had not followed. The whole
            // difference between it and the moved take is therefore the law, and the law's
            // own predicted size is computed from the template above - not from the audio.
            check (std::abs (dB (afterR, before) - moved.lawDb) <= 0.60,
                   "the level change across the move is the NORMALISATION LAW and nothing else:"
                   " it matches the two laws' own ratio, computed from the template");
            check (std::abs (dB (origR, before)) <= 0.20,
                   "...and the control that never moved the fader did not change level at all");
            check (std::abs (dB (afterR, destR)) <= 0.20,
                   "...and the moved note ends at exactly the level of a note BORN at the"
                   " destination: the same phases, so the same law, so the same loudness");
            check (crestA < crestB * 0.95,
                   "and the CREST FACTOR fell - which is where the phases show up, since the"
                   " RMS cannot see them at all");
        }

        // ============ (B2) THE FADER MOVES WHILE A MORPH IS ALREADY IN FLIGHT =========
        // The case the traps are about. Mid-morph BOTH oscillator banks are audible at
        // once, so a correction that reached only one of them would leave a partial's two
        // copies drifting apart - at weight 0.5 they sum to ZERO and the partial vanishes
        // for the length of the fade - and the wholesale alignPhase -> phase copy at morph
        // completion would annihilate a correction in flight unless both banks carried it.
        {
            const float T = 0.300f;
            const auto moved = take (0.0f, 1.0f, true, T, true);   // fader move + spectrum change
            const auto dest  = take (1.0f, 1.0f, true, T, true);   // the same morph, born scattered
            const auto orig  = take (0.0f, 0.0f, true, T, true);   // the same morph, no fader move
            const int  settle = (int) (0.6 * sr53);
            const int  w = 9600;
            const double worstDeg = worstPhaseDeg (moved, dest);
            // A partial nulled by two banks half a cycle apart is a DIP, and a dip in the
            // middle of a fade is invisible to any endpoint check - so the whole trough is
            // measured, not just where it started and where it ended.
            //
            // Measured against the NEVER-MOVED take, which is the same note making the same
            // morph without the fader gesture. The two start equal and the law can only
            // carry the moved one UP from there (it is monotone in scatter, and phases do
            // not change an RMS), so anything that dips BELOW the control is cancellation -
            // exactly what a correction reaching one bank and not the other produces.
            // (Against the DESTINATION take it would read 0.374x for an honest reason: at
            // the start of the gesture the moved note really is 7.34 dB quieter, because it
            // is still aligned. A metric that cannot tell that from a null is no metric.)
            double dip = 1.0e9;
            for (int a = moved.mark; a + 2400 < moved.mark + settle; a += 1200)
            {
                const double m = rms (moved.x, a, a + 2400);
                const double c = rms (orig.x,  a, a + 2400);
                if (c > 1.0e-9) dip = std::min (dip, m / c);
            }
            std::printf ("  the fader moved MID-MORPH (%.0f ms, spectrum changed at the same instant):\n"
                         "    sounding fader position after it: %.6f   worst phase error vs the same"
                         " morph born scattered: %.6f deg\n"
                         "    shallowest 50 ms level trough vs the never-moved take: %.3fx\n",
                         T * 1000.0, moved.snd, worstDeg, dip);
            check (moved.snd >= 0.999,
                   "a fader move that lands mid-morph still arrives");
            check (worstDeg <= 0.5,
                   "...and every partial lands on the same phase the identical morph would have"
                   " produced had the note been played scattered - which is only true if the"
                   " correction reached BOTH banks, since the completed morph copies one over"
                   " the other wholesale");
            stepCheck (moved, orig, dest, settle,
                       "...with no sample-to-sample discontinuity anywhere in it");
            check (dip >= 0.90,
                   "...and no partial is NULLED part-way through: the level never troughs"
                   " below the take that made the same morph without the gesture, which is"
                   " what two banks half a cycle apart would do to every partial they share");
        }

        // ============ (B3) SHEPARD ON: A COPY RE-ENTERING MUST NOT MOVE TWICE =========
        // A freshly claimed Shepard slot is re-seeded at the position the sounding bank is
        // at, and then owes exactly what its siblings still owe. Seed it at the FADER and
        // hand it the debt as well and it travels twice as far; give it no debt at all and
        // one copy sits at a different crest from the rest, which the voice's single
        // normalisation number cannot describe.
        {
            const float T = 0.150f;
            const auto moved = take (0.0f, 1.0f, true, T, false, true);
            const auto dest  = take (1.0f, 1.0f, true, T, false, true);
            const auto orig  = take (0.0f, 0.0f, true, T, false, true);
            const int  settle = (int) (0.4 * sr53);
            int bad = 0, firstBad = -1;
            const double worstDeg = worstPhaseDegN (moved, dest, &bad, &firstBad);
            std::printf ("  SHEPARD on, fader 0 -> 1 under a held note:\n"
                         "    sounding fader position after it: %.6f   worst phase error: %.6f deg"
                         " over %d slots (%d disagree, first at %d)\n",
                         moved.snd, worstDeg, std::min (moved.nph, dest.nph), bad, firstBad);
            {
                juce::String slots;
                for (int i = 0; i < moved.nsm; ++i)
                    slots += juce::String ((int) (((moved.sm[i] % shepard::kSlots) + shepard::kSlots) % shepard::kSlots)) + " ";
                std::printf ("    %d copies sounding, in oscillator slots: %s\n"
                             "    probe read slot %d (grid %lld, %d partials); the control read slot %d"
                             " (grid %lld, %d partials)\n",
                             moved.nsm, slots.toRawUTF8(),
                             moved.probeSlot, moved.probeM, moved.nph,
                             dest.probeSlot,  dest.probeM,  dest.nph);
                std::printf ("    same bank vs the NEVER-MOVED take: %.6f deg"
                             " (0 would mean this copy took none of the move)\n",
                             worstPhaseDeg (moved, orig));
                // WHERE THIS COPY ACTUALLY ENDED UP ON THE FADER, read out of its own
                // phases. Against the never-moved take, partial j's phase differs by
                // exactly s * delta_j, so dividing by that partial's own delta gives s -
                // and doing it for every partial says whether the copy travelled a
                // consistent distance (one number, repeated) or a ragged one.
                {
                    const double* d = spectrum::phaseset::scatterDeltas();
                    const int b = moved.probeSlot * shepard::kStride;
                    double lo = 9.0, hi = -9.0, sum = 0.0; int cnt = 0;
                    for (int i = 0; i < std::min (moved.nph, orig.nph); ++i)
                    {
                        const double dj = d[b + i];
                        if (std::abs (dj) < 0.05) continue;      // tiny delta -> noisy divide
                        double dp = moved.ph[(size_t) i] - orig.ph[(size_t) i];
                        dp -= std::floor (dp); if (dp > 0.5) dp -= 1.0;
                        const double s = dp / dj;
                        lo = std::min (lo, s); hi = std::max (hi, s); sum += s; ++cnt;
                    }
                    int nLo = 0, nMid = 0, nHi = 0, firstHi = -1;
                    for (int i = 0; i < std::min (moved.nph, orig.nph); ++i)
                    {
                        const double dj = d[b + i];
                        if (std::abs (dj) < 0.05) continue;
                        double dp = moved.ph[(size_t) i] - orig.ph[(size_t) i];
                        dp -= std::floor (dp); if (dp > 0.5) dp -= 1.0;
                        const double s = dp / dj;
                        if (s < 0.1) ++nLo; else if (s > 0.9) { ++nHi; if (firstHi < 0) firstHi = i; }
                        else ++nMid;
                    }
                    if (cnt > 0)
                        std::printf ("    this copy's OWN fader position, read back from its phases:"
                                     " mean %.4f, range %.4f..%.4f over %d partials\n"
                                     "      %d still at 0, %d part-way, %d arrived (first arrived at index %d)\n",
                                     sum / cnt, lo, hi, cnt, nLo, nMid, nHi, firstHi);
                }
            }
            check (moved.snd >= 0.999, "a Shepard voice re-phases too, and arrives");
            check (worstDeg <= 0.5,
                   "...and every slot lands where the same stack played scattered would have"
                   " put it - a copy that took the correction on top of its own re-seeding"
                   " would have travelled twice as far");
            stepCheck (moved, orig, dest, settle,
                       "...with no sample-to-sample discontinuity while the copies rotate");
        }

        // ============ (B4) DRY/WET: A CROSSFADE, NOT A BEND ==========================
        // The other transition answers the same gesture differently, and the panel has
        // always said so: the old phases fade OUT while the new ones fade IN, with nothing
        // detuned anywhere. That last clause is the point of the mode and it is what is
        // asserted - a re-phase that reached for Morph's momentary detune here would leave
        // a pitch blip on the low partials that this mode exists to avoid.
        {
            const float T = 0.200f;
            const auto moved = take (0.0f, 1.0f, false, T, false, false, true);
            const auto dest  = take (1.0f, 1.0f, false, T, false, false, true);
            const auto orig  = take (0.0f, 0.0f, false, T, false, false, true);
            const int  settle = (int) (0.5 * sr53);
            const int  w = 9600;
            const double worstDeg = worstPhaseDeg (moved, dest);
            const double before = rms (moved.x, moved.mark - w, moved.mark);
            const double afterR = rms (moved.x, moved.mark + settle, moved.mark + settle + w);
            const double destR  = rms (dest.x,  dest.mark  + settle, dest.mark  + settle + w);
            double dip = 1.0e9;
            for (int a = moved.mark; a + 2400 < moved.mark + settle; a += 1200)
            {
                const double m = rms (moved.x, a, a + 2400);
                const double c = rms (orig.x,  a, a + 2400);
                if (c > 1.0e-9) dip = std::min (dip, m / c);
            }
            std::printf ("  DRY/WET on (%.0f ms): fader 0 -> 1 under a held note\n"
                         "    sounding fader position after it: %.6f   worst phase error: %.6f deg\n"
                         "    level %+.3f dB (law predicts %+.3f); vs born-at-destination %+.3f dB;"
                         " shallowest trough %.3fx\n"
                         "    worst detune applied: %.3f cents\n",
                         T * 1000.0, moved.snd, worstDeg,
                         dB (afterR, before), moved.lawDb, dB (afterR, destR), dip, moved.cents);
            check (moved.snd >= 0.999,
                   "with DRY/WET selected the fader re-phases a held note too, and arrives");
            check (worstDeg <= 0.5,
                   "...landing on exactly the phases a note born at the destination has");
            check (moved.cents == 0.0,
                   "...and it does it WITHOUT DETUNING ANYTHING - which is the whole"
                   " difference between this mode and Morph, and the reason to pick it");
            stepCheck (moved, orig, dest, settle, "...with no sample-to-sample discontinuity");
            check (dip >= 0.90,
                   "...and no partial nulls between the two banks on the way across");
            check (std::abs (dB (afterR, before) - moved.lawDb) <= 0.60
                   && std::abs (dB (afterR, destR)) <= 0.20,
                   "...and the level it ends on is the destination's own, reached by the"
                   " normalisation law and nothing else");
        }

        // ============ (B5) DRY/WET *AND* SHEPARD: THE TWO BANKS DISAGREE =============
        // The one case where a Shepard copy's two oscillator banks are legitimately at
        // DIFFERENT fader positions at the same time: mid phase-crossfade the running bank
        // is still the old phase set and the aligned bank is already the new one. A copy
        // claimed while that is in flight has to be seeded into each bank at THAT bank's
        // position - seed both at the running bank's and the hand-over at the end of the
        // fade drops that copy back onto the old phases, so the whole stack moves except
        // the one copy that happened to enter the window during the gesture, and it then
        // rises to full loudness carrying the wrong crest for seconds. The stack is run
        // fast on purpose so a copy really is claimed inside the gesture.
        {
            // LONGER than the 500 ms rise, so a copy is GUARANTEED to be claimed while
            // the crossfade is in flight - which is the entire case this block is about.
            const float T = 0.600f;
            const auto moved = take (0.0f, 1.0f, false, T, false, true, true);
            const auto dest  = take (1.0f, 1.0f, false, T, false, true, true);
            const auto orig  = take (0.0f, 0.0f, false, T, false, true, true);
            const int  settle = (int) (0.85 * sr53);
            const double worstDeg = worstPhaseDeg (moved, dest);
            std::printf ("  DRY/WET *and* SHEPARD, fader 0 -> 1 under a held note:\n"
                         "    sounding fader position after it: %.6f   worst phase error: %.6f deg"
                         " over %d slots of grid %lld; detune applied %.3f cents\n",
                         moved.snd, worstDeg, moved.nph, moved.probeM, moved.cents);
            check (moved.snd >= 0.999, "the stack re-phases by crossfade, and arrives");
            check (worstDeg <= 0.5,
                   "...including a copy claimed while the crossfade was in flight, whose two"
                   " banks are at two different fader positions at once");
            stepCheck (moved, orig, dest, settle, "...with no sample-to-sample discontinuity");
            check (moved.cents == 0.0, "...and still without detuning anything");
        }
    }

}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ONE BLOCK, ON DEMAND: only the Phase-fader checks. See runPhaseFaderChecks.
    if (argc > 1 && std::strcmp (argv[1], "--phase") == 0)
    {
        runPhaseFaderChecks();
        std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
        return failures == 0 ? 0 : 1;
    }

    const juce::File realPresets = FractalAdditiveAudioProcessor::getPresetFolder();

    // A dropping from a RUN THAT DIED EARLY is in the folder before this one starts, so
    // the end-of-run diff sees it as one of the user's own files and never mentions it
    // again: the leak becomes permanent and invisible on its second day. Anything wearing
    // the scratch prefix can only be ours, so take it away first and SAY SO - this run's
    // own verdict then covers only this run's own mess.
    //
    // ONLY THE OLD ONES, though. There is exactly one preset folder on the machine and it
    // is the USER'S, so two copies of this suite running at once - a second checkout, a
    // second agent - share it. Sweeping every scratch file would delete the presets a
    // live run is in the middle of using and fail it for something it did not do. A file
    // still being used is seconds old; a dead run's leavings are not, so age is what
    // separates them.
    int foreignScratchAtStart = 0; (void) foreignScratchAtStart; // reported only: the redirect below is the real protection
    {
        const juce::int64 cutoffMs = juce::Time::currentTimeMillis() - 30 * 60 * 1000;
        int stale = 0, live = 0;
        for (const auto& f : realPresets
                                 .findChildFiles (juce::File::findFiles, false,
                                                  juce::String (kScratchPrefix) + "*"
                                                      + FractalAdditiveAudioProcessor::kPresetExt))
        {
            if (f.getLastModificationTime().toMilliseconds() > cutoffMs) { ++live; continue; }
            std::printf ("  [swept] a previous run left \"%s\" in the user's preset folder\n",
                         f.getFileName().toRawUTF8());
            f.deleteFile();
            ++stale;
        }
        if (stale > 0)
            std::printf ("  %d stale scratch preset(s) removed before starting\n", stale);
        // Not an assertion: it is a statement about the MACHINE, not about the product.
        // It is also the thing to read first when this run's timings look impossible -
        // a second suite on the same CPU is why a wall-clock check goes red for nothing.
        if (live > 0)
            std::printf ("  NOTE: %d fresh scratch preset(s) present - another run of this "
                         "suite may be in progress; timings and the folder audit below are "
                         "not trustworthy while that is true\n", live);
        foreignScratchAtStart = live;
    }

    const juce::StringArray presetsAtStart = presetFolderInventory (realPresets);

    // ...and then do not touch it again. Sweeping old droppings is a courtesy; the REAL
    // protection is that nothing this suite saves ever lands in the user's folder at all.
    // The redirect goes in before the first processor exists, so every block inherits it.
    const juce::File scratchPresets = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                          .getChildFile ("fiblo-verify-presets-"
                                              + juce::String (juce::Time::getHighResolutionTicks() & 0xffffffLL));
    FractalAdditiveAudioProcessor::setPresetFolderForTests (scratchPresets);

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
            s ("dryWet", 0.0f);   // ...and NO crossfade under it. The panel asks one TRANSITION question now and its
            // default answer is Dry/Wet, so a rig that only says "morph off" is asking for a
            // 0.3 s crossfade on every spectrum change it makes. Every rig below states it.
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
                awaitSpectrumIdle (*p, "one step of the Base sweep");
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
            s ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
            s ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
                    // A FLOOR AGAINST AN EMPTY SWEEP, NOT AN INVENTORY. All this has to
                    // prove is that the probe handed us controls to measure - a sweep
                    // that walks nothing reports "0 too narrow" and looks like a pass,
                    // which is the failure being guarded against.
                    //
                    // It was 6, and the view with the fewest combos had exactly 6, so
                    // deleting the phaseGlide box (its mode comes from the SPECTRUM
                    // panel's Morph / Dry-Wet transition now) failed this check while
                    // every real measurement still said "0 too narrow for their own
                    // words". A threshold pinned to the current inventory fails on any
                    // deletion and teaches the next person to ignore it, so it sits low
                    // enough to mean what it says.
                    check (P.comboTexts.size() >= 4 && P.buttonFits.size() >= 8,
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
                { 1, 2, 1, "search, from all tunings (Find + REALTIME)" },
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
                   "slope|fractal, fractal|transition)");
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
            check (names (cust) == "engine|both, slope|fractal, fractal|transition, transition|range",
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

            // (2b) NOTE NAMES INSTEAD OF DEGREE NUMBERS - the option, off by default,
            //      remembered between sessions.
            //
            //      "crees que podriamos implementar esta notacion en el teclado como
            //       opcion? ... solo para tunings regulares y para JI ... quiero que por
            //       default este off esa opcion pero que se pueda activar y el sinte se
            //       acuerde de si el usuario la activo o no"
            //
            //      A name is far wider than a digit, so everything the period-tag work
            //      above pinned - the fit, the border clearance, the shared column - has
            //      to hold again for text the keyboard never used to draw. And a name is
            //      the one label on this keyboard that can be WRONG rather than missing:
            //      the accidentals ride on ASCII letters, so drawing "Cc" in any other
            //      face prints the letters C and c, which no rectangle check can see.
            {
                std::printf ("note names on the keyboard (ups and downs):\n");
                auto edoScl = [] (int n)
                {
                    juce::String s;
                    s << "! e.scl\n" << juce::String (n) << "-EDO\n " << juce::String (n) << "\n!\n";
                    for (int i = 1; i <= n; ++i)
                        s << " " << juce::String (1200.0 * (double) i / (double) n, 6) << "\n";
                    return s;
                };
                juce::String err;
                // The user's own preference. Restored at the end of this block: it lives in
                // a GLOBAL settings file, and a verifier that left it flipped would have
                // silently changed the setting on the machine it ran on.
                const bool userPref = p.getKeyboardNoteNames();

                // OFF BY DEFAULT, and off means numbers - not blank keys.
                p.loadScalaText (edoScl (12), "edo12", err);
                ed->setNoteNamesForTests (false);
                ed->resized();
                {
                    const auto np = ed->noteNameProbeForTests();
                    check (! np.optionOn && ! np.showing,
                           "with the option off the keyboard numbers its degrees, as it always did");
                    check (np.reason.isEmpty(),
                           "and offers no excuse for it - nothing was asked for");
                    const auto tags = ed->keyboardPeriodTagsForTests();
                    check (! tags.empty() && tags.front().zeroText == "0",
                           "degree 0 still wears a \"0\"");
                }

                // ON, with an EDO that has a spelling.
                ed->setNoteNamesForTests (true);
                ed->resized();
                {
                    const auto np = ed->noteNameProbeForTests();
                    std::printf ("  12-EDO: %d names, widest \"%s\", %.1f pt at a %d px key\n",
                                 np.namedCount, np.widest.toRawUTF8(), np.fontAtMinKey,
                                 ScaleKeyboard::kCellW);
                    check (np.edo == 12 && np.namedCount == 12,
                           "12-EDO is recognised and spelled");
                    check (np.showing && np.reason.isEmpty(),
                           "and the keyboard is drawing the names");
                    const auto tags = ed->keyboardPeriodTagsForTests();
                    check (! tags.empty() && tags.front().zeroText == "C",
                           "degree 0 now wears its NAME - \"C\", not \"0\"");
                    check (! tags.empty() && tags.front().names,
                           "and the probe knows it is measuring a name, not a digit");
                }

                // OFF AGAIN, AND ON AGAIN, ON THE SAME TUNING. The one transition where
                // the scale does not move: the degree count is 12 before and after, so
                // nothing about the tuning tells the keyboard to change its mind. It is
                // therefore the only check that can catch a keyboard which caches what its
                // keys say and goes on drawing the names the user just switched off - and
                // every OTHER case in this block changes the degree count, so all of them
                // would keep passing.
                for (int round = 0; round < 2; ++round)
                {
                    ed->setNoteNamesForTests (false);
                    ed->resized();
                    const auto off  = ed->noteNameProbeForTests();
                    const auto offT = ed->keyboardPeriodTagsForTests();
                    check (! off.showing && ! offT.empty() && offT.front().zeroText == "0"
                           && ! offT.front().names,
                           "switching the option off puts the NUMBERS back on the very same tuning");
                    ed->setNoteNamesForTests (true);
                    ed->resized();
                    const auto on  = ed->noteNameProbeForTests();
                    const auto onT = ed->keyboardPeriodTagsForTests();
                    check (on.showing && ! onT.empty() && onT.front().zeroText == "C"
                           && onT.front().names,
                           "...and switching it back on puts the names back");
                }

                // EVERY EDO THIS CAN EVER SPELL, at the NARROWEST key the keyboard draws.
                // Not the four this verifier happens to load: the widest name in the whole
                // notation belongs to an EDO nobody will open, and that is exactly where a
                // squeezed label ships. Either the layout holds, or the keyboard refuses
                // the names and keeps its numbers - there is no third outcome.
                {
                    const auto np   = ed->noteNameProbeForTests();
                    const auto tags = ed->keyboardPeriodTagsForTests();
                    check (np.face != nullptr, "the accidental face is embedded and loaded");
                    if (np.face != nullptr && ! tags.empty())
                    {
                        const float cellW = (float) ScaleKeyboard::kCellW;
                        const float cellH = tags.front().cellH;
                        const auto  label = tags.front().label;
                        const auto  key   = ScaleKeyboard::cellRectForTests (cellW, cellH);
                        const float col   = ScaleKeyboard::degreeCentreXForTests (cellW, cellH, label);
                        int spellable = 0, shown = 0, refused = 0;
                        int squeezed = 0, border = 0, offCol = 0, stacked = 0;
                        int widestEdoShown = 0; juce::String widestShown;
                        float widestShownW = -1.0f;
                        juce::String firstBad;
                        for (int m = 1; m <= notenames::kMaxEdo; ++m)
                        {
                            const auto names = notenames::edoNames (m);
                            if (names.empty()) continue;
                            ++spellable;
                            // The widest name of THIS EDO, measured the way the keyboard
                            // measures it rather than counted in characters.
                            juce::String widest;
                            float bestW = -1.0f;
                            for (const auto& s : names)
                            {
                                const juce::String js (s);
                                const float w = ScaleKeyboard::labelTextWidth (js, 20.0f, np.face);
                                if (w > bestW) { bestW = w; widest = js; }
                            }
                            // The three-character period number is the worst case above it.
                            const auto z = ScaleKeyboard::layOutPeriodTagForCell (
                                cellW, cellH, label, "-10", juce::String (names[0]), widest, np.face);
                            if (z.zeroFont < ScaleKeyboard::kMinNameFont) { ++refused; continue; }
                            ++shown;
                            if (bestW > widestShownW)
                            { widestShownW = bestW; widestShown = widest; widestEdoShown = m; }
                            // Everything the numbers already had to satisfy, for a name.
                            const float need = ScaleKeyboard::labelTextWidth (
                                juce::String (names[0]), z.zeroFont, np.face);
                            if (need > z.zero.getWidth())
                            { ++squeezed; if (firstBad.isEmpty()) firstBad = juce::String (m) + "-EDO"; }
                            const auto ti = z.tag.getSmallestIntegerContainer().toFloat();
                            const auto zi = z.zero.getSmallestIntegerContainer().toFloat();
                            const float e = juce::jmin (
                                juce::jmin (ti.getY() - key.getY(), key.getBottom() - zi.getBottom()),
                                juce::jmin (juce::jmin (ti.getX() - key.getX(), key.getRight() - ti.getRight()),
                                            juce::jmin (zi.getX() - key.getX(), key.getRight() - zi.getRight())));
                            if (e < 1.0f) ++border;
                            if (std::abs (z.zero.getCentreX() - col) > 0.51f) ++offCol;
                            if (z.tag.getBottom() > z.zero.getY() + 0.01f) ++stacked;
                        }
                        std::printf ("  %d spellable EDOs: %d fit a %d px key (widest shown \"%s\","
                                     " %d-EDO), %d refused and kept their numbers\n",
                                     spellable, shown, ScaleKeyboard::kCellW,
                                     widestShown.toRawUTF8(), widestEdoShown, refused);
                        check (shown > 0, "some EDOs' names do fit a key");
                        check (refused > 0,
                               "and some do NOT - the fit is a real gate, not a formality");
                        check (squeezed == 0,
                               juce::String ("no name the keyboard agrees to draw is squeezed to fit"
                                             + (firstBad.isEmpty() ? juce::String() : " (" + firstBad + ")")).toRawUTF8());
                        check (border == 0, "and none of them touches the key's border");
                        check (offCol == 0, "and every degree-0 NAME stays on the degree column");
                        check (stacked == 0, "and the period number still sits ABOVE the name, never beside it");
                    }
                }

                // "solo para tunings regulares y para JI": everything else keeps its
                // numbers AND SAYS WHY, in whichever language is on screen. An option that
                // silently does nothing is the failure the request named.
                {
                    juce::String bp ("! bp.scl\nBohlen-Pierce\n 13\n!\n");
                    for (int i = 1; i <= 13; ++i)
                        bp << " " << juce::String (1200.0 * std::log2 (3.0) * (double) i / 13.0, 6) << "\n";
                    p.loadScalaText (bp, "bp", err);
                    ed->resized();
                    const auto en = ed->noteNameProbeInLanguage (false);
                    const auto ca = ed->noteNameProbeInLanguage (true);
                    std::printf ("  Bohlen-Pierce: EN \"%s\" / CA \"%s\"\n",
                                 en.reason.toRawUTF8(), ca.reason.toRawUTF8());
                    check (en.optionOn && ! en.showing,
                           "with the option still ON, a 3/1-period scale keeps its degree numbers");
                    check (en.reason.isNotEmpty() && ca.reason.isNotEmpty(),
                           "and the menu says why, in both languages");
                    check (en.reason != ca.reason, "and the two languages really are two texts");
                    const auto tags = ed->keyboardPeriodTagsForTests();
                    check (! tags.empty() && tags.front().zeroText == "0" && ! tags.front().names,
                           "and the keys are back to numbers, not blank");
                }
                {
                    // JUST INTONATION - the other half of "solo para tunings regulares y
                    // para JI", and the half this build cannot draw. The scale is refused
                    // like any other, but the reason must name HEJI: told only "not an
                    // equal division of the octave", a player reads it as "your scale is
                    // wrong" when what is missing is a font.
                    p.loadScalaText ("! ji.scl\nPtolemy\n 7\n!\n 9/8\n 5/4\n 4/3\n 3/2\n 5/3\n 15/8\n 2/1\n",
                                     "ji", err);
                    ed->resized();
                    const auto en = ed->noteNameProbeInLanguage (false);
                    const auto ca = ed->noteNameProbeInLanguage (true);
                    std::printf ("  Ptolemy (JI): EN \"%s\"\n", en.reason.toRawUTF8());
                    check (! en.showing && en.reason.isNotEmpty(),
                           "a just-intonation scale keeps its numbers too - HEJI is not in this build");
                    check (en.reason.contains ("HEJI") && ca.reason.contains ("HEJI"),
                           "and it says so by name, in both languages - not \"not an EDO\"");
                    check (en.reason != ca.reason, "and the two really are two texts");

                    // ...while a scale of the same SIZE written in cents is NOT called just.
                    // The two sound within a cent of each other; only the source separates
                    // them, and calling a tempered scale "just intonation" would be a lie
                    // the player has no way to check.
                    p.loadScalaText ("! near.scl\nPtolemy in cents\n 7\n!\n 203.910\n 386.314\n"
                                     " 498.045\n 701.955\n 884.359\n 1088.269\n 1200.000\n",
                                     "near", err);
                    ed->resized();
                    const auto npc = ed->noteNameProbeForTests();
                    check (! npc.showing && ! npc.reason.contains ("HEJI"),
                           "the same scale written in CENTS is not called just intonation");
                }
                {
                    // 4-EDO: an equal division of the octave with no diatonic skeleton to
                    // hang letters on. It must come back as NUMBERS with a reason, not as
                    // four plausible-looking names - the speller returns nothing here, and
                    // this is the check that the UI honours that rather than inventing.
                    p.loadScalaText (edoScl (4), "edo4", err);
                    ed->resized();
                    const auto np = ed->noteNameProbeForTests();
                    std::printf ("  4-EDO: %d names, reason \"%s\"\n",
                                 np.namedCount, np.reason.toRawUTF8());
                    check (np.edo == 4 && np.namedCount == 0 && ! np.showing,
                           "4-EDO is an equal division of the octave and still gets no names");
                    check (np.reason.isNotEmpty(), "and the menu says why rather than doing nothing");
                    const auto tags = ed->keyboardPeriodTagsForTests();
                    check (! tags.empty() && tags.front().zeroText == "0" && ! tags.front().names,
                           "and its keys are numbered, not blank");
                }

                // THE PART THE USER ASKED FOR TWICE: it is remembered. Not in the patch -
                // in the global settings - so it cannot be undone by loading a project or
                // a preset saved before the option existed.
                {
                    p.loadScalaText (edoScl (31), "edo31", err);
                    ed->setNoteNamesForTests (true);
                    ed->resized();
                    check (p.getKeyboardNoteNames(), "turning the option on writes the preference");

                    juce::MemoryBlock mb;
                    p.getStateInformation (mb);
                    p.setStateInformation (mb.getData(), (int) mb.getSize());
                    check (p.getKeyboardNoteNames(),
                           "and a full save/load of the plugin's state leaves it on - it is a "
                           "preference, not patch state");

                    // A FRESH EDITOR, built the way the host builds one when the window is
                    // reopened. This is "que se abra ya con ese setting al re-abrirse".
                    std::unique_ptr<juce::AudioProcessorEditor> e2 (p.createEditor());
                    auto* ed2 = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (e2.get());
                    check (ed2 != nullptr, "a second editor is built");
                    if (ed2 != nullptr)
                    {
                        ed2->setSize (ed->getWidth(), ed->getHeight());
                        ed2->resized();
                        const auto np = ed2->noteNameProbeForTests();
                        check (np.optionOn, "and it opens with the option already on");
                        check (np.showing && np.edo == 31 && np.namedCount == 31,
                               "with 31-EDO's names on the keys from the first paint");
                    }

                    // ...and off is remembered just as hard.
                    ed->setNoteNamesForTests (false);
                    check (! p.getKeyboardNoteNames(), "turning it off writes the preference too");
                    std::unique_ptr<juce::AudioProcessorEditor> e3 (p.createEditor());
                    auto* ed3 = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (e3.get());
                    if (ed3 != nullptr)
                    {
                        ed3->setSize (ed->getWidth(), ed->getHeight());
                        ed3->resized();
                        check (! ed3->noteNameProbeForTests().optionOn,
                               "and a fresh editor opens with numbers again");
                    }
                }

                // Put the machine back the way we found it: 12-TET (what the block above
                // left, and what the register checks below are written against) and the
                // USER'S OWN note-name preference, which lives in a global settings file
                // that a verifier has no business rewriting.
                ed->setNoteNamesForTests (userPref);
                p.clearScala();
                ed->resized();
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
                // THE BUTTON IS REPAINTED BY THE PANEL'S OWN 30 Hz TIMER, and this harness
                // has no message loop, so without this tick the text read below is whatever
                // the constructor happened to set - priced for a different spec mode, before
                // any tuning was loaded. The check then compared a fresh `ms` against a stale
                // string and its verdict was decided by nothing but how fast this machine ran
                // the one-off adaptNsPerPair() probe: measured 724, 1054, 1120, 1134, 1136 and
                // 1219 ms across six runs of the same binary, either side of the 950 ms line,
                // so the same product went red or green by the coin-toss of a stopwatch.
                ed->tickTimerForTests();
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
            // SETTLED means "nothing is owed and nothing is in flight", and waiting only
            // while the search is RUNNING does not mean that. The launch is throttled by
            // kRtAdaptTicks, so at the moment this is entered the search usually has not
            // started yet: the old loop fell straight through, started the search inside
            // its own three-tick tail, and read the counters with the result still being
            // computed. Measured: "C-E-G held -> 1 started, 0 applied" on one run in three,
            // with the product behaving perfectly - a failure that says nothing about the
            // follower and everything about the harness. Wait for QUIESCENCE instead: the
            // counters standing still, and nothing running, for longer than the throttle.
            auto settle = [&p, &tick]
            {
                int quiet = 0, lastStarts = -1, lastApplies = -1;
                for (int i = 0; i < 2000 && quiet < 30; ++i)
                {
                    tick (1);
                    const int s = p.getRealtimeStarts(), a = p.getRealtimeApplies();
                    if (p.isSpectrumAdaptRunning() || s != lastStarts || a != lastApplies) quiet = 0;
                    else                                                                   ++quiet;
                    lastStarts = s; lastApplies = a;
                }
            };

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
        s ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
        awaitSpectrumIdle (*p, "the Odd template before the note");
        pump (40, nullptr);                             // ...and the engine settled on it
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

        // RECORD ACROSS THE PUBLISH. The fade being measured BEGINS when the new template
        // lands, so waiting for the generation first and recording afterwards would miss
        // the very thing under test - while a fixed 25 ms sleep and a fixed 1.2 s window
        // is a bet that the builder wins its race, and the builder is Priority::low: under
        // load it has been measured taking 1.9 s to publish, which put the rise at the end
        // of the window with no settled tail behind it and read as a step. So the recording
        // FOLLOWS the builder - short slices until the new generation is live, then 0.6 s
        // more for the fade to finish and settle - and the 10%-90% search below finds the
        // rise wherever in the recording it happens to be.
        const auto genBeforeWiden = p->getTemplateGeneration();
        s ("spectrumRange", 10.0f);               // full range: harmonic 6 fades IN from zero
        std::vector<float> tr;
        for (int i = 0; i < 400 && p->getTemplateGeneration() == genBeforeWiden; ++i)
            pump ((int) (0.010 * sr / eb) + 1, &tr);
        check (p->getTemplateGeneration() != genBeforeWiden,
               "the widened spectrum really was published (everything below measures its arrival)");
        pump ((int) (0.6 * sr / eb), &tr);

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
            s ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
                // "8 template changes 300 ms of audio apart" is only true if each one has
                // actually LANDED before its 300 ms is rendered. A 25 ms sleep was a bet on
                // a Priority::low builder; lose it and a step lands late, or inside the
                // next segment, or not at all, and the flux this run compares against the
                // control is measuring a different number of changes.
                const auto genStep = p->getTemplateGeneration();
                s ("baseN", (stepIdx & 1) != 0 ? 1.0f : 3.0f);
                awaitTemplateChange (*p, genStep, "run 38: one step of the template stream");
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
            r->set ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
            awaitSpectrumIdle (*r->p, "the widened spectrum that arms the fade");
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
            setQ ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
        setR ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
        // Short envelope segments so the chord change itself is over within ~20 ms and the
        // only event left in the analysis window is the spectrum landing.
        setR ("ampAttack", 0.001f); setR ("ampDecay", 0.001f);
        setR ("ampSustain", 1.0f);  setR ("ampRelease", 0.003f);
        setR ("fractalLevel", 1.0f); setR ("curveSlope", 1.0f); setR ("baseN", 1.0f);

        juce::AudioBuffer<float> b (2, eb);
        std::vector<float> x;                  // the recorded stream
        struct Mark { size_t at; long long clock; juce::uint32 gen, rev; int applies, starts; bool running; };
        std::vector<Mark> marks;
        // Every completed search, start and end on the same sample clock the landings are
        // timed on. Sampled per BLOCK so a short search cannot fall between two samples.
        std::vector<std::pair<long long, long long>> searchSpans;
        bool      searchWasRunning = false;
        long long searchRunFrom    = 0;
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
                if (const bool rn = p->isSpectrumAdaptRunning(); rn != searchWasRunning)
                {
                    if (rn) searchRunFrom = clockS;
                    else    searchSpans.emplace_back (searchRunFrom, clockS);
                    searchWasRunning = rn;
                }
                if (clockS >= nextPoll)
                {
                    nextPoll += tickS;
                    p->pollParametersForTests();
                    const auto* t = p->getLiveTemplate();
                    marks.push_back ({ x.size(), clockS, t != nullptr ? t->generation : 0u,
                                       p->getSpectrumLiveRevision(),
                                       p->getRealtimeApplies(), p->getRealtimeStarts(),
                                       p->isSpectrumAdaptRunning() });
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

        // HOW LONG THE SEARCH ITSELF TOOK, in this run, on this machine. The two landing
        // budgets below are stated as "one search + two timer ticks", and that sentence can
        // only be checked against a MEASURED search: the optimisation runs on a worker
        // thread, this loop is paced to realtime so a landing time in ms is wall-clock
        // time, and the search's own cost moves with whatever else the machine is doing.
        // Spans are collected block by block (1.33 ms), not at the 60 Hz poll, because a
        // ~30 ms search can otherwise fall between two samples and read as zero.
        auto searchMsAfter = [&] (long long from) -> double
        {
            for (const auto& s : searchSpans)
                if (s.first >= from) return (double) (s.second - s.first) * 1000.0 / sr4;
            return 0.0;
        };
        constexpr double tickMs  = 1000.0 / 60.0;      // the message timer's own cadence
        const double searchMs0   = searchMsAfter (C0);
        const double searchMs2   = searchMsAfter ((long long) T2);

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
        // A SEARCH PLUS THREE POLL TICKS, MEASURED, not a flat stopwatch reading.
        //
        // The history of the old flat number is all measurements and none of them the
        // product's: 60 -> 70 when the register range became "N steps ABOVE the note",
        // 70 -> 100 when ~450 lines of run-47 TEST code shifted the same processor binary
        // path from 67 to 84 ms. It kept moving because it was timing a worker thread on
        // a shared machine: six runs of this binary at one commit read 51, 51, 67, 117 and
        // 117 ms for the first landing and 47, 47, 47, 80 and 213 for the second, so the
        // same product went green or red on how busy the machine was. A number that moves
        // for reasons outside the file cannot be an assertion - it goes red for the wrong
        // reason and the first person it inconveniences deletes it.
        //
        // What this check exists to catch is the 163 ms class of regression: the THROTTLE
        // doing the waiting, a landing that costs a search plus a 100 ms sit. That is a
        // claim about the overhead AROUND the search, so it is stated against the search
        // this run actually measured, plus the four 60 Hz boundaries the landing has to
        // cross: the tick that notices the move, the tick that publishes the result, the
        // tick this harness observes it on, and one of granularity. A throttle wait is six
        // ticks on its own and still cannot hide inside that. Measured with this bound:
        // landing 51 ms against a search of 9-11 ms, so 67 ms of allowance covers a 6x
        // slower machine before the sentence stops being checkable.
        const double budget0 = searchMs0 + 4.0 * tickMs;
        // The SECOND move is made 130 ms after the first, so the throttle really is loaded
        // and the claim being checked is narrower: it must not sit out a WHOLE throttle
        // period waiting. kRtAdaptTicks is six ticks, so that is the allowance - a landing
        // that costs a search plus the full 100 ms sit (the 163 ms class of regression this
        // was written for) still cannot fit inside it, while the poll-boundary jitter that
        // read 96 ms on a busy machine can.
        const double budget2 = searchMs2 + 6.0 * tickMs;
        std::printf ("  landings against THIS run's own searches: %.0f ms vs search %.0f + 4 ticks = %.0f;"
                     "  second %.0f ms vs search %.0f + 6 ticks = %.0f\n",
                     applyMs0, searchMs0, budget0, applyMs2, searchMs2, budget2);
        check (searchMs0 > 0.0 && searchMs2 > 0.0,
               "both moves really ran a search (the budgets below are measured against them)");
        check (applyMs0 > 0.0 && applyMs0 <= budget0,
               "the new spectrum is live within one search + two timer ticks of the chord moving");
        check (applyMs2 > 0.0 && applyMs2 <= budget2,
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
            awaitSpectrumIdle (*q, "the level-4 fractal template");

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
            awaitSpectrumIdle (*p, "playground blend at corner B");
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
            awaitSpectrumIdle (*p, "playground blend back at corner A");
            const auto* atA = p->getLiveTemplate();
            bool stillHas125 = false;
            if (atA != nullptr)
                for (const auto& pp : atA->partials)
                    if (std::abs ((double) pp.ratio - 1.25) < 1e-3) stillHas125 = true;
            check (! stillHas125, "and back at corner A none of B's partials are left behind");

            point (0.5f, 0.0f);
            awaitSpectrumIdle (*p, "playground blend at the midpoint");
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
            awaitSpectrumIdle (*p, "the pad parked at corner A before the drag");
            const auto gen0 = p->getTemplateGeneration();
            const auto rev0 = p->getSpectrumLiveRevision();
            const double dragFrom = juce::Time::getMillisecondCounterHiRes();
            for (int i = 1; i <= 20; ++i)      // a slow drag: 0.02 per frame, well under the jump threshold
            {
                sp ("pgX", (float) i * 0.02f);
                p->pollParametersForTests();
                juce::Thread::sleep (12);
            }
            const double dragMs = juce::Time::getMillisecondCounterHiRes() - dragFrom;
            awaitSpectrumIdle (*p, "the drag's last blend");
            const auto gen1 = p->getTemplateGeneration();
            const auto rev1 = p->getSpectrumLiveRevision();
            // A CADENCE IS A RATE, so it is checked as one. "+2 to +5 bumps" was really a
            // bet that twenty Thread::sleep(12) calls take 240 ms; on a busy machine they
            // take three times that, the same healthy 10 Hz throttle produces eight bumps,
            // and the check went red for the machine rather than for the code. Dividing by
            // the time the drag ACTUALLY took makes it the number the comment above claims.
            // The twenty frames go by at ~78 a second here, so "every frame" - the click
            // machine this forbids - reads about 78 Hz and the throttle reads 10-20; the
            // window below sits between them with room for the scheduler on both sides.
            const unsigned genD = gen1 - gen0, revD = rev1 - rev0;
            const double bumpHz = (double) genD * 1000.0 / std::max (1.0, dragMs);
            std::printf ("  20 drag frames over %.0f ms: generation +%u (%.1f Hz), live revision +%u\n",
                         dragMs, genD, bumpHz, revD);
            check (bumpHz > 3.0 && bumpHz < 30.0,
                   "a drag bumps the generation at the 10 Hz glide cadence - never all 60 frames");
            // ...and the amplitudes track on every frame the worker gets to. The COUNT of
            // those publishes depends on how often a Priority::low worker is scheduled (the
            // blend slot coalesces, so a starved worker publishes fewer and each carries
            // more), which is not something this file can assert; what it can assert is the
            // relationship the design rests on - republishes outnumber morph restarts, by a
            // lot. Measured 20 against 4 on an idle machine.
            check (revD >= 2 * genD && revD > 0,
                   "...while the live template really is being replaced on the way, far more often than"
                   " the generation moves");
        }

        // A JUMP is different, and must be treated as one.
        {
            const auto gen0 = p->getTemplateGeneration();
            point (1.0f, 0.0f);
            awaitSpectrumIdle (*p, "the automation jump's blend");
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

        // RULE 7: A CORNER-EDIT SESSION IS NOT PART OF THE LIVE PATCH'S HISTORY.
        //
        // The scratch surface writes through the SAME UndoManager as the live patch, so
        // every knob turned and every partial drawn while editing a corner used to land
        // on the live patch's undo stack as a step that WRITES INTO THE LIVE PATCH.
        // Measured before the fix, with the whole gesture available in the shipped GUI
        // (enter corner edit, drag the pencil, click OK, press Ctrl+Z):
        //   after leaving edit mode  gains=1 [120000:-9.00]   <- the user's own drawing
        //   after one Ctrl+Z         gains=0                  <- WIPED, and not by them
        //   after Ctrl+Y             gains=1 [190196:11.00]   <- the CORNER's drawing,
        //                                                        now on the live patch
        // Hand-drawn amplitudes are not recoverable from anywhere else, so that first
        // Ctrl+Z destroyed work. Raw undoManager.undo() on purpose: the stack itself has
        // to be clean, not merely the one key path the editor happens to use.
        {
            std::printf ("  -- undo after a corner-edit session (rule 7) --\n");
            // MIX MODE, for the same reason the top of this block uses it: in MORPH the
            // blend rewrites every continuous control sixty times a second, so a live
            // parameter proves nothing about who wrote it. In MIX the playground writes
            // nothing to the live patch, so a knob that moved, moved because undo moved it.
            sp ("pgMode", 1.0f);
            tick (6);
            // A PARAMETER ONLY REACHES THE UNDO STACK ON APVTS'S OWN TIMER, never
            // synchronously - and this harness has no message loop to run that timer, so
            // the parameter half of this defect is INVISIBLE by default: the knob checks
            // below would pass on a build that still drops a corner's values onto the
            // live patch. This is APVTS::flushParameterValuesToValueTree by hand (private
            // there, and runDispatchLoopUntil is compiled out without modal loops): the
            // same property write, with the same undo manager. Writing a value the tree
            // already holds records nothing, so this only ever pushes REAL changes.
            auto settle = [&]
            {
                tick (4);
                for (auto* ap : p->getParameters())
                    if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (ap))
                        if (auto ch = p->apvts.state.getChildWithProperty ("id", q->paramID); ch.isValid())
                            ch.setProperty ("value", (double) q->convertFrom0to1 (q->getValue()),
                                            &p->undoManager);
            };
            std::vector<spectrum::PartialGain> liveG;
            spectrum::setPartialGain (liveG, spectrum::gainKey (3.0), -9.0f);
            p->commitPartialGains (p->getPartialGains(), liveG);
            tick (2);
            settle();
            const auto  liveGains = p->getPartialGains();
            const float liveCut   = gp ("filterCutoff");
            check (liveGains.size() == 1, "the live patch carries a hand-drawn partial to lose");
            // THE USER'S OWN LAST EDIT, and deliberately a PARAMETER rather than the
            // drawing: it is what the first Ctrl+Z after the session is supposed to land
            // on, and keeping it distinct from the spectrum is what stops "undo ate the
            // drawing" and "undo did its job" from looking the same in this output.
            // A step of its own, so "undo took the drawing back" and "undo took the knob
            // back" cannot be read off the same line of output.
            p->undoManager.beginNewTransaction();
            const float gainWas = gp ("masterGain");
            sp ("masterGain", -3.0f);
            settle();
            // ...and it has to be a STEP OF ITS OWN, or the session's writes coalesce into
            // it and one Ctrl+Z would legitimately take both back - which would hide the
            // defect instead of measuring it. Plain knob turns share a transaction until
            // something opens a new one, and the pencil, the tuning box and the spectrum
            // box all do, so this is the stack any of them leaves behind.
            p->undoManager.beginNewTransaction();
            const float cornerCutWas  = p->getPlaygroundCornerParamForTests (1, "filterCutoff");
            const bool  cornerStarWas = p->isPlaygroundCornerEdited (1);   // earlier blocks left it starred

            check (p->beginPlaygroundCornerEdit (1), "entering a corner-edit session");
            tick (3);
            sp ("filterCutoff", 6543.0f);   // a knob turned INSIDE the corner's session
            tick (20);
            settle();
            std::vector<spectrum::PartialGain> cornerG;   // ...and a pencil drag in it
            spectrum::setPartialGain (cornerG, spectrum::gainKey (7.0), 11.0f);
            // THE INVARIANT ITSELF, not only its downstream effect: nothing this plugin
            // mints may reach the live patch's stack while a session is open. The unwind
            // on the way out hides a breach of this in the ordinary case - and the one
            // case it cannot cover, a boundary marker dropped off the bottom of a full
            // stack, is exactly where a stray PartialGainsAction is the unrecoverable
            // one: a parameter left behind is a number, a wiped drawing is gone.
            const int stackInSession = p->undoManager.getUndoDescriptions().size();
            p->commitPartialGains (p->getPartialGains(), cornerG);
            check (p->undoManager.getUndoDescriptions().size() == stackInSession,
                   "a pencil drag inside a corner-edit session puts NO step on the live patch's stack");
            tick (4);
            settle();

            // A BARE Ctrl+Z INSIDE THE SESSION had no edit-mode gate at all: it unwound
            // the LIVE patch's history into the scratch surface, the next pull absorbed
            // the result into the corner, and the "*" then blamed the user for a change
            // they never made. UndoManager::undo() unwinds the currently-open transaction
            // too, so there was no grace period to hide behind - measured inside the
            // first second: "fast undo: surface 4000 (starred yes); corner B now reads
            // 4000 (saved 9000)". There is no per-corner history to walk instead, so the
            // honest answer is to refuse.
            {
                const float surfaceWas = gp ("filterCutoff");
                const float storedWas  = p->getPlaygroundCornerParamForTests (1, "filterCutoff");
                check (! p->performUndo(),
                       "Ctrl+Z INSIDE a corner-edit session is refused - a corner has no history of its own");
                check (! p->performRedo(), "...and so is Ctrl+Y, for the same reason");
                tick (8);
                std::printf ("    inside the session: surface %.0f -> %.0f, corner B stored %.0f -> %.0f\n",
                             surfaceWas, gp ("filterCutoff"), storedWas,
                             p->getPlaygroundCornerParamForTests (1, "filterCutoff"));
                check (std::abs (gp ("filterCutoff") - surfaceWas) < 1.0f,
                       "...so it cannot unwind the live patch's history into the scratch surface");
                check (std::abs (p->getPlaygroundCornerParamForTests (1, "filterCutoff") - storedWas) < 1.0f,
                       "...nor replace the corner's stored sound with a copy of the live patch");
            }

            p->endPlaygroundCornerEdit();   // the OK button
            tick (4);
            settle();                       // whatever leaving wrote back reaches the stack too

            auto sameAsLive = [&]
            {
                const auto g = p->getPartialGains();
                if (g.size() != liveGains.size()) return false;
                for (size_t i = 0; i < g.size(); ++i)
                    if (g[i].key != liveGains[i].key || std::abs (g[i].dB - liveGains[i].dB) > 1.0e-4f)
                        return false;
                return true;
            };
            auto show = [&] (const char* when)
            {
                const auto g = p->getPartialGains();
                juce::String what = g.empty() ? juce::String ("-")
                                              : juce::String (g[0].key) + ":" + juce::String (g[0].dB, 2);
                std::printf ("    %-24s gains=%d [%s] cutoff=%.0f masterGain=%.2f  stack=%d/%s\n",
                             when, (int) g.size(), what.toRawUTF8(), gp ("filterCutoff"), gp ("masterGain"),
                             p->undoManager.getUndoDescriptions().size(),
                             p->undoManager.canRedo() ? "redoable" : "end");
            };
            std::printf ("    (the user's own last edit was masterGain %.2f -> -3.00)\n", gainWas);
            show ("after leaving edit mode");
            check (sameAsLive(), "leaving edit mode hands the hand-drawn spectrum back");

            p->undoManager.undo();
            tick (4);
            show ("after one Ctrl+Z");
            check (sameAsLive(),
                   "one Ctrl+Z after a corner-edit session does NOT wipe the user's own hand-drawn spectrum");
            check (std::abs (gp ("filterCutoff") - liveCut) < 1.0f,
                   "...and does not drop the corner's knob values onto the live patch either");
            // WHAT IT LANDS ON INSTEAD. The session is now ONE step, and that step writes
            // only the corner: undoing it takes the corner's edits back (something this
            // feature never had at all) and leaves the live patch alone. It is also the
            // only way to clear the redo stack the unwind fills - JUCE drops future
            // transactions when an action is performed and by no other route - so the
            // choice is this step or a Ctrl+Y that can still reinstall a corner.
            std::printf ("    corner B's stored cutoff: saved %.0f, edited to 6543, now %.0f (starred %s)\n",
                         cornerCutWas, p->getPlaygroundCornerParamForTests (1, "filterCutoff"),
                         p->isPlaygroundCornerEdited (1) ? "yes" : "no");
            check (std::abs (p->getPlaygroundCornerParamForTests (1, "filterCutoff") - cornerCutWas) < 1.0f
                   && p->isPlaygroundCornerEdited (1) == cornerStarWas,
                   "...it lands on the corner-edit session as ONE step: the corner's edits and its * go back");
            check (std::abs (gp ("masterGain") + 3.0f) < 0.01f,
                   "...and the user's own last edit is untouched by it");

            p->undoManager.undo();
            tick (4);
            show ("after a second Ctrl+Z");
            check (std::abs (gp ("masterGain") - gainWas) < 0.01f,
                   "...so the NEXT Ctrl+Z is the user's own last edit, which is what undo is for");
            check (sameAsLive(), "...and the hand-drawn spectrum is still theirs at that point too");

            p->undoManager.redo();          // the user's own edit comes forward first
            tick (4);
            check (std::abs (gp ("masterGain") + 3.0f) < 0.01f,
                   "redo brings the user's own edit forward again, as it always did");
            p->undoManager.redo();          // ...and then the corner-edit session
            tick (4);
            show ("after Ctrl+Y, Ctrl+Y");
            check (std::abs (p->getPlaygroundCornerParamForTests (1, "filterCutoff") - 6543.0f) < 1.0f
                   && p->isPlaygroundCornerEdited (1),
                   "...and redo puts the corner's edits back ON THE CORNER, * and all");
            check (sameAsLive(),
                   "...never installing the CORNER's drawing on the live patch");
            check (std::abs (gp ("filterCutoff") - liveCut) < 1.0f,
                   "...nor the corner's knobs");
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
            // `drawn` counts DRAWN strings only. The probe also returns a "Tooltip" row
            // per visible TooltipClient now (the language gates in SnapshotRig.cpp need
            // them), and those have no box to overflow - so counting them here would
            // have inflated the "did the sweep really walk the window" floor below with
            // rows that can never fail it.
            int totalOver = 0, drawn = 0, hovers = 0;
            for (int lang = 0; lang < 2; ++lang)
            {
                auto fits = ed->textFitProbeForTests (lang == 1);
                int thisDrawn = 0, thisHovers = 0;
                for (const auto& f : fits) (f.kind == "Tooltip" ? thisHovers : thisDrawn) += 1;
                drawn += thisDrawn; hovers += thisHovers;
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
                std::printf ("  %s: %d of %d drawn strings do not fit (+%d hovers, collected"
                             " for the language gates, no box to overflow)\n",
                             lang == 1 ? "ca" : "en", over, thisDrawn, thisHovers);
                totalOver += over;
            }
            std::printf ("  %d drawn strings and %d hovers walked over both languages\n",
                         drawn, hovers);
            check (drawn > 90, "the sweep really walked the whole window");
            check (hovers > 40, "...and it collected the HOVERS too - the strings no drawn-text "
                                "check can see, and the ones SnapshotRig's language gates read");
            check (totalOver == 0,
                   "every Label, Button and ComboBox in the editor fits the box it is drawn "
                   "in, in BOTH languages");
        }
    }

    //==========================================================================
    // --- A TUNING WITH NO PERIOD IS NEVER DESCRIBED AS HAVING AN OCTAVE -------------
    //
    // TuningManager splits the question in two: statedPeriodRatio() is the truth and
    // answers 0.0 when a tuning states no ASCENDING repeat interval, while periodRatio()
    // keeps a 2.0 fallback so a painter always has a finite number. The SOUND path was
    // moved onto the truth (temper refuses, and says why). The DISPLAY path had not
    // been, so every read-out that describes the scale still described a descending
    // .scl and a degenerate 1/1 scale as if the user's file contained an octave:
    //
    //   * the Tuning box's red field printed the file's own period line ("1/2", "1") and
    //     nothing else, so the one number that says WHY four other read-outs behave
    //     differently read like an ordinary period;
    //   * the degrees list ended on a period row that was degree 0 x that same 2/1,
    //     scored and coloured like a measurement;
    //   * the n-ad list called the scale equal-stepped against a 1200-cent grid and
    //     printed "any", i.e. "this shape works from every degree" about a scale that
    //     never comes back to itself;
    //   * the adaptation's register band counted in those 2/1s with no qualifier, while
    //     an absolute .tun with the very same problem got one;
    //   * and the Shepard's rise-interval box simply was not there, with the rise
    //     running at 2/1 and nothing on screen saying why the choice was withheld.
    //
    // Two legal .scl files state no period, and both are checked here against a control
    // that does state one - the point is never "refuse non-octave tunings", it is
    // "describe what the file says".
    {
        std::printf ("\na tuning that states no period is not described as having one:\n");
        auto pp = std::make_unique<FractalAdditiveAudioProcessor>();
        pp->prepareToPlay (sr, blk);
        std::unique_ptr<juce::AudioProcessorEditor> pbase (pp->createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (pbase.get());
        if (ed == nullptr) check (false, "the editor was created");
        else
        {
            juce::AudioBuffer<float> pb (2, blk);
            // A LIVE spectrum, so the degrees list really has something to rank: with an
            // empty analysis spectrum computeActiveScaleDissonance returns nothing at all
            // and "there is no period row" would pass for the wrong reason.
            auto settle = [&]
            {
                juce::MidiBuffer on;  on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                pb.clear(); pp->processBlock (pb, on);
                for (int i = 0; i < 40; ++i)
                { juce::MidiBuffer none; pb.clear(); pp->processBlock (pb, none); pp->pollParametersForTests(); }
                juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                pb.clear(); pp->processBlock (pb, off);
                for (int i = 0; i < 10; ++i) { juce::MidiBuffer none; pb.clear(); pp->processBlock (pb, none); }
                // The read-outs are written by the editor's timer, not by the load.
                for (int i = 0; i < 3; ++i) ed->tickTimerForTests();
                ed->resized();
            };
            auto labelText = [ed] (const char* name) -> juce::String
            {
                for (const auto& l : ed->layoutProbe().componentLabels)
                    if (l.name == name) return l.text;
                return {};
            };
            auto controlIsOnScreen = [ed] (const char* name)
            {
                for (const auto& c : ed->layoutProbe().controls) if (c.first == name) return true;
                return false;
            };
            auto shepHover = [ed] () -> juce::String
            {
                // The SHEPARD switch's hover is where the withheld choice is explained.
                for (const auto& f : ed->textFitProbeForTests (false))
                    if (f.kind == "Tooltip" && f.text.contains ("rises for ever")) return f.text;
                return {};
            };

            // `periodShows` is the substring the red Period field must still contain: the
            // file's own last line, never a substitute and never dropped.
            struct Case { const char* scl; const char* what; bool statesPeriod; const char* periodShows; };
            const Case cases[] =
            {
                // A degenerate period of exactly 1/1: every "period" lands on the same
                // pitch, so there is no repeat interval at all.
                { "! unison.scl\nperiod of exactly 1/1\n 3\n!\n 1/1\n 1/1\n 1/1\n",
                  "period-1 scale", false, "1" },
                // A DESCENDING scale: its period is 1/2, perfectly legal to write and not
                // a repeat interval going UP.
                { "! down.scl\ndescending\n 3\n!\n -300.0\n -700.0\n 1/2\n",
                  "descending .scl", false, "1/2" },
                // THE TRAP THE OLD CODE FELL INTO. Its DEGREES are three equal 400-cent
                // steps, so measured against periodRatio()'s 2/1 fallback it looked like
                // a perfectly regular 3-EDO and the n-ad list printed "any" - "this shape
                // works from every degree" about a scale whose period line is 1/2. The
                // two cases above never reached that code path; this one does.
                { "! trap.scl\nequal thirds, descending period\n 3\n!\n 400.0\n 800.0\n 1/2\n",
                  "equal steps, descending period", false, "1/2" },
                // THE CONTROL. A 3/1 period is a usable period: everything below must
                // still describe it, and describe it as 3/1 rather than as an octave.
                { "! bp.scl\nBohlen-Pierce-ish\n 4\n!\n 7/5\n 9/5\n 11/5\n 3/1\n",
                  "3/1 period scale", true, "3" },
            };

            for (const auto& c : cases)
            {
                juce::String err;
                std::printf ("\n  %s:\n", c.what);
                check (pp->loadScalaText (c.scl, c.what, err), "  the scale loads");
                settle();

                check (pp->tuningStatesPeriod() == c.statesPeriod,
                       "  the processor's verdict on whether a period is stated");

                // (1) THE RED FIELD under the keyboard, which is what the key colours
                //     above it are read against.
                const auto per = labelText ("tun.period");
                std::printf ("    period field: \"%s\"\n", per.toRawUTF8());
                check (per.contains (c.periodShows),
                       "  ...still prints the file's OWN period line, never a substitute");
                check (per.contains ("(no repeat)") == ! c.statesPeriod,
                       c.statesPeriod ? "  ...and does not qualify a period that really repeats"
                                      : "  ...and says that line is not a repeat interval going up");
                // ...AND THE LONGER STRING STILL FITS, IN BOTH LANGUAGES. The marker is
                // only ever on screen under a tuning no default scenario loads, so the
                // window's own sweeps never measure it: a caption that clips (or, being a
                // Label, condenses to 70%) would ship invisible to every other check.
                for (int lang = 0; lang < 2; ++lang)
                    for (const auto& l : ed->layoutProbeInLanguage (lang == 1).componentLabels)
                        if (l.name == "tun.period")
                        {
                            const bool fits = l.textWidth <= (float) l.cell.getWidth() + 0.5f;
                            if (! fits)
                                std::printf ("    %s period field needs %.0f px in %d\n",
                                             lang == 1 ? "ca" : "en", l.textWidth, l.cell.getWidth());
                            check (fits, lang == 1 ? "  ...and the Catalan field still fits its cell"
                                                   : "  ...and the English field still fits its cell");
                        }

                // (2) THE DEGREES LIST. Its last row is "degree 0, one period up", which
                //     only exists if there is a period. Degree indices are 0..D, so the
                //     highest degree present says whether the period row was written.
                const auto diss = pp->computeActiveScaleDissonance();
                int topDegree = -1;
                for (const auto& d : diss) topDegree = juce::jmax (topDegree, d.degree);
                const int D = pp->getScaleDegrees();
                std::printf ("    degrees list: %d rows, top degree %d of %d\n",
                             (int) diss.size(), topDegree, D);
                check (! diss.empty(), "  ...the list was really computed (a live spectrum)");
                check (topDegree == (c.statesPeriod ? D : D - 1),
                       c.statesPeriod ? "  ...and it ends on the scale's own period row"
                                      : "  ...and it stops before a period row it would have to invent");

                // (3) THE n-AD LIST'S "any". Equal-stepped means "the scale repeats AND
                //     every step of the repeat is the same size"; without a period the
                //     first half is false, so no shape may be shown as rootless.
                check (pp->tuningIsEqualStep() == false || c.statesPeriod,
                       "  ...no combination is called rootless in a scale that never repeats");

                // (4) THE REGISTER BAND'S UNIT. The qualifier already existed for an
                //     absolute .tun with nothing ascending in it; it now follows the
                //     FACT rather than the file format.
                using Src = FractalAdditiveAudioProcessor::RegisterPeriodSource;
                const auto pi = pp->registerPeriodInfo();
                check ((pi.source == Src::NoneStated) == ! c.statesPeriod,
                       "  ...the register band says whether its unit is a fact");

                // (5) THE SHEPARD RISE INTERVAL. The box is a choice only under a stated
                //     non-octave period; when the tuning states none, the switch's own
                //     hover carries the reason instead of the choice just vanishing.
                const bool riseBox = controlIsOnScreen ("tun.shepRise");
                const auto hover   = shepHover();
                std::printf ("    rise box on screen: %d, hover %d chars\n",
                             (int) riseBox, hover.length());
                check (riseBox == c.statesPeriod,
                       c.statesPeriod ? "  ...the rise-interval choice is offered (3/1 is not the octave)"
                                      : "  ...and no rise-interval choice is offered");
                check (hover.isNotEmpty(), "  ...the SHEPARD switch has a hover at all");
                check (hover.contains ("states no period") == ! c.statesPeriod,
                       c.statesPeriod ? "  ...which does not claim a missing period"
                                      : "  ...which SAYS the scale states no period, so the choice is withheld");
            }
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
                // THE FRAME THIS MACHINE IS ACTUALLY GIVING US, measured beside the ticks.
                // The budget is "a tick must not eat a whole 30 Hz frame", and on a machine
                // whose scheduler is handing out 60 ms frames a 41 ms tick has not eaten one
                // - it is the machine that is slow, not the window. So the loop times its
                // own Thread::sleep(33): a pure OS-responsiveness reading, taken in the same
                // loop, that the cost of the ticks cannot inflate (which a mean of the whole
                // frame period would - a regression making every tick slow would raise the
                // bar it is measured against and pass).
                double frameMs = 0.0;
                auto worstTickOf90 = [&] (int* whichTick)
                {
                    double worstMs = 0.0, sleptTotal = 0.0; int worstTick = -1;
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
                        const double s0 = juce::Time::getMillisecondCounterHiRes();
                        juce::Thread::sleep (33);   // the 30 Hz cadence the editor really runs at
                        sleptTotal += juce::Time::getMillisecondCounterHiRes() - s0;
                    }
                    *whichTick = worstTick;
                    frameMs = juce::jmax (33.0, sleptTotal / 90.0);
                    return worstMs;
                };
                // BEST OF THREE, because this is a wall-clock reading and the worst of
                // ninety samples is exactly the statistic an OS scheduling blip owns: one
                // 85.1 ms tick out of ninety was measured on a busy machine with the plugin
                // behaving perfectly, and a check that goes red for that is a check somebody
                // deletes. The regression it guards - a tick that cancels and JOINS its
                // recommendation worker on the message thread - is not a blip: it makes a
                // tick slow every single time the once-a-second recompute comes round, so it
                // cannot pass a pass, let alone three. The first clean pass wins; a failing
                // one costs three more seconds and nothing else.
                double worstMs = 0.0; int worstTick = -1, passes = 0;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    ++passes;
                    worstMs = worstTickOf90 (&worstTick);
                    if (worstMs < frameMs) break;
                }
                std::printf ("  worst editor timer tick under a held chord + live adaptation: "
                             "%.1f ms (tick %d of 90, best of %d pass%s) against a measured"
                             " %.1f ms frame\n",
                             worstMs, worstTick, passes, passes == 1 ? "" : "es", frameMs);
                // 30 Hz timer = 33 ms per frame, or whatever longer frame this machine is
                // really handing out. A tick that eats a whole one is a window that stutters
                // under the hand; anything near 100 ms is a control that visibly refuses to
                // follow the mouse.
                check (worstMs < frameMs,
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
            ed->setTuneSourceForTests (2);   // ...from all tunings (free search)
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
            int startsBefore = 0, appliesBefore = 0;
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
            sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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

            // SETTLED, MEASURED - not "1.4 s ought to be enough". The corner's own
            // adaptation is throttled and can need more than one search to converge on the
            // first chord, so a fixed wait sometimes spent BOTH of this take's adaptations
            // on chord A and left the MOVE with nothing to do: "the adaptation lands and
            // bumps the generation" then went red with a perfectly healthy processor
            // (starts/applies read 2/2 in the failing run and in the passing one - only
            // WHEN they happened differed), and the drag probe further down inherited a
            // search still in flight and measured a drag that published nothing at all
            // (generation +0, revision +1, against +4/+20). Wait for quiescence instead.
            auto settleCorners = [&]
            {
                int quiet = 0, ls = -1, la = -1;
                juce::uint32 lg = 0;
                for (int i = 0; i < 400 && quiet < 20; ++i)
                {
                    pumpFor (0.02, {});
                    const int s = p.getPlaygroundAdaptStarts(), a = p.getPlaygroundAdaptApplies();
                    // The GENERATION has to be still as well, and the worker idle. The
                    // playground's own blend publish is throttled and lands a few frames
                    // after the point settles: caught outside this wait it moved the
                    // control take's generation by 1 and failed "the control take's
                    // generation stands still across the same chord move" - a bump that
                    // had nothing to do with the chord and everything to do with when the
                    // reading was taken.
                    const juce::uint32 g = p.getTemplateGeneration();
                    if (p.isSpectrumAdaptRunning() || p.isSpectrumBuildPending()
                        || s != ls || a != la || g != lg) quiet = 0;
                    else                                  ++quiet;
                    ls = s; la = a; lg = g;
                }
            };

            pumpFor (0.30, {});
            pumpFor (1.40, keyMidi (true, { 60, 64, 67 }));      // chord A held...
            settleCorners();                                     // ...and genuinely settled
            tk.cornerBefore   = ratiosOf (p.getPlaygroundCornerTemplateForTests (1));
            tk.liveBefore     = liveRatios (p);
            tk.genBeforeMove  = p.getTemplateGeneration();
            tk.startsBefore   = p.getPlaygroundAdaptStarts();
            tk.appliesBefore  = p.getPlaygroundAdaptApplies();
            recording = true;
            pumpFor (0.10, {});
            tk.T0 = tk.x.size();                                  // the chord MOVES here
            pumpFor (0.02, keyMidi (false, { 60, 64, 67 }));
            pumpFor (1.60, keyMidi (true,  { 62, 65, 69 }));      // C major -> D minor
            tk.T1 = tk.x.size();
            recording = false;
            // ...and settled again, so the processor handed back for the follow-on probes
            // has no search in flight: the drag probe below measures publishes that a
            // running adaptation suppresses.
            settleCorners();
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
        // Counted ACROSS THE MOVE, not since the take began: an adaptation that landed
        // while chord A was still settling is no evidence that the MOVE caused one.
        const int adpOffStarts  = adpOff.startsEnd  - adpOff.startsBefore;
        const int adpOffApplies = adpOff.appliesEnd - adpOff.appliesBefore;
        std::printf ("  morph off: control starts/applies %d/%d, adaptive %d/%d ACROSS THE MOVE"
                     " (%d/%d over the whole take); gen ctl +%u adp +%u\n",
                     ctlOff.startsEnd - ctlOff.startsBefore, ctlOff.appliesEnd - ctlOff.appliesBefore,
                     adpOffStarts, adpOffApplies, adpOff.startsEnd, adpOff.appliesEnd,
                     ctlOff.genAfterSettle - ctlOff.genBeforeMove,
                     adpOff.genAfterSettle - adpOff.genBeforeMove);
        check (adpOff.p->getPlaygroundAdaptiveCount() == 1,
               "a corner saved with REALTIME armed counts as adaptive inside the playground");
        check (ctlOff.p->getPlaygroundAdaptiveCount() == 0 && ctlOff.startsEnd == 0,
               "a corner saved with it off never launches a search");
        check (adpOffStarts >= 1 && adpOffApplies >= 1,
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
        check (tOffA <= 2.0,   // the same re-phasing allowance as the morph-ON twin below
               "morph off: the landing is no louder a step than the note's own movement (no thump)");

        // ---- morph ON: same take, the voices GLIDE to each adaptive blend ----
        auto ctlOn = take (false, true);
        auto adpOn = take (true,  true);
        const double jOnC = clickOf (ctlOn), jOnA = clickOf (adpOn);
        const double tOnA = thumpOf (adpOn);
        std::printf ("  morph on:  jump/peak ctl %.3f adp %.3f; landing/calm RMS excursion %.2fx; "
                     "starts/applies %d/%d across the move\n", jOnC, jOnA, tOnA,
                     adpOn.startsEnd - adpOn.startsBefore, adpOn.appliesEnd - adpOn.appliesBefore);
        check (adpOn.appliesEnd - adpOn.appliesBefore >= 1
               && adpOn.genAfterSettle > adpOn.genBeforeMove,
               "morph on: the adaptation lands and bumps the generation (the morph glides to it)");
        check (jOnA <= jOnC * 2.5 + 0.02, "morph on: an adaptive landing does not click either");
        // 2.0, for the reason run 42's twin already records beside the same metric: the
        // landing time moving by one poll tick RE-PHASES the 30 ms excursion span against
        // the notes' own beating, and an unchanged processor was measured at 1.79x there and
        // at 1.65x here on a busy machine. A real thump - the "golpe" this metric was built
        // on - reads several times the shimmer, so 2.0 still forbids it while tolerating
        // where in the beat cycle the landing happens to fall.
        check (tOnA <= 2.0, "morph on: ...and does not thump");

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
            awaitSpectrumIdle (p, "the adaptive rig before its drag");
            const auto gen0 = p.getTemplateGeneration();
            const auto rev0 = p.getSpectrumLiveRevision();
            const double dragFrom = juce::Time::getMillisecondCounterHiRes();
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
            const double dragMs = juce::Time::getMillisecondCounterHiRes() - dragFrom;
            p.pollParametersForTests();
            awaitSpectrumIdle (p, "the adaptive rig's last drag blend");
            const auto genD = p.getTemplateGeneration() - gen0;
            const auto revD = p.getSpectrumLiveRevision() - rev0;
            // As run 41's drag: a cadence is a rate, so it is divided by the time the drag
            // really took rather than by an assumption that twenty sleep(12) calls take
            // 240 ms. 60 Hz would read 60 here.
            const double bumpHz = (double) genD * 1000.0 / std::max (1.0, dragMs);
            std::printf ("  drag with an adaptive corner engaged, over %.0f ms: generation +%u (%.1f Hz),"
                         " revision +%u\n", dragMs, genD, bumpHz, revD);
            check (bumpHz > 3.0 && bumpHz < 30.0,
                   "a drag with an adaptive corner engaged bumps at the glide cadence, never 60 Hz");
            check (revD >= 2 * genD && revD > 0,
                   "...while the live template really is being replaced along the way, far more often"
                   " than the generation moves");
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
            // NO TRANSITION anywhere in this run, including inside the corner PRESETS
            // saved from this state - the transition is a Discrete morph-space value, so
            // the winner corner would hand it straight back to any rig that loads these
            // files. The panel's one TRANSITION question defaults to Dry/Wet now, and a
            // 0.3 s crossfade running under the mode-flip click metric below is a second
            // moving thing in a measurement whose whole point is that only ONE moves.
            // The transition's own click behaviour is measured, with the mode stated, in
            // run 26 and run 50.
            sp ("dryWet", 0.0f); sp ("morph", 0.0f);
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
                rs ("dryWet", 0.0f); rs ("morph", 0.0f);   // see the corner presets above
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
            // TURNING THE VOICE COUNT DOWN WITH KEYS HELD must fade, not delete. Same two
            // faults applyMonoVoiceCount documents, in the sub-engines: reduceNumVoices()
            // deletes SOUNDING voices, and a deleted voice stops mid-waveform because the
            // object that would render its fade is gone. This is the control people reach
            // for when the CPU meter climbs, so it gets thrown mid-chord by design.
            {
                auto r = std::make_unique<Proc>();
                r->prepareToPlay (48000.0, 256);
                auto rs = [&] (const char* id, float v)
                { if (auto* q = r->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
                rs ("ampSustain", 1.0f);
                rs ("pgMode", 1.0f);
                rs ("pgVoices", 8.0f);
                {
                    juce::String e2;
                    static const char* nm[2] = { "zzz-verify-mix-A", "zzz-verify-mix-B" };
                    for (int i = 0; i < 2; ++i) r->assignPlaygroundCorner (i, nm[i], e2);
                }
                for (int i = 0; i < 6; ++i) { r->pollParametersForTests(); juce::Thread::sleep (2); }

                juce::AudioBuffer<float> b (2, 256);
                {
                    juce::MidiBuffer m;
                    for (int n = 0; n < 8; ++n)
                        m.addEvent (juce::MidiMessage::noteOn (1, 52 + n * 3, (juce::uint8) 100), 0);
                    b.clear(); r->processBlock (b, m);
                }
                for (int i = 0; i < 40; ++i)
                { juce::MidiBuffer m; b.clear(); r->processBlock (b, m);
                  if ((i % 12) == 0) r->pollParametersForTests(); }
                const double before = b.getRMSLevel (0, 0, b.getNumSamples());

                // THE CONTROL ARM, and it is what makes the number mean anything. A dense
                // spectrum moves a long way between two samples all by itself, so "the
                // worst step is 0.12" is not a verdict until the same chord, undisturbed,
                // has been asked the same question. Measure that first.
                // WHAT TO MEASURE, and the first two answers were both wrong. A per-sample
                // step cannot see this: a dense spectrum moves 0.235 between two samples on
                // its own, and six deleted voices out of eight - each already a fraction of
                // one corner of a mix - move less than that, so deleting them outright
                // scored 0.45x the chord's own motion and passed. An RMS over a whole block
                // is blinder still.
                // The ENVELOPE is what separates them. A deleted voice takes its share of
                // the signal away between one sample and the next, so a short window
                // straddling the delete drops in a single step; a 5 ms fade spreads the
                // same loss over ~240 samples at 48 kHz, so no one window can drop by much.
                // Worst single-window drop RATIO is therefore the metric, and it is scored
                // against the same chord undisturbed, in the same run.
                auto worstEnvDrop = [&] (int blocks)
                {
                    constexpr int W = 64;                 // 1.3 ms - shorter than the fade
                    double worst = 0.0, prev = -1.0;
                    for (int i = 0; i < blocks; ++i)
                    {
                        r->pollParametersForTests();     // the 60 Hz poll drives the shrink
                        juce::MidiBuffer e; b.clear(); r->processBlock (b, e);
                        for (int s = 0; s + W <= b.getNumSamples(); s += W)
                        {
                            double sum = 0.0;
                            for (int k = 0; k < W; ++k)
                            { const double v = b.getSample (0, s + k); sum += v * v; }
                            const double rms = std::sqrt (sum / W);
                            if (prev > 1.0e-5) worst = juce::jmax (worst, 1.0 - rms / prev);
                            prev = rms;
                        }
                    }
                    return worst;                        // 0 = perfectly level, 1 = to silence
                };
                const double quiet = worstEnvDrop (40);   // nothing changing

                rs ("pgVoices", 2.0f);   // 8 -> 2 with every key still down

                // THE ASSERTION THAT ACTUALLY BITES. Two audio metrics were tried first and
                // BOTH passed the sabotage - a per-sample step (the chord moves 0.235 by
                // itself) and a 1.3 ms envelope drop (it swings 0.74 by itself). Six of
                // eight voices inside one corner of a mix are simply worth less than the
                // spectrum's own motion, so no measurement of the OUTPUT can separate a
                // fade from a delete here. What can is the pool itself: a fade leaves the
                // pool oversized for a tick or two while the doomed voices go quiet, and a
                // delete collapses pool and active together in one instant.
                int poolRightAfter = -1, activeRightAfter = -1;
                {
                    r->pollParametersForTests();
                    juce::MidiBuffer e; b.clear(); r->processBlock (b, e);
                    poolRightAfter   = r->mixVoicePoolForTests (0);
                    activeRightAfter = r->mixVoicesActiveForTests (0);
                }
                const double worstStep = worstEnvDrop (40);
                const double after = b.getRMSLevel (0, 0, b.getNumSamples());
                std::printf ("  corner 0 one tick after the cut: pool %d, sounding %d "
                             "(a fade holds the pool; a delete drops both at once)\n",
                             poolRightAfter, activeRightAfter);
                check (poolRightAfter > 2,
                       "the mix shrink asks its voices to FADE before it deletes them - the pool "
                       "outlives the cut by at least one tick");
                check (r->mixVoicePoolForTests (0) == 2,
                       "...and the pool really does reach the count the user asked for");
                std::printf ("  mix voices 8 -> 2 under 8 held keys: rms %.5f -> %.5f, worst per-sample "
                             "step %.5f against %.5f for the same chord undisturbed (%.2fx)\n",
                             before, after, worstStep, quiet,
                             quiet > 1.0e-9 ? worstStep / quiet : 0.0);
                // A PER-SAMPLE metric against THIS RUN'S OWN control, because an RMS over a
                // block cannot see a waveform cut in half between two samples - which is
                // exactly what a deleted voice does, and exactly how this class of defect
                // shipped before. Deleting eight voices' worth of signal shows up as a step
                // far larger than the chord's own motion; a 5 ms fade does not.
                check (worstStep < juce::jmax (0.02, quiet * 1.5),
                       "turning mix voices DOWN under held keys fades them out - no voice is "
                       "deleted mid-waveform");
                check (after > 0.0001,
                       "...and the voices the budget keeps are still sounding afterwards");

            }

            const double e0 = bench (0, 0, 32, "extreme 32 notes: main alone");
            const double e4 = bench (4, 32, 32, "extreme 32 notes: 4 corners @32 voices");
            const double d4 = bench (4, 8, 32, "extreme 32 notes: 4 corners @default 8");
            const double r0 = bench (0, 0, 4, "realistic chord:  main alone");
            const double r4 = bench (4, 8, 4, "realistic chord:  4 corners @default 8");
            juce::ignoreUnused (e0);
            check (d4 < e4 * 0.75,
                   "the VISIBLE voice control genuinely buys back CPU (default 8 well under 32)");
            // AGAINST THIS RUN'S OWN MAIN ENGINE, not against a stopwatch. `r4 < 10.0 ms`
            // was a wall-clock reading on a shared machine: the same binary measured this
            // pair at 1.325/4.644 and at 0.766/2.288 ms per block, a 2x swing for reasons
            // entirely outside this file. A number that moves like that cannot be a
            // verdict - it goes red on an innocent change and the first person it
            // inconveniences deletes it. The absolute ms/block is what a user needs to be
            // told and it is printed above; what is ASSERTED is the structural claim, and
            // it is a ratio, so machine speed cancels: four corners plus the main engine
            // still tracking underneath is FIVE engines rendering the same chord, and the
            // mix must cost no more than that - no per-corner overhead beyond the audio
            // each corner actually renders. Measured 3.0-3.5x.
            const double mixRatio = r0 > 0.0 ? r4 / r0 : 0.0;
            std::printf ("  the mix costs %.2fx the main engine alone, measured in the same run"
                         " (4 corners + main = 5 engines)\n", mixRatio);
            check (mixRatio > 0.0 && mixRatio < 5.0,
                   "a realistic mix costs no more than its five engines - the corners add no overhead of their own");
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
        awaitSpectrumIdle (*p, "the fast-rise Shepard patch before the wrap measurement");
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
            // "WELL UNDER REALTIME" USED TO BE A STOPWATCH READING (< 0.5 x the block's own
            // duration), and it was measured swinging 3.4x - 1.31 to 4.45 ms/block - between
            // runs of the same binary on the same machine. A number that moves for reasons
            // outside this file cannot be an assertion. The absolute cost is still printed,
            // in ms and as a percentage of realtime, because that is what a user has to be
            // told; what is ASSERTED is the engineering claim underneath it, and it is a
            // ratio, so machine speed cancels: a Shepard partial is not a more expensive
            // kind of partial than an ordinary one. Measured 0.9x at C4.
            const double oscOff = dflt.first.first  / std::max (1.0, (double) dflt.first.second);
            const double oscOn  = dflt.second.first / std::max (1.0, (double) dflt.second.second);
            std::printf ("    per oscillator: %.4f us off, %.4f us on (x%.2f) - the mode's cost is its"
                         " oscillator COUNT, not a more expensive oscillator\n",
                         oscOff * 1000.0, oscOn * 1000.0, oscOff > 0.0 ? oscOn / oscOff : 0.0);
            check (oscOff > 0.0 && oscOn < oscOff * 2.5,
                   "and a Shepard copy's partial costs no more than an ordinary partial (the mode buys"
                   " oscillators, it does not make them dearer)");

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
            awaitSpectrumIdle (*p, "playground blend before an RMS reading");
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
            sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
        sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
                sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
                    // The voice can only bend towards a blend that has been PUBLISHED, and
                    // the blend runs on a Priority::low worker. Rendering three blocks and
                    // hoping was measured reading 0 of 50 in-between ratios on a busy
                    // machine - the drag was perfect, the worker had simply not been
                    // scheduled yet - against 6 of 50 on an idle one. In the plugin the
                    // worker keeps up with a 60 Hz drag; here it is waited for.
                    awaitSpectrumIdle (*p, "one frame of the gliding drag");
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
                sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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
            sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
        }

        // ---- MIX mode: same trips, nothing may touch the live patch, RMS holds -----
        {
            sp ("pgMode", 1.0f);
            park (0.0f, 0.0f, 12);
            awaitSpectrumIdle (*p, "the mix corner engines building");
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
        auto applyPatch = [&]
        {
            sp ("morph", 0.0f);
            sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
            sp ("ampAttack", 0.001f); sp ("ampDecay", 0.030f);
            sp ("ampSustain", 0.2f);  sp ("ampRelease", 0.010f);
            sp ("phaseMode", 0.0f);
            sp ("filterCutoff", 9600.0f);   // wide open in cents: the filter must not colour f0
            sp ("pitchEnvAmount", 0.0f);
        };
        applyPatch();

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
        // A ONE-SAMPLE DISCONTINUITY, and the reference that says whether it is one.
        // An RMS over any window is structurally blind to this: the energy is identical
        // whether a term leaves the output in one sample or is ramped out over five
        // milliseconds. So `edgeAt` is the single jump across the sample in question and
        // `neighAt` is the biggest jump the same waveform makes in the millisecond either
        // side of it - a ramp cannot beat that reference, and a cliff cannot help but.
        // Used by (k) and (l), which are the two places a sounding voice is interfered
        // with by something other than a note the player pressed.
        auto edgeAt = [&] (size_t K) -> double
        {
            return (K == 0 || K >= x.size()) ? 0.0
                 : std::abs ((double) x[K] - (double) x[K - 1]);
        };
        auto neighAt = [&] (size_t K, int w) -> double
        {
            double m = 0.0;
            if (K < (size_t) w + 1 || K + (size_t) w + 1 >= x.size()) return m;
            for (size_t i = K - (size_t) w; i + 1 < K; ++i)
                m = std::max (m, std::abs ((double) x[i + 1] - (double) x[i]));
            for (size_t i = K + 1; i + 1 < K + (size_t) w; ++i)
                m = std::max (m, std::abs ((double) x[i + 1] - (double) x[i]));
            return m;
        };
        auto peakOf = [&] (size_t at, int win) -> double
        {
            double m = 0.0;
            for (int i = 0; i < win && at + (size_t) i < x.size(); ++i)
                m = std::max (m, std::abs ((double) x[at + (size_t) i]));
            return m;
        };

        const double fLo = p->getNoteFrequency (60);
        const double fHi = p->getNoteFrequency (67);   // a fifth: two clearly separate pitches
        constexpr double glideMs = 200.0;

        // EVERY LETTERED BLOCK BELOW STARTS FROM A PROCESSOR NOBODY ELSE HAS TOUCHED.
        //
        // They used to share one, and the sharing quietly disarmed the checks. Block (f)
        // saves its corner presets by MOVING Base N and left it on 5, which put the
        // spectrum's lowest partial on the 5th harmonic - so (g)'s autocorrelation read a
        // pitch two octaves adrift and failed a claim the glide was keeping perfectly.
        // That was patched by hand, at the end of (f), by putting Base N back; the same
        // class then came round again in (i), whose own CONTROL arm - the arm that exists
        // to prove the measurement can see a glide at all - measured "0% of the readings
        // lie between the two pitches with the gate off, 0% with it ON" while the
        // identical glide forty lines earlier travelled fine. A control arm that reads
        // zero cannot distinguish the bug it is there to catch from a broken rig.
        //
        // One general fix instead of a growing list of per-symptom restores: a new
        // processor and the same declared patch at the head of each block. The hand-patch
        // in (f) is deleted with it, because a general fix that leaves the workaround in
        // place has never been tested. (The lambdas below all reach `p` by reference, so
        // replacing it here is all it takes; (c) and (d) deliberately keep (b)'s
        // processor and recording - they are three readings of ONE take.)
        auto fresh = [&]
        {
            p = std::make_unique<FractalAdditiveAudioProcessor>();
            p->prepareToPlay (sr8, b8);
            applyPatch();
            sp ("glideTime", (float) glideMs);
            x.clear(); clk = 0;
        };

        //-- (a) the CONTROL: mono OFF is a jump, and a second key adds a voice ----------
        fresh();
        sp ("monoGlide", 0.0f);
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
        // (b), (c) and (d) are three readings of ONE take, so they share this processor
        // and this recording; the reset is here, at the head of the take.
        fresh();
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
        // The mode is armed HERE rather than inherited, so this block still measures a
        // transition rather than whatever the block before it happened to leave behind.
        fresh();
        sp ("monoGlide", 1.0f);
        check (p->synthVoiceCountForTests() == 1, "(the mode really is on before it is switched off)");
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
            fresh();
            FractalAdditiveAudioProcessor::getPresetFolder().createDirectory();
            juce::String err;
            ScopedPreset presetA, presetB;
            sp ("baseN", 2.0f);  const bool okA = presetA.save (*p, "run48-A");
            sp ("baseN", 5.0f);  const bool okB = presetB.save (*p, "run48-B");
            // The assertion runs 45 and 47 already end with, at the OTHER end: a preset
            // that was never written is a corner assignment that silently does nothing,
            // and every voice count below would then be measuring the default patch.
            check (okA && okB, "the two corner presets really were written to disk");
            p->assignPlaygroundCorner (0, presetA.name, err);
            p->assignPlaygroundCorner (1, presetB.name, err);
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
            // presetA/presetB delete themselves as this scope closes.
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
            fresh();
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
            fresh();
            sp ("monoGlide", 1.0f);       // the takeover this block is about needs the mode on
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
            fresh();
            sp ("monoGlide", 1.0f);       // the glide under test - armed here, not inherited
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

        //-- (j) THE GLIDE IS CHARGED FOR SAMPLES RENDERED, AT EVERY SUB-BLOCK SIZE -------
        // Everything above this point renders at ONE block size (256) and never varies
        // it, and that is exactly what hid the worst bug this feature ever had: the glide
        // was advanced from inside the FILTER CUTOFF chunk gate, a counter re-seeded to 32
        // at the top of every renderNextBlock. A sub-block of 31 samples or fewer never
        // reached the branch, so glideLeft never decremented: the note stopped dead 700
        // cents flat and stayed there for ever, and because lastPitchLog carries the glide
        // offset the NEXT takeover started from the stuck pitch too.
        //
        // Sub-blocks that short are not exotic. MPESynthesiserBase::renderNextBlock
        // renders 1 sample for the first event of a block and always renders the trailing
        // remainder whatever its size, so ANY host buffer size produces them - which is
        // why the second half of this block plays the same glide at a perfectly ordinary
        // 256-sample buffer with a stream of controller messages underneath it, the way a
        // real MPE controller or a moving automation lane sends them.
        //
        // Both halves assert BOTH ends of the claim: WHERE the glide arrives and HOW LONG
        // it took. An arrival check alone passes on a glide that took twice as long, and
        // a duration check alone passes on one that stopped early at the wrong pitch.
        {
            const int sizes[] = { 8, 16, 24, 31, 32, 64, 256, 2048 };
            const int glideSamples = (int) std::lround (glideMs * 0.001 * sr8);
            // Rendered per size: enough to settle on the low note, then the glide plus a
            // long tail - if the portamento were stuck, "still stuck" would be measured
            // here, a third of a second after it should have arrived.
            const int settleN = (int) (0.100 * sr8);
            const int afterN  = (int) (0.600 * sr8);
            const int cc      = 20;   // undefined controller: inert to the synth, but it
                                      // still splits the block into sub-blocks
            for (int si = 0; si < (int) (sizeof (sizes) / sizeof (sizes[0])) + 1; ++si)
            {
                // One extra pass past the end of the table: the same 256-sample buffer,
                // this time with controller traffic every 100 samples.
                const bool  ccPass = si == (int) (sizeof (sizes) / sizeof (sizes[0]));
                const int   bs     = ccPass ? 256 : sizes[si];
                p = std::make_unique<FractalAdditiveAudioProcessor>();
                p->prepareToPlay (sr8, bs);
                applyPatch();
                sp ("glideTime", (float) glideMs);
                sp ("monoGlide", 1.0f);
                x.clear(); clk = 0;

                juce::AudioBuffer<float> bb (2, bs);
                auto step = [&] (const juce::MidiBuffer& m)
                {
                    juce::MidiBuffer mm (m);
                    if (ccPass)
                        for (int s = 0; s < bs; s += 100)
                            mm.addEvent (juce::MidiMessage::controllerEvent (1, cc, 64), s);
                    bb.clear();
                    p->processBlock (bb, mm);
                    const float* r = bb.getReadPointer (0);
                    for (int k = 0; k < bs; ++k) x.push_back (r[k]);
                };

                for (int done = 0, first = 1; done < settleN; done += bs, first = 0)
                    step (first ? on (60) : juce::MidiBuffer {});

                const size_t at = x.size();
                int elapsed = -1;
                for (int done = 0, first = 1; done < afterN; done += bs, first = 0)
                {
                    step (first ? on (67) : juce::MidiBuffer {});
                    if (elapsed < 0 && ! p->voiceGlidingForTests()) elapsed = done + bs;
                }

                // The voice's own end-of-block pitch, and the pitch actually HEARD a third
                // of a second after the glide was due - a stuck glide fails both.
                const double hz    = p->voiceSoundingHzForTests();
                const double heard = f0At (at + (size_t) (0.450 * sr8), (int) (0.030 * sr8));
                const double cents = hz > 0.0 ? 1200.0 * std::log2 (hz / fHi) : -9999.0;
                const double hCent = heard > 0.0 ? 1200.0 * std::log2 (heard / fHi) : -9999.0;
                // Block granularity is the only slack the measurement itself needs: the
                // glide can only be seen to have ended on a block boundary.
                const int    slack = 2 * bs + 64;
                std::printf ("  block %5d%s: arrived %.2f Hz (%+.1f c), heard %.2f Hz (%+.1f c), "
                             "glide took %d samples (asked %d, slack %d)\n",
                             bs, ccPass ? " +CC" : "    ", hz, cents, heard, hCent,
                             elapsed, glideSamples, slack);
                juce::String tag ("block size ");
                tag << bs << (ccPass ? " with controller traffic" : "");
                check (std::abs (cents) < 5.0,
                       (tag + ": the glide ARRIVES on the new note's tuned pitch").toRawUTF8());
                check (std::abs (hCent) < 20.0,
                       (tag + ": ...and that is the pitch the rendered audio sounds at").toRawUTF8());
                check (elapsed > 0 && std::abs (elapsed - glideSamples) <= slack,
                       (tag + ": ...having taken the Glide Time asked for, no more and no less").toRawUTF8());
            }
        }

        //-- (k) A TAKEOVER MID-MORPH RAMPS THE CROSSFADE OUT - it does not drop it -------
        // AdditiveVoice states one invariant about the morph crossfade: alignPhase[] may
        // only be written at weight 0 and phase[] only at weight 1, because at any weight
        // in between BOTH banks are audible and moving either one is a coherent step in
        // every partial at once. The note-on reset zeroed morphWeight outright, which is
        // the same step taken from the other side: the aligned bank's whole contribution
        // left the output between two samples.
        //
        // The reset predates the crossfade, and for a long time nothing hit it: a steal
        // re-attacked the amplitude envelope, which masked the step. MONO GLIDE removed
        // both excuses - one voice means every second note is a steal, and the legato rule
        // deliberately suppresses that re-attack - while templates now arrive in streams
        // (10 Hz drag bumps, the realtime follower), so a note landing mid-morph is the
        // normal case rather than a coincidence.
        //
        // The metric is ONE SAMPLE WIDE and it has to be: run 48's steal check is an RMS
        // over a 5 ms window, and a one-sample cliff is invisible to it - the energy is
        // identical whether the weight leaves in one sample or is ramped out over five
        // milliseconds. So what is measured is the single jump ACROSS the note-on
        // boundary, against the biggest jump the very same waveform makes in the
        // millisecond either side of it. A ramp cannot beat that reference; a cliff
        // cannot help but. Measuring over a window instead would fold in the rising
        // pitch of the portamento, which legitimately makes the waveform steeper and
        // was enough to hide the cliff behind the note-on's honest cost.
        {

            // Same gesture twice: once with Morph ON (a crossfade really is in flight when
            // the second key lands) and once with it OFF (the control - there is no weight
            // to drop, so whatever this arm measures is the takeover's honest cost).
            double stepOn = 0.0, busyOn = 0.0, pkOn = 0.0, wOn = 0.0;
            double stepOff = 0.0, busyOff = 0.0;
            for (int arm = 0; arm < 2; ++arm)
            {
                const bool morphOn = arm == 0;
                fresh();
                sp ("monoGlide", 1.0f);
                sp ("morph", morphOn ? 1.0f : 0.0f);
                sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
                sp ("morphTime", 0.300f);
                // A SPARSE spectrum on purpose. With ten octaves of partials the waveform
                // is so jagged sample to sample that a genuine cliff hides inside its own
                // noise floor; three octaves is a smooth tone whose steps are the ones the
                // synth actually took.
                sp ("spectrumRange", 3.0f);
                sp ("baseN", 1.0f);
                awaitSpectrumIdle (*p, "run 48 (k): the spectrum the note starts on");
                x.clear(); clk = 0;
                pump (60, on (60));                    // settle on the low note
                // The morph's target MOVES EVERY PARTIAL: the harmonic base shift is a
                // frequency change, so the aligned bank really is a different signal from
                // the running one and the weight between them really is audible. A change
                // that only appends partials at unchanged frequencies leaves the two banks
                // nearly identical - dropping the weight would then be a step of nothing,
                // and the test would pass on a rig that measures nothing.
                const auto gen0 = p->getTemplateGeneration();
                sp ("baseN", 7.0f);
                awaitTemplateChange (*p, gen0, "run 48 (k): the morph's target spectrum");
                pump (40, {});                         // ~213 ms of a 300 ms morph: mid-fade
                const double w = p->voiceMorphWeightForTests();
                const size_t K = x.size();             // the takeover lands on the next block
                pump (2, on (67));
                pump (40, {});

                const int wNeigh = (int) (0.001 * sr8);   // one millisecond either side
                const double busy = neighAt (K, wNeigh);
                const double step = edgeAt (K);
                if (morphOn) { stepOn = step; busyOn = busy; wOn = w;
                               pkOn = peakOf (K - (size_t) (0.020 * sr8), (int) (0.020 * sr8)); }
                else         { stepOff = step; busyOff = busy; }

                juce::MidiBuffer o1; o1.addEvent (juce::MidiMessage::noteOff (1, 67), 0);
                juce::MidiBuffer o2; o2.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                pump (2, o1); pump (2, o2); pump (40, {});
            }

            std::printf ("  takeover mid-morph (crossfade weight %.3f when the key went down): the jump "
                         "ACROSS the note-on is %.5f, the biggest jump 1 ms either side is %.5f "
                         "(%.1fx), peak %.5f\n",
                         wOn, stepOn, busyOn, busyOn > 1.0e-12 ? stepOn / busyOn : -1.0, pkOn);
            std::printf ("  the same takeover with Morph OFF: across %.5f, either side %.5f (%.1fx)\n",
                         stepOff, busyOff, busyOff > 1.0e-12 ? stepOff / busyOff : -1.0);
            // The rig has to be armed before its verdict means anything: a weight that was
            // never mid-fade cannot be dropped, and the two assertions below would then be
            // measuring the ordinary cost of a note-on and calling the bug fixed.
            check (wOn > 0.05 && wOn < 0.95,
                   "the rig is armed: the crossfade really is part-way through when the second key lands");
            check (busyOff > 1.0e-9 && stepOff <= 1.2 * busyOff,
                   "the control: with Morph OFF the identical takeover is not a jump at all");
            check (busyOn > 1.0e-9 && stepOn <= 1.2 * busyOn,
                   "and with a crossfade half way through, the note-on is STILL not a jump - "
                   "the weight is ramped out, not dropped");
        }

        //-- (l) SWITCHING THE MODE ON UNDER A HELD CHORD ---------------------------------
        // The switch is automatable and lives on a panel, so it WILL be thrown with keys
        // down. Dropping the pool to one voice means voices have to go, and two separate
        // things were wrong with how they went.
        //
        //   * They were DELETED. reduceNumVoices() removes the voice objects outright, so
        //     a sounding voice stopped mid-waveform - a step, not a fade.
        //   * The WRONG one survived. JUCE's findVoiceToSteal protects the lowest and the
        //     topmost sounding note and gives the lowest the final say, so the note left
        //     playing was the BOTTOM of the chord. This mode's contract, written out in
        //     MonoGlide.h, is last-note priority: "the newest key always wins; a low-note
        //     rule would be a different instrument."
        //
        // The chord is pressed 60, 72, 65 so that the lowest note, the highest note and
        // the last one pressed are three DIFFERENT keys. A test on a chord pressed in
        // rising order cannot tell last-note priority from highest-note priority, and one
        // pressed in falling order cannot tell it from lowest - either would have passed
        // on the behaviour this block exists to reject.
        {
            fresh();
            sp ("monoGlide", 0.0f);          // start polyphonic, with a real chord down
            sp ("spectrumRange", 3.0f);      // a smooth tone: see (k)
            awaitSpectrumIdle (*p, "run 48 (l): the chord's spectrum");
            x.clear(); clk = 0;
            pump (30, on (60));
            pump (30, on (72));
            pump (60, on (65));              // ...pressed LAST, and neither the lowest nor the highest
            const int chordVoices = p->activeVoiceCountForTests();

            const size_t L = x.size();       // the switch is thrown between these two blocks
            sp ("monoGlide", 1.0f);
            // The shrink is allowed to take a few ticks - the losers fade first and the
            // pool follows once they are quiet - so the 60 Hz timer is driven here the way
            // the host drives it, one call per rendered block.
            int ticks = 0;
            for (; ticks < 60 && p->synthVoiceCountForTests() > 1; ++ticks)
            { pump (1, {}); p->pollParametersForTests(); }
            pump (60, {});

            const int    left  = p->activeVoiceCountForTests();
            const int    pool  = p->synthVoiceCountForTests();
            const double stayHz = p->voiceSoundingHzForTests();
            const double heard  = f0At (L + (size_t) (0.150 * sr8), (int) (0.030 * sr8));
            const double edge   = edgeAt (L);
            const double neigh  = neighAt (L, (int) (0.001 * sr8));
            const double pk     = peakOf (L - (size_t) (0.020 * sr8), (int) (0.020 * sr8));
            // DID THE DROPPED NOTES FADE, OR VANISH? Compare the millisecond after the
            // switch with the millisecond before it. A deleted voice takes its whole
            // contribution with it at once, so two of three notes leaving cuts the level
            // straight to about 1/sqrt(3); a 5 ms fade has barely started by then and the
            // chord is still almost all there. This is the measurement, not the
            // one-sample edge above: dropping notes from a chord is SUPPOSED to lower the
            // level, and what is being asserted is that it lowers it over time.
            const int    wMs    = (int) (0.001 * sr8);
            const double rB     = rmsAt (L - (size_t) wMs, wMs);
            const double rA     = rmsAt (L, wMs);
            const double keepR  = rB > 1.0e-9 ? rA / rB : -1.0;
            const double wantHz = p->getNoteFrequency (65);
            std::printf ("  chord 60/72/65 (65 pressed last), then the switch: %d voice(s) before, %d after, "
                         "pool %d, settled on %.2f Hz (last key %.2f, lowest %.2f), heard %.2f Hz\n",
                         chordVoices, left, pool, stayHz, wantHz, p->getNoteFrequency (60), heard);
            std::printf ("  the jump ACROSS the switch is %.5f, the biggest jump 1 ms either side is %.5f "
                         "(%.1fx), peak %.5f, shrink took %d tick(s)\n",
                         edge, neigh, neigh > 1.0e-12 ? edge / neigh : -1.0, pk, ticks);
            std::printf ("  RMS of the millisecond after the switch / the millisecond before it: %.3f "
                         "(%.5f -> %.5f)\n", keepR, rB, rA);
            check (chordVoices == 3, "the rig is armed: three keys really were sounding on three voices");
            check (pool == 1, "the pool does end up at ONE voice - the mode's promise is kept");
            check (left == 1, "...and exactly one note is left sounding");
            check (stayHz > 0.0 && std::abs (1200.0 * std::log2 (stayHz / wantHz)) < 5.0,
                   "...and it is the note pressed LAST, not the lowest one JUCE would have protected");
            check (heard > 0.0 && std::abs (1200.0 * std::log2 (heard / wantHz)) < 25.0,
                   "...which is also the pitch the rendered audio settles on");
            check (keepR > 0.8,
                   "...and the notes it had to drop LEAVE OVER A FADE - a millisecond after the "
                   "switch the chord is still nearly all there, not cut to a third");
            check (neigh > 1.0e-9 && edge <= 1.2 * neigh,
                   "...with no one-sample step across the switch either");
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
        sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
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

        // ---- (d) AND THE PANEL SAYS WHICH ONE IS RUNNING ------------------------------
        // MORPH and DRY/WET are ONE dropdown now: "haz que morph y dry/wet sean dos
        // opciones dentro de un mismo desplegable en el submenu transition". That box owns
        // no parameter - it is a VIEW of the "morph" and "dryWet" booleans - and a view
        // that can disagree with what the voice is doing is worse than no view at all, so
        // what it shows is measured against every state those two flags can be in.
        //
        // FOUR STATES, THREE ITEMS, and the fourth is the one worth measuring: with both
        // flags set the voice MORPHS (a voice has one crossfade between two banks, so
        // Morph wins - bf0f768, and the audio checks above), and the box must therefore
        // read Morph. That is also what makes an old project open on the right item with
        // no migration at all, so it is a claim about users' files and not only about a
        // combo. The widest ITEM has to fit too, in both languages: a ComboBox neither
        // clips visibly nor resizes when the mode changes, so a box wide enough for the
        // selected word silently truncates the others.
        {
            auto owned = std::make_unique<FractalAdditiveAudioProcessor>();
            owned->prepareToPlay (srD, eb);
            std::unique_ptr<juce::AudioProcessorEditor> base (owned->createEditor());
            auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
            check (ed != nullptr, "the editor builds");
            if (ed != nullptr)
            {
                const auto setFlags = [&] (bool morphOn, bool dwOn)
                {
                    if (auto* q = owned->apvts.getParameter ("morph"))  q->setValueNotifyingHost (morphOn ? 1.0f : 0.0f);
                    if (auto* q = owned->apvts.getParameter ("dryWet")) q->setValueNotifyingHost (dwOn    ? 1.0f : 0.0f);
                    owned->pollParametersForTests();
                    ed->applyTransitionModeForTests();   // the same call the 30 Hz timer makes
                    ed->resized();
                };
                // The four states, once: what the box SHOWS is an item id, which is the
                // same number in either language.
                struct Row { bool morph, dw; int want; const char* what; };
                const Row rows[] = { { false, false, 1, "both off  -> Off" },
                                     { true,  false, 2, "morph     -> Morph" },
                                     { false, true,  3, "dryWet    -> Dry/Wet" },
                                     { true,  true,  2, "both on   -> Morph (the voice's own rule)" } };
                for (const auto& r : rows)
                {
                    setFlags (r.morph, r.dw);
                    const int got = ed->transitionItemShownForTests();
                    std::printf ("  %-42s box shows %d\n", r.what, got);
                    check (got == r.want,
                           "the TRANSITION box shows what the voice will actually do with these two flags");
                    // ...and the ONE Time fader on screen is that mode's own, so the
                    // number under it is the time that will really be used.
                    check (ed->morphTimeFaderShownForTests() == (r.want == 2),
                           "...and the Time fader on screen belongs to the mode it shows");
                    // Off can time nothing, so its fader is dead - the same idiom AMP
                    // DECAY uses when manual amplitudes freeze it.
                    check (ed->transitionTimeFaderLiveForTests() == (r.want != 1),
                           "...and it is live exactly when there is a transition to time");
                }

                // The WIDEST item against the room the painter really leaves it, read off
                // the probe's own combo measurement (see LayoutProbe::comboTexts). From
                // the RETURNED probe and never from the editor afterwards:
                // layoutProbeInLanguage measures in the language asked for and then puts
                // the editor back the way it found it, so a check that re-read the live
                // box would measure English twice and report the Catalan words untested.
                setFlags (false, true);
                for (int lang = 0; lang < 2; ++lang)
                {
                    const auto pr = ed->layoutProbeInLanguage (lang == 1);
                    bool seen = false;
                    for (const auto& c : pr.comboTexts)
                        if (c.name == "transition")
                        {
                            seen = true;
                            std::printf ("  %s: widest TRANSITION item \"%s\" needs %.1f px of %d\n",
                                         lang == 1 ? "ca" : "en", c.text.toRawUTF8(), c.need, c.avail);
                            check (c.need <= (float) c.avail + 0.5f,
                                   "every TRANSITION item fits the box, in this language - the box does not"
                                   " resize when the mode changes and a clipped item says nothing");
                        }
                    check (seen, "...and that box is actually probed, in this language");
                }
                ed->layoutProbeInLanguage (false);   // leave the panel in English
            }
        }
    }

    // --- run 51: CASE #88 - A CORNER IS A FIXED POINT OF ITS OWN BLEND ---------------
    // "si por ejemplo voy adelante y atras entre dos presets uno con menos amp sustain
    //  que el otro, el volumen va bajando hasta silenciarse... curiosamente con el modo
    //  mix pasa lo mismo."
    //
    // Run 47 drove ten A->B->A round trips through the real drivePlayground at its real
    // cadence and found 0 of ~200 parameters drifted, RMS ratio 1.000. The poll path is
    // clean and the loop does not live in memory: IT CLOSES THROUGH THE .gsting FILE.
    // While the pad drives, the live control surface is the blend's OUTPUT; a corner's
    // stored values and a corner's file are the blend's INPUT. Anywhere output can reach
    // input the corner stops being a fixed point and becomes the previous
    // interpolation's result, so every further trip drags it geometrically towards the
    // other corner - "va bajando hasta silenciarse" is a geometric series.
    //
    // Three doors, measured here:
    //   (a) Save a preset under an assigned corner's own name while parked mid-pad, then
    //       re-assign that corner        -> the corner's file IS the blend.
    //   (b) Same save, then a genuine host project save + reload, which re-reads every
    //       corner from disk by name     -> the drift comes back in with the project.
    //   (c) An UNTOUCHED corner-edit visit, on a corner whose preset predates a
    //       parameter -> the blend's value for that parameter is absorbed into the
    //       corner store and the "*" says nothing happened.
    // And (d): what a corner MEANS for a parameter its preset predates has to be ONE
    // answer that morph, mix and edit all agree on, or the vertex is path-dependent.
    {
        std::printf ("\nrun 51: case #88 - a corner must be a FIXED POINT of its own blend:\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr double psr = 48000.0;
        constexpr int    pblk = 256;

        // A folder of its own even within the suite's own scratch folder, because this
        // block rewrites the SAME preset name twenty times: nothing else may be reading
        // "zzz88-A" while it walks. Restores whatever was in force before, never "the
        // user's real folder" - that mistake would hand the twenty overwrites straight
        // to Documents/Galactic Sting/Presets.
        const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("fiblo-run51-"
                                     + juce::String (juce::Time::getHighResolutionTicks() & 0xffffffLL));
        struct ScratchFolder
        {
            explicit ScratchFolder (const juce::File& d)
                : dir (d), prev (Proc::getPresetFolder()) { Proc::setPresetFolderForTests (d); }
            ~ScratchFolder() { Proc::setPresetFolderForTests (prev); dir.deleteRecursively(); }
            juce::File dir, prev;
        } scratchGuard (scratch);
        check (Proc::getPresetFolder() == scratch,
               "run 51 writes its presets in a scratch folder of its own");

        auto p = std::make_unique<Proc>();
        p->prepareToPlay (psr, pblk);
        juce::AudioBuffer<float> ab (2, pblk);

        auto sp = [&] (const char* id, float plain)
        {
            if (auto* q = p->apvts.getParameter (id))
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
        };
        auto gp = [&] (const char* id) { return p->apvts.getRawParameterValue (id)->load(); };
        auto paramDefault = [&] (const char* id)
        {
            auto* q = p->apvts.getParameter (id);
            return q != nullptr ? q->convertFrom0to1 (q->getDefaultValue()) : 0.0f;
        };
        auto frame = [&] { p->pollParametersForTests(); juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); };
        auto park  = [&] (float x, float y, int frames = 12)
        {
            sp ("pgX", x); sp ("pgY", y);
            for (int i = 0; i < frames; ++i) frame();
        };
        // Run 47's measurement, unchanged: a freshly-struck note, RMS well into its sustain.
        auto noteRms = [&]
        {
            awaitSpectrumIdle (*p, "run 51 blend before an RMS reading");
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
            for (int b = 0; b < (int) (1.5 * psr / pblk); ++b)
            { juce::MidiBuffer m; ab.clear(); p->processBlock (ab, m); }
            return std::sqrt (sum / (double) juce::jmax (1L, n));
        };

        // TWO CORNERS THAT DIFFER ONLY IN LEVEL, which is the user's own case ("uno con
        // menos amp sustain que el otro"): everything else identical, so any change in
        // the rendered RMS at corner A is the drain and nothing else.
        auto writeCorners = [&]
        {
            sp ("timbreOn", 0.0f); sp ("timbreTemper", 0.0f); sp ("phaseMode", 0.0f);
            sp ("morph", 0.0f); sp ("pgMode", 0.0f);
            sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
            sp ("ampAttack", 0.005f); sp ("ampDecay", 0.05f); sp ("ampRelease", 0.05f);
            sp ("filterCutoff", 12000.0f); sp ("baseN", 1.0f); sp ("curveSlope", 1.5f);
            sp ("ampSustain", 0.90f); sp ("masterGain", 0.0f);
            p->pollParametersForTests();
            check (p->savePreset ("zzz88-A").isNotEmpty(), "corner preset A saved (sustain 0.90, gain 0 dB)");
            sp ("ampSustain", 0.05f); sp ("masterGain", -18.0f);
            p->pollParametersForTests();
            check (p->savePreset ("zzz88-B").isNotEmpty(), "corner preset B saved (sustain 0.05, gain -18 dB)");
        };
        writeCorners();

        juce::String err;
        check (p->assignPlaygroundCorner (0, "zzz88-A", err), "A on corner 0");
        check (p->assignPlaygroundCorner (1, "zzz88-B", err), "B on corner 1");
        check (p->isPlaygroundActive(), "two corners engage the pad");

        // ---- THE RULE ITSELF, before the drain it exists to kill is measured --------
        // A CORNER IS A FIXED POINT. Three things have to hold at once for that to be a
        // rule and not a lockout, so all three are asserted, not just the refusal: it
        // refuses the write that closes the loop, it refuses ONLY that write, and it
        // leaves the one legitimate door onto a corner's file open.
        {
            park (0.5f, 0.0f);                 // the pad drives: the surface is blend(A, B)
            const auto before = Proc::presetFile ("zzz88-A").loadFileAsString();

            auto why  = Proc::PresetWriteRefusal::None;
            int  blkd = -1;
            const auto refused = p->savePreset ("zzz88-A", why, blkd);
            check (refused.isEmpty() && why == Proc::PresetWriteRefusal::CornerFixedPoint && blkd == 0,
                   "saving over an assigned corner's own name while the pad drives is REFUSED, "
                   "and the refusal names the corner so the editor can say which one");
            check (Proc::presetFile ("zzz88-A").loadFileAsString() == before,
                   "...and corner A's file is byte-for-byte untouched - refused, not half-written");

            // NOT a lockout: every other name still saves mid-pad, exactly as before. A
            // rule that stopped the user saving the blend they just dialled in would be a
            // worse bug than the drain.
            auto why2 = Proc::PresetWriteRefusal::None; int b2 = -1;
            check (p->savePreset ("zzz88-blend", why2, b2).isNotEmpty()
                       && why2 == Proc::PresetWriteRefusal::None,
                   "...while saving that same blend under a name of its own is still allowed");
            Proc::presetFile ("zzz88-blend").deleteFile();

            // And the door that IS legitimate: inside corner A's Edit session the surface
            // is corner A - beginPlaygroundCornerEdit put it there - so its own Save onto
            // its own name goes through. That asymmetry is the whole rule.
            check (p->beginPlaygroundCornerEdit (0), "corner A's Edit session opens");
            check (p->savePlaygroundCornerAs (0, "zzz88-A") == "zzz88-A",
                   "...and the corner's OWN Edit -> Save, onto its own name, still goes through");
            p->endPlaygroundCornerEdit();

            // Put file and corner back exactly as this block found them: the Edit -> Save
            // above legitimately rewrote zzz88-A, and the measurement below compares the
            // corner's store against what its file says.
            Proc::presetFile ("zzz88-A").replaceWithText (before);
            juce::String e3;
            check (p->assignPlaygroundCorner (0, "zzz88-A", e3),
                   "corner A restored from its original file for the measurement");
            park (0.0f, 0.0f);
        }

        // ---- (a)/(b) SAVE MID-PAD, THEN RE-READ THE CORNER. Twenty round trips. ----
        constexpr int kTrips = 20;
        auto roundTrips = [&] (bool viaProjectReload, const char* label)
        {
            park (0.0f, 0.0f);
            const auto store0 = p->getPlaygroundCornerParamsForTests (0);
            const double rms0 = noteRms();
            std::vector<double> sus, gain, rms;
            sus.push_back (p->getPlaygroundCornerParamForTests (0, "ampSustain"));
            gain.push_back (p->getPlaygroundCornerParamForTests (0, "masterGain"));
            rms.push_back (rms0);
            int firstDrift = -1;

            for (int trip = 0; trip < kTrips; ++trip)
            {
                // Parked MID-PAD: the surface is the blend of A and B, not A.
                park (0.5f, 0.0f);
                // The user hits Save and types the name the corner already carries. This
                // is the whole gesture - nothing exotic, nothing scripted.
                p->savePreset ("zzz88-A");
                if (viaProjectReload)
                {
                    // A genuine host project save + reload: setStateInformation re-reads
                    // every corner from disk BY NAME (captureState stores only names).
                    juce::MemoryBlock mb;
                    p->getStateInformation (mb);
                    auto q = std::make_unique<Proc>();
                    q->prepareToPlay (psr, pblk);
                    q->setStateInformation (mb.getData(), (int) mb.getSize());
                    p = std::move (q);
                }
                else
                {
                    juce::String e2;
                    p->assignPlaygroundCorner (0, "zzz88-A", e2);
                }
                park (0.0f, 0.0f);
                const auto now = p->getPlaygroundCornerParamsForTests (0);
                if (firstDrift < 0 && now != store0) firstDrift = trip;
                sus.push_back (p->getPlaygroundCornerParamForTests (0, "ampSustain"));
                gain.push_back (p->getPlaygroundCornerParamForTests (0, "masterGain"));
                if (trip < 6 || trip == kTrips - 1) rms.push_back (noteRms());
            }

            std::printf ("  [%s] corner A's STORED ampSustain per cycle:", label);
            for (size_t i = 0; i < sus.size() && i < 9; ++i) std::printf (" %.4f", sus[i]);
            std::printf (" ... %.4f\n", sus.back());
            std::printf ("  [%s] corner A's STORED masterGain  per cycle:", label);
            for (size_t i = 0; i < gain.size() && i < 9; ++i) std::printf (" %.2f", gain[i]);
            std::printf (" ... %.2f\n", gain.back());
            std::printf ("  [%s] rendered RMS at corner A     :", label);
            for (const double v : rms) std::printf (" %.5f", v);
            std::printf ("\n");

            // Is the fall MONOTONE and GEOMETRIC? Each cycle's remaining gap to B's own
            // value, over the previous cycle's gap, is the blend weight; a CONSTANT
            // ratio is the signature of a feedback loop closing, as opposed to noise or
            // a one-off mistake. This is the claim the fix has to kill.
            {
                bool mono = true; double rMin = 1.0e9, rMax = -1.0e9;
                for (size_t i = 1; i < sus.size(); ++i)
                {
                    if (sus[i] > sus[i - 1] + 1.0e-6) mono = false;
                    const double gPrev = sus[i - 1] - 0.05, gNow = sus[i] - 0.05;
                    if (gPrev > 1.0e-4) { rMin = std::min (rMin, gNow / gPrev); rMax = std::max (rMax, gNow / gPrev); }
                }
                std::printf ("  [%s] fall: monotone %s, gap ratio %.4f..%.4f, first drifting cycle %d\n",
                             label, mono ? "YES" : "no", rMin, rMax, firstDrift);
            }

            const auto storeEnd = p->getPlaygroundCornerParamsForTests (0);
            check (storeEnd == store0,
                   viaProjectReload
                     ? "twenty save + PROJECT RELOAD cycles leave corner A's stored values bit-identical"
                     : "twenty save + RE-ASSIGN cycles leave corner A's stored values bit-identical");
            const double rmsEnd = rms.back();
            check (rmsEnd > rms0 * 0.98 && rmsEnd < rms0 * 1.02,
                   viaProjectReload
                     ? "...and the note at corner A is as loud as it started (no drain through the project)"
                     : "...and the note at corner A is as loud as it started (no drain through the file)");
        };
        roundTrips (false, "save + re-assign");

        // Start (b) from clean files: (a) has already rewritten zzz88-A if the bug is live.
        p->assignPlaygroundCorner (0, {}, err);
        p->assignPlaygroundCorner (1, {}, err);
        writeCorners();
        check (p->assignPlaygroundCorner (0, "zzz88-A", err), "A back on corner 0, from a clean file");
        check (p->assignPlaygroundCorner (1, "zzz88-B", err), "B back on corner 1");
        roundTrips (true, "save + project reload");

        // ---- (c) AN UNTOUCHED CORNER-EDIT VISIT MUST CHANGE NOTHING ----------------
        // A corner whose preset PREDATES a parameter is ordinary: any preset saved
        // before a feature existed predates it, and five of the seven presets in the
        // user's own folder predate at least one. Built here by deleting one parameter
        // from a saved file, which is byte-for-byte what an older preset looks like.
        {
            p->assignPlaygroundCorner (0, {}, err);
            p->assignPlaygroundCorner (1, {}, err);
            writeCorners();
            auto oldFile = Proc::presetFile ("zzz88-A");
            bool stripped = false;
            if (auto xml = juce::XmlDocument::parse (oldFile))
            {
                juce::XmlElement* victim = nullptr;
                for (auto* ch : xml->getChildIterator())
                    if (ch->getStringAttribute ("id") == "ampSustain") { victim = ch; break; }
                if (victim != nullptr) { xml->removeChildElement (victim, true); stripped = xml->writeTo (oldFile); }
            }
            check (stripped, "corner A's preset is rewritten WITHOUT ampSustain (a preset that predates it)");

            check (p->assignPlaygroundCorner (0, "zzz88-A", err), "the predating preset goes on corner 0");
            check (p->assignPlaygroundCorner (1, "zzz88-B", err), "B on corner 1");

            // Park MID-PAD so the blend is driving the surface, then visit corner 0's
            // editor and leave without touching a thing.
            park (0.5f, 0.0f);
            const float blendSus = gp ("ampSustain");
            const auto store0 = p->getPlaygroundCornerParamsForTests (0);
            const float sus0   = p->getPlaygroundCornerParamForTests (0, "ampSustain");
            park (0.0f, 0.0f);
            const double rmsBefore = noteRms();

            park (0.5f, 0.0f);
            check (p->beginPlaygroundCornerEdit (0), "Edit opens on the corner whose preset predates ampSustain");
            for (int i = 0; i < 8; ++i) frame();      // an untouched visit: nothing but time passes
            p->endPlaygroundCornerEdit();
            for (int i = 0; i < 8; ++i) frame();

            const auto store1 = p->getPlaygroundCornerParamsForTests (0);
            const float sus1  = p->getPlaygroundCornerParamForTests (0, "ampSustain");
            park (0.0f, 0.0f);
            const double rmsAfter = noteRms();
            std::printf ("  untouched corner-edit visit: the blend was driving ampSustain at %.4f; "
                         "corner 0's stored value %.4f -> %.4f (default %.4f); \"*\" says %s; "
                         "RMS at corner A %.5f -> %.5f\n",
                         blendSus, sus0, sus1, paramDefault ("ampSustain"),
                         p->isPlaygroundCornerEdited (0) ? "EDITED" : "nothing happened",
                         rmsBefore, rmsAfter);
            check (store1 == store0,
                   "an UNTOUCHED corner-edit visit leaves the corner's stored values exactly as it found them");
            check (! p->isPlaygroundCornerEdited (0),
                   "...and the \"*\" is right that nothing happened");
            check (rmsAfter > rmsBefore * 0.98 && rmsAfter < rmsBefore * 1.02,
                   "...and corner A still sounds exactly as loud as before the visit");

            // ---- (d) ONE ANSWER for a parameter the corner's preset predates -------
            // The corner's own preset has no value for ampSustain, so the corner must
            // mean the parameter's DEFAULT - the rule ParamBank::fill already applies in
            // MIX mode and cornerValueOr already documents. Anything else makes the
            // vertex path-dependent: parked exactly on corner 0 the value would be
            // whatever the last write happened to leave there.
            const float dflt = paramDefault ("ampSustain");
            park (0.0f, 0.0f);
            const float vertexFresh = gp ("ampSustain");
            park (0.5f, 0.0f);
            const float mid = gp ("ampSustain");
            park (1.0f, 0.0f);
            park (0.0f, 0.0f);
            const float vertexAfter = gp ("ampSustain");
            std::printf ("  predated parameter: default %.4f; at corner 0 fresh %.4f, "
                         "at mid-pad %.4f, back at corner 0 %.4f\n",
                         dflt, vertexFresh, mid, vertexAfter);
            check (std::abs (vertexFresh - vertexAfter) < 1.0e-4f,
                   "the vertex value for a predated parameter is NOT path-dependent");
            check (std::abs (vertexFresh - dflt) < 1.0e-4f,
                   "...and it is the parameter's DEFAULT - the one answer morph, mix and edit share");
            // STRICTLY BETWEEN, not merely "within the range". Written as a range first,
            // this assertion PASSED while the parameter read 0.0500 - corner B's own
            // value - at every point on the pad including corner 0's vertex, which is
            // the precise symptom (hunt entry 11) it exists to catch: B's value is an
            // endpoint of the range, so a range test can never see 100%-ruled-by-B.
            const float lo = std::min (dflt, 0.05f), hi = std::max (dflt, 0.05f);
            check (mid > lo + 0.05f * (hi - lo) && mid < hi - 0.05f * (hi - lo),
                   "...so mid-pad really blends the two corners instead of being ruled 100% by one");
        }

        p->assignPlaygroundCorner (0, {}, err);
        p->assignPlaygroundCorner (1, {}, err);
    }


    // --- run 52: THE TRANSPORT, AND WHAT IT IS ALLOWED TO DESTROY --------------------
    // Five claims about what the transport and the host are allowed to destroy, measured
    // separately because they fail for five different reasons and one fix implies none
    // of the others:
    //   (A) with lookahead OFF - the shipping default - pressing PLAY or STOP, or a loop
    //       wrapping, must not touch a key the player is holding down;
    //   (B) with lookahead ARMED the same jump DELIBERATELY still kills, because there
    //       the ring really does hold events stamped for bars the transport has left;
    //   (C) a pending flush must be consumed by the block that owns it, never by the next;
    //   (D) whatever a flush throws away, a note-OFF already in flight must still be
    //       delivered, or everything downstream of the delay goes on believing the key
    //       is down - the rhythm engine loudest of all;
    //   (E) AudioProcessor::reset() is the host's "stop making sound now", and it owes
    //       both halves: silence, AND a synth that is not left deaf.
    // Every take here is offline and sample-counted - no wall clock anywhere in this run.
    {
        std::printf ("\nrun 52: the transport, and what it is allowed to destroy:\n");
        using Proc = FractalAdditiveAudioProcessor;
        constexpr double sr52 = 48000.0;
        constexpr int    b52  = 64;

        struct Head52 : juce::AudioPlayHead
        {
            juce::AudioPlayHead::PositionInfo pos;
            juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return pos; }
        };

        // A processor with a flat sustaining envelope and nothing standing between a
        // note-on and its voice: quantize-to-pulse is ON by default and would hold notes
        // back for reasons that have nothing to do with the transport.
        auto make52 = [&] ()
        {
            auto p = std::make_unique<Proc>();
            p->prepareToPlay (sr52, b52);
            auto sp = [&p] (const char* id, float v)
            { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
              p->pollParametersForTests(); };
            sp ("morph", 0.0f);
            sp ("dryWet", 0.0f);   // transition off too (it now defaults to Dry/Wet)
            sp ("ampAttack", 0.001f); sp ("ampDecay", 0.001f);
            sp ("ampSustain", 1.0f);  sp ("ampRelease", 0.003f);
            sp ("rhythmQPulse", 0.0f);
            return p;
        };
        auto noteOn52  = [] (int k) { juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0); return m; };
        auto noteOff52 = [] (int k) { juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOff (1, k), 0); return m; };

        //-- (A) LOOKAHEAD OFF: THE TRANSPORT MUST NOT CUT A HELD KEY --------------------
        // rtSet.lookahead defaults to false, so budget == 0 is not "a configuration" -
        // it is what every player gets who never opens the realtime panel. The kill this
        // guards against arrived in d44263e (whose parent has no transport-jump handling
        // at all) and it is symmetric, so STOP cut the held key exactly as PLAY did.
        {
            auto p = make52();
            Head52 head;
            head.pos.setBpm (120.0); head.pos.setIsPlaying (false); head.pos.setPpqPosition (0.0);
            p->setPlayHead (&head);
            check (! p->isLookaheadArmed() && p->getLookaheadSamples() == 0,
                   "run 52(A) measures the state a player who never opens the realtime panel is in: lookahead OFF");

            juce::AudioBuffer<float> ab (2, b52);
            double ppq = 0.0;
            const double dPpq = (double) b52 * 120.0 / (60.0 * sr52);
            auto pump = [&] (int blocks, juce::MidiBuffer first) -> double
            {
                double sum = 0.0; int n = 0;
                for (int i = 0; i < blocks; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m = first;
                    head.pos.setPpqPosition (ppq);
                    if (head.pos.getIsPlaying()) ppq += dPpq;
                    ab.clear();
                    p->processBlock (ab, m);
                    const float* r = ab.getReadPointer (0);
                    for (int k = 0; k < b52; ++k) { sum += (double) r[k] * r[k]; ++n; }
                }
                return n > 0 ? std::sqrt (sum / (double) n) : 0.0;
            };

            pump (40, noteOn52 (60));                       // key pressed, transport stopped
            const double rmsHeld = pump (40, {});
            const int    vHeld   = p->activeVoiceCountForTests();

            head.pos.setIsPlaying (true);                   // ... the player presses PLAY
            const double rmsPlay = pump (40, {});
            const int    vPlay   = p->activeVoiceCountForTests();

            ppq = 0.0;                                      // ... the loop wraps under it
            const double rmsWrap = pump (40, {});
            const int    vWrap   = p->activeVoiceCountForTests();

            head.pos.setIsPlaying (false);                  // ... and the player presses STOP
            const double rmsStop = pump (40, {});
            const int    vStop   = p->activeVoiceCountForTests();

            std::printf ("  key HELD, lookahead off: voices %d -> %d (play) -> %d (loop wrap) -> %d (stop),"
                         " rms %.5f -> %.5f -> %.5f -> %.5f\n",
                         vHeld, vPlay, vWrap, vStop, rmsHeld, rmsPlay, rmsWrap, rmsStop);
            check (vHeld == 1 && rmsHeld > 1.0e-4,
                   "run 52(A) control: the held key really is sounding before the transport moves");
            check (vPlay == vHeld && rmsPlay > rmsHeld * 0.5,
                   "pressing PLAY does not touch a key the player is holding (lookahead off: nothing is in flight)");
            check (vWrap == vHeld && rmsWrap > rmsHeld * 0.5,
                   "...nor does the loop wrapping under it");
            check (vStop == vHeld && rmsStop > rmsHeld * 0.5,
                   "...nor does pressing STOP (the jump edge is symmetric, so it killed too)");
            p->setPlayHead (nullptr);
        }

        //-- (B) LOOKAHEAD ARMED: the same jump STILL flushes, and that is on purpose ----
        // The two halves differ because what they are protecting differs. Armed, the ring
        // holds events the host stamped for bars it has now left; replaying them at the
        // loop point is a ghost note the player never wrote. Off, there is no ring at all,
        // so there is nothing the jump could possibly invalidate.
        {
            auto p = make52();
            Head52 head;
            head.pos.setBpm (120.0); head.pos.setIsPlaying (true); head.pos.setPpqPosition (0.0);
            p->setPlayHead (&head);
            Proc::RtSettings rs; rs.lookahead = true;
            // SpectrumFromTuning arms the budget without a chord-driven search: this run
            // measures MIDI timing, and a searcher launching on every note-on would only
            // add CPU to a take that must stay deterministic.
            p->setRealtimeLink (Proc::RtLink::SpectrumFromTuning, rs);
            const int bud = p->getLookaheadSamples();
            check (bud > 0, "run 52(B) control: arming lookahead really does buy a budget");

            juce::AudioBuffer<float> ab (2, b52);
            double ppq = 0.0;
            const double dPpq = (double) b52 * 120.0 / (60.0 * sr52);
            auto pump = [&] (int blocks, juce::MidiBuffer first)
            {
                for (int i = 0; i < blocks; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m = first;
                    head.pos.setPpqPosition (ppq); ppq += dPpq;
                    ab.clear();
                    p->processBlock (ab, m);
                }
            };
            pump (4, {});
            pump (bud / b52 + 40, noteOn52 (60));   // past the budget: the note is sounding
            const int vArmed = p->activeVoiceCountForTests();
            ppq = 0.0;                              // the loop wraps
            pump (8, {});
            const int vAfter = p->activeVoiceCountForTests();
            std::printf ("  key HELD, lookahead ARMED (%d-sample budget): voices %d -> %d across a loop wrap\n",
                         bud, vArmed, vAfter);
            check (vArmed == 1, "run 52(B) control: the delayed note really did sound");
            check (vAfter == 0,
                   "with lookahead ARMED a loop wrap still silences - the ring holds bars the transport has left");
            p->setRealtimeLink (Proc::RtLink::Off, rs);
            p->setPlayHead (nullptr);
        }

        //-- (C) A PENDING FLUSH IS CONSUMED BY THE BLOCK THAT OWNS IT ------------------
        // `jumped || laFlush.exchange (...)` short-circuits: on a block that had jumped
        // the exchange never ran, so the pending flush survived into the NEXT block and
        // destroyed a ring that by then held a real note. The pending flag is set on the
        // first poll after prepareToPlay (laLastSr starts at 0), so this fires on every
        // offline bounce and every transport start with lookahead armed.
        // The two takes are the SAME processor, the SAME MIDI and the SAME block count.
        // The only difference is whether a playhead is attached - which is the only thing
        // that can make `jumped` true - so a difference between them can only be this.
        {
            int budSeen = 0;
            auto firstAudible = [&] (bool withHead) -> long long
            {
                auto p = make52();
                Head52 head;
                head.pos.setBpm (120.0); head.pos.setIsPlaying (true); head.pos.setPpqPosition (0.0);
                if (withHead) p->setPlayHead (&head);
                Proc::RtSettings rs; rs.lookahead = true;
                p->setRealtimeLink (Proc::RtLink::SpectrumFromTuning, rs);   // <- leaves a flush pending
                budSeen = p->getLookaheadSamples();
                juce::AudioBuffer<float> ab (2, b52);
                double ppq = 0.0;
                const double dPpq = (double) b52 * 120.0 / (60.0 * sr52);
                long long onset = -1, at = 0;
                for (int i = 0; i < 400 && onset < 0; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m = noteOn52 (60);
                    head.pos.setPpqPosition (ppq); ppq += dPpq;
                    ab.clear();
                    p->processBlock (ab, m);
                    const float* r = ab.getReadPointer (0);
                    for (int k = 0; k < b52; ++k)
                        if (onset < 0 && std::abs (r[k]) > 1.0e-7f) onset = at + k;
                    at += b52;
                }
                p->setRealtimeLink (Proc::RtLink::Off, rs);
                p->setPlayHead (nullptr);
                return onset;
            };
            const long long noHead   = firstAudible (false);
            const long long withHead = firstAudible (true);
            std::printf ("  a note struck in the first block, lookahead armed: first audible sample"
                         " %lld with no playhead, %lld with one (budget %d; -1 = never sounded)\n",
                         noHead, withHead, budSeen);
            check (noHead == (long long) budSeen,
                   "run 52(C) control: with no playhead the note sounds exactly one budget later");
            check (withHead == noHead,
                   "attaching a PLAYHEAD does not swallow the note - a pending flush fires in its own block, not the next");
        }

        //-- (D) A NOTE-OFF IN FLIGHT IS STILL DELIVERED --------------------------------
        // The note-on it belonged to left the ring one budget earlier: it has already
        // sounded and, more to the point, has already been counted by everything
        // downstream of the delay - the note gate, the mono glide, the modulator tally
        // and the rhythm engine's held chord. Dropping only its note-off leaves all four
        // believing the key is still down, and the rhythm engine then generates pulse
        // trains from a key nobody is holding and emits that MIDI to the DAW for the rest
        // of the session. Silencing the voices, which the flush already does, cures the
        // audio and none of that.
        {
            auto p = make52();
            // One pulse train, routed and fast: divisor 2^3 puts harmonic 2 of key 60 at
            // ~65 Hz, so a train that is still running is unmistakable within the tail
            // below instead of being one pulse every 367 blocks at the default 2^8.
            {
                auto sp = [&p] (const char* id, float v)
                { auto* q = p->apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v));
                  p->pollParametersForTests(); };
                sp ("rhythmOn", 1.0f);          // pulses leave the plugin, so they are countable
                sp ("rhythmDivN", 3.0f);
                p->getRhythm().slots[0].partial = 1; p->getRhythm().slots[0].enabled = true;
                p->getRhythm().slots[0].steps   = 0; p->getRhythm().slots[0].durMs = 5.0f;
            }
            Head52 head;
            head.pos.setBpm (120.0); head.pos.setIsPlaying (true); head.pos.setPpqPosition (0.0);
            p->setPlayHead (&head);
            Proc::RtSettings rs; rs.lookahead = true;
            p->setRealtimeLink (Proc::RtLink::SpectrumFromTuning, rs);
            const int bud = p->getLookaheadSamples();

            juce::AudioBuffer<float> ab (2, b52);
            double ppq = 0.0;
            const double dPpq = (double) b52 * 120.0 / (60.0 * sr52);
            int pulsesBefore = 0, pulsesAfter = 0;
            const int budBlocks = bud / b52;
            const int onBlock   = 2;
            const int offBlock  = onBlock + budBlocks + 40;   // the note has long since sounded
            const int wrapBlock = offBlock + 4;               // its note-off is still in the ring
            const int settle    = wrapBlock + budBlocks + 40; // past every gate the wrap owed
            const int tail      = 600;
            for (int i = 0; i < settle + tail; ++i)
            {
                juce::MidiBuffer m;
                if (i == onBlock)  m = noteOn52 (60);
                if (i == offBlock) m = noteOff52 (60);
                if (i == wrapBlock) ppq = 0.0;                // the loop wraps
                head.pos.setPpqPosition (ppq); ppq += dPpq;
                ab.clear();
                p->processBlock (ab, m);
                for (const auto meta : m)
                    if (meta.getMessage().isNoteOn())
                    { if (i < offBlock) ++pulsesBefore; else if (i >= settle) ++pulsesAfter; }
            }
            std::printf ("  rhythm MIDI out: %d pulse note-ons while the key was down,"
                         " %d in the %d blocks AFTER the wrap ate its note-off\n",
                         pulsesBefore, pulsesAfter, tail);
            check (pulsesBefore > 0,
                   "run 52(D) control: this configuration really does emit pulse MIDI while a key is held");
            check (pulsesAfter == 0,
                   "a note-off caught in the flush is still delivered - no ghost pulses from a key nobody is holding");
            check (p->activeVoiceCountForTests() == 0,
                   "...and nothing is left sounding either");
            p->setRealtimeLink (Proc::RtLink::Off, rs);
            p->setPlayHead (nullptr);
        }
        //-- (E) reset() IS THE HOST'S "STOP MAKING SOUND NOW" --------------------------
        // A bypass or a panic calls it and expects silence. It must also NOT leave the
        // synth deaf: a reset that silences for ever is the opposite failure and is just
        // as reportable, so both halves are asserted in one take.
        {
            auto p = make52();
            juce::AudioBuffer<float> ab (2, b52);
            auto pump = [&] (int blocks, juce::MidiBuffer first) -> double
            {
                double sum = 0.0; int n = 0;
                for (int i = 0; i < blocks; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m = first;
                    ab.clear();
                    p->processBlock (ab, m);
                    const float* r = ab.getReadPointer (0);
                    for (int k = 0; k < b52; ++k) { sum += (double) r[k] * r[k]; ++n; }
                }
                return n > 0 ? std::sqrt (sum / (double) n) : 0.0;
            };
            const double rmsOn = pump (80, noteOn52 (60));
            const int    vOn   = p->activeVoiceCountForTests();
            p->reset();
            // The kill is a fast FADE, not a hard cut, because a hard cut clicks - so
            // silence is asserted AFTER it, not during it. Five blocks is 6.7 ms here
            // against a fade measured at about 1 ms of equivalent energy; the window
            // that follows is the one that has to be flat, and it is 40 blocks long so
            // a fade that had quietly become a slow release could not hide inside it.
            const double rmsFade  = pump (5, {});
            const double rmsReset = pump (40, {});
            const int    vReset   = p->activeVoiceCountForTests();
            const double rmsNew   = pump (80, noteOn52 (67));
            const int    vNew     = p->activeVoiceCountForTests();
            std::printf ("  reset(): voices %d -> %d -> %d (new note), rms %.5f -> %.7f (fade) ->"
                         " %.7f -> %.5f\n",
                         vOn, vReset, vNew, rmsOn, rmsFade, rmsReset, rmsNew);
            check (vOn == 1 && rmsOn > 1.0e-4, "run 52(E) control: the note really is sounding before the reset");
            check (vReset == 0 && rmsReset < 1.0e-6,
                   "reset() silences the sounding voice - a host bypass or panic does not leave a note hanging");
            check (vNew == 1 && rmsNew > 1.0e-4,
                   "...and the synth is not left DEAF: a note struck after the reset sounds normally");

            // The lookahead ring is part of "stop making sound now" too: a note already
            // buffered when the reset arrives must never surface a budget later.
            auto q = make52();
            Proc::RtSettings rs; rs.lookahead = true;
            q->setRealtimeLink (Proc::RtLink::SpectrumFromTuning, rs);
            const int bud = q->getLookaheadSamples();
            juce::AudioBuffer<float> qb (2, b52);
            double sum = 0.0; int n = 0;
            for (int i = 0; i < bud / b52 + 200; ++i)
            {
                juce::MidiBuffer m; if (i == 0) m = noteOn52 (60);
                if (i == 2) q->reset();
                qb.clear();
                q->processBlock (qb, m);
                const float* r = qb.getReadPointer (0);
                for (int k = 0; k < b52; ++k) { sum += (double) r[k] * r[k]; ++n; }
            }
            const double rmsBuffered = std::sqrt (sum / (double) n);
            std::printf ("  reset() 2 blocks after a note entered a %d-sample ring: rms over the whole take %.7f\n",
                         bud, rmsBuffered);
            check (rmsBuffered < 1.0e-6,
                   "reset() flushes the lookahead ring - a buffered note does not surface a budget later");
            q->setRealtimeLink (Proc::RtLink::Off, rs);

            // THE ONE THING THE OLD reset() DID RIGHT, and it must survive being rewritten:
            // any pulse gate that is OPEN when the reset lands still owes its note-off, or
            // the receiving track is left with a suspended note and no rhythm under it.
            // Gate length is set long on purpose so a gate is certainly open at the moment
            // reset() is called - the balance below is only a real question while one is.
            auto g = make52();
            {
                auto sp = [&g] (const char* id, float v)
                { auto* x = g->apvts.getParameter (id); x->setValueNotifyingHost (x->convertTo0to1 (v));
                  g->pollParametersForTests(); };
                sp ("rhythmOn", 1.0f);
                sp ("rhythmDivN", 3.0f);
                g->getRhythm().slots[0].partial = 1; g->getRhythm().slots[0].enabled = true;
                g->getRhythm().slots[0].steps   = 0; g->getRhythm().slots[0].durMs = 500.0f;
            }
            juce::AudioBuffer<float> gb (2, b52);
            int gOns = 0, gOffs = 0;
            for (int i = 0; i < 400; ++i)
            {
                juce::MidiBuffer m; if (i == 0) m = noteOn52 (60);
                if (i == 60) g->reset();          // mid-gate, by construction
                gb.clear();
                g->processBlock (gb, m);
                for (const auto meta : m)
                { if (meta.getMessage().isNoteOn()) ++gOns; else if (meta.getMessage().isNoteOff()) ++gOffs; }
            }
            std::printf ("  reset() during an open %g ms pulse gate: %d pulse note-ons, %d note-offs\n",
                         500.0, gOns, gOffs);
            check (gOns > 0, "run 52(E) control: a pulse gate really was open when the reset landed");
            check (gOns == gOffs,
                   "reset() still owes every open pulse gate its note-off - nothing hangs in the receiving track");
        }
    }

    // --- the audit: what did this suite leave behind, and where? ---------------------
    {
        std::printf ("\npreset folders, before and after:\n");
        // (1) THE SCRATCH FOLDER: the same per-site cleanup audit as before. Every file
        // still here is a block that saved a preset and did not remove it.
        const auto leftBehind = presetFolderInventory (scratchPresets);
        std::printf ("  scratch folder %s: %d preset(s) left behind%s%s\n",
                     scratchPresets.getFileName().toRawUTF8(), leftBehind.size(),
                     leftBehind.isEmpty() ? "" : ": ",
                     leftBehind.joinIntoString (", ").toRawUTF8());
        check (leftBehind.isEmpty(),
               "every block that saved a preset cleaned it up again");

        // (2) THE USER'S OWN FOLDER. What is asserted here is a fact about THIS PROCESS,
        // not about the directory, and the difference matters.
        //
        // The tempting assertion - "the user's folder is byte-for-byte the list it was" -
        // is not this suite's to make: measured on 2026-08-29, two RhythmVerify processes
        // running at once made it fail BOTH WAYS in consecutive runs of the same binary
        // (5 -> 7 presets, then 7 -> 5) as the other process's presets appeared and were
        // cleaned up again. A verdict that flips on what a different program is doing is
        // the same non-determinism that made every other verdict here untrustworthy, and
        // it would have blamed this run for someone else's files.
        //
        // What this process CAN prove is that it never had the user's folder open. The
        // redirect is installed before the first processor exists, and every block that
        // narrows it further restores what it FOUND, so the folder in force at the end
        // must still be the scratch one. A block that restored to {} instead of to its
        // predecessor - exactly what run 51's ScratchFolder guard did until it was fixed,
        // which would have handed twenty overwrites of one name to the user's folder -
        // lands here as a failure, deterministically, with no other process able to
        // affect the verdict.
        check (FractalAdditiveAudioProcessor::getPresetFolder() == scratchPresets,
               "the preset-folder redirect held for the WHOLE run: no block left the "
               "user's own folder in force under a later block's saves");

        FractalAdditiveAudioProcessor::setPresetFolderForTests ({});
        scratchPresets.deleteRecursively();
        const auto realNow = presetFolderInventory (realPresets);
        // Still REPORTED as a list, not a count - a run that deleted one patch and added
        // one of its own would pass a count - but reported, not asserted, and it names
        // the only thing it can honestly mean.
        std::printf ("  the user's folder: %d preset(s) before, %d after%s\n",
                     presetsAtStart.size(), realNow.size(),
                     realNow == presetsAtStart
                       ? "  (unchanged)"
                       : "  <- CHANGED, and not by this run: something else is writing it"
                         " (a second RhythmVerify? the plugin open in a host?)");
    }

    // --- run 52: THE CUSTOM ENGINE'S TEXT BOX DRIVES THE SPECTRUM -------------------
    //
    // "el custom spectrum no funciona bien ahora, solo funciona al modo random, no
    //  reacciona a los intervalos que se escriben o borran en la caja de texto."
    //
    // Nothing in this build ever asserted that TYPING into the Spectrum Builder changes
    // what sounds - the box had a layout check (its clearances) and a colour check (its
    // token spans) and no behaviour check at all, which is precisely how it could stop
    // working without a single verdict going red.
    //
    // Measured on the COMPILED PARTIAL LIST, not on the processor's stored text: the
    // stored text can be perfect while nothing rebuilds, and the sound is the claim.
    {
        std::printf ("\nthe Custom Engine's interval box drives the spectrum:\n");
        auto owned = std::make_unique<FractalAdditiveAudioProcessor>();
        auto& p = *owned;
        p.prepareToPlay (44100.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> base (p.createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
        check (ed != nullptr, "the editor builds");
        if (ed != nullptr)
        {
            ed->clickEngineChipForTests (1);   // the CUSTOM view: where the box lives

            // The ratios the live template really carries inside one period, rounded to
            // the cent - the sound, read back, rather than the settings that asked for it.
            auto sounding = [&p] () -> juce::String
            {
                p.pollParametersForTests();          // the rebuild test lives in the timer
                awaitSpectrumIdle (p, "typed intervals");
                const auto* t = p.getLiveTemplate();
                if (t == nullptr) return "<none>";
                juce::StringArray s;
                for (const auto& q : t->partials)
                    if (q.ratio <= 2.0001f)
                        s.add (juce::String ((int) std::llround (1200.0 * std::log2 ((double) q.ratio))) + "c");
                return s.joinIntoString (" ");
            };

            // TWICE. First with nothing else running - the plain question, "does the
            // handler work". Then LIVE: the editor on a real (off-screen) peer, so the
            // box holds REAL keyboard focus and the keys arrive through JUCE's own
            // routing, and this editor's 30 Hz timer ticking after every keystroke, which
            // is what a human's typing speed guarantees. That timer re-seeds this very box
            // from the processor; whether typing survives it is the question the first
            // pass structurally cannot ask, and it is the one the report is about.
            for (int mode = 0; mode < 2; ++mode)
            {
                const bool live = mode == 1;
                if (live)
                {
                    ed->setTopLeftPosition (-4000, -4000); // off every real screen
                    ed->setVisible (true);
                    ed->addToDesktop (0);
                    if (ed->getPeer() == nullptr) { std::printf ("  (no peer; live pass skipped)\n"); break; }
                }
                std::printf ("  --- %s ---\n", live ? "LIVE: real peer, real focus, the 30 Hz timer ticking between keys"
                                                    : "quiet: the handler alone, nothing else running");

                const auto a = ed->typeIntervalsProbeForTests ("5/4 3/2 2", 0, live);
                const auto soundA = sounding();
                std::printf ("  typed \"5/4 3/2 2\" -> box \"%s\", processor \"%s\", %d degrees, focus %d\n"
                             "    sounding (<= 1 period): %s\n",
                             a.boxText.toRawUTF8(), a.timbreSource.toRawUTF8(), a.timbreNotes,
                             (int) a.hadFocus, soundA.toRawUTF8());
                check (a.visible && a.enabled && ! a.readOnly && ! a.bounds.isEmpty(),
                       "the interval box is on screen, laid out, and can be typed into");
                check (a.boxText == "5/4 3/2 2",
                       "every character typed into it arrives - the box holds exactly what was typed");
                check (a.timbreSource == "5/4 3/2 2",
                       "...and pressing Return hands that text to the Custom Engine");
                check (a.timbreNotes == 3, "which reads it as three degrees (1/1, 5/4, 3/2 at period 2)");
                // Whole tokens, never substrings: "702c" is inside "1702c" and "-702c",
                // and a check that can be satisfied by a partial nobody asked about is
                // not a check. (It also nearly shipped as `contains ("386c")` against a
                // read-out printing 386.314c, which passed nothing and proved nothing.)
                auto has = [] (const juce::String& s, const char* cents)
                { return juce::StringArray::fromTokens (s, " ", "").contains (cents); };
                check (has (soundA, "386c") && has (soundA, "702c") && has (soundA, "1200c"),
                       "and the SOUND is that spectrum: 5/4 (386c) and 3/2 (702c) inside a"
                       " 2/1 period (1200c), in the compiled template");

                // ...AND DELETING ONE CHANGES IT BACK. Two backspaces take " 2" off the
                // end, which makes 3/2 the period and drops the 702c partial from inside
                // it - a change no "did the text change" check could tell from a no-op.
                const auto b = ed->typeIntervalsProbeForTests ("5/4 3/2 2", 2, live);
                const auto soundB = sounding();
                std::printf ("  then 2 backspaces -> box \"%s\", processor \"%s\", %d degrees, focus %d\n"
                             "    sounding (<= 1 period): %s\n",
                             b.boxText.toRawUTF8(), b.timbreSource.toRawUTF8(), b.timbreNotes,
                             (int) b.hadFocus, soundB.toRawUTF8());
                check (b.boxText == "5/4 3/2",
                       "backspace deletes from the box - it holds what is left");
                check (b.timbreSource == "5/4 3/2",
                       "...and Return hands the SHORTER list over, so a deletion is an edit too");
                check (b.timbreNotes == 2, "two degrees now (1/1 and 5/4, at period 3/2)");
                check (soundB != soundA,
                       "and the compiled partial set really changed - the box drives the sound,"
                       " it does not merely store a string");
                // 3/2 is now the PERIOD, so 702c is still there (it is the repeat) - what
                // must be gone is the OCTAVE the deleted token used to be: with the stack
                // repeating at 702c there is no partial at 1200c any more. Naming the
                // right cent value is the whole difference between a check that would
                // have caught this regression and one that fails on a spectrum that is
                // perfectly correct.
                check (! has (soundB, "1200c"),
                       "the deleted 2/1 is gone: the stack repeats at 3/2 now, so nothing"
                       " sits on the octave any more");
            }
            // AN EDIT THAT HAS NOT BEEN APPLIED YET, when the keyboard moves on.
            // "no reacciona a los intervalos que se escriben o borran en la caja de
            //  texto." Typing does not change the sound until Return - so until then the
            //  box holds a DRAFT, and a draft the program throws away on its own is
            //  indistinguishable, from the chair, from a box that ignores what you type.
            {
                juce::String err;
                p.loadTimbreText ("5/4 3/2 2", err);
                const auto d = ed->intervalDraftProbeForTests (" 7/4");
                std::printf ("  a draft in the box: seeded \"%s\" -> typed \"%s\""
                             " -> after the keyboard moved on \"%s\"\n",
                             d.seeded.toRawUTF8(), d.afterEdit.toRawUTF8(), d.afterTick.toRawUTF8());
                check (d.seeded == "5/4 3/2 2", "the box shows the spectrum that is loaded");
                check (d.afterEdit == "5/4 3/2 2 7/4", "and it takes the typed edit");
                check (d.survived,
                       "an edit still sitting in the box SURVIVES the keyboard moving on -"
                       " nothing throws away what the user typed but has not applied yet");
            }

            if (ed->isOnDesktop()) ed->removeFromDesktop();

            // ...AT EVERY WINDOW SIZE, not just the shipped one. The box is laid out from
            // what is LEFT of the engine panel after its three bottom rows are reserved,
            // so a panel that loses height does not shrink it evenly - it takes the whole
            // loss out of this one control. A box a few pixels tall is still "visible",
            // still "enabled" and still passes every rect check in this file, and it is
            // one you cannot click into or read: from the user's chair that is exactly
            // "it does not react to what I type", with Random - a button - still working.
            std::printf ("  the interval box across the window's size range:\n");
            auto boxAt = [ed] (int w, int h)
            {
                ed->setSize (w, h);
                juce::Rectangle<int> box;
                for (const auto& c : ed->layoutProbe().controls)
                    if (c.first == "cust.intervals") box = c.second;
                std::printf ("    %4dx%-4d -> %dx%d at (%d,%d)\n", w, h,
                             box.getWidth(), box.getHeight(), box.getX(), box.getY());
                return box.getHeight();
            };
            int worstH = 1 << 20; juce::String worstAt;
            // From the snapshot rig's own measured floor (870x540 - it photographs there,
            // and that is the picture the collapse was found in) up past the shipped size.
            for (const auto sz : { std::pair<int,int> { 870, 540 }, { 1200, 540 }, { 1110, 560 },
                                   { 1400, 600 }, { 1600, 900 }, { 1920, 1080 } })
                if (const int h = boxAt (sz.first, sz.second); h < worstH)
                { worstH = h; worstAt = juce::String (sz.first) + "x" + juce::String (sz.second); }
            const int shipped = boxAt (1200, 650);

            // ONE readable row of its own 13 px font inside its own 4/5 px borders. Below
            // this the box holds text nobody can see: at 870x540 it was 3 px and at
            // 1200x540 it was 13, on screen and enabled the whole time, which is
            // indistinguishable from a box that ignores what is typed into it.
            check (worstH >= 22,
                   "the interval box stays tall enough to show a line of what is typed in it,"
                   " at every size the editor is laid out at - it is never the row the panel"
                   " takes all its shrinkage out of");
            if (worstH < 22)
                std::printf ("    the worst is %d px tall, at %s\n", worstH, worstAt.toRawUTF8());
            // ...and the fix pays for itself out of SLACK, so the window everybody has
            // actually seen is untouched. A change here that moved the shipped layout by
            // one pixel would be a redesign smuggled in as a bug fix.
            check (shipped == 68,
                   "and the SHIPPED 1200x650 layout is pixel-identical: the rows only give"
                   " way where the panel cannot afford them");
        }
    }



    // --- run 53: A TALLER PANEL SPREADS ITS ROWS INSTEAD OF GROWING ONE HOLE --------
    //
    // Commit 5c970cf fixed two panels that turned extra height into a single void, and
    // nothing has asserted it since - the fix is real, both rigs are green either way,
    // and it can be undone by any future edit to resized() without a verdict moving.
    // This is that verdict, at three sizes and in both languages, because the row
    // heights come from the strings in the language on screen.
    //
    //   TUNING took every row off the top at a fixed height and reserved the Shepard
    //   block off the bottom, so all the slack pooled between them: 168 px of empty
    //   panel at 1600x900 against 43 at 1200x650, the Shepard row orphaned against the
    //   bottom edge and every other row huddled under the title. The spare height is
    //   divided over the gaps now, so the leftover gap is the size of its neighbours
    //   rather than the whole void.
    //
    //   ESSENTIALS capped OCTAVE at 40 px and split the rest between GAIN and
    //   REFERENCE: 37/37/38 at the floor (invisible) and 126/126/40 at 1600x900, two
    //   cells swelling with the window and a third that cannot, with the last divider
    //   stranded near the bottom edge.
    //
    // Both are measured through the SAME probe the window is drawn from - the cells
    // paint() strokes its dividers between, and the control rects resized() assigns -
    // so this cannot pass against a layout only the test can reach.
    {
        std::printf ("\na taller panel spreads its rows instead of growing one hole:\n");
        auto owned = std::make_unique<FractalAdditiveAudioProcessor>();
        auto& p = *owned;
        p.prepareToPlay (44100.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> base (p.createEditor());
        auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
        check (ed != nullptr, "the editor builds");
        if (ed != nullptr)
        {
            // The tuning panel's rows, top to bottom, by the control that spans each
            // row's full height. Collected by NAME and then sorted by y, so an optional
            // row appearing (or a future row being added) cannot silently reorder this.
            const char* rowAnchors[] = { "tun.period", "tun.itsSpectrum", "tun.presetTuning",
                                         "tun.specTuning", "tun.recent", "tun.bankLbl",
                                         "tun.shepOn" };
            for (const auto sz : { std::pair<int,int> { 870, 540 }, { 1200, 650 }, { 1600, 900 } })
                for (int lang = 0; lang < 2; ++lang)
                {
                    ed->setSize (sz.first, sz.second);
                    const auto L = ed->layoutProbeInLanguage (lang == 1);
                    const juce::String where = juce::String (sz.first) + "x" + juce::String (sz.second)
                                             + " " + (lang == 1 ? "ca" : "en");

                    //-- ESSENTIALS: three cells, and they tile the box evenly ---------
                    auto cellH = [&L] (const char* n)
                    {
                        for (const auto& c : L.cells) if (c.first == n) return c.second.getHeight();
                        return -1;
                    };
                    const int hg = cellH ("ess.gain"), hr = cellH ("ess.ref"), ho = cellH ("ess.oct");
                    std::printf ("  %-12s ESSENTIALS gain/ref/oct = %d/%d/%d\n",
                                 where.toRawUTF8(), hg, hr, ho);
                    check (hg > 0 && hr > 0 && ho > 0, "  the three ESSENTIALS cells are laid out");
                    // <= 2: the third cell IS the remainder (so the three always tile the
                    // box exactly), and a box height that is not a multiple of three
                    // leaves it one or two pixels taller. The defect this replaces was
                    // 126/126/40 - a gap of 86.
                    check (juce::jmax (hg, hr, ho) - juce::jmin (hg, hr, ho) <= 2,
                           "  ...as even thirds - the extra height is air around each control,"
                           " never two cells that swell and a third capped at 40 px");

                    //-- the chip strip and the mode box start on the same line ---------
                    // They are in different panels, side by side, and their first rows
                    // begin at the same y - so the two words in them read as one line
                    // across the window. A 19 px chip in a band of its own height next to
                    // a 24 px combo put those words 2.5 px apart, which is worse than an
                    // obvious misalignment: it reads as a mistake nobody can name. The
                    // chip keeps its 19 px (the engine strip shares that number) and
                    // stands centred in a control row instead.
                    {
                        auto rectOf = [&L] (const char* n)
                        {
                            for (const auto& c : L.controls) if (c.first == n) return c.second;
                            return juce::Rectangle<int>();
                        };
                        const auto chip = rectOf ("mod.tabAmp");
                        const auto mode = rectOf ("tun.mode");
                        if (! chip.isEmpty() && ! mode.isEmpty() && chip.getY() < mode.getBottom()
                            && mode.getY() < chip.getBottom())
                        {
                            const int dc = std::abs ((chip.getY() + chip.getHeight() / 2)
                                                     - (mode.getY() + mode.getHeight() / 2));
                            std::printf ("  %-12s chip strip centre vs mode box centre: %d px\n",
                                         where.toRawUTF8(), dc);
                            check (dc <= 1,
                                   "  the chip strip and the mode box beside it share a centre line"
                                   " - a 19 px chip stands centred in a control row, not in a band"
                                   " of its own height");
                        }
                    }

                    //-- TUNING: the slack is shared out, not pooled -------------------
                    std::vector<juce::Rectangle<int>> rows;
                    for (const char* n : rowAnchors)
                        for (const auto& c : L.controls)
                            if (c.first == n) rows.push_back (c.second);
                    std::sort (rows.begin(), rows.end(),
                               [] (const auto& a, const auto& b) { return a.getY() < b.getY(); });
                    std::vector<int> gaps;
                    for (size_t i = 1; i < rows.size(); ++i)
                        gaps.push_back (rows[i].getY() - rows[i - 1].getBottom());
                    juce::String printed;
                    for (const int g : gaps) printed << g << " ";
                    std::printf ("  %-12s TUNING %d rows, gaps: %s\n",
                                 where.toRawUTF8(), (int) rows.size(), printed.toRawUTF8());
                    // >= 2 gaps: at the 870x540 floor the bank row hides itself rather
                    // than laying out at an unreadable height (the `fits` guard in
                    // resized()), so the floor legitimately walks one row fewer.
                    check (gaps.size() >= 2, "  the tuning panel really has rows to space");
                    if (! gaps.empty())
                    {
                        const int lo = *std::min_element (gaps.begin(), gaps.end());
                        const int hi = *std::max_element (gaps.begin(), gaps.end());
                        // The share is an integer division over 4 + (optional rows) gaps,
                        // and the LAST gap also carries the division's remainder, so the
                        // gaps can differ by up to that divisor. They cannot differ by
                        // 165, which is what "one row got all the air" looks like.
                        check (hi - lo <= 7,
                               "  every gap between the tuning panel's rows is the same size -"
                               " the spare height is shared out, never pooled into one void");
                        // ...and at a TALL window it really is spare height being shared:
                        // gaps of 3 px everywhere would satisfy the line above while the
                        // void sat somewhere this walk cannot see.
                        if (sz.second >= 900)
                            check (lo >= 10,
                                   "  ...and at 1600x900 the gaps have actually GROWN, so the"
                                   " panel's extra height went into them");
                    }
                }
        }
    }

    // --- run 54: ONE PENCIL DRAG IS ONE Ctrl+Z, HOWEVER LONG THE DRAG TOOK ---------
    //
    // The processor's 60 Hz timer cuts parameter edits into ~1-second undo transactions
    // so that a slider drag undoes as one step instead of hundreds of micro-moves. A
    // PENCIL drag is one user action too, and it easily lasts longer than a second - so
    // that cut landed INSIDE it.
    //
    // On the first pencil drag of a session that is not a cosmetic split. The gesture
    // arms manual mode: armManualPartialAmpsForEdit() opens the transaction and the
    // parameter flip reaches the undo stack on the APVTS's own 20-500 ms timer, early in
    // the drag, while the amplitudes are committed at mouse-up. With a cut between them
    // the flip and the edits are two steps, so ONE Ctrl+Z takes back the amplitudes and
    // leaves MANUAL MODE ARMED WITH NO EDITS - the state commit f40623d exists to
    // forbid, and the state in which SLOPE is frozen for a spectrum nobody drew.
    //
    // Both drags below are identical except for how long they take.
    {
        std::printf ("\none pencil drag is one Ctrl+Z, however long the drag takes:\n");
        for (const int ticks : { 0, 60, 200 })
        {
            auto owned = std::make_unique<FractalAdditiveAudioProcessor>();
            auto& p = *owned;
            p.prepareToPlay (44100.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> base (p.createEditor());
            auto* ed = dynamic_cast<FractalAdditiveAudioProcessorEditor*> (base.get());
            if (ed == nullptr) { check (false, "the editor builds"); continue; }

            // A live template, so the pencil grid has knobs to drag at all.
            {
                juce::AudioBuffer<float> b (2, 512);
                juce::MidiBuffer on; on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                b.clear(); p.processBlock (b, on);
                for (int i = 0; i < 40; ++i)
                { juce::MidiBuffer none; b.clear(); p.processBlock (b, none); p.pollParametersForTests(); }
                juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                b.clear(); p.processBlock (b, off);
                for (int i = 0; i < 10; ++i) { juce::MidiBuffer none; b.clear(); p.processBlock (b, none); }
            }

            const auto r = ed->pencilGestureUndoProbeForTests (1, -9.0f, ticks);
            std::printf ("  a drag spanning %3d timer ticks (~%.1f s): armed=%d, %d knobs,"
                         " undo steps %d -> %d (flip flushed) -> %d (drag ticks) -> %d;"
                         " one Ctrl+Z -> manual=%d, %d edit(s) left\n",
                         ticks, (double) ticks / 60.0, (int) r.armed, r.cells,
                         r.stepsBefore, r.stepsAfterFlush, r.stepsAfterTicks, r.stepsAfter,
                         (int) r.manualAfterUndo, r.editsAfterUndo);

            check (r.cells > 1, "  the pencil grid really had knobs to drag");
            check (r.armed, "  the first drag of the session armed manual mode itself");
            check (r.stepsAfter - r.stepsBefore == 1,
                   "  the whole gesture is ONE undo step - the ~1 s transaction cut does not"
                   " fall inside a drag");
            check (r.undid, "  and Ctrl+Z has something to take back");
            // The two halves of the same claim, because either one alone can pass while
            // the user is still left somewhere they never asked to be.
            check (r.editsAfterUndo == 0,
                   "  one Ctrl+Z takes the hand-drawn amplitudes back...");
            check (! r.manualAfterUndo,
                   "  ...AND switches manual mode back off - never armed with no edits,"
                   " with SLOPE frozen for a spectrum nobody drew");
        }
    }

    runPhaseFaderChecks();


    // --- run 54: THE SHEPARD'S THREE STATED INVARIANTS, MEASURED --------------------
    //
    // Run 46 proves the Shepard on the DEFAULT patch at a 256-sample block. Every one of
    // the three defects below lives outside that one corner, and each of them breaks a
    // rule this codebase writes down in prose:
    //
    //   S1  "the window reaches EXACTLY zero at both edges, so the copy leaving the top
    //        leaves silent". A dense enough spectrum lifts the band's bottom edge until
    //        the band is narrower than two rise intervals and the stack is ONE copy - and
    //        the per-voice normaliser then divides that one copy's window straight back
    //        out, so the wrap stops being silent and the whole illusion's guarantee is
    //        void. Measured as the LEVEL the voice holds while its only copy's window
    //        sweeps 0 -> 1 -> 0.
    //
    //   S2  "defensive; never fires", on the kSlots clamp in planShepard. At an
    //        offline-render block size the grid union really is wider than kSlots, and
    //        the clamp deletes a copy in the middle of the window.
    //
    //   S3  "a slot is claimed at a window edge, where the window is ~0 ... and that is
    //        what makes re-seeding it inaudible", and "alignPhase[] is only ever WRITTEN
    //        while the crossfade weight is 0". A template that turns dense mid-note moves
    //        the band's bottom edge in ONE step, which re-claims slots wherever the window
    //        happens to be and re-seeds both banks there.
    //
    // The metric throughout is PER-SAMPLE, and deliberately so: an RMS over a 5 ms window
    // is structurally blind to a one-sample discontinuity, and that blindness is exactly
    // how these three shipped past a suite that was already measuring loudness.
    {
        std::printf ("\nrun 53: the Shepard's three stated invariants, measured:\n");
        constexpr double ssr = 48000.0;

        // The analytic ceiling for a SMOOTH render of exactly the partials sounding in
        // this block: |x[k]-x[k-1]| <= sum_j a_j * 2 sin(pi f_j / sr), calibrated from
        // the block's own RMS because the overall gain is unknown here. Same rig run 46
        // uses for the default patch; it is repeated on the states run 46 never enters.
        struct StepMetric
        {
            float prev = 0.0f; bool have = false;
            double worst = 0.0; float maxJump = 0.0f; double bound = 0.0;
            void feed (const juce::AudioBuffer<float>& b, int nS, double sumA2, double sumB)
            {
                const float* L = b.getReadPointer (0);
                double energy = 0.0; float jump = 0.0f;
                for (int i = 0; i < nS; ++i)
                {
                    const float x = L[i];
                    energy += (double) x * x;
                    if (have) jump = std::max (jump, std::abs (x - prev));
                    prev = x; have = true;
                }
                const double rms = std::sqrt (energy / (double) nS);
                const double G   = sumA2 > 1.0e-18 ? rms / std::sqrt (0.5 * sumA2) : 0.0;
                const double bnd = G * sumB * 1.35 + 1.0e-4;
                maxJump = std::max (maxJump, jump);
                bound   = std::max (bound, bnd);
                if (bnd > 0.0) worst = std::max (worst, (double) jump / bnd);
            }
        };

        struct TestPlayHead : juce::AudioPlayHead
        {
            double bpm = 120.0;
            juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
            { juce::AudioPlayHead::PositionInfo pi; pi.setBpm (bpm); pi.setIsPlaying (false); return pi; }
        };

        long long pm [shepard::kSlots];
        double    pr [shepard::kSlots], pw[shepard::kSlots];
        int       pcn[shepard::kSlots];

        // ONE harness, re-made per scenario because the BLOCK SIZE is one of the
        // variables under test and prepareToPlay is what fixes it.
        auto makeRig = [&] (int blk, TestPlayHead& ph)
        {
            auto q = std::make_unique<FractalAdditiveAudioProcessor>();
            q->prepareToPlay (ssr, blk);
            q->setPlayHead (&ph);
            return q;
        };

        //== S1: a dense spectrum narrows the band - what happens at the bottom of it ==
        {
            TestPlayHead ph;
            constexpr int blk = 256;
            auto p = makeRig (blk, ph);
            juce::AudioBuffer<float> b (2, blk);
            auto set = [&] (const char* id, float plain)
            {
                auto* q = p->apvts.getParameter (id);
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
                p->pollParametersForTests();
            };
            juce::MidiBuffer pending;
            auto pump = [&] (double seconds)
            {
                const int nB = juce::jmax (1, (int) std::lround (seconds * ssr / blk));
                for (int i = 0; i < nB; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m.swapWith (pending);
                    b.clear(); p->processBlock (b, m);
                }
                return nB;
            };

            // The densest LOW-REACHING spectrum the panel can ask for: everything the
            // fractal engine makes, hard-cut to the bottom few octaves, so hundreds of
            // partials sit under ratio 8 and the density floor really binds. This is a
            // reachable patch, not a contrivance - spectrumRange is a knob on the panel.
            set ("shepOn", 0.0f);
            set ("phaseMode", 1.0f);
            set ("ampSustain", 1.0f);
            set ("ampRelease", 0.30f);
            set ("shepMode", 0.0f);      // Free (ms)
            set ("shepTime", 700.0f);    // several wraps inside the measured window
            // THE TWO KNOBS THAT NARROW THE BAND, both first-class controls on the panel:
            //   Base Harmonic N at its top packs 16321 partials from ratio 1 upward, which
            //     lifts the density floor to log2(nyquist / ratio[kDensityGuard]) and takes
            //     the audible band from 9.97 octaves down to 3.52;
            //   a tuning whose PERIOD is wide, with the rise dropdown set to "Period pitch
            //     bend", makes each rise step 4 octaves instead of 1 (riseRatio clamps at
            //     16, so this is a case the engine explicitly anticipates).
            // 3.52 octaves is LESS than one 4-octave step, so bandFor takes its degenerate
            // branch: the band is exactly one rise interval wide and the stack is ONE copy.
            // Nothing here is a contrivance - it is two panel controls at settings the code
            // names in its own constants.
            juce::String err;
            check (p->loadScalaText ("! wide\nwide-period test\n 1\n 16/1\n", "wide-period", err),
                   "a 16/1-period scale loads (riseRatio's own ceiling, so the engine expects it)");
            set ("shepRise", 1.0f);      // "Period pitch bend"
            {
                const auto gen0 = p->getTemplateGeneration();
                set ("baseN", 64.0f);
                awaitTemplateChange (*p, gen0, "run 53: the dense narrow spectrum");
                awaitSpectrumIdle (*p, "run 53: the dense narrow spectrum");
            }
            if (const auto* T = p->getLiveTemplate())
            {
                const int np = (int) T->partials.size();
                const double rG = np > shepard::kDensityGuard
                                ? (double) T->partials[(size_t) shepard::kDensityGuard].ratio : 0.0;
                const double loD = rG > 1.0e-9 ? std::log2 (0.5 * ssr / rG) : -1.0e9;
                const auto bd = shepard::bandFor (ssr, std::log2 (16.0), loD);
                std::printf ("  patch: %d partials, ratio[%d] = %.3f -> density floor %.3f log2 Hz;"
                             " band %.3f oct over a %.3f oct rise step = %.2f copies\n",
                             np, shepard::kDensityGuard, rG, loD, bd.span, std::log2 (16.0),
                             bd.span / std::log2 (16.0));
            }
            pump (0.3);

            set ("shepOn", 1.0f);
            pending.addEvent (juce::MidiMessage::noteOn (1, 72, 0.9f), 0);
            pump (0.4);
            p->shepardEvidenceResetForTests();

            StepMetric sm;
            int    minCopies = 99, maxCopies = 0;
            double loudestAtLowWindow = 0.0, worstWindowSeen = 0.0;
            double peakLo = 1.0e9, peakHi = 0.0;
            const int blocks = (int) (3.0 * ssr / blk);
            for (int k = 0; k < blocks; ++k)
            {
                juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                const int n = p->voiceShepardForTests (pm, pr, pw, pcn, shepard::kSlots);
                if (n == 0) continue;
                minCopies = std::min (minCopies, n); maxCopies = std::max (maxCopies, n);
                const auto* T = p->getLiveTemplate();
                double sumA2 = 0.0, sumB = 0.0, sumW = 0.0;
                if (T != nullptr)
                    for (int c = 0; c < n; ++c)
                    {
                        sumW += pw[c];
                        const int nc = std::min (pcn[c], (int) T->partials.size());
                        for (int i = 0; i < nc; ++i)
                        {
                            const double a = (double) T->partials[(size_t) i].amp * pw[c];
                            const double f = std::min ((double) T->partials[(size_t) i].ratio * pr[c], 0.4999 * ssr);
                            sumA2 += a * a;
                            sumB  += a * 2.0 * std::sin (juce::MathConstants<double>::pi * f / ssr);
                        }
                    }
                sm.feed (b, blk, sumA2, sumB);
                // THE S1 MEASUREMENT, and it is a RATIO, not a level. The window is the
                // whole illusion: a copy leaving the top must LEAVE SILENT, so the voice's
                // amplitude has to fall with the window sum. If the normaliser divides the
                // window back out, the level is the SAME at a window sum of 0.05 as at 1.0
                // - and then the wrap is not silent and the guarantee the design rests on
                // is void. Two buckets, near the bottom of the window and near its top.
                const double mag = (double) b.getMagnitude (0, blk);
                if (sumW > 1.0e-9 && sumW < 0.10)
                { loudestAtLowWindow = std::max (loudestAtLowWindow, mag); worstWindowSeen = std::max (worstWindowSeen, sumW); }
                if (sumW > 0.90) peakHi = std::max (peakHi, mag);
                if (mag > 1.0e-6) peakLo = std::min (peakLo, mag);
            }
            double cw, cx, dw, ls; int pl, dr, mc, bl, cb, cwo;
            p->shepardEvidenceForTests (cw, cx, dw, ls, pl, dr, mc, bl, cb, cwo);
            std::printf ("  dense narrow band: %d..%d copies over %d blocks\n",
                         minCopies, maxCopies, blocks);
            std::printf ("  loudest block with the stack's window sum BELOW 0.10: %.4f (worst such sum %.3f);"
                         " loudest with it ABOVE 0.90: %.4f -> the window survives to the output by a factor"
                         " of %.2f (it should be about %.2f)\n",
                         loudestAtLowWindow, worstWindowSeen, peakHi,
                         loudestAtLowWindow > 1.0e-9 ? peakHi / loudestAtLowWindow : 0.0,
                         worstWindowSeen > 1.0e-9 ? 1.0 / worstWindowSeen : 0.0);
            std::printf ("  click metric (dense narrow band): max per-sample jump %.5f, bound %.5f"
                         " (worst jump/bound %.2f)\n", sm.maxJump, sm.bound, sm.worst);
            check (minCopies <= 1,
                   "a dense spectrum under a wide rise interval really does narrow the stack to"
                   " ONE copy (the test is standing in the state it claims)");
            // THE SILENT WRAP, MEASURED IN THE AUDIO INSTEAD OF IN THE WINDOW. Run 46 checks
            // that the WINDOW is ~0 at a wrap, which stays true however the voice is
            // normalised - it was true while this was broken. What has to hold is that the
            // OUTPUT follows it down: with one copy, a normaliser that divided by the
            // window's own sum cancelled it exactly, and the voice was as loud at a window
            // sum of 0.098 as at 0.95 (ratio 0.90 where the window says 10.2). Anything
            // above 5 here is a voice whose level really does fall with its window.
            check (loudestAtLowWindow > 1.0e-9 && peakHi / loudestAtLowWindow > 5.0,
                   "and at ONE copy the voice's level still falls with the window - the copy"
                   " leaving the top really does leave silent (it did not: ratio 0.90)");
            std::printf ("  evidence: worst claim window %.4f at crossfade weight %.3f;"
                         " widest planned union %d; copies dropped %d (worst %.4f); worst lo step %.4f oct\n",
                         cw, cx, pl, dr, dw, ls);
            check (dr == 0 && pl <= shepard::kSlots,
                   "...and a one-copy stack still never asks for more slots than there are");
            pending.addEvent (juce::MidiMessage::noteOff (1, 72), 0);
            pump (2.0);
        }

        //== S2: an OFFLINE-RENDER block size, and the clamp that "never fires" =======
        {
            TestPlayHead ph;
            constexpr int blk = 8192;             // a bounce buffer, not a live one
            auto p = makeRig (blk, ph);
            juce::AudioBuffer<float> b (2, blk);
            auto set = [&] (const char* id, float plain)
            {
                auto* q = p->apvts.getParameter (id);
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
                p->pollParametersForTests();
            };
            juce::MidiBuffer pending;
            auto pump = [&] (int nB)
            {
                for (int i = 0; i < nB; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m.swapWith (pending);
                    b.clear(); p->processBlock (b, m);
                }
            };
            set ("shepOn", 1.0f);
            set ("shepMode", 1.0f);      // Sync Project Tempo
            set ("shepDiv", 0.0f);       // 1/16 at 120 BPM -> the fastest rise there is
            set ("phaseMode", 1.0f);
            set ("ampSustain", 1.0f);
            awaitSpectrumIdle (*p, "run 53: the offline-block patch");
            pump (4);
            pending.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            pump (4);
            p->shepardEvidenceResetForTests();
            StepMetric sm;
            for (int k = 0; k < 24; ++k)
            {
                juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                const int n = p->voiceShepardForTests (pm, pr, pw, pcn, shepard::kSlots);
                const auto* T = p->getLiveTemplate();
                double sumA2 = 0.0, sumB = 0.0;
                if (T != nullptr)
                    for (int c = 0; c < n; ++c)
                    {
                        const int nc = std::min (pcn[c], (int) T->partials.size());
                        for (int i = 0; i < nc; ++i)
                        {
                            const double a = (double) T->partials[(size_t) i].amp * pw[c];
                            const double f = std::min ((double) T->partials[(size_t) i].ratio * pr[c], 0.4999 * ssr);
                            sumA2 += a * a;
                            sumB  += a * 2.0 * std::sin (juce::MathConstants<double>::pi * f / ssr);
                        }
                    }
                sm.feed (b, blk, sumA2, sumB);
            }
            double cw, cx, dw, ls; int pl, dr, mc, bl, cb, cwo;
            p->shepardEvidenceForTests (cw, cx, dw, ls, pl, dr, mc, bl, cb, cwo);
            std::printf ("  %d-sample blocks, fastest sync rise: widest planned union %d slots (kSlots = %d),"
                         " %d blocks deleted a copy, loudest deleted copy's window %.4f\n",
                         blk, pl, shepard::kSlots, dr, dw);
            // "DEFENSIVE; NEVER FIRES" IS NOW A MEASUREMENT. It used to be a comment, and at
            // this exact block size it was false: the plan asked for 12 slots and the clamp
            // deleted a copy whose window was still 0.2689 - a partial dropped in silence
            // and a step in one event. renderNextBlock splits the block on what the slots
            // can honestly cover (shepard::maxPlanSamples); this is the assertion that says
            // the sentence is true, and it fails the moment the split is removed.
            check (bl > 0, "the offline-block scenario really rendered Shepard blocks");
            check (pl <= shepard::kSlots,
                   "at an OFFLINE-RENDER block size the plan still never asks for more grid"
                   " indices than there are slots (it asked for 12 of 10)");
            check (dr == 0 && dw < 1.0e-9,
                   "...so the kSlots clamp never fires, and no copy is deleted in mid-window");
            std::printf ("  click metric (offline block): max per-sample jump %.5f, bound %.5f"
                         " (worst jump/bound %.2f)\n", sm.maxJump, sm.bound, sm.worst);
            check (sm.worst <= 1.0,
                   "...and no per-sample step at that block size exceeds what these partials"
                   " can smoothly produce");
            pending.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            pump (8);
        }

        //== S3: the template turns DENSE under a held note ===========================
        {
            TestPlayHead ph;
            constexpr int blk = 256;
            auto p = makeRig (blk, ph);
            juce::AudioBuffer<float> b (2, blk);
            auto set = [&] (const char* id, float plain)
            {
                auto* q = p->apvts.getParameter (id);
                q->setValueNotifyingHost (q->convertTo0to1 (plain));
                p->pollParametersForTests();
            };
            juce::MidiBuffer pending;
            auto pump = [&] (double seconds)
            {
                const int nB = juce::jmax (1, (int) std::lround (seconds * ssr / blk));
                for (int i = 0; i < nB; ++i)
                {
                    juce::MidiBuffer m; if (i == 0) m.swapWith (pending);
                    b.clear(); p->processBlock (b, m);
                }
                return nB;
            };
            set ("shepOn", 1.0f);
            set ("shepMode", 0.0f);
            set ("shepTime", 2000.0f);
            set ("phaseMode", 1.0f);
            set ("ampSustain", 1.0f);
            set ("ampRelease", 0.30f);
            set ("baseN", 1.0f);
            // MORPH ON, and slow. The second invariant this scenario is about is
            // "alignPhase[] is only ever WRITTEN while the crossfade weight is 0", and a
            // re-seed writes BOTH banks - so the density change has to land while a
            // crossfade is genuinely mid-flight, or the check measures nothing and passes.
            set ("morph", 1.0f);
            set ("morphTime", 1.5f);
            awaitSpectrumIdle (*p, "run 53: the sparse spectrum before the density change");
            pump (0.3);
            pending.addEvent (juce::MidiMessage::noteOn (1, 72, 0.9f), 0);
            pump (0.6);
            p->shepardEvidenceResetForTests();

            StepMetric sm;
            bool   sawCapFlag = false;
            double worstMorphW = 0.0;
            // The change itself: Base Harmonic N pushed to the top under the held key, which
            // takes the template from 1024 partials to 16321 and its density floor from
            // 4.87 to 10.00 log2 Hz. This is a patch tweak while a note is down - the most
            // ordinary thing anyone does with a synth - and it is what moves the band.
            set ("baseN", 64.0f);
            for (int k = 0; k < (int) (2.5 * ssr / blk); ++k)
            {
                juce::MidiBuffer m; b.clear(); p->processBlock (b, m);
                const int n = p->voiceShepardForTests (pm, pr, pw, pcn, shepard::kSlots);
                const auto* T = p->getLiveTemplate();
                double sumA2 = 0.0, sumB = 0.0;
                if (T != nullptr)
                    for (int c = 0; c < n; ++c)
                    {
                        const int nc = std::min (pcn[c], (int) T->partials.size());
                        for (int i = 0; i < nc; ++i)
                        {
                            const double a = (double) T->partials[(size_t) i].amp * pw[c];
                            const double f = std::min ((double) T->partials[(size_t) i].ratio * pr[c], 0.4999 * ssr);
                            sumA2 += a * a;
                            sumB  += a * 2.0 * std::sin (juce::MathConstants<double>::pi * f / ssr);
                        }
                    }
                sm.feed (b, blk, sumA2, sumB);
                sawCapFlag  = sawCapFlag || p->isShepardSlotCapped();
                worstMorphW = std::max (worstMorphW, p->voiceMorphWeightForTests());
            }
            double cw, cx, dw, ls; int pl, dr, mc, bl, cb, cwo;
            p->shepardEvidenceForTests (cw, cx, dw, ls, pl, dr, mc, bl, cb, cwo);
            std::printf ("  template turns dense under a held key: worst ONE-BLOCK move of the band's"
                         " bottom edge %.4f oct (it jumped 4.4234 before this was a ramp)\n", ls);
            std::printf ("  worst slot claim: window %.4f, worst crossfade weight at any claim %.3f;"
                         " the crossfade really did run (peak weight %.3f)\n", cw, cx, worstMorphW);
            check (worstMorphW > 0.05,
                   "the density change really landed mid-crossfade (the check below is measuring"
                   " something)");
            // THE TWO INVARIANTS THIS FILE WRITES DOWN, and ONE number settles both.
            //
            // A slot claim re-seeds phase[] AND alignPhase[]. The file says a slot is
            // claimed "at a window edge, where the window is ~0 ... and that is what makes
            // re-seeding it inaudible", and separately that alignPhase[] is only ever
            // written while the crossfade weight is 0 - because at weight w a write
            // contributes w x delta, which at 0.5 is 34% of full scale.
            //
            // Claims DO still happen while a crossfade is in flight (cx above is not 0, and
            // it should not be - a density change is exactly what starts one). What makes
            // that harmless is the FIRST invariant holding: the write goes into a copy whose
            // window factor is exactly zero, so w x delta reaches the output multiplied by
            // 0. The weight's own rule is about writes that are AUDIBLE, and at a window
            // zero none is. Before the bottom edge became a ramp this measured 0.9721 -
            // a re-seed of both banks at 97% of full window, mid-crossfade.
            check (cw < 1.0e-3,
                   "every slot is claimed where the window really is ZERO - so re-seeding it"
                   " writes BOTH banks into silence, whatever the crossfade is doing");
            std::printf ("  base list hit the kStride cap in %d of %d blocks (worst: %d partials"
                         " short of the template); meter showed the cap: %s\n",
                         cb, bl, cwo, sawCapFlag ? "yes" : "NO");
            // THE ONE CAP LEFT, AND IT IS NOT SILENT. While the bottom edge travels up to a
            // density change the deepest copies are still audible and cannot fit their
            // kStride slot. That is a limit which changes what is heard, so it goes on the
            // meter for as long as a human could see it - and it must be BRIEF, because a
            // cap that outlives the edge's travel would be a permanent cut wearing a
            // transient's clothes.
            check (cb == 0 || sawCapFlag,
                   "...and while the deepest copy does not fit its slot, the meter SAYS SO"
                   " (the cap is never silent)");
            check (cb <= 12,
                   "...and it lasts only the handful of blocks the edge takes to travel, not"
                   " the length of the note");
            std::printf ("  click metric (density change): max per-sample jump %.5f, bound %.5f"
                         " (worst jump/bound %.2f)\n", sm.maxJump, sm.bound, sm.worst);
            check (sm.worst <= 1.0,
                   "no per-sample step exceeds what these partials can smoothly produce, across"
                   " the density change");
            pending.addEvent (juce::MidiMessage::noteOff (1, 72), 0);
            pump (2.0);

            // ...AND IT GOES AWAY AGAIN, WHICH ONLY A SHORT NOTE CAN PROVE. The flag is
            // HELD for half a second so a human can see a 16 ms event, and a voice that
            // has stopped rendering stops counting that half second down. Letting the
            // note above run its course does not test anything: its 0.30 s release keeps
            // rendering long enough for the hold to expire on its own, so the check passed
            // with the clean-up deliberately deleted. This one takes the key UP 50 ms
            // after the density change, while the hold is still live, so the flag really
            // is up at the moment the note ends - and without the drop in finishNote()
            // "(safety cap)" would then sit on the meter for the rest of the session. A
            // permanent warning about a transient is the same lie as no warning at all.
            {
                const auto gen0 = p->getTemplateGeneration();
                set ("baseN", 1.0f);
                awaitTemplateChange (*p, gen0, "run 53: back to the sparse spectrum");
                awaitSpectrumIdle (*p, "run 53: back to the sparse spectrum");
                set ("ampRelease", 0.05f);
                pump (0.5);
                pending.addEvent (juce::MidiMessage::noteOn (1, 72, 0.9f), 0);
                pump (0.4);
                set ("baseN", 64.0f);
                pump (0.05);                       // the cap fires inside this
                const bool upWhileHeld = p->isShepardSlotCapped();
                pending.addEvent (juce::MidiMessage::noteOff (1, 72), 0);
                pump (0.25);                       // 0.05 s release: the note is over well
                                                   // inside the flag's own half-second hold
                check (upWhileHeld,
                       "a note released 50 ms after the density change still had the cap flag"
                       " UP when it ended (the check below is measuring something)");
                check (! p->isShepardSlotCapped(),
                       "...and the meter drops it with the note - the cap reads as the"
                       " transient it is, instead of staying lit for the session");
                set ("ampRelease", 0.30f);
                pump (1.0);
            }
        }
    }


    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
