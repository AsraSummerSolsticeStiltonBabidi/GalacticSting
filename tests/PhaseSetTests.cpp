// Standalone unit tests for source/dsp/PhaseSets.h (no JUCE, no plugin).
//
//   PhaseSetTests.exe            -> exit code 0 = all passed, non-zero = failures.
//   PhaseSetTests.exe --tables   -> also print the measurement tables below.
//   PhaseSetTests.exe -v         -> print every check.
//
// Build (two translation units; nothing beyond source/ on the include path):
//   cl /O2 /std:c++17 /EHsc /I source tests\PhaseSetTests.cpp source\dsp\Spectrum.cpp
//
//==============================================================================
// THE ONE RULE THIS FILE EXISTS TO ENFORCE.
//
// Version 1 of the phase engine shipped a suite that was GREEN and a feature that
// was BROKEN, and the two facts had the same cause: its measuring function called
// the same bin-snapping analysis grid the search minimised on. Every "beats all
// three closed forms" and "never worse than the hash" assertion was scored on the
// surrogate the search had gamed. On a 22-EDO timbre it reported a 2.32 dB win
// over the shipping hash and RENDERED 1.50 dB worse than it.
//
// So: THE MEASURING RIG IN THIS FILE SHARES NOTHING WITH THE ENGINE. `ref::crest`
// below is a direct std::sin over the TRUE, UNQUANTISED ratios at explicit times.
// It has no table, no fixed-point accumulator, no bins, no grid plan, and it does
// not call into namespace phaseset::detail for anything. If the engine and this
// rig ever disagree, the rig is right.
//
// Every assertion about what the listener hears is scored with ref::crest.
//
//==============================================================================
// THE SPECTRA. They come from the REAL compileSpectrum in source/dsp/Spectrum.cpp,
// not from hand-rolled approximations, so the partial lists, the amplitude ties
// and the truncation flags are the ones the plugin actually produces. v1's
// determinism test used harmonicSet(32, 1.0), where every amplitude is distinct,
// and therefore never exercised a single tie - which is how replacing three
// std::stable_sort with std::sort left that suite green while changing the output.
// `fractal L3 at maxPartials 384` is in this file's reference set precisely
// because compileSpectrum gives it 193 exact float amplitude ties (73 of them in
// the loudest 128), and testDeterminism now includes a PERMUTATION test, which is
// the property those sorts exist to provide and is stronger than watching one sort
// call get swapped.
//
//==============================================================================
// WHAT THE MEASUREMENTS SAID. Every number below was produced by this file;
// --tables reprints them. Machine: single-threaded /O2 x64, MSVC 14.51.
// See the tables printed by --tables for the live values.
//==============================================================================

#include "dsp/PhaseSets.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace spectrum;
using namespace spectrum::phaseset;

//==============================================================================
static int  failures = 0;
static int  checks   = 0;
static bool verbose  = false;
static bool tables   = false;

static void check (bool ok, const std::string& what, const std::string& detail = "")
{
    ++checks;
    if (! ok) { ++failures; printf ("FAIL: %s %s\n", what.c_str(), detail.c_str()); }
    else if (verbose) printf ("ok:   %s %s\n", what.c_str(), detail.c_str());
}

static std::string fmt (const char* f, double a, double b = 0, double c = 0, double d = 0)
{
    char buf[512];
    std::snprintf (buf, sizeof buf, f, a, b, c, d);
    return std::string (buf);
}

//==============================================================================
// ref: THE INDEPENDENT MEASUREMENT RIG.
//
// x(t) = sum_i amp_i * sin(2*pi*(phase_i + ratio_i * t)), t in fundamental
// periods, sampled at `periods * oversample * topRatio` evenly spaced instants.
// Direct std::sin on the true ratio and the true time. No shared code with the
// engine, by design and on purpose.
namespace ref
{
    struct Stats { double peak = 0.0, rms = 0.0, crest = 0.0; };

    static long long sampleCount (const Partial* p, int n, double periods, int oversample)
    {
        double topR = 1.0;
        for (int i = 0; i < n; ++i) topR = std::max (topR, (double) p[i].ratio);
        long long T = (long long) std::ceil (periods * topR * (double) oversample);
        if (T < 64) T = 64;
        return T;
    }

    // Several phase sets at once, so the sines are computed once and shared. Still
    // one std::sin per (partial, sample) per set-independent term - the sharing is
    // an angle-addition identity, not an approximation:
    //   sin(2pi(ph + r t)) = sin(2pi ph) cos(2pi r t) + cos(2pi ph) sin(2pi r t)
    static void crestMulti (const Partial* p, int n,
                            const std::vector<const double*>& phaseSets,
                            double periods, int oversample,
                            std::vector<Stats>& out)
    {
        const long long T = sampleCount (p, n, periods, oversample);
        const size_t K = phaseSets.size();
        out.assign (K, Stats());
        std::vector<double> acc (K, 0.0), pk (K, 0.0), ss (K, 0.0);
        std::vector<double> sph (K * (size_t) n), cph (K * (size_t) n);
        for (size_t k = 0; k < K; ++k)
            for (int i = 0; i < n; ++i)
            {
                const double a = 6.283185307179586476925286766559 * phaseSets[k][i];
                sph[k * (size_t) n + (size_t) i] = std::sin (a);
                cph[k * (size_t) n + (size_t) i] = std::cos (a);
            }
        for (long long s = 0; s < T; ++s)
        {
            const double t = periods * (double) s / (double) T;
            std::fill (acc.begin(), acc.end(), 0.0);
            for (int i = 0; i < n; ++i)
            {
                const double ang = 6.283185307179586476925286766559 * (double) p[i].ratio * t;
                const double sr = std::sin (ang), cr = std::cos (ang);
                const double a = (double) p[i].amp;
                for (size_t k = 0; k < K; ++k)
                    acc[k] += a * (sph[k * (size_t) n + (size_t) i] * cr
                                   + cph[k * (size_t) n + (size_t) i] * sr);
            }
            for (size_t k = 0; k < K; ++k)
            {
                const double v = std::fabs (acc[k]); if (v > pk[k]) pk[k] = v;
                ss[k] += acc[k] * acc[k];
            }
        }
        for (size_t k = 0; k < K; ++k)
        {
            out[k].peak = pk[k];
            out[k].rms  = std::sqrt (ss[k] / (double) T);
            out[k].crest = out[k].rms > 0.0 ? pk[k] / out[k].rms : 0.0;
        }
    }

    static Stats crest (const Partial* p, int n, const double* phase,
                        double periods, int oversample)
    {
        std::vector<Stats> s;
        crestMulti (p, n, { phase }, periods, oversample, s);
        return s[0];
    }

    // Short-time RMS frequency over 16 blocks of ONE fundamental period, and its
    // correlation with the block index. Written from the description of the
    // measurement, not from the engine's implementation, and using a finite
    // difference of the directly synthesised waveform rather than an analytic
    // derivative - so it is genuinely a second opinion.
    static double sweepCorr (const Partial* p, int n, const double* phase, int samplesPerPeriod)
    {
        const int B = 16;
        const int T = samplesPerPeriod;
        if (T < B * 4) return 0.0;
        std::vector<double> x ((size_t) T + 1, 0.0);
        for (int s = 0; s <= T; ++s)
        {
            const double t = (double) s / (double) T;
            double v = 0.0;
            for (int i = 0; i < n; ++i)
                v += (double) p[i].amp
                     * std::sin (6.283185307179586476925286766559
                                 * ((double) phase[i] + (double) p[i].ratio * t));
            x[(size_t) s] = v;
        }
        const int len = T / B;
        double f[16]; int used = 0;
        for (int b = 0; b < B; ++b)
        {
            double sx = 0.0, sd = 0.0;
            for (int s = b * len; s < b * len + len; ++s)
            {
                const double d = (x[(size_t) s + 1] - x[(size_t) s]) * (double) T;
                sx += x[(size_t) s] * x[(size_t) s];
                sd += d * d;
            }
            if (sx <= 0.0) return 0.0;
            f[used++] = std::sqrt (sd / sx);
        }
        double mb = 0.0, mf = 0.0;
        for (int b = 0; b < used; ++b) { mb += b; mf += f[b]; }
        mb /= used; mf /= used;
        double num = 0.0, db = 0.0, df = 0.0;
        for (int b = 0; b < used; ++b)
        {
            const double eb = b - mb, ef = f[b] - mf;
            num += eb * ef; db += eb * eb; df += ef * ef;
        }
        if (db <= 0.0 || df <= 0.0) return 0.0;
        return std::fabs (num / std::sqrt (db * df));
    }

    static double dB (double from, double to)
    {
        if (from <= 0.0 || to <= 0.0) return 0.0;
        return 20.0 * std::log10 (from / to);
    }
} // namespace ref

//==============================================================================
// The shipping hash and Schroeder, written out HERE so the tests do not depend on
// the engine's copies being right. If these two ever diverge from
// AdditiveVoice::seedPhase or from the engine's closed forms, a test says so.
// One index. This is the test file's OWN copy of AdditiveVoice::seedPhase(i, false);
// the engine's phaseset::hashPhaseAt must equal it and is never used to produce the
// expected value anywhere below.
static double refHashAt (int i)
{
    const double h = std::sin ((double) i * 12.9898) * 43758.5453;
    return h - std::floor (h);
}

static void hashPhases (double* ph, int n)
{
    for (int i = 0; i < n; ++i) ph[i] = refHashAt (i);
}

static void schroederPhases (const Partial* p, int n, double gamma, double* ph)
{
    if (n <= 0) return;
    std::vector<double> w ((size_t) n);
    double tot = 0.0;
    for (int i = 0; i < n; ++i)
    {
        w[(size_t) i] = std::pow (std::max (1.0e-30, (double) p[i].amp), 2.0 * gamma);
        tot += w[(size_t) i];
    }
    for (int i = 0; i < n; ++i) w[(size_t) i] /= tot;
    double run = 0.0, acc = 0.0;
    ph[0] = kAlignedPhase;
    for (int i = 1; i < n; ++i)
    {
        run += w[(size_t) i - 1];
        acc += run;
        ph[i] = kAlignedPhase - acc - std::floor (kAlignedPhase - acc);
    }
}

//==============================================================================
// THE REFERENCE SPECTRA, from the real compiler.
struct Ref { const char* name; SpectrumSettings settings; SpectrumTemplate t; };

static SpectrumSettings scalaSettings (int edo, int repeats, double period = 2.0)
{
    SpectrumSettings s;
    s.source = Source::ScalaTimbre;
    s.timbrePeriod = (float) period;
    s.timbrePeriodRepeats = repeats;
    for (int i = 0; i < edo; ++i)
        s.timbreDegrees.push_back ((float) std::pow (period, (double) i / (double) edo));
    return s;
}

static std::vector<Ref>& referenceSpectra()
{
    static std::vector<Ref> refs;
    if (! refs.empty()) return refs;

    auto add = [&] (const char* name, SpectrumSettings s)
    {
        Ref r; r.name = name; r.settings = s; r.t = compileSpectrum (s);
        refs.push_back (std::move (r));
    };

    { SpectrumSettings s;                                   add ("default n=1024", s); }
    { SpectrumSettings s; s.maxPartials = 128; s.curveSlope = 0.3f;
                                                            add ("default maxP128 s0.3", s); }
    { SpectrumSettings s; s.maxPartials = 128;              add ("harmonic s1 n=128", s); }
    { SpectrumSettings s; s.curveSlope = 0.0f; s.maxPartials = 512;
                                                            add ("FLAT n=512", s); }
    { SpectrumSettings s; s.fractalLevel = 3.0f; s.maxPartials = 384;
                                                            add ("fractal L3 maxP384", s); }
    { SpectrumSettings s; s.fractalLevel = 4.0f;            add ("fractal L4", s); }
    { SpectrumSettings s; s.primeLimit = 7; s.primeLimitPos = 3.0f;
                                                            add ("prime-limit 7", s); }
    { SpectrumSettings s; s.selection = 2.0f; s.maxPartials = 512;
                                                            add ("odd maxP512", s); }
    add ("scala 22EDO x5", scalaSettings (22, 5));
    add ("scala 19EDO x6", scalaSettings (19, 6));
    add ("scala 31EDO x4", scalaSettings (31, 4));
    add ("scala 13ED3 x3", scalaSettings (13, 3, 3.0));
    return refs;
}

// The four the keyboard sweep must cover, chosen to span the engines: the shipped
// default, a fractal, a flat spectrum and a Scala-as-timbre patch.
static const char* kKeyboardPatches[] = { "default n=1024", "fractal L3 maxP384",
                                          "FLAT n=512", "scala 19EDO x6",
                                          "scala 31EDO x4", "prime-limit 7" };

static const Ref& refByName (const char* name)
{
    for (auto& r : referenceSpectra()) if (std::strcmp (r.name, name) == 0) return r;
    printf ("FATAL: no reference spectrum '%s'\n", name);
    std::exit (2);
}

//==============================================================================
// 1. THE RIG ITSELF. If ref::crest is wrong, everything below is worthless, so it
// is checked against values that are known in closed form.
static void testRig()
{
    // A single sine: peak/rms = sqrt(2), exactly, at any phase.
    {
        Partial p { 1.0f, 1.0f };
        for (double ph : { 0.0, 0.1, 0.25, 0.7 })
        {
            const ref::Stats s = ref::crest (&p, 1, &ph, 1.0, 64);
            check (std::fabs (s.crest - std::sqrt (2.0)) < 2.0e-3,
                   "rig: one sine has crest sqrt(2)", fmt ("phase %.2f -> %.5f", ph, s.crest));
        }
    }
    // N flat partials all at kAlignedPhase: peak = N (they meet at t=0),
    // rms = sqrt(N/2), so crest = sqrt(2N).
    for (int n : { 4, 32, 128 })
    {
        std::vector<Partial> p;
        for (int k = 1; k <= n; ++k) p.push_back ({ (float) k, 1.0f });
        std::vector<double> ph ((size_t) n, kAlignedPhase);
        const ref::Stats s = ref::crest (p.data(), n, ph.data(), 1.0, 32);
        check (std::fabs (s.crest - std::sqrt (2.0 * n)) / std::sqrt (2.0 * n) < 5.0e-3,
               "rig: N aligned flat partials have crest sqrt(2N)",
               fmt ("N=%.0f -> %.4f want %.4f", (double) n, s.crest, std::sqrt (2.0 * n)));
    }
    // Refining the rig must not move it: a converged measurement is stable.
    {
        const Ref& r = refByName ("scala 19EDO x6");
        const int n = 60;
        std::vector<double> ph ((size_t) n);
        hashPhases (ph.data(), n);
        const double a = ref::crest (r.t.partials.data(), n, ph.data(), 8.0, 16).crest;
        const double b = ref::crest (r.t.partials.data(), n, ph.data(), 8.0, 64).crest;
        check (std::fabs (ref::dB (a, b)) < 0.15,
               "rig: 16x and 64x oversampling agree", fmt ("%.4f vs %.4f", a, b));
    }
    // The engine's closed forms must be the ones this file believes in.
    {
        const int n = 64;
        std::vector<double> mine ((size_t) n), theirs ((size_t) n);
        hashPhases (mine.data(), n);
        detail::closedHash (theirs.data(), n);
        double worst = 0.0;
        for (int i = 0; i < n; ++i) worst = std::max (worst, std::fabs (mine[i] - theirs[i]));
        check (worst == 0.0, "engine's hash is AdditiveVoice::seedPhase, bit for bit",
               fmt ("worst delta %.3e", worst));
    }
}

//==============================================================================
// 2. FAILURE 1: THE ANALYSIS GRID MUST NOT BE A SURROGATE.
//
// v1 snapped every ratio to llround(ratio * W). On integer harmonics that is
// exact, so this test would have passed on the harmonic rows and FAILED on every
// inharmonic one: v1's report on a 22-EDO timbre was 3.42 dB away from what the
// same phases render.
//
// What is asserted: the engine's own reported crests, for the hash and for what it
// returns, agree with the independent rig - on inharmonic spectra as well as
// harmonic ones - and so does the dB gap between them, which is the number the
// decision is actually made on.
static void testTrueRatioAgreement()
{
    if (tables) printf ("\n-- grid agreement: engine report vs direct std::sin on true ratios --\n"
                        "%-24s %5s | %8s %8s | %8s %8s | %7s %7s\n",
                        "spectrum", "pfx", "engHash", "refHash", "engChos", "refChos",
                        "engGain", "refGain");
    for (auto& r : referenceSpectra())
    {
        Options opt;
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        if (! rep.measured || rep.analysed <= 0) continue;
        const int p = rep.analysed;
        std::vector<double> hp ((size_t) r.t.partials.size());
        hashPhases (hp.data(), (int) r.t.partials.size());
        std::vector<ref::Stats> s;
        ref::crestMulti (r.t.partials.data(), p, { hp.data(), plan.phasesFor (p) },
                         rep.verifyWindow, 16, s);
        const double engGain = ref::dB (rep.crestHash, rep.crestChosen);
        const double refGain = ref::dB (s[0].crest, s[1].crest);

        if (tables)
            printf ("%-24s %5d | %8.4f %8.4f | %8.4f %8.4f | %+7.2f %+7.2f\n",
                    r.name, p, rep.crestHash, s[0].crest, rep.crestChosen, s[1].crest,
                    engGain, refGain);

        check (std::fabs (ref::dB (rep.crestHash, s[0].crest)) < 0.5,
               std::string ("true-ratio agreement, hash: ") + r.name,
               fmt ("engine %.4f, rig %.4f", rep.crestHash, s[0].crest));
        check (std::fabs (ref::dB (rep.crestChosen, s[1].crest)) < 0.5,
               std::string ("true-ratio agreement, returned set: ") + r.name,
               fmt ("engine %.4f, rig %.4f", rep.crestChosen, s[1].crest));
        // The gap is what the plan is decided on, so it is held tighter than either
        // absolute value: a common-mode grid bias cancels in it, and only a real
        // surrogate error survives.
        check (std::fabs (engGain - refGain) < 0.5,
               std::string ("true-ratio agreement, the dB gap: ") + r.name,
               fmt ("engine %+.2f dB, rig %+.2f dB", engGain, refGain));
    }
}

//==============================================================================
// 3. FAILURE 2: THE NYQUIST PREFIX. NO NOTE MAY BE WORSE THAN TODAY.
//
// The voice renders only the partials below Nyquist at the played pitch. v1 built
// one phase set for the whole template: on a default patch it reported a 4.14 dB
// win and was 3.04 dB WORSE than the hash at MIDI 96, with 34 of 88 notes worse.
//
// THE COUNT ASSERTED HERE IS ZERO, scored by the independent rig on the true
// ratios of exactly the partials that sound at each pitch.
//
// AND IT IS SCORED THROUGH A LENS THE ENGINE DOES NOT CHOOSE. Measuring each note
// over that plan's own PhaseReport::verifyWindow would let the engine pick the
// window its answer looks best in - a second, subtler version of the circularity
// that shipped v1. `blindWindowPeriods` below is the window the partial spacing
// demands with NO amplitude floor and no budget: the longest beat in the prefix,
// capped only at Options::maxWindowPeriods. It is a strict superset of what the
// engine measured wherever kBeatAmpFloor shortened the window, so a plan that still
// wins here wins on a ruler it had no hand in choosing.
static double blindWindowPeriods (const Partial* p, int n)
{
    double minGap = 0.0;
    for (int i = 1; i < n; ++i)
    {
        const double d = (double) p[i].ratio - (double) p[i - 1].ratio;
        if (d > 1.0e-9 && (minGap <= 0.0 || d < minGap)) minGap = d;
    }
    if (minGap <= 0.0) minGap = 1.0;
    double w = std::ceil (1.0 / minGap);
    if (! (w >= 1.0)) w = 1.0;
    return std::min (128.0, w);      // the same musical ceiling Options uses
}

static void testKeyboardNeverWorse()
{
    if (tables) printf ("\n-- keyboard sweep, MIDI 21..108, direct std::sin on true ratios --\n"
                        "%-24s %5s %5s | worse | %8s %8s %8s | improved\n",
                        "spectrum", "anly", "built", "best dB", "worst dB", "mean dB");
    for (const char* name : kKeyboardPatches)
    {
        const Ref& r = refByName (name);
        const int n = (int) r.t.partials.size();
        Options opt;
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);

        int worse = 0, improvedNotes = 0, notes = 0;
        double best = 0.0, worstDb = 1.0e30, sum = 0.0;
        int worstMidi = 0;
        for (int m = 21; m <= 108; ++m)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
            if (p <= 0) continue;
            ++notes;
            if (plan.improved (p)) ++improvedNotes;
            std::vector<ref::Stats> s;
            ref::crestMulti (r.t.partials.data(), p, { hp.data(), plan.phasesFor (p) },
                             blindWindowPeriods (r.t.partials.data(), p), 12, s);
            const double d = ref::dB (s[0].crest, s[1].crest);
            // A hair of tolerance for the rig's own sampling, far below the
            // engine's kPreferMarginDb: a real regression is dB, not millidB.
            if (d < -0.02) { ++worse; }
            if (d > best) best = d;
            if (d < worstDb) { worstDb = d; worstMidi = m; }
            sum += d;
        }
        if (tables)
            printf ("%-24s %5d %5d | %5d | %+8.2f %+8.2f %+8.2f | %d/%d\n",
                    name, rep.analysed, rep.built, worse, best, worstDb, sum / notes,
                    improvedNotes, notes);

        check (worse == 0,
               std::string ("keyboard: no note worse than the shipping hash: ") + name,
               fmt ("%.0f of %.0f notes worse, worst %+.2f dB at MIDI %.0f",
                    (double) worse, (double) notes, worstDb, (double) worstMidi));
        // "Better on average" is only claimable where the engine CLAIMED an
        // improvement. On an aperiodic (Scala) template this version deliberately
        // returns the hash on every prefix - the span ladder refuses every candidate
        // - and the mean is then exactly +0.00 dB. Asserting "> 0" there, as version
        // 2's suite did, is asserting that the engine must claim something; it is the
        // pressure that produced the 581-millisecond guarantee. So the claim is
        // conditioned on improvedNotes, and the unconditional half of it is `worse == 0`
        // above, which holds everywhere.
        if (improvedNotes > 0)
            check (sum / notes > 0.0,
                   std::string ("keyboard: where prefixes improved, the average note is "
                                "better than today: ") + name,
                   fmt ("mean %+.2f dB over %.0f notes, %.0f of them improved",
                        sum / notes, (double) notes, (double) improvedNotes));
        else
            check (std::fabs (sum / notes) < 1.0e-12,
                   std::string ("keyboard: where NOTHING improved, every note is the hash "
                                "bit for bit - not 'about the same': ") + name,
                   fmt ("mean %+.4g dB", sum / notes));
    }
}

//==============================================================================
// 4. FAILURE 3: THE NOISE GATE MUST CATCH A SWEEP, NOT JUST A PULSE.
//
// v1 gated on the crest of dx/dt. On a FLAT spectrum, Schroeder gamma=1 has a low
// derivative crest - a chirp has a flat envelope, which is the whole point - so v1
// accepted a set whose short-time RMS frequency ramped from 141 to 491 in harmonic
// number inside every period, correlation +0.980, and reported the constraint
// PASSED with 2.8 dB to spare.
//
// Asserted here, all scored by ref::sweepCorr (a finite difference of the directly
// synthesised waveform, not the engine's analytic derivative):
//   - Schroeder gamma=1 on a flat spectrum is a sweep and is REFUSED;
//   - the shipping hash is not a sweep and is ACCEPTED;
//   - a random draw is not a sweep and is ACCEPTED;
//   - and the set the engine actually returns is not a sweep.
static void testSweepDetector()
{
    if (tables) printf ("\n-- sweep detector: |corr(block index, block RMS frequency)|, 16 blocks/period --\n"
                        "%-24s %5s | %8s %8s %8s %8s | limit %.2f\n",
                        "spectrum", "n", "hash", "random", "Schr g1", "returned", kSweepCorrLimit);
    // The four leftmost columns are the RIG's reading (a finite difference of a
    // directly synthesised waveform); engSchr1 is what the ENGINE reads for the same
    // set on its own grid, printed so the header's numbers can be checked rather
    // than believed.
    for (const char* name : { "FLAT n=512", "default maxP128 s0.3", "harmonic s1 n=128",
                              "fractal L3 maxP384" })
    {
        const Ref& r = refByName (name);
        const int n = std::min (512, (int) r.t.partials.size());
        std::vector<double> hp ((size_t) n), rp ((size_t) n), s1 ((size_t) n);
        hashPhases (hp.data(), n);
        detail::closedRandom (n, 12345, rp.data());
        schroederPhases (r.t.partials.data(), n, 1.0, s1.data());

        // 16 blocks of one fundamental period, at least 64 samples per block.
        const int sp = std::max (1024, 16 * 64);
        const double cHash = ref::sweepCorr (r.t.partials.data(), n, hp.data(), sp);
        const double cRand = ref::sweepCorr (r.t.partials.data(), n, rp.data(), sp);
        const double cSchr = ref::sweepCorr (r.t.partials.data(), n, s1.data(), sp);

        Options opt;
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        const int p = std::min (n, std::max (1, rep.analysed));
        const double cPlan = ref::sweepCorr (r.t.partials.data(), p, plan.phasesFor (p), sp);

        if (tables)
            printf ("%-24s %5d | %8.3f %8.3f %8.3f %8.3f | engine reads Schr g1 %.3f, "
                    "returned %.3f\n", name, n, cHash, cRand, cSchr, cPlan,
                    rep.sweepSchroeder1, rep.sweepChosen);

        check (cHash < kSweepCorrLimit,
               std::string ("sweep: the shipping hash is accepted: ") + name, fmt ("%.3f", cHash));
        check (cRand < kSweepCorrLimit,
               std::string ("sweep: a random draw is accepted: ") + name, fmt ("%.3f", cRand));
        check (cPlan < kSweepCorrLimit,
               std::string ("sweep: what the engine returns is not a sweep: ") + name,
               fmt ("%.3f", cPlan));
        if (std::strcmp (name, "FLAT n=512") == 0)
            check (cSchr > kSweepCorrLimit,
                   "sweep: Schroeder gamma=1 on a FLAT spectrum is refused",
                   fmt ("%.3f, limit %.2f", cSchr, kSweepCorrLimit));
    }

    // The engine must reach the same verdict on the flat-spectrum Schroeder set as
    // the independent rig does. This is the assertion v1 inverted.
    {
        const Ref& r = refByName ("FLAT n=512");
        Options opt;
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        check (rep.sweepSchroeder1 > kSweepCorrLimit,
               "sweep: the ENGINE also refuses flat Schroeder gamma=1",
               fmt ("engine reads %.3f, limit %.2f", rep.sweepSchroeder1, kSweepCorrLimit));
        check (rep.sweepChosen <= kSweepCorrLimit,
               "sweep: the engine's returned set passes its own gate",
               fmt ("%.3f", rep.sweepChosen));
    }

    // And the two descriptors must be doing DIFFERENT jobs: on a sloped spectrum
    // Schroeder gamma=1 barely sweeps at all and is caught by the derivative crest
    // instead. If one of them were redundant this would show it.
    {
        const Ref& r = refByName ("harmonic s1 n=128");
        const int n = (int) r.t.partials.size();
        std::vector<double> s1 ((size_t) n), rp ((size_t) n);
        schroederPhases (r.t.partials.data(), n, 1.0, s1.data());
        detail::closedRandom (n, 999, rp.data());
        const double cSchr = ref::sweepCorr (r.t.partials.data(), n, s1.data(), 2048);
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
        check (cSchr < kSweepCorrLimit,
               "sweep: on a SLOPED spectrum Schroeder g=1 does not sweep (so one "
               "descriptor is not enough)", fmt ("%.3f", cSchr));
        check (rep.derivSchroeder1 > rep.derivReference,
               "sweep: ...and the derivative crest catches it there instead",
               fmt ("deriv %.3f against reference %.3f", rep.derivSchroeder1, rep.derivReference));
    }
}

//==============================================================================
// 5. FAILURE 4: THE COST MUST FIT THE THREAD IT RUNS ON.
//
// The worker that runs this also serves the preset playground, which posts a blend
// every 16.7 ms; blendTemplates costs 0.02-2.24 ms. v1's buildTargetPhases cost
// 60.5-147 ms - 29x to 3200x a recompile - and ate the 100 ms realtime-adaptation
// budget whole.
//
// FOUR THINGS ARE ASSERTED, and they are deliberately different in kind, because
// the first version of this test asserted one wall-clock number and that number was
// a property of the machine's mood rather than of the engine.
//
//   1. workUsed <= workBudget, exactly. Machine-independent, and the actual
//      contract: it is what makes the cost the same on every box.
//   2. The DECLARED budgets honour their own declared millisecond names at the
//      header's declared slow-end rate. Pure arithmetic on the header's constants,
//      so a future edit that raises a budget without raising the ceiling fails here
//      rather than in a user's DAW.
//   3. RATE UNIFORMITY: no reference spectrum costs more than kUniformitySlack times
//      as much PER WORK UNIT as the cheapest one in the same run. That is a ratio
//      taken in one run, so the machine's speed cancels out of it, and it is the
//      shape of the only cost blow-up this engine has ever had: v1's call that the
//      work counter had bounded at 16 ms took 115 ms because a wide grid fell out of
//      L2 and a work unit stopped costing what the calibration said. 7x. This check
//      catches that and is blind to how busy the box is.
//   4. The wall clock and the measured ns per work unit are PRINTED on every run,
//      verbose or not, and asserted on NOTHING.
//
// WHY THE WALL CLOCK IS NOT ASSERTED, AND WHY THAT IS NOT THE TEST GOING SOFT. The
// same buildPlan call, same binary, same budget, measured on this one box while
// writing this file: 7.13 ms with testCost first in main(); 18.84 ms in its normal
// place after the brute-force rig; 45.62 ms with a second compile running; 189.71 ms
// with the machine fully saturated by another job. Per work unit that is 1.25 ns to
// 35 ns - a 28x spread with the work done identical to the unit. A number that moves
// 28x for reasons outside this file cannot be an assertion: it goes red for the wrong
// reason, and the first person it inconveniences deletes it.
//
// So the cost contract is 1 and 2. (1) is exact and machine-independent. (2) is what
// makes the millisecond NAMES on the budget constants true rather than aspirational,
// which is exactly what the first version of this header got wrong - it named a
// 5,520,000-unit budget "8 ms" from a rate measured on an idle core, and the same
// budget cost 15-22 ms in this suite. Correcting the quoted rate to the slow end and
// asserting budget x rate <= ceiling is what stops that recurring. (3) catches the
// one failure mode a work counter genuinely cannot see. And the printed numbers mean
// nobody has to take any of it on trust.
static constexpr double kDragCeilingMs     = 14.0;  // 16.7 ms frame minus a 2.24 ms blend
// 50.0, not v2's 45.0, and the reason is written down because a ceiling that moves
// to accommodate a budget is not a ceiling. The span ladder costs kSpansAperiodic
// verification passes where v2 paid for one, and a budget that cannot afford the
// ladder returns the hash (kMinSpansAperiodic) - so on an aperiodic template the
// extra 20 ms buys the guarantee itself, not reach. 50.0 ms is EXACTLY half of the
// plugin's 100 ms realtime-adaptation budget, which is a number that exists outside
// this file; that is what makes it a ceiling rather than a restatement of the budget.
static constexpr double kCompileCeilingMs  = 50.0;
static constexpr double kQuotedNsPerUnit   = 1.0e6 / (double) kWorkUnitsPerMillisecond;
static constexpr double kUniformitySlack   = 6.0;

// The wall clock is taken as the MINIMUM of several runs. The minimum is the honest
// estimate of what the call costs when the worker gets the core it is entitled to;
// the work counter, not this, is what bounds the contended case.
template <typename Fn>
static double minMs (int runs, Fn&& fn)
{
    double best = 1.0e30;
    for (int i = 0; i < runs; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double, std::milli> (t1 - t0).count());
    }
    return best;
}

static void testCost()
{
    // ---- 2. the declared budgets honour their own declared names, by arithmetic.
    check ((double) kWorkBudgetDrag * kQuotedNsPerUnit / 1.0e6 <= kDragCeilingMs,
           "cost: the DRAG budget fits a 60 Hz frame at the header's own quoted rate",
           fmt ("%.1f ms of a %.1f ms frame share",
                (double) kWorkBudgetDrag * kQuotedNsPerUnit / 1.0e6, kDragCeilingMs));
    check ((double) kWorkBudgetCompile * kQuotedNsPerUnit / 1.0e6 <= kCompileCeilingMs,
           "cost: the COMPILE budget fits its ceiling at the header's own quoted rate",
           fmt ("%.1f ms, ceiling %.1f ms",
                (double) kWorkBudgetCompile * kQuotedNsPerUnit / 1.0e6, kCompileCeilingMs));
    check ((double) kWorkBudgetCompile * kQuotedNsPerUnit / 1.0e6 <= 50.0,
           "cost: ...and leaves at least half the 100 ms realtime-adaptation budget",
           fmt ("%.1f ms of 100", (double) kWorkBudgetCompile * kQuotedNsPerUnit / 1.0e6));

    struct Budget { const char* label; long long units; double ceilingMs; };
    const Budget budgets[] = { { "drag",    kWorkBudgetDrag,    kDragCeilingMs },
                               { "compile", kWorkBudgetCompile, kCompileCeilingMs } };

    double fastestNs = 1.0e30, slowestNs = 0.0;
    std::string slowestName;
    for (const Budget& b : budgets)
    {
        if (tables) printf ("\n-- cost, %s budget (%lld units, ceiling %.1f ms) --\n"
                            "%-24s %5s %5s %5s %6s %6s | %9s %9s | %7s %8s\n",
                            b.label, b.units, b.ceilingMs,
                            "spectrum", "n", "anly", "built", "Tc", "Tv",
                            "work", "projected", "ms", "ns/unit");
        double worstMs = 0.0; std::string worstName;
        for (auto& r : referenceSpectra())
        {
            Options opt; opt.workBudget = b.units;
            PhasePlan plan;
            PhaseReport rep;
            const double ms = minMs (5, [&] {
                rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan); });
            if (ms > worstMs) { worstMs = ms; worstName = r.name; }
            const double ns = rep.workUsed > 0 ? ms * 1.0e6 / (double) rep.workUsed : 1.0e30;
            fastestNs = std::min (fastestNs, ns);
            if (ns > slowestNs && ns < 1.0e29)
            { slowestNs = ns; slowestName = std::string (b.label) + " " + r.name; }
            if (tables)
                printf ("%-24s %5d %5d %5d %6d %6d | %9lld %9lld | %7.2f %8.2f%s\n",
                        r.name, rep.n, rep.analysed, rep.built, rep.gridPoints,
                        rep.verifyGridPoints, rep.workUsed, rep.workProjected, ms, ns,
                        rep.reachReduced ? "  (reach cut)" : "");

            check (rep.workUsed <= rep.workBudget,
                   std::string ("cost: workUsed <= workBudget: ") + b.label + " " + r.name,
                   fmt ("%.0f used, %.0f allowed", (double) rep.workUsed, (double) rep.workBudget));
            check (rep.workProjected <= rep.workBudget,
                   std::string ("cost: the projection the reach was chosen from also fits: ")
                   + b.label + " " + r.name,
                   fmt ("%.0f projected, %.0f allowed",
                        (double) rep.workProjected, (double) rep.workBudget));
        }
        printf ("   %s budget: worst wall clock %.2f ms (%s); the budget says %.1f ms at the "
                "header's quoted rate\n", b.label, worstMs, worstName.c_str(),
                (double) b.units * kQuotedNsPerUnit / 1.0e6);
    }

    // ---- 3. the header's quoted rate is not optimistic on THIS machine.
    // Taken as the fastest per-unit rate seen above, i.e. the best case the worker
    // could ever get. If even that is slower than the quoted rate, every millisecond
    // figure in the header is wrong here and the reader has to be told.
    printf ("   work-unit rate this run: %.2f ns best, %.2f ns worst (%s); the header quotes "
            "%.2f ns/unit.\n   A best far above the quote means the BOX was busy, not that the "
            "engine changed - the work counter is the contract.\n",
            fastestNs, slowestNs, slowestName.c_str(), kQuotedNsPerUnit);
    check (slowestNs <= kUniformitySlack * fastestNs,
           "cost: every spectrum costs about the same PER WORK UNIT - no grid blows out "
           "of cache and makes a work unit stop meaning what the budget says",
           fmt ("worst %.2f ns/unit against a best of %.2f (%.2fx, limit %.1fx)",
                slowestNs, fastestNs, slowestNs / fastestNs, kUniformitySlack));

    // Adversarial: a near-coincident pair asks for a window thousands of periods
    // long. That must shorten the WINDOW (and say so), never blow the budget. The
    // pair sits at ratio 3, low enough that every prefix the engine analyses
    // contains it - a pair at the TOP of the list would simply fall outside the
    // reach and prove nothing.
    {
        std::vector<Partial> p;
        for (int k = 1; k <= 200; ++k) p.push_back ({ (float) k, (float) (1.0 / k) });
        p.push_back ({ 3.0008f, 0.3f });           // 0.46 cents from harmonic 3
        std::sort (p.begin(), p.end(), [] (const Partial& a, const Partial& b)
                   { return a.ratio < b.ratio; });
        Options opt;
        PhasePlan plan;
        PhaseReport rep;
        const double ms = minMs (5, [&] { rep = buildPlan (p.data(), (int) p.size(), opt, plan); });
        if (tables) printf ("%-24s %5d %5d %5d %6d %6d | %9lld %9lld %9lld | %7.2f  capped=%d W=%.0f\n",
                            "ADVERSARIAL near-pair", rep.n, rep.analysed, rep.built,
                            rep.gridPoints, rep.verifyGridPoints, rep.workUsed,
                            rep.workProjected, rep.workBudget, ms,
                            (int) rep.windowCapped, rep.verifyWindow);
        check (rep.workUsed <= rep.workBudget,
               "cost: an adversarial near-coincident pair still fits the budget",
               fmt ("%.0f used, %.0f allowed", (double) rep.workUsed, (double) rep.workBudget));
        check (rep.workProjected <= rep.workBudget,
               "cost: ...and the projection it chose its reach from fits too",
               fmt ("%.0f projected, %.0f allowed, wall clock %.2f ms",
                    (double) rep.workProjected, (double) rep.workBudget, ms));
        check (rep.windowCapped,
               "cost: ...and it is REPORTED as a shortened window, not hidden", "");
    }

    // The grid ceiling is what keeps a work unit costing what the calibration says.
    for (auto& r : referenceSpectra())
    {
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        check (rep.verifyGridPoints <= kMaxGridPoints,
               std::string ("cost: the grid stays inside kMaxGridPoints: ") + r.name,
               fmt ("Tv %.0f, ceiling %.0f", (double) rep.verifyGridPoints,
                    (double) kMaxGridPoints));
    }
}

//==============================================================================
// 6. FAILURE 5: THE ENGINE MUST NOT BE INERT ON THE SHIPPED DEFAULT PATCH.
//
// v1's budget gate returned today's hash, unmeasured, at n=1024 - which is exactly
// what the default SpectrumSettings compile to - so its headline "+1.6 to +7.5 dB"
// table described spectra the plugin does not produce unless the user first cuts
// the partial count.
static void testRunsOnTheShippedDefaults()
{
    for (auto& r : referenceSpectra())
    {
        Options opt;
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        check (rep.measured,
               std::string ("default patch: the engine measured something: ") + r.name,
               fmt ("n=%.0f analysed=%.0f", (double) rep.n, (double) rep.analysed));
        // A REAL REACH means the VERIFICATION reached real partials. `built` is the
        // CONSTRUCTION's reach, and on an aperiodic template it is deliberately zero
        // (Options::optimiseAperiodic) - the budget buys the span ladder there
        // instead. Version 2's suite demanded built > 0 everywhere, which is a
        // requirement that a search must run, not that the answer must be good; that
        // is the pressure that shipped the 581-millisecond guarantee. What must hold
        // everywhere is that the ANALYSIS reached something and the closed forms were
        // measured against the hash on it.
        check (rep.analysed > 0,
               std::string ("default patch: the verification reached a real prefix: ") + r.name,
               fmt ("analysed=%.0f of n=%.0f", (double) rep.analysed, (double) rep.n));
        check (rep.exactlyPeriodic ? rep.built > 0 : true,
               std::string ("default patch: on an exactly-periodic template the construction "
                            "also ran: ") + r.name,
               fmt ("analysed=%.0f built=%.0f", (double) rep.analysed, (double) rep.built));
        check (rep.crestHash > 0.0 && rep.crestSchroeder1 > 0.0 && rep.crestSchroederHalf > 0.0,
               std::string ("default patch: all three closed forms were MEASURED, even where "
                            "no search ran: ") + r.name,
               fmt ("hash %.4f, Schr1 %.4f, Schr0.5 %.4f",
                    rep.crestHash, rep.crestSchroeder1, rep.crestSchroederHalf));
    }
    // And on the actual shipped default it must improve something the keyboard uses.
    {
        const Ref& r = refByName ("default n=1024");
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        check (rep.n == 1024, "default patch: compiles to n=1024, as reported", fmt ("%.0f", (double) rep.n));
        check (rep.outcome == Outcome::Chosen && rep.improvedPrefixes > 0,
               "default patch: prefixes are actually improved",
               fmt ("%.0f of %.0f", (double) rep.improvedPrefixes, (double) rep.analysed));
        // Middle C must be one of them - a feature that only works above the top
        // octave is not a feature.
        const int p = renderedPrefix (r.t.partials.data(), (int) r.t.partials.size(),
                                      261.6255, opt.sampleRate);
        check (plan.improved (p), "default patch: middle C is improved",
               fmt ("prefix %.0f, analysed %.0f", (double) p, (double) rep.analysed));
    }
    // baseN 8 compiles to 8185 partials. The engine may not reach far up that, but
    // it must still measure, still improve the low prefixes, and still say so.
    {
        SpectrumSettings s; s.baseN = 8;
        const SpectrumTemplate t = compileSpectrum (s);
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (t.partials.data(), (int) t.partials.size(), opt, plan);
        check (rep.n > 8000, "baseN 8 compiles to more than 8000 partials",
               fmt ("%.0f", (double) rep.n));
        check (rep.measured && rep.analysed > 0,
               "baseN 8: still measured rather than skipped",
               fmt ("analysed %.0f", (double) rep.analysed));
        check (rep.reachReduced && rep.analysed < rep.n,
               "baseN 8: the shortened reach is REPORTED", "");
        check (rep.lowestFullyCoveredHz > 0.0,
               "baseN 8: the pitch the reach runs out at is published for the UI",
               fmt ("%.1f Hz", rep.lowestFullyCoveredHz));
    }
}

//==============================================================================
// 7. FAILURE 6: HONEST REPORTING.
//
// v1 returned 0.0 in every crest field on its Tier A path, so a downstream
// normalisation of the proposed form  peakC = 0.7071 / (crest * sqrt(sumSq))
// evaluated to 0 and produced SILENCE. And buildPhases(..., scatter = 0, ...)
// returned a default-constructed report that a UI could not tell apart from a
// fallback, so a badge keyed on it would have claimed "spectrum too large"
// whenever the fader sat at zero.
static void testHonestReporting()
{
    auto noZeroCrests = [&] (const PhaseReport& rep, const char* what)
    {
        const double f[] = { rep.crestChosen, rep.crestHash, rep.crestAligned,
                             rep.crestGreedy, rep.crestBestRandom, rep.crestSchroeder1,
                             rep.crestSchroederHalf, rep.derivChosen, rep.derivHash,
                             rep.derivReference, rep.ratioCap, rep.gridWindow,
                             rep.verifyWindow, rep.lowestFullyCoveredHz };
        for (double v : f)
            check (v != 0.0, std::string ("reporting: no field is 0.0 (") + what + ")",
                   "a caller may divide by these");
        for (double v : f)
            check (v == kUnmeasured || v > 0.0,
                   std::string ("reporting: every field is either kUnmeasured or real (")
                   + what + ")", fmt ("saw %.6f", v));
    };

    // Every reference spectrum, the ordinary path.
    for (auto& r : referenceSpectra())
    {
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        noZeroCrests (rep, r.name);
        check (rep.crestAligned > rep.crestHash,
               std::string ("reporting: the exact aligned crest is the worst of them: ") + r.name,
               fmt ("aligned %.4f, hash %.4f", rep.crestAligned, rep.crestHash));
        // improvedPrefixes is the ONLY claim of improvement, and it must be earned.
        if (rep.outcome == Outcome::Chosen)
            check (rep.improvedPrefixes > 0 && rep.worstGainDb >= kPreferMarginDb - 1.0e-9,
                   std::string ("reporting: Outcome::Chosen means a real, measured win: ") + r.name,
                   fmt ("%.0f prefixes, worst gain %.3f dB",
                        (double) rep.improvedPrefixes, rep.worstGainDb));
        else
            check (rep.improvedPrefixes == 0,
                   std::string ("reporting: not Chosen means nothing claimed improved: ") + r.name, "");
        // v2 could only ever emit two sets - the hash and one challenger - so it
        // asserted planSets == 2. This engine CHOOSES PER PREFIX among four families
        // (hash, Schroeder gamma 1, Schroeder gamma 0.5, the construction), so a
        // template on which two different families each win somewhere legitimately
        // holds three or four sets, and FLAT n=512 is exactly that template. The
        // invariant that survives is: set 0 is always the hash, a plan that improved
        // nothing is one set, and no plan holds a set no prefix asked for.
        check (rep.planSets >= 1 && rep.planSets <= 4,
               std::string ("reporting: planSets is within the candidate families: ") + r.name,
               fmt ("%.0f sets", (double) rep.planSets));
        check ((rep.planSets == 1) == (rep.improvedPrefixes == 0),
               std::string ("reporting: one set means nothing improved, and vice versa: ") + r.name,
               fmt ("%.0f sets, %.0f improved prefixes",
                    (double) rep.planSets, (double) rep.improvedPrefixes));
        {
            // No dead sets: every set index the plan holds is picked by some prefix.
            std::vector<int> used ((size_t) std::max (1, rep.planSets), 0);
            for (int p = 1; p <= plan.n; ++p)
                used[(size_t) plan.pick[(size_t) p - 1]] = 1;
            int dead = 0;
            for (int s = 0; s < rep.planSets; ++s) if (! used[(size_t) s]) ++dead;
            check (dead == 0,
                   std::string ("reporting: the plan carries no set that no prefix uses: ") + r.name,
                   fmt ("%.0f dead of %.0f", (double) dead, (double) rep.planSets));
        }
        check (rep.prefixesSchroeder1 + rep.prefixesSchroederHalf + rep.prefixesConstructed
                   == rep.improvedPrefixes,
               std::string ("reporting: the per-family prefix counts add up to improvedPrefixes "
                            "- the breakdown cannot claim more than the total: ") + r.name,
               fmt ("%.0f + %.0f + %.0f against %.0f",
                    (double) rep.prefixesSchroeder1, (double) rep.prefixesSchroederHalf,
                    (double) rep.prefixesConstructed, (double) rep.improvedPrefixes));
    }

    // A tiny budget: the engine must still say what happened, with real numbers or
    // the documented sentinel, and never a zero.
    {
        const Ref& r = refByName ("default n=1024");
        Options opt; opt.workBudget = 1000;      // absurdly small
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        noZeroCrests (rep, "starved budget");
        check (rep.outcome == Outcome::Hash,
               "reporting: a starved budget returns today's hash and says so",
               fmt ("outcome %.0f", (double) (int) rep.outcome));
        check (! rep.measured && rep.analysed == 0,
               "reporting: ...and does not claim to have measured anything", "");
        check (plan.valid() && plan.numSets == 1,
               "reporting: ...and the plan it hands back is still complete and usable", "");
        // Every returned phase is exactly today's hash.
        std::vector<double> hp ((size_t) r.t.partials.size());
        hashPhases (hp.data(), (int) r.t.partials.size());
        double worst = 0.0;
        const double* got = plan.phasesFor ((int) r.t.partials.size());
        for (size_t i = 0; i < r.t.partials.size(); ++i)
            worst = std::max (worst, std::fabs (got[i] - hp[i]));
        check (worst == 0.0, "reporting: a starved budget IS today's Scattered mode, bit for bit",
               fmt ("worst delta %.3e", worst));
    }

    // scatter == 0 must be distinguishable from a fallback, and must carry the real
    // aligned crest rather than a zero a caller could divide by.
    {
        const Ref& r = refByName ("harmonic s1 n=128");
        const int n = (int) r.t.partials.size();
        std::vector<double> out ((size_t) n);
        Options opt;
        const PhaseReport rep = buildPhases (r.t.partials.data(), n, 0.0, opt, out.data());
        check (rep.outcome == Outcome::Aligned,
               "reporting: scatter == 0 reports Aligned, not a fallback", "");
        check (rep.measured && rep.crestChosen > 1.0,
               "reporting: ...and carries a real crest, not 0.0",
               fmt ("%.4f", rep.crestChosen));
        // and it is bit-identical to today's Aligned mode
        bool allAligned = true;
        for (int i = 0; i < n; ++i) allAligned = allAligned && (out[i] == kAlignedPhase);
        check (allAligned, "reporting: scatter == 0 is bit-identical to today's Aligned mode", "");
        // The closed-form aligned crest must match the rig.
        const ref::Stats s = ref::crest (r.t.partials.data(), n, out.data(), 1.0, 32);
        check (std::fabs (ref::dB (rep.crestAligned, s.crest)) < 0.1,
               "reporting: the closed-form aligned crest matches direct synthesis",
               fmt ("closed form %.4f, rig %.4f", rep.crestAligned, s.crest));
    }

    // n == 0 is Empty, and everything is the sentinel.
    {
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (nullptr, 0, opt, plan);
        check (rep.outcome == Outcome::Empty && ! rep.measured,
               "reporting: an empty partial list is Empty", "");
        check (rep.crestChosen == kUnmeasured && rep.crestHash == kUnmeasured,
               "reporting: ...with the documented sentinel, not 0.0", "");
    }

    // The normalisation a caller would actually write must be finite and sane.
    for (auto& r : referenceSpectra())
    {
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        const double c = rep.crestChosen >= 0.0 ? rep.crestChosen : rep.crestAligned;
        double sumSq = 0.0;
        for (auto& p : r.t.partials) sumSq += (double) p.amp * p.amp;
        const double normGain = 0.7071 / (c * std::sqrt (sumSq));
        check (std::isfinite (normGain) && normGain > 0.0,
               std::string ("reporting: the obvious downstream normaliser is not silent: ") + r.name,
               fmt ("gain %.6f from crest %.4f", normGain, c));
    }
}

//==============================================================================
// 8. THE MORPH. A blend frame must not step the crest the way v1 did (2.91 dB when
// the user parked on a playground corner, because the union of 451 partials was
// Tier A while the 76-partial corner cleared the gate).
//
// What is measured: the crest of what a HELD NOTE actually gets, at several
// pitches, across a 41-frame morph between two corners running different engines -
// with the shipping hash on the same frames as the control, because the blended
// spectrum itself moves and part of any step is not ours.
// TWO numbers are measured here and they mean different things.
//
//   INSIDE THE REACH - frames where the rendered prefix is one the engine actually
//   analysed. This is what the engine itself contributes to a morph, and it is
//   asserted TIGHTLY: no worse than what the shipping hash does on the same frames
//   plus a small allowance. If the plan flapped between accepted and refused, or
//   if the construction were wildly path-dependent, this is where it would show.
//
//   ACROSS THE REACH BOUNDARY - frames where the rendered prefix crosses from
//   inside the analysed range to outside it, at which point the plan hands back
//   the hash and the crest jumps by whatever the improvement was worth. This is a
//   real limit and it is asserted at a MEASURED ceiling, not a hoped-for one.
//
// THE MEASUREMENT WINDOW IS FIXED ACROSS THE SWEEP, AND THE FIRST VERSION OF THIS
// TEST GOT THAT WRONG. It measured each frame over that frame's own
// PhaseReport::verifyWindow. Blending a fractal (integer ratios, window 1 period)
// with a Scala timbre (irrational ratios, window ~80 periods) moves that window by
// two orders of magnitude in a single frame, so the lens changed at the same instant
// the spectrum did and the two effects were added together and reported as one
// "step". A HELD NOTE DOES NOT RE-WINDOW WHEN THE USER DRAGS A CONTROL: it hears the
// same few tens of milliseconds either way. MEASURED, the same sweep scored both
// ways: 4.31 dB worst step through the moving lens, 1.98 dB through this fixed one -
// so more than half of what the old number blamed on the engine was the ruler.
//
// 8 fundamental periods is that fixed lens: 15 ms at MIDI 72, 122 ms at MIDI 36 -
// the scale of a note's attack, which is where a crest step would be heard as a
// level jump.
static constexpr double kMorphWindowPeriods = 8.0;

// The ceilings. `inside the reach` is asserted RELATIVE to the shipping hash on the
// same frames, because the blended spectrum itself moves and part of any step is not
// ours: the hash's own worst step on this sweep is 1.84 dB, and an absolute ceiling
// below that would be asserting something about the world rather than about the
// engine. (That is exactly what the previous 1.00 dB constant did, and it failed for
// that reason.) The absolute ceilings are set above what was measured, so they still
// catch a blow-up.
//
// THE BOUNDARY STEP IS 3.71 dB AND THAT IS THIS ENGINE'S WORST NUMBER. It is the
// value of the improvement being switched off: at t=0 the fractal corner renders 45
// partials at MIDI 72 and the plan is 2.44 dB better than the hash; one frame later
// the union of the two corners is 492 partials whose closest spacing demands an
// 81-period window, the verify grid triples, the reach falls from 225/185 to 96/43,
// nothing beats the hash at any prefix and the plan hands back today's sound.
// EVERY FRAME IS STILL AT LEAST AS GOOD AS TODAY - the step is a step in how much
// better, never into a regression - but a normalisation law driven by crestChosen
// would turn it into a level jump between two notes played 17 ms apart, so the
// integrator must not drive one from a value that moves like this.
// The fix that would remove it was built and measured and costs the guarantee; see
// kBeatAmpFloor in the header for the table, and the blind-window assertion at the
// end of this test for what pins the rejection.
static constexpr double kMorphRelativeAllowanceDb   = 0.50;
static constexpr double kMorphStepInsideReachCapDb  = 3.00;
static constexpr double kMorphStepBoundaryCeilingDb = 4.00;

static void testMorphStep()
{
    const Ref& A = refByName ("fractal L3 maxP384");
    const Ref& B = refByName ("scala 19EDO x6");
    if (tables) printf ("\n-- playground morph, fractal L3 -> 19-EDO timbre, 41 frames, "
                        "fixed %.0f-period lens --\n"
                        "%5s %6s | %9s %9s %9s | %s\n", kMorphWindowPeriods, "MIDI", "prefix",
                        "plan dB", "hash dB", "aligned dB", "inside-reach plan dB");
    double worstAny = 0.0, worstHash = 0.0, worstInside = 0.0;
    int worstMidi = 0; double worstAt = 0.0;
    int    blindFrames = 0, blindWorse = 0; double blindWorstDb = 0.0;
    for (int m : { 36, 48, 60, 72, 84, 96 })
    {
        const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
        double prevPlan = -1, prevHash = -1, prevAlign = -1;
        bool prevInside = false;
        double sp = 0, sh = 0, sa = 0, si = 0;
        int lastPrefix = 0;
        for (int f = 0; f <= 40; ++f)
        {
            const double t = (double) f / 40.0;
            const SpectrumTemplate mt = blendTemplates (A.t, B.t, t);
            const int n = (int) mt.partials.size();
            if (n <= 0) continue;
            Options opt; PhasePlan plan;
            const PhaseReport rep = buildPlan (mt.partials.data(), n, opt, plan);
            const int p = renderedPrefix (mt.partials.data(), n, f0, opt.sampleRate);
            if (p <= 0) continue;
            lastPrefix = p;
            const bool inside = (p <= rep.analysed);
            std::vector<double> hp ((size_t) n), ap ((size_t) n, kAlignedPhase);
            hashPhases (hp.data(), n);
            std::vector<ref::Stats> s;
            ref::crestMulti (mt.partials.data(), p,
                             { plan.phasesFor (p), hp.data(), ap.data() },
                             kMorphWindowPeriods, 8, s);
            // THE GUARANTEE, ON A BLENDED TEMPLATE, THROUGH THE BLIND LENS. This is
            // where kBeatAmpFloor changes what the engine measures - it is a no-op on
            // every unblended reference spectrum - so it is exactly here that the
            // shorter window has to be shown not to have bought a lie. Scored over
            // the amplitude-blind window the floor declined to pay for.
            if (plan.improved (p))
            {
                std::vector<ref::Stats> b;
                ref::crestMulti (mt.partials.data(), p, { hp.data(), plan.phasesFor (p) },
                                 blindWindowPeriods (mt.partials.data(), p), 6, b);
                const double gb = ref::dB (b[0].crest, b[1].crest);
                ++blindFrames;
                if (gb < -0.02) ++blindWorse;
                if (gb < blindWorstDb) blindWorstDb = gb;
            }
            const double stepPlan = (prevPlan > 0) ? std::fabs (ref::dB (prevPlan, s[0].crest)) : 0.0;
            if (prevPlan  > 0) sp = std::max (sp, stepPlan);
            if (prevHash  > 0) sh = std::max (sh, std::fabs (ref::dB (prevHash,  s[1].crest)));
            if (prevAlign > 0) sa = std::max (sa, std::fabs (ref::dB (prevAlign, s[2].crest)));
            if (prevPlan > 0 && inside && prevInside) si = std::max (si, stepPlan);
            if (stepPlan > worstAny) { worstAny = stepPlan; worstMidi = m; worstAt = t; }
            prevPlan = s[0].crest; prevHash = s[1].crest; prevAlign = s[2].crest;
            prevInside = inside;
        }
        worstHash   = std::max (worstHash, sh);
        worstInside = std::max (worstInside, si);
        if (tables) printf ("%5d %6d | %9.2f %9.2f %9.2f | %9.2f\n", m, lastPrefix, sp, sh, sa, si);
    }
    if (tables) printf ("   worst step: %.2f dB overall (MIDI %d, t=%.3f); %.2f dB inside the "
                        "reach; the hash's own worst is %.2f dB\n",
                        worstAny, worstMidi, worstAt, worstInside, worstHash);

    check (worstInside <= worstHash + kMorphRelativeAllowanceDb,
           "morph: inside the analysed reach the plan is no jumpier than today's "
           "Scattered mode on the same frames",
           fmt ("plan %.2f dB, hash %.2f dB, allowance %.2f dB",
                worstInside, worstHash, kMorphRelativeAllowanceDb));
    check (worstInside < kMorphStepInsideReachCapDb,
           "morph: ...and inside the reach it never steps more than the measured cap",
           fmt ("%.2f dB, cap %.2f dB", worstInside, kMorphStepInsideReachCapDb));
    check (worstAny < kMorphStepBoundaryCeilingDb,
           "morph: crossing the reach boundary steps by no more than the measured ceiling",
           fmt ("%.2f dB, ceiling %.2f dB - THIS IS THE ENGINE'S KNOWN LIMIT, see the "
                "comment above the test", worstAny, kMorphStepBoundaryCeilingDb));
    // v2's beat-amplitude floor is GONE from this engine - it was a way to buy a
    // shorter window, and this version does not shorten windows, it adds spans. The
    // assertion that replaced it is the blind-window one below, which is the thing
    // the floor could actually have broken.
    check (blindFrames > 4,
           "morph: there are improved blend frames to check the beat floor on",
           fmt ("%.0f frames", (double) blindFrames));
    check (blindWorse == 0,
           "morph: on a BLENDED template the plan is better through the amplitude-blind "
           "window too - the beat floor did not buy a shorter window with a worse answer",
           fmt ("%.0f of %.0f improved frames worse, worst %+.2f dB",
                (double) blindWorse, (double) blindFrames, blindWorstDb));
}

//==============================================================================
// 9. DETERMINISM. The preset playground crossfades compiled templates and the
// morph glides between them; if the same settings compiled to different phases the
// morph would chase a moving target and the sustain would never settle.
//
// v1's version of this test used harmonicSet(32, 1.0) only, where every amplitude
// is distinct - so replacing all three std::stable_sort with std::sort left the
// suite GREEN while changing the output (fractal L3 n=128 crest 2.0050 -> 2.1128).
// This one uses the real fractal L3 at maxPartials 384, which carries 193 exact
// float amplitude ties, and it tests the PROPERTY those sorts exist to provide -
// invariance to the order the caller hands the partials over in - which no sort
// swap can satisfy by accident.
static void testDeterminism()
{
    for (auto& r : referenceSpectra())
    {
        const int n = (int) r.t.partials.size();
        if (n <= 0 || n > 2048) continue;      // keep the repeat count affordable
        Options opt;

        PhasePlan first;
        buildPlan (r.t.partials.data(), n, opt, first);

        // (a) repeats in this process
        for (int rep = 0; rep < 4; ++rep)
        {
            PhasePlan p;
            buildPlan (r.t.partials.data(), n, opt, p);
            bool same = p.numSets == first.numSets && p.pick == first.pick
                        && p.phase.size() == first.phase.size();
            for (size_t i = 0; same && i < p.phase.size(); ++i)
                same = (p.phase[i] == first.phase[i]);
            check (same, std::string ("determinism: repeated calls agree bit for bit: ") + r.name, "");
        }

        // (b) concurrent threads
        {
            std::atomic<int> bad { 0 };
            std::vector<std::thread> th;
            for (int k = 0; k < 4; ++k)
                th.emplace_back ([&]
                {
                    PhasePlan p;
                    buildPlan (r.t.partials.data(), n, opt, p);
                    if (p.phase != first.phase || p.pick != first.pick) ++bad;
                });
            for (auto& t : th) t.join();
            check (bad.load() == 0,
                   std::string ("determinism: four concurrent threads agree: ") + r.name, "");
        }

        // (c) INPUT ORDER. The engine sorts by (ratio, amp) so the returned array is
        // a pure function of the partial multiset. Shuffle the input deterministically,
        // un-shuffle the output, and it must be identical.
        //
        // THIS IS THE PROPERTY, not a proxy for it. v1's suite watched one sort call
        // and used harmonicSet(32, 1.0), where every amplitude is distinct, so no tie
        // was ever exercised and swapping stable_sort for sort left it green while
        // changing the output. Here the reference set includes fractal L3 at
        // maxPartials 384, which carries 193 exact float amplitude ties (73 inside the
        // loudest 128), and this assertion tests order-independence itself.
        // MEASURED BY SABOTAGE: removing the sort fails this on 10 of 12 spectra with
        // a worst phase delta of 0.98. Swapping stable_sort for sort does NOT fail it,
        // and leaves the whole suite green - because the (ratio, amp) key leaves no
        // tie whose order can reach the output. See the note at the sort in
        // PhaseSets.h. That is the v1 defect DESIGNED OUT rather than merely tested
        // for, which is why this assertion is about permutation and not about sorts.
        {
            std::vector<int> perm ((size_t) n);
            for (int i = 0; i < n; ++i) perm[(size_t) i] = i;
            for (int i = n - 1; i > 0; --i)      // fixed splitmix64 shuffle, no clock
            {
                const uint64_t h = detail::splitmix64 (0xC0FFEEull + (uint64_t) i);
                std::swap (perm[(size_t) i], perm[(size_t) (h % (uint64_t) (i + 1))]);
            }
            std::vector<Partial> shuffled ((size_t) n);
            for (int i = 0; i < n; ++i) shuffled[(size_t) i] = r.t.partials[(size_t) perm[(size_t) i]];
            PhasePlan p;
            buildPlan (shuffled.data(), n, opt, p);
            check (p.numSets == first.numSets && p.pick == first.pick,
                   std::string ("determinism: a permuted input gives the same PLAN: ") + r.name, "");
            bool same = (p.numSets == first.numSets);
            double worst = 0.0;
            for (int s = 0; same && s < p.numSets; ++s)
                for (int i = 0; i < n; ++i)
                {
                    const double got  = p.phase[(size_t) s * (size_t) n + (size_t) i];
                    const double want = first.phase[(size_t) s * (size_t) n + (size_t) perm[(size_t) i]];
                    worst = std::max (worst, std::fabs (got - want));
                }
            check (worst == 0.0,
                   std::string ("determinism: a permuted input gives the same PHASES: ") + r.name,
                   fmt ("worst delta %.3e", worst));
        }
    }

    // The tie-carrying spectrum, named because it is the one v1's suite never had.
    {
        const Ref& r = refByName ("fractal L3 maxP384");
        std::vector<float> a;
        for (auto& p : r.t.partials) a.push_back (p.amp);
        std::sort (a.begin(), a.end());
        int ties = 0;
        for (size_t i = 1; i < a.size(); ++i) if (a[i] == a[i - 1]) ++ties;
        check (ties > 150,
               "determinism: the tie-carrying spectrum really does carry ties",
               fmt ("%.0f exact float amplitude ties among %.0f partials",
                    (double) ties, (double) a.size()));
    }
}

//==============================================================================
// 10. THE FADER. scatter 0 is exactly today's Aligned; scatter 1 is exactly the
// plan's target; in between the crest comes down without going backwards.
static void testFader()
{
    for (auto& r : referenceSpectra())
    {
        const int n = std::min (256, (int) r.t.partials.size());
        if (n <= 0) continue;
        Options opt;
        PhasePlan plan;
        buildPlan (r.t.partials.data(), n, opt, plan);
        const double* target = plan.phasesFor (n);
        std::vector<double> out ((size_t) n);

        interpolatePhases (target, n, 0.0, out.data());
        bool ok = true;
        for (int i = 0; i < n; ++i) ok = ok && (out[i] == kAlignedPhase);
        check (ok, std::string ("fader: scatter 0 is exactly Aligned: ") + r.name, "");

        interpolatePhases (target, n, 1.0, out.data());
        ok = true;
        for (int i = 0; i < n; ++i) ok = ok && (out[i] == target[i]);
        check (ok, std::string ("fader: scatter 1 is exactly the target: ") + r.name, "");

        // Down in the ear, not just in the parameter. The travel is NOT asserted to
        // be strictly monotone and it is not: interpolating each phase along its own
        // shortest arc is a straight line in phase space, not in crest, so a partial
        // can pass through a locally worse alignment on its way. What is asserted is
        // that the ripple is small and that the fader never has to be pushed past a
        // point to get quieter than an earlier one - i.e. every step is below the
        // running best by more than the ripple. MEASURED: the worst single-step rise
        // over the twelve reference spectra is under 6%, i.e. half a decibel.
        double prev = -1.0, worstRise = 1.0, first = 0.0, last = 0.0;
        for (int k = 0; k <= 20; ++k)
        {
            const double u = (double) k / 20.0;
            interpolatePhases (target, n, std::pow (u, (double) kTaperExponent), out.data());
            const double c = ref::crest (r.t.partials.data(), n, out.data(), 1.0, 8).crest;
            if (k == 0)  first = c;
            if (k == 20) last  = c;
            if (prev > 0 && c > prev) worstRise = std::max (worstRise, c / prev);
            prev = c;
        }
        check (worstRise < 1.10,
               std::string ("fader: the travel has no big step backwards: ") + r.name,
               fmt ("worst single-step rise x%.4f", worstRise));
        check (last < first,
               std::string ("fader: the far end is quieter than the near end: ") + r.name,
               fmt ("%.4f -> %.4f", first, last));
    }

    // THE TAPER EXPONENT, measured rather than asserted from memory. For each
    // candidate exponent: walk 40 evenly spaced FADER positions u, apply
    // x = pow(u, e), measure the real crest, and ask two things of the resulting dB
    // curve - how much of the total span it has bought by the half-way position
    // (a dead zone is the user's actual complaint about the fractal fader), and its
    // worst deviation from a straight dB ramp. The winner is the exponent that
    // minimises the worst deviation; that it also lands nearest the half-way dB at
    // the half-way position is a consequence, not a second criterion.
    {
        const double exps[5] = { 1.00, 0.70, 0.50, 0.45, 0.35 };
        double fracSum[5]  = { 0, 0, 0, 0, 0 };   // sum of (dB by u=0.5) / (dB total)
        double devWorst[5] = { 0, 0, 0, 0, 0 };
        double spanSum[5]  = { 0, 0, 0, 0, 0 };
        int    used[5]     = { 0, 0, 0, 0, 0 };
        for (auto& r : referenceSpectra())
        {
            const int n = std::min (256, (int) r.t.partials.size());
            if (n <= 0) continue;
            Options opt; PhasePlan plan;
            buildPlan (r.t.partials.data(), n, opt, plan);
            const double* target = plan.phasesFor (n);
            std::vector<double> out ((size_t) n);
            for (int e = 0; e < 5; ++e)
            {
                std::vector<double> db (41, 0.0);
                double c0 = 0.0;
                for (int k = 0; k <= 40; ++k)
                {
                    const double u = (double) k / 40.0;
                    interpolatePhases (target, n, std::pow (u, exps[e]), out.data());
                    const double c = ref::crest (r.t.partials.data(), n, out.data(), 1.0, 8).crest;
                    if (k == 0) c0 = c;
                    db[(size_t) k] = ref::dB (c0, c);      // dB gained since u = 0
                }
                const double span = db[40];
                if (span <= 0.5) continue;                 // nothing to shape here
                const double half = db[20];
                double dev = 0.0;
                for (int k = 0; k <= 40; ++k)
                    dev = std::max (dev, std::fabs (db[(size_t) k] - span * (double) k / 40.0));
                fracSum[e] += half / span;
                spanSum[e] += span;
                ++used[e];
                devWorst[e] = std::max (devWorst[e], dev);
            }
        }
        printf ("\n-- fader taper: x = pow(u, e), 40 positions, %d reference spectra --\n", used[0]);
        printf ("%10s   %-32s   %s\n", "exponent",
                "share of the dB span by u = 0.5", "worst deviation from a straight dB ramp");
        for (int e = 0; e < 5; ++e)
            printf ("%10.2f   %5.1f%% of a %.2f dB span%12s%.2f dB\n", exps[e],
                    100.0 * fracSum[e] / std::max (1, used[e]),
                    spanSum[e] / std::max (1, used[e]), "", devWorst[e]);
        int best = 0;
        for (int e = 1; e < 5; ++e) if (devWorst[e] < devWorst[best]) best = e;
        printf ("   taper: the exponent minimising the worst deviation is %.2f "
                "(%.2f dB); the header ships %.2f.\n", exps[best], devWorst[best],
                (double) kTaperExponent);
        // Tolerance 1e-3, not 0: kTaperExponent is a float, and 0.45f is
        // 0.44999998807907104 as a double. The candidate exponents are 0.10 apart at
        // the closest, so 1e-3 cannot pick the wrong one.
        check (std::fabs (exps[best] - (double) kTaperExponent) < 1.0e-3,
               "fader: kTaperExponent IS the measured minimum of the worst-deviation column "
               "- the constant is the measurement's answer, not a guess",
               fmt ("measured %.2f, shipped %.2f", exps[best], (double) kTaperExponent));
        const double fracLinear = fracSum[0] / std::max (1, used[0]);
        const double fracShipped = fracSum[best] / std::max (1, used[best]);
        check (fracLinear < 0.35,
               "fader: ...and the untapered fader really does have the dead zone the user "
               "complained about - the first half of the travel buys under a third of the span",
               fmt ("%.0f%% by u = 0.5, over %.0f spectra",
                    100.0 * fracLinear, (double) used[0]));
        check (fracShipped > fracLinear + 0.10,
               "fader: ...and the shipped exponent moves a real amount of that span into the "
               "first half of the travel",
               fmt ("%.0f%% at e = %.2f against %.0f%% at e = 1.00",
                    100.0 * fracShipped, exps[best], 100.0 * fracLinear));
    }

    // interpolatePhases is the note-on half: it must be cheap and allocation-free.
    {
        // The MINIMUM of nine batches, not the average of one. An average over a
        // single 12 ms window is a wall-clock number that one OS stall can multiply
        // by twenty, and this assertion did go red once for exactly that reason
        // while this file was being written - which is the failure mode the whole
        // cost section of this suite exists to avoid. The minimum is the honest
        // estimate of what the call costs when it gets the core it is entitled to,
        // and the bound below is three orders of magnitude above it, so it is a
        // "did somebody put an allocation in here" check rather than a benchmark.
        const int n = 128;
        std::vector<double> target ((size_t) n), out ((size_t) n);
        detail::closedRandom (n, 7, target.data());
        double us = 1.0e30;
        for (int b = 0; b < 9; ++b)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20000; ++i) interpolatePhases (target.data(), n, 0.5, out.data());
            const auto t1 = std::chrono::steady_clock::now();
            us = std::min (us, std::chrono::duration<double, std::micro> (t1 - t0).count() / 20000.0);
        }
        printf ("   interpolatePhases at n=128: %.3f us per call (minimum of nine batches).\n", us);
        check (us < 20.0, "fader: interpolatePhases is note-on cheap", fmt ("%.3f us", us));
    }
}

//==============================================================================
// 11. THE PLAN'S NOTE-ON PATH. phasesFor must be total, clamped and consistent.
static void testPlanLookup()
{
    const Ref& r = refByName ("harmonic s1 n=128");
    const int n = (int) r.t.partials.size();
    Options opt; PhasePlan plan;
    const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
    check (plan.valid() && plan.n == n, "plan: covers every partial handed in", "");
    check ((int) plan.pick.size() == n, "plan: has a decision for every prefix length", "");
    for (int p : { -5, 0, 1, n / 2, n, n + 1, n + 10000 })
        check (plan.phasesFor (p) != nullptr, "plan: phasesFor is total", fmt ("prefix %.0f", (double) p));
    // Past the analysed reach the plan must hand back today's hash, unconditionally.
    if (rep.analysed < n)
        for (int p = rep.analysed + 1; p <= n; ++p)
            check (! plan.improved (p),
                   "plan: past the reach it is today's hash", fmt ("prefix %.0f", (double) p));
    // seedFromPlan writes exactly prefixLen phases and nothing beyond.
    {
        std::vector<double> out ((size_t) n + 8, -12345.0);
        seedFromPlan (plan, RenderedPrefix { 40 }, FillSpan { 40 }, 1.0, out.data());
        bool wrote = true, spilled = false;
        for (int i = 0; i < 40; ++i) wrote = wrote && (out[(size_t) i] != -12345.0);
        for (int i = 40; i < n + 8; ++i) spilled = spilled || (out[(size_t) i] != -12345.0);
        check (wrote && ! spilled, "plan: seedFromPlan writes exactly the rendered prefix", "");
    }
}

//==============================================================================
// 12. THE PER-NOTE SALT. It is a pure time shift, so a single voice's crest is
// mathematically unchanged; what it changes is what happens when voices add.
static void testSalt()
{
    const Ref& r = refByName ("harmonic s1 n=128");
    const int n = (int) r.t.partials.size();
    Options a; PhasePlan pa; buildPlan (r.t.partials.data(), n, a, pa);
    Options b; b.perNoteSalt = true; b.noteNumber = 67;
    PhasePlan pb; buildPlan (r.t.partials.data(), n, b, pb);

    const double ca = ref::crest (r.t.partials.data(), n, pa.phasesFor (n), 1.0, 24).crest;
    const double cb = ref::crest (r.t.partials.data(), n, pb.phasesFor (n), 1.0, 24).crest;
    check (std::fabs (ref::dB (ca, cb)) < 0.15,
           "salt: a time shift does not change the crest", fmt ("%.4f vs %.4f", ca, cb));

    // Same note number twice = identical. Different note numbers = different.
    Options c = b; PhasePlan pc; buildPlan (r.t.partials.data(), n, c, pc);
    check (pc.phase == pb.phase, "salt: the same note number gives the same phases", "");
    Options d = b; d.noteNumber = 68; PhasePlan pd; buildPlan (r.t.partials.data(), n, d, pd);
    check (pd.phase != pb.phase, "salt: a different note number gives different phases", "");
}

//==============================================================================
// 13. DEGENERATE INPUTS. None of these may crash, allocate unboundedly or return
// something a caller could divide by and get silence.
static void testDegenerate()
{
    Options opt;
    { PhasePlan p; const PhaseReport r = buildPlan (nullptr, 0, opt, p);
      check (r.outcome == Outcome::Empty && ! p.valid(), "degenerate: n = 0", ""); }
    { Partial one { 1.0f, 1.0f }; PhasePlan p;
      const PhaseReport r = buildPlan (&one, 1, opt, p);
      check (p.valid() && p.phasesFor (1) != nullptr, "degenerate: n = 1", "");
      check (r.crestAligned > 0.0, "degenerate: n = 1 still reports a real aligned crest",
             fmt ("%.4f", r.crestAligned)); }
    { std::vector<Partial> z (64, Partial { 1.0f, 0.0f });
      for (int i = 0; i < 64; ++i) z[(size_t) i].ratio = (float) (i + 1);
      PhasePlan p; buildPlan (z.data(), 64, opt, p);
      check (p.valid(), "degenerate: every amplitude zero", ""); }
    { std::vector<Partial> dup (64, Partial { 3.0f, 0.5f });   // all the same ratio
      PhasePlan p; const PhaseReport r = buildPlan (dup.data(), 64, opt, p);
      check (p.valid(), "degenerate: 64 partials sharing one ratio", "");
      check (r.crestAligned > 0.0, "degenerate: ...still has an aligned crest",
             fmt ("%.4f", r.crestAligned)); }
    { std::vector<Partial> huge;
      for (int k = 1; k <= 400; ++k) huge.push_back ({ (float) (k * 40), (float) (1.0 / k) });
      PhasePlan p; const PhaseReport r = buildPlan (huge.data(), 400, opt, p);
      check (p.valid(), "degenerate: nothing below ratio 40", "");
      check (r.workUsed <= r.workBudget, "degenerate: ...within budget", ""); }
}

//==============================================================================
// 14. THE LONG-WINDOW CAVEAT, pinned so it cannot be quietly forgotten.
//
// On an INHARMONIC spectrum the improvement is real over the analysis window and
// WASHES OUT beyond it: once the measurement runs past the fastest beat, every
// phase set explores the same envelope and converges on the same crest. This test
// documents that, and - the important half - shows it is a property of the METRIC
// and not of the engine, by running five independent RANDOM draws through the same
// measurement and watching them do exactly the same thing.
static void testLongWindowWander()
{
    const Ref& r = refByName ("scala 19EDO x6");
    const int n = (int) r.t.partials.size();
    Options opt; PhasePlan plan;
    const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
    const int p = std::min (86, rep.analysed);
    std::vector<double> hp ((size_t) n);
    hashPhases (hp.data(), n);

    if (tables) printf ("\n-- long-window wander, 19-EDO x6 prefix %d, window multiples of %.0f --\n"
                        "%-10s %8s %8s %8s\n", p, rep.verifyWindow, "set", "1x", "4x", "16x");
    auto gapAt = [&] (const double* ph, double mult)
    {
        std::vector<ref::Stats> s;
        ref::crestMulti (r.t.partials.data(), p, { hp.data(), ph }, rep.verifyWindow * mult,
                         mult <= 2 ? 12 : 5, s);
        return ref::dB (s[0].crest, s[1].crest);
    };
    const double planAt1  = gapAt (plan.phasesFor (p), 1.0);
    const double planAt16 = gapAt (plan.phasesFor (p), 16.0);
    if (tables) printf ("%-10s %8.2f %8.2f %8.2f\n", "plan", planAt1, gapAt (plan.phasesFor (p), 4.0), planAt16);

    double worstRandom16 = 0.0;
    for (int s = 0; s < 5; ++s)
    {
        std::vector<double> rp ((size_t) n);
        detail::closedRandom (n, (uint64_t) (1000 + s), rp.data());
        const double at16 = gapAt (rp.data(), 16.0);
        worstRandom16 = std::max (worstRandom16, std::fabs (at16));
        if (tables) printf ("random%-4d %8.2f %8.2f %8.2f\n", s, gapAt (rp.data(), 1.0),
                            gapAt (rp.data(), 4.0), at16);
    }

    // v2 asserted `planAt1 > 1.0` here - "the improvement is real over the analysis
    // window" - and that assertion is precisely the refuted claim: it is scored over
    // the engine's own window and says nothing about a longer note. This version
    // returns the hash on this template, so the honest assertion is that the gap is
    // EXACTLY zero, at every window multiple, because the phases are the same bits.
    check (rep.improvedPrefixes > 0 ? planAt1 > 1.0 : planAt1 == 0.0,
           "long window: either the plan improved this prefix over the window, or it is "
           "the hash bit for bit and the gap is exactly zero",
           fmt ("%+.2f dB at 1x, improvedPrefixes %.0f", planAt1, (double) rep.improvedPrefixes));
    check (std::fabs (planAt16) <= worstRandom16 + 0.5,
           "long window: past the beat the plan wanders no more than a random draw does "
           "- it is the metric that washes out, not the engine",
           fmt ("plan %+.2f dB at 16x, worst random draw %.2f dB", planAt16, worstRandom16));

    // On a HARMONIC spectrum there is no wander at all: the waveform is periodic,
    // so the guarantee is absolute at every window length.
    {
        const Ref& h = refByName ("harmonic s1 n=128");
        const int hn = (int) h.t.partials.size();
        Options o; PhasePlan hp2;
        buildPlan (h.t.partials.data(), hn, o, hp2);
        std::vector<double> hh ((size_t) hn);
        hashPhases (hh.data(), hn);
        double g1 = 0, g16 = 0;
        {
            std::vector<ref::Stats> s;
            ref::crestMulti (h.t.partials.data(), hn, { hh.data(), hp2.phasesFor (hn) }, 1.0, 16, s);
            g1 = ref::dB (s[0].crest, s[1].crest);
            ref::crestMulti (h.t.partials.data(), hn, { hh.data(), hp2.phasesFor (hn) }, 16.0, 8, s);
            g16 = ref::dB (s[0].crest, s[1].crest);
        }
        if (tables) printf ("harmonic s1 n=128: %+.2f dB at 1x, %+.2f dB at 16x\n", g1, g16);
        check (std::fabs (g1 - g16) < 0.2,
               "long window: on a harmonic spectrum the gain does not move at all",
               fmt ("%+.2f dB at 1x, %+.2f dB at 16x", g1, g16));
    }
}

//==============================================================================
// 15. THE ENGINE'S OWN GUARANTEE, restated as an assertion: for every prefix the
// plan marks improved, the challenger really is better ON THE INDEPENDENT RIG.
// This is the whole contract, checked prefix by prefix rather than at the ends.
static void testGuaranteeAtEveryPrefix()
{
    for (const char* name : { "harmonic s1 n=128", "prime-limit 7", "scala 22EDO x5",
                              "fractal L3 maxP384" })
    {
        const Ref& r = refByName (name);
        const int n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);
        int checkedPrefixes = 0, bad = 0; double worst = 0.0; int worstP = 0;
        // Every prefix would be thousands of syntheses; walk a dense ladder instead,
        // including the two ends and every power-of-two step between them.
        for (int p = 1; p <= rep.analysed; p = (p < 16 ? p + 1 : p + p / 8))
        {
            if (! plan.improved (p)) continue;
            std::vector<ref::Stats> s;
            ref::crestMulti (r.t.partials.data(), p, { hp.data(), plan.phasesFor (p) },
                             rep.verifyWindow, 12, s);
            const double d = ref::dB (s[0].crest, s[1].crest);
            ++checkedPrefixes;
            if (d < -0.02) { ++bad; }
            if (d < worst) { worst = d; worstP = p; }
        }
        // On an aperiodic template this engine improves NOTHING by design, so there
        // may be nothing to check - and that is the correct answer, not a hole in the
        // suite. What must never happen is `improvedPrefixes > 0` with nothing to
        // check, so the two are cross-examined against each other.
        check ((checkedPrefixes > 0) == (rep.improvedPrefixes > 0),
               std::string ("guarantee: the report's improvedPrefixes and the prefixes the "
                            "plan actually marks agree: ") + name,
               fmt ("%.0f found on the ladder, report says %.0f",
                    (double) checkedPrefixes, (double) rep.improvedPrefixes));
        if (rep.exactlyPeriodic)
            check (checkedPrefixes > 4,
                   std::string ("guarantee: on an exactly-periodic template there ARE improved "
                                "prefixes to check: ") + name,
                   fmt ("%.0f", (double) checkedPrefixes));
        check (bad == 0,
               std::string ("guarantee: every improved prefix really is better: ") + name,
               fmt ("%.0f of %.0f failed, worst %+.2f dB at prefix %.0f",
                    (double) bad, (double) checkedPrefixes, worst, (double) worstP));
    }
}

//==============================================================================
// 16. SCATTER 0 IS SACRED. Every phase exactly kAlignedPhase, on every path, on
// every spectrum, bit for bit - not "within 1e-12".
//
// A skeptic found version 2 breaking this: interpolatePhases read `target` before
// it looked at `scatter`, so an invalid plan at scatter 0 returned whatever the
// caller's buffer already held instead of today's Aligned mode. Every existing
// patch that sits at Aligned would have changed, and the change would have been
// different on every note. The paths below are ALL of them: the convenience
// entry point, the note-on entry point, the interpolator directly, an invalid
// plan, a null target, and a prefix longer than the plan.
static void testScatterZeroIsSacred()
{
    auto allAligned = [&] (const std::vector<double>& v, const std::string& what)
    {
        int bad = 0; double worst = 0.0;
        for (double x : v)
            if (x != kAlignedPhase) { ++bad; worst = std::max (worst, std::fabs (x - kAlignedPhase)); }
        // == , not a tolerance. A phase that is 0.25 + 1e-16 is a different double and
        // would render a different sample; "close to Aligned" is not Aligned.
        check (bad == 0, std::string ("scatter 0 is sacred: ") + what,
               fmt ("%.0f of %.0f entries differ, worst |delta| %.3e",
                    (double) bad, (double) v.size(), worst));
    };

    for (auto& r : referenceSpectra())
    {
        const int n = (int) r.t.partials.size();
        Options opt;

        // (a) buildPhases, the one-shot path.
        {
            std::vector<double> out ((size_t) n, -7.0);
            const PhaseReport rep = buildPhases (r.t.partials.data(), n, 0.0, opt, out.data());
            allAligned (out, std::string ("buildPhases(scatter=0): ") + r.name);
            check (rep.outcome == Outcome::Aligned && rep.measured,
                   std::string ("scatter 0 is sacred: ...and says Aligned, not a fallback: ") + r.name,
                   fmt ("outcome %.0f", (double) rep.outcome));
            check (rep.crestAligned > 0.0 && rep.crestChosen == rep.crestAligned,
                   std::string ("scatter 0 is sacred: ...and reports the exact aligned crest: ")
                   + r.name, fmt ("%.4f", rep.crestAligned));
        }

        // (b) seedFromPlan, the note-on path, at several rendered prefixes including
        // one PAST the plan's n (a Shepard copy can legitimately ask for that).
        {
            PhasePlan plan;
            buildPlan (r.t.partials.data(), n, opt, plan);
            for (int p : { 1, 7, n / 3, n, n + 64 })
            {
                if (p <= 0) continue;
                std::vector<double> out ((size_t) p, -7.0);
                seedFromPlan (plan, RenderedPrefix { p }, FillSpan { p }, 0.0, out.data());
                allAligned (out, std::string ("seedFromPlan(prefix=") + std::to_string (p)
                                 + ", scatter=0): " + r.name);
            }
        }
    }

    // (c) THE PATH THE SKEPTIC BROKE: an INVALID plan and a NULL target. Neither may
    // leave one byte of the caller's buffer holding what the last note left there.
    {
        PhasePlan dead;                       // default-constructed: valid() == false
        std::vector<double> out (64, -7.0);
        seedFromPlan (dead, RenderedPrefix { 64 }, FillSpan { 64 }, 0.0, out.data());
        allAligned (out, "seedFromPlan with an INVALID plan");

        std::vector<double> out2 (64, -7.0);
        interpolatePhases (nullptr, 64, 0.0, out2.data());
        allAligned (out2, "interpolatePhases with a NULL target");

        // And at a NON-zero scatter a null target still may not leave the buffer
        // untouched - it falls back to Aligned, which is a defined sound.
        std::vector<double> out3 (64, -7.0);
        interpolatePhases (nullptr, 64, 0.73, out3.data());
        allAligned (out3, "interpolatePhases with a NULL target at scatter 0.73");
    }

    // (d) A NEGATIVE scatter - what an unclamped modulated parameter hands over -
    // is Aligned too, not an extrapolation past it.
    {
        const Ref& r = refByName ("default n=1024");
        const int n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        buildPlan (r.t.partials.data(), n, opt, plan);
        std::vector<double> out ((size_t) n, -7.0);
        seedFromPlan (plan, RenderedPrefix { n }, FillSpan { n }, -0.4, out.data());
        allAligned (out, "seedFromPlan at a NEGATIVE scatter");
    }
}

//==============================================================================
// 17. THE SPAN LADDER'S FOUNDATION: on an exactly-periodic template, span 1 is
// span 0. Not "close to", not "within the margin" - THE SAME BITS.
//
// This is what makes "one span is every note length" a proof rather than an
// economy, and it is the only reason the search is allowed to keep its win on
// harmonic templates. If it ever stopped holding, the harmonic half of the
// guarantee would be scoped to a window again - Failure 4, one class over.
//
// Written so version 2 fails it: v2 had no periodicity test and no span offsets at
// all, so `exactlyPeriodic` does not exist there and neither does the identity this
// asserts.
static void testSpanLadderExactness()
{
    int periodicSeen = 0, aperiodicSeen = 0;
    for (auto& r : referenceSpectra())
    {
        const int n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
        if (! rep.measured) continue;

        // The classification must be a property of the RATIOS, recomputed here from
        // the template rather than read back from the report.
        bool allInteger = true;
        for (int i = 0; i < rep.analysed; ++i)
        {
            const double x = (double) r.t.partials[(size_t) i].ratio * rep.verifyWindow;
            if (std::fabs (x - std::floor (x + 0.5)) > 1.0e-9) { allInteger = false; break; }
        }
        check (allInteger == rep.exactlyPeriodic,
               std::string ("span ladder: the periodicity CLASS is what the ratios say, "
                            "independently recomputed: ") + r.name,
               fmt ("ratios say %.0f, report says %.0f, window %.0f",
                    (double) allInteger, (double) rep.exactlyPeriodic, rep.verifyWindow));

        if (rep.exactlyPeriodic)
        {
            ++periodicSeen;
            check (rep.spans == 1,
                   std::string ("span ladder: an exactly-periodic template needs ONE span: ")
                   + r.name, fmt ("%.0f", (double) rep.spans));
            check (rep.verifiedSpanPeriods == kUnboundedSpan,
                   std::string ("span ladder: ...and its reach is reported as unbounded, not "
                                "as a finite number that would be a lie: ") + r.name, "");

            // THE IDENTITY, on the independent rig: the waveform over the SECOND
            // window is the same waveform as over the first, sample for sample, at
            // the phases the plan returns. Checked at the sample instants the rig
            // chooses, which the engine had no hand in.
            const int p = std::min (rep.analysed, 96);
            const double* ph = plan.phasesFor (p);
            const double W = rep.verifyWindow;
            double worst = 0.0;
            for (int k = 0; k <= 64; ++k)
            {
                const double t = W * (double) k / 64.0;
                double a = 0.0, b = 0.0;
                for (int i = 0; i < p; ++i)
                {
                    const double rr = (double) r.t.partials[(size_t) i].ratio;
                    const double aa = (double) r.t.partials[(size_t) i].amp;
                    a += aa * std::sin (6.283185307179586476925286766559 * (ph[i] + rr * t));
                    b += aa * std::sin (6.283185307179586476925286766559 * (ph[i] + rr * (t + W)));
                }
                worst = std::max (worst, std::fabs (a - b));
            }
            check (worst < 1.0e-9,
                   std::string ("span ladder: span 1 IS span 0 on an exactly-periodic "
                                "template - this is what makes one span a proof: ") + r.name,
                   fmt ("worst |x(t) - x(t+W)| = %.3e over 65 instants", worst));
        }
        else
        {
            ++aperiodicSeen;
            check (rep.spans >= kMinSpansAperiodic,
                   std::string ("span ladder: an aperiodic template gets the full ladder or "
                                "the hash - a SHORT ladder is the refuted claim: ") + r.name,
                   fmt ("%.0f spans, minimum %.0f", (double) rep.spans, (double) kMinSpansAperiodic));
            check (rep.verifiedSpanPeriods >= (double) kLadderReachPeriods,
                   std::string ("span ladder: ...and it reaches kLadderReachPeriods: ") + r.name,
                   fmt ("%.0f periods, asked for %.0f",
                        rep.verifiedSpanPeriods, (double) kLadderReachPeriods));
        }
    }
    check (periodicSeen > 3 && aperiodicSeen > 2,
           "span ladder: the reference set contains both classes, so neither branch is "
           "untested", fmt ("%.0f periodic, %.0f aperiodic",
                            (double) periodicSeen, (double) aperiodicSeen));

    // The measurement knob must ship OFF. spansOverride == 1 reproduces version 2's
    // one-window claim exactly, and it exists so the regression can be MEASURED
    // rather than described - never so it can be shipped.
    { Options d; check (d.spansOverride == 0,
                        "span ladder: spansOverride ships at 0 - it is a measurement knob",
                        fmt ("%.0f", (double) d.spansOverride)); }
}

//==============================================================================
// 18. FAILURE 4, AS AN ASSERTION: A HELD NOTE MANY TIMES THE ANALYSIS WINDOW.
//
// This is the test version 2 fails. v2 measured over ONE window and returned a set
// it called improved; the skeptics' rig found it 1.15 dB WORSE than today's hash on
// a 1.0 s note on a 53-EDO timbre whose window is 581 ms, and beaten by 238 of 256
// random draws. A held note is not obliged to be shorter than a window we chose.
//
// Scored on the independent rig at 1x, 4x, 16x and 64x the engine's own window, on
// every aperiodic reference spectrum, at the prefix the keyboard actually renders.
//
// The second half is what makes this a REGRESSION test and not just a green tick:
// the same measurement is repeated with Options::spansOverride = 1 and
// optimiseAperiodic = true, which IS version 2, and that configuration is asserted
// to be caught - i.e. this test would go red if the engine went back to one window.
static void testHeldNoteNeverWorse()
{
    struct Score { int claimed = 0, worse = 0; double worst = 0.0; int worstMult = 0; };

    auto scoreAt = [&] (const Ref& r, const Options& opt) -> Score
    {
        Score sc;
        const int n = (int) r.t.partials.size();
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
        if (! rep.measured || rep.verifyWindow <= 0.0) return sc;
        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);
        for (int m = 24; m <= 108; m += 6)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
            if (p <= 1 || ! plan.improved (p)) continue;
            ++sc.claimed;
            for (int mult : { 1, 4, 16, 64 })
            {
                std::vector<ref::Stats> s;
                ref::crestMulti (r.t.partials.data(), p, { hp.data(), plan.phasesFor (p) },
                                 rep.verifyWindow * (double) mult,
                                 mult <= 4 ? 8 : 3, s);
                const double d = ref::dB (s[0].crest, s[1].crest);
                if (d < -0.05) { ++sc.worse; }
                if (d < sc.worst) { sc.worst = d; sc.worstMult = mult; }
            }
        }
        return sc;
    };

    if (tables) printf ("\n-- held note, many times the analysis window, APERIODIC templates --\n"
                        "%-20s | %-30s | %-30s\n", "spectrum",
                        "THIS ENGINE (8-span ladder)", "ONE WINDOW (what v2 did)");

    int shippedWorse = 0, v2Worse = 0, v2Claimed = 0, shippedClaimed = 0;
    double v2WorstDb = 0.0;
    for (auto& r : referenceSpectra())
    {
        Options probe; PhasePlan pp;
        const PhaseReport pr = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), probe, pp);
        if (pr.exactlyPeriodic) continue;      // the periodic class is testSpanLadderExactness

        Options shipped;                        // exactly what an integrator gets
        Options oneWindow;                      // exactly what version 2 was
        oneWindow.spansOverride     = 1;
        oneWindow.optimiseAperiodic = true;

        const Score a = scoreAt (r, shipped);
        const Score b = scoreAt (r, oneWindow);
        shippedWorse += a.worse; shippedClaimed += a.claimed;
        v2Worse += b.worse; v2Claimed += b.claimed;
        v2WorstDb = std::min (v2WorstDb, b.worst);

        if (tables)
            printf ("%-20s | claimed %3d, worse %3d, worst %+6.2f dB | "
                    "claimed %3d, worse %3d, worst %+6.2f dB\n",
                    r.name, a.claimed, a.worse, a.worst, b.claimed, b.worse, b.worst);

        check (a.worse == 0,
               std::string ("held note: no note the plan CLAIMS is improved is worse than "
                            "today's hash at 1x/4x/16x/64x the window: ") + r.name,
               fmt ("%.0f readings worse of %.0f notes claimed, worst %+.2f dB at %.0fx",
                    (double) a.worse, (double) a.claimed, a.worst, (double) a.worstMult));
    }

    check (shippedWorse == 0,
           "held note: ZERO readings worse than the hash across every aperiodic reference "
           "spectrum, at every multiple of the window",
           fmt ("%.0f worse, %.0f notes claimed improved",
                (double) shippedWorse, (double) shippedClaimed));

    // THE SABOTAGE, BUILT IN. One window is what version 2 shipped; if the engine
    // ever goes back to it, this is what a listener gets, and the number below is the
    // reason kSpansAperiodic is 8 and not 1.
    check (v2Claimed > 0,
           "held note: the one-window configuration does claim improvements - so the "
           "comparison below is between two engines that both claim something",
           fmt ("%.0f notes claimed", (double) v2Claimed));
    check (v2Worse > 0,
           "held note: ...and the one-window configuration IS caught by this test, which "
           "is what makes the 8-span result above a result and not a tautology",
           fmt ("%.0f readings worse, worst %+.2f dB", (double) v2Worse, v2WorstDb));
}

//==============================================================================
// 19. HOW MUCH IS THE CHOOSER AND HOW MUCH IS THE SEARCH?
//
// The user's question, answered by measurement rather than by architecture talk:
// three closed forms chosen per prefix (the CHOOSER) against the greedy construction
// (the SEARCH). Both are scored on the independent rig at the prefixes the keyboard
// renders.
//
// This also pins the claim the header makes about Schroeder: it is NOT a reporting
// knob. On a flat spectrum it wins at some prefixes and is far worse at others, so
// it only pays as a per-prefix candidate - which is what makes the chooser a chooser.
static void testChooserAlone()
{
    if (tables) printf ("\n-- chooser vs search, MIDI 21..108 on the independent rig --\n"
                        "%-20s | %-24s | %-24s\n", "spectrum",
                        "CHOOSER ALONE (3 closed)", "SHIPPED (chooser + search)");

    int chooserWins = 0, chooserWorseTotal = 0, shippedWorseTotal = 0;
    int chooserPrefixes = 0, shippedPrefixes = 0, schroederPrefixesShipped = 0;
    for (const char* name : { "odd maxP512", "FLAT n=512", "default maxP128 s0.3",
                              "fractal L4", "default n=1024", "scala 19EDO x6" })
    {
        const Ref& r = refByName (name);
        const int n = (int) r.t.partials.size();
        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);

        auto sweep = [&] (const Options& opt, int& improvedNotes, double& mean, int& worse)
        {
            PhasePlan plan;
            const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
            improvedNotes = 0; worse = 0; mean = 0.0; int notes = 0;
            for (int m = 21; m <= 108; m += 3)
            {
                const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
                const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
                if (p <= 1) continue;
                ++notes;
                if (plan.improved (p)) ++improvedNotes;
                std::vector<ref::Stats> s;
                ref::crestMulti (r.t.partials.data(), p, { hp.data(), plan.phasesFor (p) },
                                 blindWindowPeriods (r.t.partials.data(), p), 10, s);
                const double d = ref::dB (s[0].crest, s[1].crest);
                mean += d;
                if (d < -0.02) ++worse;
            }
            if (notes > 0) mean /= notes;
            return rep;
        };

        Options chooser; chooser.useConstruction = false;
        Options shipped;
        int ci = 0, si = 0, cw = 0, sw = 0; double cm = 0.0, sm = 0.0;
        const PhaseReport cr = sweep (chooser, ci, cm, cw);
        const PhaseReport sr = sweep (shipped, si, sm, sw);
        chooserWorseTotal += cw; shippedWorseTotal += sw;
        chooserPrefixes += cr.improvedPrefixes;
        shippedPrefixes += sr.improvedPrefixes;
        schroederPrefixesShipped += sr.prefixesSchroeder1 + sr.prefixesSchroederHalf;
        if (cr.improvedPrefixes > 0) ++chooserWins;

        if (tables)
            printf ("%-20s | %3d/30 notes %3d prefixes %+6.2f dB | %3d/30 notes %3d prefixes "
                    "%+6.2f dB   [shipped: Schr1 %d, Schr0.5 %d, constructed %d]\n",
                    name, ci, cr.improvedPrefixes, cm, si, sr.improvedPrefixes, sm,
                    sr.prefixesSchroeder1, sr.prefixesSchroederHalf, sr.prefixesConstructed);

        check (cw == 0,
               std::string ("chooser: the closed forms ALONE never make a note worse: ") + name,
               fmt ("%.0f notes worse, mean %+.2f dB", (double) cw, cm));
        check (sw == 0,
               std::string ("chooser: and neither does the shipped combination: ") + name,
               fmt ("%.0f notes worse, mean %+.2f dB", (double) sw, sm));
        check (sm >= cm - 1.0e-9,
               std::string ("chooser: adding the search never costs quality - it is one more "
                            "candidate under the same gate, never a replacement: ") + name,
               fmt ("chooser %+.3f dB, shipped %+.3f dB", cm, sm));
    }

    check (chooserWins >= 3,
           "chooser: the closed forms ALONE beat the hash on several reference spectra - "
           "the guaranteed floor is a real floor, not an empty one",
           fmt ("%.0f of 6 spectra, %.0f prefixes in total",
                (double) chooserWins, (double) chooserPrefixes));
    check (chooserWorseTotal == 0 && shippedWorseTotal == 0,
           "chooser: neither configuration is ever worse than today's hash",
           fmt ("chooser %.0f, shipped %.0f", (double) chooserWorseTotal, (double) shippedWorseTotal));
    // THE HONEST SPLIT, and it overrules the architecture note this engine was asked
    // to evaluate. "Choosing per spectrum is most of the available win" is FALSE on
    // this reference set: the closed forms alone reach a few dozen to a couple of
    // hundred prefixes, the search reaches nearly every one of them. The chooser is
    // the floor that cannot be wrong; the search is the win.
    printf ("   chooser alone %d improved prefixes; shipped %d; of the shipped ones %d are "
            "still won by a Schroeder form.\n",
            chooserPrefixes, shippedPrefixes, schroederPrefixesShipped);
    check (shippedPrefixes > 2 * chooserPrefixes,
           "chooser: ...and the SEARCH is where most of the win is - the closed forms are "
           "the floor, not the answer",
           fmt ("%.0f prefixes from the chooser, %.0f shipped",
                (double) chooserPrefixes, (double) shippedPrefixes));
    check (schroederPrefixesShipped > 0,
           "chooser: ...yet Schroeder still wins prefixes the search does not, so keeping "
           "all three closed forms is paid for",
           fmt ("%.0f prefixes", (double) schroederPrefixesShipped));
}

//==============================================================================
// 20. WHY THE SEARCH IS OFF ON APERIODIC TEMPLATES, asserted rather than asserted-at.
//
// Options::optimiseAperiodic exists so this can be measured instead of believed.
// Forcing the construction on for a Scala timbre must not produce a plan that claims
// improvements - because over the span ladder its sets do not survive, and a set
// that does not survive the ladder is exactly the thing that shipped in version 2.
static void testAperiodicOptimiser()
{
    if (tables) printf ("\n-- the construction, forced on for an aperiodic template --\n"
                        "%-20s %6s %6s | %8s %8s %8s %8s\n", "spectrum", "anly", "built",
                        "improved", "byLadder", "byMargin", "bySweep");
    int totalBuilt = 0, totalImproved = 0, totalLadderRefusals = 0;
    for (const char* name : { "scala 22EDO x5", "scala 19EDO x6", "scala 31EDO x4",
                              "scala 13ED3 x3" })
    {
        const Ref& r = refByName (name);
        Options opt; opt.optimiseAperiodic = true;
        PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), (int) r.t.partials.size(), opt, plan);
        totalBuilt += rep.built; totalImproved += rep.improvedPrefixes;
        totalLadderRefusals += rep.refusedByLadder;
        if (tables) printf ("%-20s %6d %6d | %8d %8d %8d %8d\n", name, rep.analysed, rep.built,
                            rep.improvedPrefixes, rep.refusedByLadder, rep.refusedByMargin,
                            rep.refusedBySweep);
        check (rep.built > 0,
               std::string ("aperiodic optimiser: forcing it on really does run a search: ")
               + name, fmt ("built %.0f of analysed %.0f", (double) rep.built, (double) rep.analysed));
        check (rep.improvedPrefixes == 0,
               std::string ("aperiodic optimiser: and NOTHING it builds survives the span "
                            "ladder - which is why it ships off: ") + name,
               fmt ("%.0f prefixes claimed improved", (double) rep.improvedPrefixes));
    }
    check (totalBuilt > 100 && totalImproved == 0,
           "aperiodic optimiser: a real search ran on every Scala family and won nothing "
           "that holds over a long note",
           fmt ("%.0f partials shaped in total, %.0f prefixes improved",
                (double) totalBuilt, (double) totalImproved));
    check (totalLadderRefusals > 0,
           "aperiodic optimiser: ...and the LADDER is what refused them, not the margin - "
           "so the sets really did win over one window and lose over a long note",
           fmt ("%.0f prefixes refused by the ladder", (double) totalLadderRefusals));

    // The default must stay off. This is a shipping decision backed by the table above.
    { Options d; check (! d.optimiseAperiodic,
                        "aperiodic optimiser: ships OFF", ""); }
}

//==============================================================================
// 21. THE CONSUMER-SIDE TRAP THE INTEGRATION NOTES NAME, measured here so the note
// is not a guess. AdditiveVoice::renderNextBlock selects its NORMALISATION LAW from
// the same phaseMode switch this fader replaces:
//
//     normGain = aligned ? min(rmsN, peakN) : rmsN
//     rmsN  = 0.25 / sqrt(sum of squared amplitudes)
//     peakN = 0.5  / (sum of amplitudes)
//
// So the two ends of the fader are not only two phase sets, they are two GAINS, and
// a continuous fader that blends the phases while stepping the gain would jump at
// u = 0. This test measures the gap on every reference spectrum, at the prefixes the
// keyboard renders, so the integration note quotes a number this suite produced.
//
// It is pure arithmetic on the compiled amplitudes - no synthesis, nothing
// machine-dependent - so it is safe to assert on.
static void testNormalisationLawGap()
{
    if (tables) printf ("\n-- the normalisation law phaseMode ALSO selects (AdditiveVoice) --\n"
                        "%-24s %6s | %10s %10s | %s\n", "spectrum", "prefix",
                        "rmsN", "peakN", "dB the fader must blend");
    double worstGap = 0.0; std::string worstWhere;
    int gapsSeen = 0;
    for (auto& r : referenceSpectra())
    {
        const int n = (int) r.t.partials.size();
        double showGap = 0.0; int showP = 0;
        for (int m = 21; m <= 108; m += 3)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (r.t.partials.data(), n, f0, 48000.0);
            if (p <= 0) continue;
            double sum = 0.0, sumSq = 0.0;
            for (int i = 0; i < p; ++i)
            {
                const double a = (double) r.t.partials[(size_t) i].amp;
                sum += a; sumSq += a * a;
            }
            if (sum <= 1.0e-9 || sumSq <= 1.0e-12) continue;
            const double rmsN  = 0.25 / std::sqrt (sumSq);
            const double peakN = 0.5 / sum;
            // Aligned takes the smaller; Scattered takes rmsN. The gap is what the
            // voice's gain steps by when phaseMode flips, at this pitch.
            const double gapDb = 20.0 * std::log10 (rmsN / std::min (rmsN, peakN));
            if (gapDb > 1.0e-9) ++gapsSeen;
            if (gapDb > showGap) { showGap = gapDb; showP = p; }
            if (gapDb > worstGap) { worstGap = gapDb; worstWhere = r.name; }
        }
        if (tables)
        {
            double sum = 0.0, sumSq = 0.0;
            for (int i = 0; i < std::max (1, showP); ++i)
            { const double a = (double) r.t.partials[(size_t) i].amp; sum += a; sumSq += a * a; }
            printf ("%-24s %6d | %10.6f %10.6f | %+.2f dB\n", r.name, showP,
                    sumSq > 0 ? 0.25 / std::sqrt (sumSq) : 0.0,
                    sum > 0 ? 0.5 / sum : 0.0, showGap);
        }
    }
    printf ("   normalisation law gap: worst %.2f dB (%s). The fader MUST blend this by the "
            "same control, or u=0 steps by it.\n", worstGap, worstWhere.c_str());
    check (gapsSeen > 0 && worstGap > 6.0,
           "consumer trap: the phaseMode-selected normalisation law really is a large, "
           "real gain difference - so a fader that blends only the phases is broken",
           fmt ("worst %.2f dB on %.0f pitch/spectrum pairs", worstGap, (double) gapsSeen));
    check (worstGap < 40.0,
           "consumer trap: ...and the gap is bounded, so blending it is a finite job",
           fmt ("%.2f dB", worstGap));
}

//==============================================================================
// 22. THE ONE PLACE THE PERIODIC GUARANTEE STOPS, found by the boundary sweep and
// pinned here so it cannot be rediscovered as a surprise.
//
// An exactly-periodic template's guarantee is over ONE full period of the CONTINUOUS
// waveform, which is the shortest complete observation that exists - there is no
// shorter window to be scoped to. But a real 48 kHz render at a low pitch samples
// that period at a NON-INTEGER rate (65.406 Hz is 733.9 samples per period), so a
// note only one or two periods long sees a sparse, shifting subset of the waveform
// and its SAMPLED peak has not converged on the continuous one. MEASURED, FLAT n=512
// at MIDI 36 on the skeptics' real-render rig:
//
//   note length   periods   plan's sampled peak vs the hash's
//     0.0153 s      1.00          +0.22 dB   (the plan is worse)
//     0.0306 s      2.00          +0.26 dB
//     0.0500 s      3.27          -0.82 dB   (the plan is better)
//     0.2500 s     16.35          -1.73 dB
//     1.0000 s     65.41          -1.74 dB
//
// One note of thirty, at one length, on one spectrum. It is NOT the disease this
// file exists to cure - it is not a window we chose, it is the sampling of the
// shortest window that exists - but it is the honest boundary of the claim, and the
// test below is what stops it being quietly widened.
static void testShortNoteSampling()
{
    const Ref& r = refByName ("FLAT n=512");
    const int n = (int) r.t.partials.size();
    Options opt; PhasePlan plan;
    const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
    std::vector<double> hp ((size_t) n);
    hashPhases (hp.data(), n);

    check (rep.exactlyPeriodic,
           "short note: the spectrum this is measured on is exactly periodic - so the "
           "engine's own guarantee is over one whole period and nothing shorter", "");

    // On the CONTINUOUS waveform - which is what the engine measures and what
    // ref::crest samples densely - the plan wins at one period and keeps winning.
    int bad = 0; double worst = 0.0; int worstM = 0;
    for (int m = 21; m <= 108; m += 3)
    {
        const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
        const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
        if (p <= 1 || ! plan.improved (p)) continue;
        std::vector<ref::Stats> s;
        ref::crestMulti (r.t.partials.data(), p, { hp.data(), plan.phasesFor (p) }, 1.0, 24, s);
        const double d = ref::dB (s[0].crest, s[1].crest);
        if (d < -0.02) ++bad;
        if (d < worst) { worst = d; worstM = m; }
    }
    check (bad == 0,
           "short note: over ONE densely sampled period the plan is better at every pitch "
           "- the guarantee itself is intact",
           fmt ("%.0f pitches worse, worst %+.2f dB at MIDI %.0f",
                (double) bad, worst, (double) worstM));

    // And the boundary: at a real 48 kHz sample grid, a one-period note at MIDI 36
    // is 733.9 samples and the sampled peak has not converged. This is a MEASUREMENT
    // of the rig, pinned, not a demand on the engine - so it asserts the SHAPE
    // (it converges as the note lengthens), not a millisecond number.
    {
        const double f0 = 440.0 * std::pow (2.0, (36 - 69) / 12.0);
        const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
        auto sampledPeak = [&] (const double* ph, double durSec)
        {
            const long long N = (long long) (durSec * opt.sampleRate);
            double pk = 0.0;
            for (long long sIdx = 0; sIdx < N; ++sIdx)
            {
                double acc = 0.0;
                for (int i = 0; i < p; ++i)
                {
                    const double inc = (double) r.t.partials[(size_t) i].ratio * f0 / opt.sampleRate;
                    acc += (double) r.t.partials[(size_t) i].amp
                           * std::sin (6.283185307179586476925286766559
                                       * (inc * (double) sIdx + ph[i]));
                }
                pk = std::max (pk, std::fabs (acc));
            }
            return pk;
        };
        const double one = 1.0 / f0, many = 16.0 / f0;
        const double h1 = sampledPeak (hp.data(), one),  g1 = sampledPeak (plan.phasesFor (p), one);
        const double h2 = sampledPeak (hp.data(), many), g2 = sampledPeak (plan.phasesFor (p), many);
        const double d1 = ref::dB (g1, h1), d2 = ref::dB (g2, h2);
        printf ("   short note, FLAT n=512 MIDI 36 on a real 48 kHz grid: plan peak vs hash "
                "%+.2f dB at 1 period, %+.2f dB at 16 periods.\n", d1, d2);
        check (d2 < -1.0,
               "short note: by sixteen periods the real-grid peak agrees with the "
               "continuous one and the plan is clearly better",
               fmt ("%+.2f dB", d2));
        check (d2 < d1,
               "short note: and the sampled reading CONVERGES as the note lengthens - the "
               "one-period disagreement is the sample grid, not the phases",
               fmt ("%+.2f dB at 1 period, %+.2f dB at 16", d1, d2));
    }
}

//==============================================================================
// 23. THE PRECONDITION, PROVED IN BOTH DIRECTIONS: THE AMPLITUDES buildPlan IS
// GIVEN MUST BE THE AMPLITUDES THE VOICE RENDERS.
//
// This is the same disease as the four refutations - a guarantee scoped to
// something that is not what the listener hears - in the one place it is the
// CALLER's to avoid, so it is asserted here rather than only described.
//
// Reachability, read off the plugin source, not imagined: Spectrum.h:137 says the
// per-partial gain vector is NEVER written by compileSpectrum, and
// AdditiveVoice.h:714-719 multiplies it in afterwards. So a pencil edit in the
// partial-gain editor changes what sounds and does not change what buildPlan saw.
// MEASURED by the skeptics' rig on a true continuous peak (2^20 samples over one
// exact period): FLAT n=512 at MIDI 99, one partial pulled to silence, the plan
// goes from +0.75 dB BETTER to 2.10 dB WORSE than today's hash.
//
// The second half is the remedy, and it is why this is a precondition and not a
// defect: rebuilding the plan on the EDITED amplitudes restores the guarantee
// exactly. The integration note tells the caller to do that; this asserts it works.
static void testAmplitudesAreAPrecondition()
{
    struct Case { const char* name; int midi; int silence; };
    const Case cases[] = { { "FLAT n=512", 99, 1 }, { "fractal L3 maxP384", 96, 1 },
                           { "default n=1024", 96, 2 } };
    if (tables) printf ("\n-- the amplitude precondition: a pencil edit the plan never saw --\n"
                        "%-20s %5s %5s | %10s %10s | %10s\n", "spectrum", "MIDI", "sounds",
                        "stale plan", "rebuilt", "hash");
    int staleWorse = 0, rebuiltWorse = 0;
    double worstStale = 0.0, worstRebuilt = 0.0;
    for (const Case& c : cases)
    {
        const Ref& r = refByName (c.name);
        const int n = (int) r.t.partials.size();
        Options opt;
        PhasePlan stale;
        buildPlan (r.t.partials.data(), n, opt, stale);

        const double f0 = 440.0 * std::pow (2.0, (c.midi - 69) / 12.0);
        const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
        if (p <= 2) continue;

        // THE EDIT: one partial pulled to silence. The partial keeps its OSCILLATOR
        // SLOT - AdditiveVoice multiplies the gain into the amplitude and never
        // reindexes the bank - so the edited list is the same p partials with one
        // amplitude at zero, NOT a shorter list. Getting that wrong makes the
        // comparison nonsense: the shipping hash is indexed by slot
        // (seedPhase takes the global oscillator index), so a reindexed list would be
        // scored against a different hash than the one the voice would actually play.
        std::vector<Partial> edited;
        std::vector<double>  stalePh, hashPh;
        std::vector<double>  fullHash ((size_t) n);
        hashPhases (fullHash.data(), n);
        const double* sp = stale.phasesFor (p);
        for (int i = 0; i < p; ++i)
        {
            Partial q = r.t.partials[(size_t) i];
            if (i == c.silence) q.amp = 0.0f;
            edited.push_back (q);
            stalePh.push_back (sp[i]);
            hashPh.push_back (fullHash[(size_t) i]);
        }
        const int en = (int) edited.size();

        // The remedy: a plan built on WHAT SOUNDS.
        PhasePlan fresh;
        const PhaseReport fr = buildPlan (edited.data(), en, opt, fresh);

        std::vector<ref::Stats> s;
        ref::crestMulti (edited.data(), en,
                         { hashPh.data(), stalePh.data(), fresh.phasesFor (en) },
                         blindWindowPeriods (edited.data(), en), 16, s);
        const double dStale   = ref::dB (s[0].crest, s[1].crest);
        const double dRebuilt = ref::dB (s[0].crest, s[2].crest);
        if (dStale   < -0.02) { ++staleWorse;   worstStale   = std::min (worstStale, dStale); }
        if (dRebuilt < -0.02) { ++rebuiltWorse; worstRebuilt = std::min (worstRebuilt, dRebuilt); }

        if (tables)
            printf ("%-20s %5d %5d | %+9.2f dB %+9.2f dB | crest %.4f  (rebuilt improved=%d)\n",
                    c.name, c.midi, en, dStale, dRebuilt, s[0].crest,
                    (int) fresh.improved (en));

        check (dRebuilt >= -0.02,
               std::string ("amplitude precondition: a plan REBUILT on what actually sounds is "
                            "never worse than the hash: ") + c.name,
               fmt ("%+.2f dB, %.0f of %.0f partials sounding, improvedPrefixes %.0f",
                    dRebuilt, (double) en, (double) p, (double) fr.improvedPrefixes));
    }

    // THE DEFECT ITSELF, asserted as a fact rather than left as a warning: a stale
    // plan really can be worse. If a future change made this stop happening the
    // integration note would be over-cautious, and that is worth knowing too - but
    // it must never be the case that the note is DROPPED while the risk stands.
    check (staleWorse > 0,
           "amplitude precondition: a STALE plan - built before the pencil edit - really is "
           "worse than the hash, which is why the integration note is not optional",
           fmt ("%.0f of 3 cases worse, worst %+.2f dB", (double) staleWorse, worstStale));
    check (rebuiltWorse == 0,
           "amplitude precondition: and rebuilding fixes every one of them",
           fmt ("%.0f cases still worse, worst %+.2f dB", (double) rebuiltWorse, worstRebuilt));

    //--------------------------------------------------------------------------
    // THE OTHER WAY THE AMPLITUDES DRIFT, and the one a rebuild cannot fix: SLOPE
    // MODULATION. PluginProcessor compiles the template at
    // slope = param + modulationSpan_LO and AdditiveVoice rescales every amplitude
    // by ratio^-dS per voice at render time, so dS is 0 exactly when Slope carries
    // NO modulation and grows to the width of the modulation span when it does.
    // A per-voice, continuously moving quantity cannot be met by rebuilding a cached
    // plan, so the honest thing is to measure how the win decays and publish it.
    //
    // The decay is printed on every run and asserted only in SHAPE: dS = 0 is the
    // best reading (it is what the plan was built for), and a large dS really can go
    // negative (which is why the integration note is not optional). No dB number
    // here is asserted - the numbers belong to the reference spectra, not to a
    // machine, but they are a property of this engine's choices and will move when
    // those change.
    {
        const Ref& r = refByName ("default n=1024");
        const int n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        buildPlan (r.t.partials.data(), n, opt, plan);
        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);
        const double dSs[5] = { 0.0, 0.25, 0.5, 1.0, 2.0 };
        double mean[5] = { 0, 0, 0, 0, 0 };
        int notes = 0, negativeAtLargeDs = 0;
        for (int m = 36; m <= 96; m += 6)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
            if (p <= 2 || ! plan.improved (p)) continue;
            ++notes;
            for (int d = 0; d < 5; ++d)
            {
                std::vector<Partial> tilted;
                for (int i = 0; i < p; ++i)
                {
                    Partial q = r.t.partials[(size_t) i];
                    q.amp = (float) ((double) q.amp
                            * std::pow (std::max (1.0e-9, (double) q.ratio), -dSs[d]));
                    tilted.push_back (q);
                }
                std::vector<ref::Stats> s;
                ref::crestMulti (tilted.data(), p, { hp.data(), plan.phasesFor (p) },
                                 blindWindowPeriods (tilted.data(), p), 10, s);
                const double g = ref::dB (s[0].crest, s[1].crest);
                mean[d] += g;
                if (d == 4 && g < -0.02) ++negativeAtLargeDs;
            }
        }
        for (int d = 0; d < 5; ++d) mean[d] /= std::max (1, notes);
        printf ("   slope drift, default n=1024, %d notes: dB vs hash at dS = 0 / .25 / .5 / 1 / 2"
                " = %+.2f / %+.2f / %+.2f / %+.2f / %+.2f\n",
                notes, mean[0], mean[1], mean[2], mean[3], mean[4]);
        check (notes > 4, "slope drift: there are improved notes to tilt",
               fmt ("%.0f", (double) notes));
        check (mean[0] > mean[1] && mean[1] > mean[2] && mean[2] > mean[3],
               "slope drift: the win DECAYS with the slope offset and is largest at dS = 0, "
               "which is what the plan was built for - it is a smooth decay, not a cliff",
               fmt ("%+.2f / %+.2f / %+.2f / %+.2f", mean[0], mean[1], mean[2], mean[3]));
        check (negativeAtLargeDs > 0,
               "slope drift: and at a large offset it really does go below the hash - so "
               "the amplitude precondition is a real one, not a precaution. (What to DO "
               "about it is testSlopeCompilePosition's business, and the answer is not "
               "the one an earlier revision of the header gave.)",
               fmt ("%.0f of %.0f notes worse at dS = 2", (double) negativeAtLargeDs, (double) notes));
    }
}

//==============================================================================
// 24. R1: WHERE THE PLAN IS COMPILED ALONG THE SLOPE MODULATION SPAN.
//
// An earlier revision of PhaseSets.h told the integrator to "compile the phase plan
// against the amplitudes at the CENTRE of the slope modulation span rather than its
// low end, which halves the worst |dS| and keeps the mean positive across the whole
// reachable range". That instruction was never measured. It is measured here, and it
// is withdrawn. This test exists so it cannot silently come back.
//
// The shipped path, read off the plugin:
//   PluginProcessor.cpp:574-580  compileSlope = param + modulationSpan_LO
//   ModEngine.h:317-318          `lo` accumulates ONLY negative ranges
//   AdditiveVoice.h:833-837      amp *= ratio^-(sVoice - tmpl->curveSlope)
// so compileSlope is the reachable FLOOR and dS >= 0 everywhere. Centring is the
// only rule in the design that makes dS negative.
//
// Everything below is scored with ref::crest on the true float ratios. Nothing is
// asserted as a dB number - the numbers belong to the reference spectra - but the
// ORDERINGS are asserted, and every one of them is what the header now says.
//
// AND THE COVERAGE THAT WAS MISSING. The version of this test that shipped with the
// previous revision swept [0.5, 2.5] and nothing lower. curveSlope is
// NormalisableRange(0.0, 4.0) default 1.0 (PluginProcessor.cpp:126-127) and the
// compile point is param + lo where ModEngine.h:317-318 sums only NEGATIVE ranges into
// lo, so ONE modulator at -0.25 normalised compiles the default patch at 0.0. That is
// an ordinary patch and the old sweep never reached it. The floor range is now swept
// beside the mid range, and where the two disagree the test says so out loud instead
// of asserting the mid range's answer twice.
static void testSlopeCompilePosition()
{
    struct Range { const char* nm; double lo, hi; };
    // [0.5, 2.5] is the mid range the previous revision swept. [0.0, 1.0] is the
    // skeptic's, and it is the one that inverts the family-level argument.
    const Range kRanges[] = { { "[0.5,2.5]", 0.5, 2.5 },
                              { "[0.0,1.0]", 0.0, 1.0 },
                              { "[0.0,4.0]", 0.0, 4.0 } };
    const int   kSlopes   = 5;

    struct Row { const char* name; const char* range; double lo;
                 double worstShipped, worstCentre;
                 int worseShipped, worseCentre, cases; };
    std::vector<Row> rows;

    if (tables) printf ("\n-- R1: compile at the reachable LOW END vs at the CENTRE --\n"
                        "%-20s %-10s | %-22s | %-22s\n", "spectrum", "reachable",
                        "SHIPPED (low end)", "CENTRE (not shipped)");

  for (const Range& rg : kRanges)
  {
    const double sLo = rg.lo, sHi = rg.hi, sCen = 0.5 * (sLo + sHi);
    for (const char* name : { "default n=1024", "fractal L3 maxP384", "odd maxP512" })
    {
        // The template is the SHIPPED one in both arms - compiled at the reachable
        // floor - so no partial goes missing and the only thing that differs is which
        // amplitudes buildPlan was optimising for. This is the withdrawn instruction
        // read as charitably as it can be read; recompiling the spectrum at the centre
        // is strictly worse still, and the partial counts asserted at the end say why.
        SpectrumSettings s = refByName (name).settings;
        s.curveSlope = (float) sLo;
        const SpectrumTemplate t = compileSpectrum (s);
        const int n = (int) t.partials.size();
        if (n < 2) continue;

        Options opt;
        PhasePlan planShipped, planCentre;
        buildPlan (t.partials.data(), n, opt, planShipped);
        {
            std::vector<Partial> centred (t.partials.begin(), t.partials.end());
            for (int i = 0; i < n; ++i)
                centred[(size_t) i].amp = (float) ((double) centred[(size_t) i].amp
                    * std::pow (std::max (1.0e-9, (double) centred[(size_t) i].ratio),
                                -(sCen - sLo)));
            buildPlan (centred.data(), n, opt, planCentre);
        }

        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);

        Row row { name, rg.nm, sLo, 1.0e30, 1.0e30, 0, 0, 0 };
        for (int m = 21; m <= 108; m += 2)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (t.partials.data(), n, f0, opt.sampleRate);
            if (p <= 2) continue;
            for (int k = 0; k < kSlopes; ++k)
            {
                const double sV = sLo + (sHi - sLo) * (double) k / (double) (kSlopes - 1);
                const double dS = sV - sLo;          // the SHIPPED dS: always >= 0
                std::vector<Partial> tilted;
                tilted.reserve ((size_t) p);
                for (int i = 0; i < p; ++i)
                {
                    Partial q = t.partials[(size_t) i];
                    q.amp = (float) ((double) q.amp
                            * std::pow (std::max (1.0e-9, (double) q.ratio), -dS));
                    tilted.push_back (q);
                }
                std::vector<ref::Stats> st;
                ref::crestMulti (tilted.data(), p,
                                 { hp.data(), planShipped.phasesFor (p), planCentre.phasesFor (p) },
                                 blindWindowPeriods (tilted.data(), p), 10, st);
                const double gS = ref::dB (st[0].crest, st[1].crest);
                const double gC = ref::dB (st[0].crest, st[2].crest);
                ++row.cases;
                if (gS < row.worstShipped) row.worstShipped = gS;
                if (gC < row.worstCentre)  row.worstCentre  = gC;
                if (gS < -0.02) ++row.worseShipped;
                if (gC < -0.02) ++row.worseCentre;
            }
        }
        if (row.cases > 0) rows.push_back (row);
        if (tables && row.cases > 0)
            printf ("%-20s %-10s | worst %+6.2f dB, %3d/%3d | worst %+6.2f dB, %3d/%3d\n",
                    name, rg.nm, row.worstShipped, row.worseShipped, row.cases,
                    row.worstCentre, row.worseCentre, row.cases);
    }
  }

    check (rows.size() == 9, "R1: all three families produced cases to compare at the mid "
                             "range, the floor range and the whole parameter",
           fmt ("%.0f rows", (double) rows.size()));

    auto rowFor = [&rows] (const char* nm, const char* rg) -> const Row*
    {
        for (auto& r : rows)
            if (std::strcmp (r.name, nm) == 0 && std::strcmp (r.range, rg) == 0) return &r;
        return nullptr;
    };

    // (a) THE DEFAULT PATCH, ALL THREE WAYS, BECAUSE IT DOES NOT GIVE ONE ANSWER.
    //     This is the check that caught the previous revision's headline - and then
    //     caught this revision's first attempt to restate it, which claimed the same
    //     ordering held at the floor. It does not. On the patch most listeners hear:
    //       [0.5,2.5]  centring is worse   (the mid range the old sweep looked at)
    //       [0.0,1.0]  centring is BETTER, and by several dB (the floor)
    //       [0.0,4.0]  centring is far worse (the whole reachable parameter)
    //     All three are asserted. A header that quotes any one of them alone is
    //     quoting a sample.
    if (const Row* r = rowFor ("default n=1024", "[0.5,2.5]"))
        check (r->worstCentre < r->worstShipped - 0.3,
               "R1: default patch, MID range - centring makes the worst case worse",
               fmt ("shipped worst %+.2f dB, centred worst %+.2f dB over %.0f "
                    "(note, slope) cases", r->worstShipped, r->worstCentre,
                    (double) r->cases));
    if (const Row* r = rowFor ("default n=1024", "[0.0,1.0]"))
        check (r->worstShipped < r->worstCentre - 0.3,
               "R1: default patch, FLOOR range - the ordering INVERTS and the shipped "
               "rule is the one with the deeper worst case. The mid-range result is not "
               "a property of the two rules, and this header may not present it as one",
               fmt ("shipped worst %+.2f dB, centred worst %+.2f dB over %.0f "
                    "(note, slope) cases", r->worstShipped, r->worstCentre,
                    (double) r->cases));
    if (const Row* r = rowFor ("default n=1024", "[0.0,4.0]"))
        check (r->worstCentre < r->worstShipped - 3.0,
               "R1: default patch, the WHOLE reachable parameter - centring's worst case "
               "is several dB below the shipped rule's. THIS is the case the shipped rule "
               "actually rests on: not a better average and not a better result at every "
               "range, but a shallower tail over the full parameter",
               fmt ("shipped worst %+.2f dB, centred worst %+.2f dB over %.0f "
                    "(note, slope) cases", r->worstShipped, r->worstCentre,
                    (double) r->cases));

    // (b) AND THE ARGUMENT THAT DOES NOT GENERALISE, PINNED AS THE INVERSION IT IS.
    //     The previous revision asserted only the first half of this and read it as a
    //     general property of the two rules. It is a property of the range. On fractal
    //     L3 the shipped rule is flawless at [0.5, 2.5] and centring introduces
    //     regressions; at the parameter floor the SHIPPED rule is the one with
    //     regressions and centring has fewer. Both halves are asserted so that neither
    //     can be quoted without the other.
    if (const Row* mid = rowFor ("fractal L3 maxP384", "[0.5,2.5]"))
        check (mid->worseShipped == 0 && mid->worseCentre > 0,
               "R1: at the MID range, fractal L3 shows the shipped rule never worse than "
               "the hash and centring introducing cases that are",
               fmt ("shipped %.0f worse, centred %.0f worse, of %.0f cases",
                    (double) mid->worseShipped, (double) mid->worseCentre,
                    (double) mid->cases));
    if (const Row* flr = rowFor ("fractal L3 maxP384", "[0.0,1.0]"))
        check (flr->worseShipped > 0 && flr->worstShipped < flr->worstCentre,
               "R1: ...and AT THE PARAMETER FLOOR the same family inverts it - the "
               "SHIPPED rule is the one that falls below the hash, and by more. The "
               "family-level argument is a property of the reachable range, not of the "
               "two rules, and this header may not quote half of it",
               fmt ("shipped %.0f worse (worst %+.2f dB), centred %.0f worse "
                    "(worst %+.2f dB)",
                    (double) flr->worseShipped, flr->worstShipped,
                    (double) flr->worseCentre, flr->worstCentre));

    // (b2) THE NUMBER THE PREVIOUS REVISION PUBLISHED AS THE SHIPPED RULE'S WORST CASE
    //      WAS THE WORST CASE OF THE RANGES IT LOOKED AT. `odd maxP512` at the floor is
    //      several dB below anything the old four-range sweep found, so the floor must
    //      stay in this test: it is the only thing standing between the header and that
    //      mistake being made a third time.
    {
        const Row* mid = rowFor ("odd maxP512", "[0.5,2.5]");
        const Row* flr = rowFor ("odd maxP512", "[0.0,1.0]");
        if (mid != nullptr && flr != nullptr)
            check (flr->worstShipped < mid->worstShipped - 1.0,
                   "R1: the shipped rule's worst case at the PARAMETER FLOOR is materially "
                   "worse than at the mid range - the coverage gap was load-bearing, and "
                   "a sweep that stops at 0.5 understates the shipped rule's own damage",
                   fmt ("odd maxP512 worst: mid %+.2f dB, floor %+.2f dB",
                        mid->worstShipped, flr->worstShipped));
    }

    // (c) THE MECHANISM, WHICH SURVIVED THE FLOOR SWEEP INTACT AND IS STILL NOT
    //     SUFFICIENT. Damage is not a function of |dS|: the same magnitude costs MORE
    //     on the brightening side, which is the side only centring can reach. Asserted
    //     here at compile anchor 1.0, the patch default. It is worth knowing what this
    //     check does NOT say: the asymmetry is real at every anchor the rig swept, and
    //     it still does not decide the question, because at the parameter floor the
    //     shipped rule pays the FULL positive width of the span while centring pays
    //     half of it in each direction - which is why (b) inverts there. A sign
    //     advantage over a doubled magnitude is not automatically a win.
    {
        const Ref& r = refByName ("default n=1024");
        const int n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        buildPlan (r.t.partials.data(), n, opt, plan);
        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);

        double meanNeg = 0.0, meanPos = 0.0; int notes = 0;
        for (int m = 36; m <= 96; m += 6)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
            if (p <= 2 || ! plan.improved (p)) continue;
            ++notes;
            for (int sgn = 0; sgn < 2; ++sgn)
            {
                const double dS = sgn == 0 ? -0.5 : +0.5;
                std::vector<Partial> tilted;
                for (int i = 0; i < p; ++i)
                {
                    Partial q = r.t.partials[(size_t) i];
                    q.amp = (float) ((double) q.amp
                            * std::pow (std::max (1.0e-9, (double) q.ratio), -dS));
                    tilted.push_back (q);
                }
                std::vector<ref::Stats> st;
                ref::crestMulti (tilted.data(), p, { hp.data(), plan.phasesFor (p) },
                                 blindWindowPeriods (tilted.data(), p), 10, st);
                (sgn == 0 ? meanNeg : meanPos) += ref::dB (st[0].crest, st[1].crest);
            }
        }
        meanNeg /= std::max (1, notes); meanPos /= std::max (1, notes);
        if (tables)
            printf ("   R1 asymmetry, default n=1024, %d notes: dS = -0.5 gives %+.2f dB, "
                    "dS = +0.5 gives %+.2f dB\n", notes, meanNeg, meanPos);
        check (notes > 4, "R1: there are improved notes to tilt both ways",
               fmt ("%.0f", (double) notes));
        check (meanNeg < meanPos,
               "R1: the SAME |dS| costs more on the brightening side than the darkening "
               "side, so halving |dS| by flipping its sign is not a trade worth making - "
               "this is the mechanism, and centring is the only rule that reaches dS < 0",
               fmt ("dS = -0.5: %+.2f dB, dS = +0.5: %+.2f dB", meanNeg, meanPos));
    }

    // (d) AND THE PART THAT IS NOT ABOUT PHASE AT ALL. compileSpectrum applies the
    //     amplitude floor and the keep-loudest cap AT the compile slope, so the
    //     reading of the withdrawn instruction that recompiles the spectrum at the
    //     centre DELETES partials the bright half of the reachable range needs.
    //     PluginProcessor.cpp:560-562 compiles at the reachable minimum for exactly
    //     this reason.
    {
        SpectrumSettings a = refByName ("default n=1024").settings; a.curveSlope = 2.0f;
        SpectrumSettings b = a;                                     b.curveSlope = 2.5f;
        const int na = (int) compileSpectrum (a).partials.size();
        const int nb = (int) compileSpectrum (b).partials.size();
        if (tables)
            printf ("   R1 fidelity: default patch compiles to %d partials at slope 2.0 "
                    "and %d at slope 2.5\n", na, nb);
        check (nb < na,
               "R1: recompiling at a steeper (centred) slope DELETES partials the bright "
               "half of the reachable range needs, and no phase set can put them back",
               fmt ("%.0f partials at slope 2.0, %.0f at slope 2.5", (double) na, (double) nb));

        // ...AND THE LIMIT OF THAT ARGUMENT, asserted so it cannot be quoted as though
        // it settled the floor. Every floor-touching reachable range centres at 2.0 or
        // below, where the count does not move - so on exactly the ranges where (b)
        // inverts, the fidelity argument has nothing to say and CENTRE-AMP and
        // CENTRE-ALL are the same measurement.
        SpectrumSettings f0 = a; f0.curveSlope = 0.0f;
        SpectrumSettings f1 = a; f1.curveSlope = 0.5f;
        const int n0 = (int) compileSpectrum (f0).partials.size();
        const int n1 = (int) compileSpectrum (f1).partials.size();
        check (n0 == n1,
               "R1: ...but at the parameter floor centring deletes NOTHING, so the "
               "fidelity argument does not reach the ranges where the family-level "
               "argument inverts",
               fmt ("%.0f partials at slope 0.0, %.0f at slope 0.5", (double) n0, (double) n1));
    }
}

//==============================================================================
// 25. R2: THE GUARANTEE MUST NOT STOP AT plan.n, BECAUSE THE LISTENER DOES NOT.
//
// seedFromPlan used to fill [plan.n, prefixLen) with kAlignedPhase at EVERY scatter.
// At scatter 0 that is correct. At scatter 1 - full Scattered, the mode this file
// exists to improve - it wrote every one of those partials at its PEAK, which is the
// loudest arrangement available and the exact opposite of what the caller asked for.
//
// The path is ordinary, not exotic. AdditiveVoice.h:519 and :646 take
// seedSpan = min(max(hi, activeCount), oscCap()) with activeCount still holding the
// PREVIOUS, larger template's slot count; :914 takes span = max(n, activeCount); and
// each Shepard copy is seeded over whatever oscCap() gives it - 1638 with Shepard on,
// 16384 with it off. Every one of those routinely exceeds plan.n.
//
// THE PROPERTY, asserted here at exactly those spans:
//   past plan.n, at scatter 1 the seeded phase is what AdditiveVoice::seedPhase(j,
//   aligned = false) writes today, BIT FOR BIT; at scatter 0 it is kAlignedPhase; and
//   in between it is the same interpolation the plan itself gets.
static void testSeedPastThePlan()
{
    const int  kOscCapShepard = 1638;      // AdditiveVoice::oscCap() with Shepard on
    const int  kOscCapPlain   = 16384;     // ...and with it off (kMaxPartials)
    const Ref& r = refByName ("harmonic s1 n=128");
    const int  n = (int) r.t.partials.size();
    Options opt; PhasePlan plan;
    buildPlan (r.t.partials.data(), n, opt, plan);
    check (plan.valid() && plan.n == n && n < kOscCapShepard,
           "R2: the fixture really does have a plan shorter than the spans the traps name",
           fmt ("plan.n = %.0f", (double) plan.n));

    // The spans integration trap 4 names, plus "an activeCount larger than the new n"
    // - the previous template's slot count still sitting in the voice.
    const int spans[] = { n + 1, n + 64, 512, kOscCapShepard, kOscCapPlain };

    for (int span : spans)
    {
        const std::string at = std::string (" at prefixLen ") + std::to_string (span);

        // (a) scatter 1: every index past plan.n is TODAY'S HASH, bit for bit. `==`,
        //     not a tolerance - a phase that is the hash plus 1e-16 is a different
        //     double and renders a different sample.
        {
            std::vector<double> out ((size_t) span, -7.0);
            seedFromPlan (plan, RenderedPrefix { plan.n }, FillSpan { span }, 1.0, out.data());
            int bad = 0; double worst = 0.0; int firstBad = -1;
            for (int i = plan.n; i < span; ++i)
            {
                const double want = refHashAt (i);
                if (out[(size_t) i] != want)
                { ++bad; if (firstBad < 0) firstBad = i;
                  worst = std::max (worst, std::fabs (out[(size_t) i] - want)); }
            }
            check (bad == 0,
                   "R2: past plan.n at scatter 1, every phase is exactly what "
                   "AdditiveVoice::seedPhase(j, aligned=false) writes today" + at,
                   fmt ("%.0f of %.0f entries differ, first at index %.0f, worst |delta| %.3e",
                        (double) bad, (double) (span - plan.n), (double) firstBad, worst));
        }

        // (b) scatter 0 stays sacred past plan.n too.
        {
            std::vector<double> out ((size_t) span, -7.0);
            seedFromPlan (plan, RenderedPrefix { plan.n }, FillSpan { span }, 0.0, out.data());
            int bad = 0;
            for (int i = 0; i < span; ++i) if (out[(size_t) i] != kAlignedPhase) ++bad;
            check (bad == 0,
                   "R2: past plan.n at scatter 0 it is still kAlignedPhase, exactly" + at,
                   fmt ("%.0f of %.0f entries differ", (double) bad, (double) span));
        }

        // (c) in between, the tail interpolates the SAME way the plan's own indices do.
        //     Asserted by construction: run interpolatePhases over the hash for those
        //     indices and demand the identical bits.
        for (double sc : { 0.17, 0.5, 0.83 })
        {
            std::vector<double> out ((size_t) span, -7.0);
            seedFromPlan (plan, RenderedPrefix { plan.n }, FillSpan { span }, sc, out.data());
            const int tail = span - plan.n;
            std::vector<double> want ((size_t) tail), tgt ((size_t) tail);
            for (int i = 0; i < tail; ++i) tgt[(size_t) i] = refHashAt (plan.n + i);
            interpolatePhases (tgt.data(), tail, sc, want.data());
            int bad = 0;
            for (int i = 0; i < tail; ++i)
                if (out[(size_t) (plan.n + i)] != want[(size_t) i]) ++bad;
            check (bad == 0,
                   "R2: past plan.n at a mid scatter it interpolates identically to the "
                   "plan's own indices" + at + fmt (", scatter %.2f", sc),
                   fmt ("%.0f of %.0f entries differ", (double) bad, (double) tail));
        }

        // (d) and it writes exactly prefixLen entries, not one more.
        {
            std::vector<double> out ((size_t) span + 8, -12345.0);
            seedFromPlan (plan, RenderedPrefix { plan.n }, FillSpan { span }, 1.0, out.data());
            bool wrote = true, spilled = false;
            for (int i = 0; i < span; ++i) wrote = wrote && (out[(size_t) i] != -12345.0);
            for (int i = span; i < span + 8; ++i) spilled = spilled || (out[(size_t) i] != -12345.0);
            check (wrote && ! spilled,
                   "R2: ...and still writes exactly the requested span and nothing beyond" + at, "");
        }
    }

    // (e) THE ACCESSOR ITSELF, which is what a caller seeding a Shepard copy by hand
    //     needs. It must agree with the hash index for index over the whole reachable
    //     range, and it must be the same formula the PLAN carries at set 0 - otherwise
    //     the sound would change at the boundary even though the phases "match".
    {
        int bad = 0;
        for (int i = 0; i < kOscCapPlain; ++i)
        {
            if (hashPhaseAt (i) != refHashAt (i)) ++bad;
        }
        check (bad == 0, "R2: hashPhaseAt is the shipping hash at every index to 16384",
               fmt ("%.0f differ", (double) bad));

        std::vector<double> engineHash ((size_t) n);
        hashPhases (engineHash.data(), n);
        int badPlan = 0;
        for (int i = 0; i < n; ++i)
            if (hashPhaseAt (i) != engineHash[(size_t) i]) ++badPlan;
        check (badPlan == 0,
               "R2: ...and it is the same value the plan carries inside [0, n), so the "
               "boundary at plan.n is inaudible rather than merely small", "");
    }

    // (f) AN INVALID PLAN takes the same path. Before, "no plan" sounded like Aligned
    //     no matter where the fader was; now it sounds like today's Scattered, which
    //     is the floor this whole file promises never to fall below.
    {
        PhasePlan dead;
        std::vector<double> out (256, -7.0);
        seedFromPlan (dead, RenderedPrefix { 256 }, FillSpan { 256 }, 1.0, out.data());
        int bad = 0;
        for (int i = 0; i < 256; ++i) if (out[(size_t) i] != refHashAt (i)) ++bad;
        check (bad == 0,
               "R2: an INVALID plan at scatter 1 is today's Scattered mode, not Aligned",
               fmt ("%.0f of 256 differ", (double) bad));
    }

    // (g) THE SOUND, not just the bits. The old fill put every tail partial at its
    //     PEAK, so this is scored on the independent rig against BOTH references:
    //     today's Scattered mode, and the fill that used to ship. The dB number is
    //     the size of the defect the caller was living with.
    {
        const int span = 512;
        std::vector<Partial> wide (r.t.partials.begin(), r.t.partials.end());
        for (int k = (int) wide.size(); k < span; ++k)
            wide.push_back ({ (float) (k + 1), (float) (1.0 / (double) (k + 1)) });

        std::vector<double> seeded ((size_t) span), todays ((size_t) span),
                            oldFill ((size_t) span);
        seedFromPlan (plan, RenderedPrefix { plan.n }, FillSpan { span }, 1.0, seeded.data());
        hashPhases (todays.data(), span);
        // Exactly what seedFromPlan did before this fix: the plan inside, 0.25 outside.
        interpolatePhases (plan.phasesFor (plan.n), plan.n, 1.0, oldFill.data());
        for (int i = plan.n; i < span; ++i) oldFill[(size_t) i] = kAlignedPhase;

        std::vector<ref::Stats> st;
        ref::crestMulti (wide.data(), span,
                         { todays.data(), seeded.data(), oldFill.data() },
                         blindWindowPeriods (wide.data(), span), 10, st);
        const double gNew = ref::dB (st[0].crest, st[1].crest);
        const double gOld = ref::dB (st[0].crest, st[2].crest);
        if (tables)
            printf ("   R2: %d-partial span on a %d-partial plan, vs today's Scattered: "
                    "new fill %+.2f dB, the old kAlignedPhase fill %+.2f dB\n",
                    span, plan.n, gNew, gOld);
        check (gOld < -1.0,
               "R2: the old kAlignedPhase fill really was audibly worse than today's "
               "Scattered mode past plan.n - this is the defect, not a tidiness point",
               fmt ("%+.2f dB, crest %.4f vs %.4f", gOld, st[2].crest, st[0].crest));
        check (gNew > gOld + 1.0,
               "R2: ...and the hash-continued fill removes it",
               fmt ("new %+.2f dB vs old %+.2f dB", gNew, gOld));
    }
}

//==============================================================================
// 26. FAILURE 6: THE PLAYED SET MUST BE CHOSEN BY THE RENDERED PREFIX, NOT BY THE
//     SPAN OF SLOTS WE HAPPEN TO BE FILLING.
//
// THE DEFECT. seedFromPlan used to take ONE int, `prefixLen`, and spend it twice:
// once as the number of slots to fill, and once - through phasesFor's clamp
// p = min(prefixLen, plan.n) - as the choice of WHICH PHASE SET TO PLAY. Those are
// different quantities. The first is a BANK SPAN. The second is the RENDERED PREFIX,
// how many partials sit below Nyquist at the played pitch, and choosing by it is in
// the header's own capitals "the entire answer to Failure 2".
//
// Every real call site passes a bank span, because that is how many slots a voice
// has: seedSpan = min(max(hi, activeCount), oscCap()) is 1638 with Shepard on and
// 16384 with it off. With any span >= plan.n the clamp collapses to plan.n, so every
// pitch played pick[plan.n - 1] and the pitch-dependent choice reached no voice at
// all. On the skeptics' rig, 70 templates x MIDI 21-108 at three note lengths: of the
// 53 templates where pick[n-1] != 0, 636 of 1851 cases were WORSE than today's hash.
//
// WHY THIS SUITE DID NOT CATCH IT, which matters more than the fix. Every audible
// assertion in this file - testKeyboardNeverWorse, testGuaranteeAtEveryPrefix,
// testHeldNoteNeverWorse - scores plan.phasesFor(p) DIRECTLY. Not one of them ever
// went through seedFromPlan with a span a voice would really pass. The guarantee was
// true of the accessor and false of the function the plugin calls. That is the same
// disease as Failures 1-3, one layer further out, so this test is written the other
// way round: it seeds THROUGH seedFromPlan, at the real bank spans, and scores what
// comes out.
static void testSetChosenByRenderedPrefix()
{
    const int kOscCapShepard = 1638;   // AdditiveVoice::oscCap() with Shepard on
    const int kOscCapPlain   = 16384;  // ...and with it off (spectrum::kMaxPartials)

    // (a) THE TYPES. The wrong call must not be WRITABLE, not merely discouraged -
    //     that is the difference between this fix and the comment the previous
    //     revision relied on. A bare int converts to neither tag, and the two tags do
    //     not convert to each other, so `seedFromPlan (plan, seedSpan, ...)` and a
    //     swapped pair are both compile errors now.
    static_assert (! std::is_convertible<int, RenderedPrefix>::value,
                   "a bare int must not silently become a RenderedPrefix");
    static_assert (! std::is_convertible<int, FillSpan>::value,
                   "a bare int must not silently become a FillSpan");
    static_assert (! std::is_constructible<RenderedPrefix, FillSpan>::value,
                   "a FillSpan must not be usable where the set is chosen");
    static_assert (! std::is_constructible<FillSpan, RenderedPrefix>::value,
                   "a RenderedPrefix must not be usable as a slot count");
    check (true, "F6: the two counts are distinct types - the swapped call and the "
                 "bare-int call do not compile", "static_assert, checked at build time");

    // (b) THE MECHANISM, on the fixture that shows it most sharply. The property is:
    //     what seedFromPlan writes into [0, rendered) is the set the RENDERED PREFIX
    //     chose, bit for bit, AT EVERY FILL SPAN. Under the old code the two large
    //     spans both produced pick[plan.n - 1] instead.
    int fixturesWithDivergence = 0;
    for (const char* name : kKeyboardPatches)
    {
        const Ref& r = refByName (name);
        const int  n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        buildPlan (r.t.partials.data(), n, opt, plan);
        if (! plan.valid()) continue;

        // The prefixes where the choice actually differs from the span-collapsed one.
        // If there are none on a fixture the fixture proves nothing here, so the count
        // is asserted afterwards rather than assumed.
        const int tail = (int) plan.pick[(size_t) (plan.n - 1)];
        std::vector<int> divergent;
        for (int p = 1; p <= plan.n; ++p)
            if ((int) plan.pick[(size_t) (p - 1)] != tail) divergent.push_back (p);
        if (divergent.empty()) continue;
        ++fixturesWithDivergence;

        int bad = 0, cases = 0, firstBadPrefix = -1;
        for (int span : { kOscCapShepard, kOscCapPlain, 512, 40 })
        {
            for (size_t k = 0; k < divergent.size(); k += (divergent.size() / 24 + 1))
            {
                const int p = divergent[k];
                const int head = std::min (p, std::min (span, plan.n));
                if (head <= 0) continue;
                ++cases;
                std::vector<double> out ((size_t) span, -7.0);
                seedFromPlan (plan, RenderedPrefix { p }, FillSpan { span }, 1.0, out.data());
                // scatter 1 is the target set exactly, so this compares against the
                // accessor with no interpolation in between.
                const double* want = plan.phasesFor (RenderedPrefix { p });
                bool ok = true;
                for (int i = 0; i < head; ++i) ok = ok && (out[(size_t) i] == want[i]);
                if (! ok) { ++bad; if (firstBadPrefix < 0) firstBadPrefix = p; }
            }
        }
        check (bad == 0,
               std::string ("F6: the played set is chosen by the RENDERED PREFIX at every "
                            "fill span, including 1638 and 16384: ") + name,
               fmt ("%.0f of %.0f (prefix, span) pairs played the wrong set, first at "
                    "rendered prefix %.0f; %.0f prefixes disagree with pick[n-1]",
                    (double) bad, (double) cases, (double) firstBadPrefix,
                    (double) divergent.size()));

        // (c) THE SPAN IS INERT. Same rendered prefix, four wildly different spans:
        //     the head must be IDENTICAL. This is the property stated as an
        //     independence rather than as an equality against the accessor, so it
        //     holds even if the accessor itself were ever changed.
        {
            int drift = 0, cmp = 0;
            for (size_t k = 0; k < divergent.size(); k += (divergent.size() / 24 + 1))
            {
                const int p = divergent[k];
                std::vector<double> a ((size_t) kOscCapPlain, -7.0), b ((size_t) 64, -7.0);
                seedFromPlan (plan, RenderedPrefix { p }, FillSpan { kOscCapPlain }, 0.63, a.data());
                seedFromPlan (plan, RenderedPrefix { p }, FillSpan { 64 },            0.63, b.data());
                const int head = std::min (p, std::min (64, plan.n));
                for (int i = 0; i < head; ++i) { ++cmp; if (a[(size_t) i] != b[(size_t) i]) ++drift; }
            }
            check (drift == 0,
                   std::string ("F6: changing ONLY the fill span changes nothing the "
                                "rendered prefix plays: ") + name,
                   fmt ("%.0f of %.0f head entries moved", (double) drift, (double) cmp));
        }
    }
    check (fixturesWithDivergence > 0,
           "F6: at least one keyboard fixture really does pick a different set at some "
           "rendered prefix than at plan.n - otherwise (b) and (c) assert nothing",
           fmt ("%.0f of %.0f fixtures diverge", (double) fixturesWithDivergence,
                (double) (sizeof (kKeyboardPatches) / sizeof (kKeyboardPatches[0]))));

    //--------------------------------------------------------------------------
    // (d) THE SOUND, WHICH IS THE WHOLE POINT. MIDI 21-108, seeded through
    //     seedFromPlan at the REAL bank span, scored on the independent rig against
    //     the shipping hash over the partials that pitch actually renders.
    //
    //     Two columns, and the second is the one that makes this a regression test
    //     rather than a tautology:
    //       RENDERED - what ships now: the set this note's own prefix chose.
    //       SPAN     - what the old signature played: pick[plan.n - 1] at every pitch,
    //                  which is exactly plan.phasesFor(plan.n).
    //     RENDERED must never be worse than the hash. SPAN is measured beside it so
    //     the defect is on the record as a dB number and cannot be called cosmetic.
    //
    //     AND THE DEFECT HAS TWO SHAPES, so the fixture list is not kKeyboardPatches.
    //     Which shape a template is in is decided by pick[plan.n - 1]:
    //
    //       pick[n-1] == 0  The span collapsed onto the HASH. Nothing sounds worse than
    //                       today - the feature simply does nothing, at every pitch, on
    //                       every note. This is the quiet shape, and it is the one all
    //                       six keyboard patches are in, which is why the +1.6 dB this
    //                       file measures everywhere else was never actually reaching a
    //                       voice.
    //       pick[n-1] != 0  The span collapsed onto a set built for a DIFFERENT prefix,
    //                       and playing it at a pitch that renders fewer partials is
    //                       audibly worse than the hash. This is the skeptics' 636 of
    //                       1851. `default maxP128 s0.3` and `harmonic s1 n=128` are in
    //                       this shape and are in this suite's reference set already.
    //
    //     Both shapes are asserted below, and the test refuses to pass unless the
    //     fixture list actually contains one of each.
    static const char* kF6Patches[] = { "default n=1024", "fractal L3 maxP384",
                                        "FLAT n=512", "scala 19EDO x6", "prime-limit 7",
                                        "default maxP128 s0.3", "harmonic s1 n=128" };
    int quietShape = 0, loudShape = 0, loudShapeWithRegression = 0;
    double loudWorst = 1.0e30;

    if (tables) printf ("\n-- F6: seeded THROUGH seedFromPlan at bank span %d, MIDI 21..108 "
                        "(every semitone under 256 partials); 1 window if periodic, "
                        "1x/4x/16x if not --\n"
                        "%-24s %9s | %5s %8s %8s | %5s %8s %8s\n",
                        kOscCapPlain, "spectrum", "pick[n-1]",
                        "worse", "worst dB", "mean dB", "worse", "worst dB", "mean dB");
    for (const char* name : kF6Patches)
    {
        const Ref& r = refByName (name);
        const int  n = (int) r.t.partials.size();
        Options opt; PhasePlan plan;
        const PhaseReport rep = buildPlan (r.t.partials.data(), n, opt, plan);
        if (! plan.valid()) continue;

        std::vector<double> hp ((size_t) n);
        hashPhases (hp.data(), n);
        std::vector<double> seeded ((size_t) kOscCapPlain);

        int notes = 0, cases = 0, improvedNotes = 0, worseNew = 0, worseOld = 0, worstOldMidi = 0;
        double worstNew = 1.0e30, worstOld = 1.0e30, sumNew = 0.0, sumOld = 0.0;

        // THE NOTE GRID, AND WHY IT IS NOT UNIFORM. Three observation lengths on a
        // coarse grid is the cost shape testHeldNoteNeverWorse already carries, and on
        // a 1024-partial template a 16x window at every semitone costs minutes. But the
        // span-collapsed regression this test has to catch is PER-PITCH and small: on
        // `default maxP128 s0.3` it appears at ONE semitone in eighty-eight, and a
        // step-3 grid walks straight past it and reports the defect as absent. So the
        // small templates - which are the ones with pick[n-1] != 0, and are cheap - get
        // every semitone, and the large ones get every third.
        const int step = n <= 256 ? 1 : 3;
        for (int m = 21; m <= 108; m += step)
        {
            const double f0 = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            const int p = renderedPrefix (r.t.partials.data(), n, f0, opt.sampleRate);
            if (p <= 1) continue;
            ++notes;
            if (plan.improved (RenderedPrefix { p })) ++improvedNotes;

            // THE NOTE-ON PATH, written exactly as the integration plan now writes it:
            // the rendered prefix chooses, the bank span fills.
            seedFromPlan (plan, RenderedPrefix { p }, FillSpan { kOscCapPlain },
                          1.0, seeded.data());

            // What the previous signature played at this pitch, for the same span.
            const double* spanChosen = plan.phasesFor (plan.n);

            // AT SEVERAL OBSERVATION LENGTHS ON AN APERIODIC TEMPLATE, AND AT EXACTLY
            // ONE ON A PERIODIC ONE. The skeptics' rig scored real 0.05 / 0.25 / 1.00 s
            // renders and multiples of the blind window are this file's equivalent - but
            // only where a longer window can show something new.
            //
            // ON AN EXACTLY-PERIODIC TEMPLATE IT CANNOT. The waveform REPEATS with the
            // blind window, so 4x and 16x measure the same waveform four and sixteen
            // times over. That is this header's own claim, and it means the extra
            // lengths carry no information - only rig error. MEASURED, and this is why
            // the ladder is written this way rather than "more is safer": at mult 16
            // with the oversample dropped to 3 the way testHeldNoteNeverWorse drops it,
            // ref::sampleCount asks for ceil(periods * topRatio * 3) points, i.e. THREE
            // SAMPLES PER CYCLE OF THE TOP PARTIAL - under-sampled outright. At MIDI 108
            // on `default maxP128 s0.3`, five partials sounding, that is fifteen samples
            // per period, it understates the HASH's peak more than the plan's, and it
            // reported the plan 0.40 dB WORSE than the hash on a template where a denser
            // grid at every length says +0.00. A sparser grid can only miss a peak, so
            // the oversample may never fall as the window grows.
            const double w = blindWindowPeriods (r.t.partials.data(), p);
            const int    kMults[3] = { 1, 4, 16 };
            const int    nMults = rep.exactlyPeriodic ? 1 : 3;
            for (int mi = 0; mi < nMults; ++mi)
            {
                const int mult = kMults[mi];
                std::vector<ref::Stats> s;
                ref::crestMulti (r.t.partials.data(), p,
                                 { hp.data(), seeded.data(), spanChosen },
                                 w * (double) mult, 8, s);
                const double dNew = ref::dB (s[0].crest, s[1].crest);
                const double dOld = ref::dB (s[0].crest, s[2].crest);
                if (dNew < -0.02) ++worseNew;
                if (dOld < -0.02) ++worseOld;
                if (dNew < worstNew) worstNew = dNew;
                if (dOld < worstOld) { worstOld = dOld; worstOldMidi = m; }
                sumNew += dNew; sumOld += dOld;
                ++cases;
            }
        }
        if (notes == 0 || cases == 0) continue;

        const int tailPick = (int) plan.pick[(size_t) (plan.n - 1)];
        if (tailPick == 0) ++quietShape;
        else
        {
            ++loudShape;
            if (worseOld > 0) ++loudShapeWithRegression;
            if (worstOld < loudWorst) loudWorst = worstOld;
        }

        if (tables)
            printf ("%-24s %9d | %5d %+8.2f %+8.2f | %5d %+8.2f %+8.2f\n",
                    name, tailPick, worseNew, worstNew, sumNew / cases,
                    worseOld, worstOld, sumOld / cases);

        // THE GUARANTEE, now scoped to the function the plugin actually calls, to the
        // span it is actually called with, and to every observation length measured.
        // This is the assertion the whole of Failure 6 exists to make.
        check (worseNew == 0,
               std::string ("F6: seeded through seedFromPlan at the real bank span, no "
                            "(note, length) case is worse than the shipping hash: ") + name,
               fmt ("%.0f of %.0f cases worse, worst %+.2f dB", (double) worseNew,
                    (double) cases, worstNew));

        // WHAT THIS TEST DELIBERATELY DOES NOT ASSERT, because the measurement refused
        // it. The first draft of this test asserted that the rendered choice is never
        // worse than the span-collapsed one it replaces. That is FALSE, and
        // `harmonic s1 n=128` is the counter-example: the span choice scores +2.07 dB
        // mean against the rendered choice's +2.03, and its worst case over the
        // keyboard is +1.19 dB - better than the hash at every pitch, by luck.
        //
        // The difference between the two is not that one wins more. It is that ONE OF
        // THEM IS GUARANTEED AND THE OTHER IS A COINCIDENCE. pick[p-1] was verified
        // against the hash at prefix p, at every length in the ladder; pick[n-1] was
        // verified at prefix n and at no other, so at a pitch that renders fewer than
        // n partials nothing has been checked at all. On `default maxP128 s0.3` that
        // coincidence runs out and the span choice goes below the hash. Asserting the
        // stronger, false thing would have been this file's disease all over again:
        // a claim that happens to be true of the fixtures in front of it.

        // THE QUIET SHAPE, NAMED, AND THIS IS THE CHECK THAT BREAKS ON THE OLD CODE
        // FOR THE MAJORITY OF PATCHES. Where the span collapsed onto the hash the old
        // signature was not audibly wrong - it was audibly NOTHING, at every pitch, on
        // every note, on five of the seven fixtures here. Conditioned on the plan having
        // claimed something, exactly as testKeyboardNeverWorse conditions its mean: on
        // an aperiodic template the engine deliberately claims nothing and +0.00 is the
        // correct answer, not a failure.
        if (tailPick == 0)
        {
            if (improvedNotes > 0)
                check (sumNew / cases > 0.0,
                       std::string ("F6: where the span collapsed onto the hash, the "
                                    "rendered choice is what recovers the win the fader "
                                    "was sold on: ") + name,
                       fmt ("rendered %+.2f dB mean, span-collapsed %+.2f dB mean over "
                            "%.0f cases, %.0f notes improved",
                            sumNew / cases, sumOld / cases, (double) cases,
                            (double) improvedNotes));
            else
                check (std::fabs (sumNew / cases) < 1.0e-12,
                       std::string ("F6: where the plan claims NOTHING, seedFromPlan "
                                    "returns the hash bit for bit - not 'about the same': ")
                           + name,
                       fmt ("mean %+.4g dB", sumNew / cases));
        }
    }

    // (e) BOTH SHAPES MUST BE PRESENT, and the loud one must really break the contract.
    //     Without this the whole of (d) could pass on a fixture list where the defect
    //     happens to be invisible - which is precisely how it survived every previous
    //     revision of this suite.
    check (quietShape > 0 && loudShape > 0,
           "F6: the fixture list contains BOTH shapes of the defect - templates whose "
           "span collapses onto the hash and templates whose span collapses onto a set "
           "built for another prefix",
           fmt ("%.0f quiet, %.0f loud", (double) quietShape, (double) loudShape));

    // THE ASYMMETRY THAT IS THE WHOLE POINT, stated as the contract and not as a size.
    // The rendered choice is worse than the hash in ZERO cases across every fixture
    // above. The span-collapsed choice is worse in at least one, on a template that is
    // in this suite's own reference set. That is the difference between a verified
    // property and a coincidence, and it is why the argument had to become a type.
    check (loudShapeWithRegression > 0,
           "F6: on a template where pick[n-1] != 0 the span-collapsed choice DOES fall "
           "below today's hash at a real pitch, where the rendered choice never does - "
           "the old signature had no guarantee to lose because it never had one",
           fmt ("%.0f of %.0f such fixtures regress, worst %+.3f dB",
                (double) loudShapeWithRegression, (double) loudShape, loudWorst));
}

//==============================================================================
int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "-v") == 0)      verbose = true;
        if (std::strcmp (argv[i], "--tables") == 0) tables = true;
    }

    testRig();
    testTrueRatioAgreement();
    testKeyboardNeverWorse();
    testSweepDetector();
    testCost();
    testRunsOnTheShippedDefaults();
    testHonestReporting();
    testMorphStep();
    testDeterminism();
    testFader();
    testPlanLookup();
    testSalt();
    testDegenerate();
    testLongWindowWander();
    testGuaranteeAtEveryPrefix();
    testScatterZeroIsSacred();
    testSpanLadderExactness();
    testHeldNoteNeverWorse();
    testChooserAlone();
    testAperiodicOptimiser();
    testNormalisationLawGap();
    testShortNoteSampling();
    testAmplitudesAreAPrecondition();
    testSlopeCompilePosition();
    testSeedPastThePlan();
    testSetChosenByRenderedPrefix();

    printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
