// Standalone unit tests for the spectrum compiler (no JUCE).
// Verifies the numeric examples from the adversarially-verified math design.
//
//   build\...\SpectrumTests.exe   -> exit code 0 = all passed, non-zero = failures.
//
// `reach` bounds the generated harmonic range (top ratio). In the plugin it is fixed
// high (reach to Nyquist); the tests set it small to check exact partial sets.

#include "dsp/Spectrum.h"
#include "dsp/Dissonance.h"
#include "dsp/PartialGains.h"
#include "PartialAmpEdit.h"
#include "IntervalTokens.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <map>
#include <string>
#include <cstdio>
#include <cmath>

using namespace spectrum;

static int failures = 0;

static void check (bool cond, const char* what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (! cond) ++failures;
}

static float ampAtRatio (const SpectrumTemplate& t, double ratio, double tol = 1e-3)
{
    for (const auto& p : t.partials)
        if (std::abs ((double) p.ratio - ratio) < tol)
            return p.amp;
    return -1.0f;
}

static bool hasRatio (const SpectrumTemplate& t, double ratio, double tol = 1e-3)
{
    return ampAtRatio (t, ratio, tol) >= 0.0f;
}

int main()
{
    std::printf ("Spectrum compiler tests\n");

    //--------------------------------------------------------------------------
    std::printf ("prime helpers:\n");
    check (isPrime (2) && isPrime (3) && isPrime (7) && isPrime (13), "small primes recognised");
    check (! isPrime (1) && ! isPrime (9) && ! isPrime (14), "non-primes rejected");
    check (largestPrimeFactor (14) == 7 && largestPrimeFactor (25) == 5
           && largestPrimeFactor (33) == 11 && largestPrimeFactor (1) == 1, "largest prime factor");
    check (primeLimitOk (14, 7) && primeLimitOk (21, 7) && primeLimitOk (25, 7), "7-limit keeps 14, 21, 25");
    check (! primeLimitOk (11, 7) && ! primeLimitOk (13, 7) && ! primeLimitOk (22, 7)
           && ! primeLimitOk (33, 7), "7-limit drops 11, 13, 22, 33");

    //--------------------------------------------------------------------------
    std::printf ("saw (All, N=1, slope=1):\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 8; s.selection = Selection::All; s.curveSlope = 1.0f;
        auto t = compileSpectrum (s);
        check (t.partials.size() == 8, "8 partials (reach 8)");
        float a1 = ampAtRatio (t, 1.0), a2 = ampAtRatio (t, 2.0), a4 = ampAtRatio (t, 4.0);
        check (a1 > 0 && std::abs (a2 / a1 - 0.5) < 1e-3, "2nd harmonic ~ 1/2");
        check (a1 > 0 && std::abs (a4 / a1 - 0.25) < 1e-3, "4th harmonic ~ 1/4");
    }

    //--------------------------------------------------------------------------
    std::printf ("square (Odd, N=1, slope=1):\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 9; s.selection = Selection::Odd; s.curveSlope = 1.0f;
        auto t = compileSpectrum (s);
        check (hasRatio (t, 1) && hasRatio (t, 3) && hasRatio (t, 5) && hasRatio (t, 7) && hasRatio (t, 9),
               "odd ratios 1,3,5,7,9 present");
        check (! hasRatio (t, 2) && ! hasRatio (t, 4), "even harmonics absent");
    }

    //--------------------------------------------------------------------------
    std::printf ("base shift GRAVE (N=6, slope=0): lowest partial stays at played pitch:\n");
    {
        SpectrumSettings s; s.baseN = 6; s.baseMode = BaseMode::Transposed; s.reach = 2;
        s.selection = Selection::All; s.curveSlope = 0.0f; // reach 2 -> harmonics 6..12
        auto t = compileSpectrum (s);
        check (hasRatio (t, 1.0), "lowest partial at ratio 1.0 (played pitch)");
        check (hasRatio (t, 7.0 / 6.0) && hasRatio (t, 2.0), "ratios 7/6 and 12/6 present");
    }

    //--------------------------------------------------------------------------
    std::printf ("base shift BRIGHT (N=6): harmonics at natural positions (lowest = N):\n");
    {
        SpectrumSettings g; g.baseN = 6; g.baseMode = BaseMode::Transposed; g.reach = 12;
        g.selection = Selection::All; g.curveSlope = 0.0f;
        SpectrumSettings b = g; b.baseMode = BaseMode::Original;
        auto tg = compileSpectrum (g);
        auto tb = compileSpectrum (b);
        check (std::abs ((double) tg.partials.front().ratio - 1.0) < 1e-3, "grave lowest ratio = 1");
        check (std::abs ((double) tb.partials.front().ratio - 6.0) < 1e-3, "bright lowest ratio = N (6)");
        // For a harmonic present in both (k=7), bright ratio = N * grave ratio.
        check (hasRatio (tg, 7.0 / 6.0), "grave has harmonic 7 at ratio 7/6");
        check (hasRatio (tb, 7.0), "bright has harmonic 7 at ratio 7 (= N * grave)");
    }

    //--------------------------------------------------------------------------
    std::printf ("prime-limit in pipeline (7-limit, N=1, All):\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 13; s.selection = Selection::All;
        s.curveSlope = 0.0f; s.primeLimit = 7;
        auto t = compileSpectrum (s);
        check (! hasRatio (t, 11.0) && ! hasRatio (t, 13.0), "11 and 13 excluded");
        check (hasRatio (t, 12.0) && hasRatio (t, 10.0), "12 and 10 kept");
    }

    //--------------------------------------------------------------------------
    std::printf ("fractal L=2 (base {1,2,3}, slope=1):\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 3; s.selection = Selection::All;
        s.curveSlope = 1.0f; s.fractalLevel = 2.0f;
        auto t = compileSpectrum (s);
        check (t.partials.size() == 6, "6 merged partials {1,2,3,4,6,9}");
        check (hasRatio (t, 1) && hasRatio (t, 2) && hasRatio (t, 3)
               && hasRatio (t, 4) && hasRatio (t, 6) && hasRatio (t, 9), "ratios 1,2,3,4,6,9 present");
        float a1 = ampAtRatio (t, 1.0);
        check (a1 > 0 && std::abs (ampAtRatio (t, 2.0) / a1 - 1.0)    < 2e-3, "amp(2)/amp(1) ~ 1.0 (coincidence sum)");
        check (a1 > 0 && std::abs (ampAtRatio (t, 4.0) / a1 - 0.25)   < 2e-3, "amp(4)/amp(1) ~ 0.25");
        check (a1 > 0 && std::abs (ampAtRatio (t, 9.0) / a1 - 0.1111) < 2e-3, "amp(9)/amp(1) ~ 0.1111");
    }

    //--------------------------------------------------------------------------
    std::printf ("fractal = a copy of the spectrum built on EVERY partial:\n");
    {
        // The definition being checked: at level 2 the partial set must be exactly
        // the union over every base partial r of (r * the whole base set).
        SpectrumSettings s; s.baseN = 1; s.reach = 5; s.selection = Selection::All;
        s.curveSlope = 0.0f;
        s.fractalLevel = 1.0f; auto base = compileSpectrum (s);
        s.fractalLevel = 2.0f; auto lvl2 = compileSpectrum (s);

        // `reach` bounds the BASE set; the copies built on each partial legitimately
        // reach higher (that is the point), so every product counts.
        std::vector<double> expect;
        for (const auto& a : base.partials)
            for (const auto& b : base.partials)
                expect.push_back ((double) a.ratio * (double) b.ratio);
        std::sort (expect.begin(), expect.end());
        expect.erase (std::unique (expect.begin(), expect.end(),
                                   [] (double x, double y) { return std::abs (x - y) < 1e-6; }), expect.end());

        bool allThere = true;
        for (const double r : expect) if (! hasRatio (lvl2, r)) { allThere = false; break; }
        bool noExtra = true;
        for (const auto& p : lvl2.partials)
        {
            bool found = false;
            for (const double r : expect) if (std::abs (r - (double) p.ratio) < 1e-3) { found = true; break; }
            if (! found) { noExtra = false; break; }
        }
        std::printf ("   (base %d partials -> level 2 has %d, expected %d)\n",
                     (int) base.partials.size(), (int) lvl2.partials.size(), (int) expect.size());
        check (allThere, "every partial of the base spawns a full scaled copy of the spectrum");
        check (noExtra,  "and NOTHING else appears (the set is exactly that union)");
    }

    //--------------------------------------------------------------------------
    std::printf ("fractional fractal level (continuous fader, low->high entry):\n");
    {
        // Base {1,2,3}: level 1 = {1,2,3}; level 2 = {1,2,3,4,6,9}. The NEW partials
        // at level 2 are 4, 6, 9 - they must enter LOW to HIGH as the level rises,
        // and the endpoints must reproduce the integer levels exactly.
        SpectrumSettings s; s.baseN = 1; s.reach = 3; s.selection = Selection::All;
        s.curveSlope = 1.0f;
        s.fractalLevel = 1.0f; auto t1 = compileSpectrum (s);
        s.fractalLevel = 2.0f; auto t2 = compileSpectrum (s);
        check (t1.partials.size() == 3 && t2.partials.size() == 6, "endpoints: 3 and 6 partials");

        s.fractalLevel = 1.2f; auto ta = compileSpectrum (s);
        s.fractalLevel = 1.5f; auto tb = compileSpectrum (s);
        s.fractalLevel = 1.9f; auto tc = compileSpectrum (s);
        check (hasRatio (ta, 4.0) && ! hasRatio (ta, 9.0),
               "at 1.2 the LOWEST new partial (4) is in, the highest (9) is not");
        check (hasRatio (tb, 4.0) && hasRatio (tb, 6.0) && ! hasRatio (tb, 9.0),
               "at 1.5 partials 4 and 6 are in, 9 still out (low->high)");
        check (hasRatio (tc, 4.0) && hasRatio (tc, 6.0) && hasRatio (tc, 9.0),
               "at 1.9 all three new partials are in");
        check (ta.partials.size() <= tb.partials.size() && tb.partials.size() <= tc.partials.size(),
               "partial count grows monotonically with the fractal level");
        // Continuity at the top end: 1.99 must be within one partial of level 2.
        s.fractalLevel = 1.99f; auto td = compileSpectrum (s);
        check (td.partials.size() == t2.partials.size(), "1.99 already has level 2's partial set");
        // The blended amplitudes must stay RMS-normalised like every other template.
        double sq = 0.0; for (const auto& p : tb.partials) sq += (double) p.amp * (double) p.amp;
        check (std::abs (sq - 1.0) < 1e-3, "blended template is RMS-normalised");
    }

    //--------------------------------------------------------------------------
    std::printf ("continuous Harmonics axis (Even - All - Odd - Prime):\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 9; s.curveSlope = 0.0f;
        s.selection = (float) Selection::Even; auto tE = compileSpectrum (s);
        s.selection = (float) Selection::All;  auto tA = compileSpectrum (s);
        s.selection = (float) Selection::Odd;  auto tO = compileSpectrum (s);
        check (! hasRatio (tE, 3.0) && hasRatio (tE, 2.0), "Even: harmonic 3 out, 2 in");
        check (hasRatio (tA, 3.0) && hasRatio (tA, 2.0),   "All: both in");
        check (hasRatio (tO, 3.0) && ! hasRatio (tO, 2.0), "Odd: 3 in, 2 out (fundamental aside)");
        // Half-way Even<->All: the odd harmonics (in All only) come in at HALF weight,
        // the even ones (in both) stay at full - the timbre morphs, it does not switch.
        s.selection = 0.5f; auto tHalf = compileSpectrum (s);
        const float a2 = ampAtRatio (tHalf, 2.0), a3 = ampAtRatio (tHalf, 3.0);
        check (a2 > 0.0f && a3 > 0.0f, "half-way: both even and odd harmonics are present");
        check (a2 > 0.0f && std::abs (a3 / a2 - 0.5f) < 1e-3f, "half-way: the odd ones sound at half amplitude");
    }

    //--------------------------------------------------------------------------
    std::printf ("temper mode snaps partials onto the scale grid and merges coincidences:\n");
    {
        // A 1-degree "scale" (bare octaves): every harmonic must land on a power of 2,
        // duplicates must MERGE (no coherent stacking), and the played pitch stays at 1.
        SpectrumSettings s; s.baseN = 1; s.reach = 9; s.selection = Selection::All;
        s.curveSlope = 0.0f;
        s.temperToTimbre = true; s.timbreDegrees = { 1.0f }; s.timbrePeriod = 2.0f;
        auto t = compileSpectrum (s);
        check (std::abs (t.partials.front().ratio - 1.0f) < 1e-3f, "played pitch still at ratio 1");
        bool allOctaves = true, distinct = true;
        for (size_t i = 0; i < t.partials.size(); ++i)
        {
            const double l = std::log2 ((double) t.partials[i].ratio);
            if (std::abs (l - std::round (l)) > 1e-3) allOctaves = false;
            if (i > 0 && std::abs ((double) t.partials[i].ratio - (double) t.partials[i - 1].ratio) < 1e-6)
                distinct = false;
        }
        check (allOctaves, "every tempered partial sits on the scale grid (a power of 2 here)");
        check (distinct, "coincident partials were merged, not duplicated");
    }

    //--------------------------------------------------------------------------
    std::printf ("temper with a DENSE scale (311-EDO): exact grid, no collapse, fast:\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 64; s.curveSlope = 1.0f;
        s.temperToTimbre = true; s.timbrePeriod = 2.0f;
        s.timbreDegrees.clear();
        for (int d = 0; d < 311; ++d) s.timbreDegrees.push_back ((float) std::pow (2.0, (double) d / 311.0));
        const auto t0 = std::chrono::steady_clock::now();
        auto t = compileSpectrum (s);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("   (%d partials, %.0f ms)\n", (int) t.partials.size(), ms);
        check (! t.partials.empty(), "the dense-grid spectrum is not empty");
        // Every partial must sit on a 311-EDO step (within half a step = 1.93 c).
        bool onGrid = true;
        for (const auto& p : t.partials)
        {
            const double steps = 311.0 * std::log2 (std::max (1.0e-9, (double) p.ratio));
            if (std::abs (steps - std::round (steps)) > 0.01) { onGrid = false; break; }
        }
        check (onGrid, "every partial lands exactly on a scale degree of the dense grid");
        check (t.partials.size() > 30, "a fine grid does NOT collapse the spectrum onto a few pitches");
        check (ms < 2000.0, "the dense-grid snap stays fast (no seconds-long rebuild)");
    }

    //--------------------------------------------------------------------------
    std::printf ("amplitude curve is relative to the lowest present harmonic:\n");
    {
        // N=6, slope=1: lowest present (k=6) must be the loudest (amp 1 before norm),
        // NOT 1/6. Check the roll-off is (k/6)^-1, i.e. amp(7/6)/amp(1) = 6/7.
        SpectrumSettings s; s.baseN = 6; s.baseMode = BaseMode::Transposed; s.reach = 4;
        s.selection = Selection::All; s.curveSlope = 1.0f;
        auto t = compileSpectrum (s);
        float a1 = ampAtRatio (t, 1.0), a2 = ampAtRatio (t, 7.0 / 6.0);
        check (a1 > 0 && a2 > 0 && a1 >= a2, "lowest present partial is the loudest");
        check (a1 > 0 && std::abs (a2 / a1 - 6.0 / 7.0) < 2e-3, "roll-off is (k/kMin)^-1, not k^-1");
    }

    //--------------------------------------------------------------------------
    std::printf ("raising base N adds harmonics (does not shrink the set):\n");
    {
        SpectrumSettings a; a.baseN = 1; a.reach = 8; a.selection = Selection::All; a.curveSlope = 1.0f;
        SpectrumSettings b = a; b.baseN = 4; // grave: harmonics 4..32
        auto ta = compileSpectrum (a);
        auto tb = compileSpectrum (b);
        check (tb.partials.size() > ta.partials.size(), "N=4 has MORE partials than N=1 (denser, richer)");
        check (std::abs ((double) tb.partials.front().ratio - 1.0) < 1e-3, "lowest partial still at played pitch (grave)");
    }

    //--------------------------------------------------------------------------
    std::printf ("Transposed mode always lands the lowest partial on the played pitch:\n");
    {
        // Prime whitelist excludes harmonic 1 (not prime).
        SpectrumSettings t; t.baseN = 1; t.reach = 12; t.selection = Selection::Prime;
        t.curveSlope = 0.0f; t.baseMode = BaseMode::Transposed;
        auto tt = compileSpectrum (t);
        check (std::abs (tt.partials.front().ratio - 1.0f) < 1e-3f,
               "Transposed + Prime (harmonic 1 excluded): lowest still at played pitch");

        // The FUNDAMENTAL (k == N) is now ALWAYS kept by the selection filter -
        // otherwise Transposed "Even" would collapse onto a full 1,2,3,4 series
        // (identical to All) and the whitelist would audibly do nothing. So in
        // Original mode with Prime, the lowest partial is harmonic 1 (the kept
        // fundamental), with harmonic 2 (prime) present above it.
        SpectrumSettings o = t; o.baseMode = BaseMode::Original;
        auto oo = compileSpectrum (o);
        check (std::abs (oo.partials.front().ratio - 1.0f) < 1e-3f,
               "Original + Prime: fundamental kept (lowest at harmonic 1)");
        {
            bool hasH2 = false;
            for (const auto& p : oo.partials) if (std::abs (p.ratio - 2.0f) < 1e-3f) hasH2 = true;
            check (hasH2, "Original + Prime: harmonic 2 present above the kept fundamental");
        }

        // Prime-limit 2 = octaves. In Transposed mode the sound is independent of base N
        // (this is the bug the user hit: base 3 used to sound a fourth higher).
        SpectrumSettings a; a.baseN = 1; a.reach = 64; a.selection = Selection::All;
        a.curveSlope = 1.0f; a.primeLimit = 2; a.baseMode = BaseMode::Transposed;
        SpectrumSettings b = a; b.baseN = 3; // 3 is not a power of 2
        auto ta = compileSpectrum (a);
        auto tb = compileSpectrum (b);
        check (std::abs (ta.partials.front().ratio - 1.0f) < 1e-3f
               && std::abs (tb.partials.front().ratio - 1.0f) < 1e-3f,
               "Prime-limit 2 + Transposed: lowest at played pitch for both N=1 and N=3");
        check (hasRatio (ta, 2.0) && hasRatio (ta, 4.0) && hasRatio (tb, 2.0) && hasRatio (tb, 4.0),
               "Prime-limit 2: octave ratios present regardless of base");
    }

    //--------------------------------------------------------------------------
    std::printf ("prime whitelist (only primes 2 and 3 allowed):\n");
    {
        const unsigned only23 = 0b11u; // bit0=2, bit1=3
        check (primeWhitelistOk (6, only23) && primeWhitelistOk (12, only23) && primeWhitelistOk (1, only23),
               "whitelist {2,3} keeps 1, 6, 12");
        check (! primeWhitelistOk (5, only23) && ! primeWhitelistOk (10, only23) && ! primeWhitelistOk (7, only23),
               "whitelist {2,3} drops 5, 7, 10");

        SpectrumSettings s; s.baseN = 1; s.reach = 16; s.selection = Selection::All; s.curveSlope = 0.0f;
        s.primeUseWhitelist = true; s.primeWhitelistMask = only23;
        auto t = compileSpectrum (s);
        check (hasRatio (t, 2) && hasRatio (t, 3) && hasRatio (t, 4) && hasRatio (t, 6)
               && hasRatio (t, 8) && hasRatio (t, 9), "3-smooth harmonics 2,3,4,6,8,9 present");
        check (! hasRatio (t, 5) && ! hasRatio (t, 7) && ! hasRatio (t, 10) && ! hasRatio (t, 11),
               "5,7,10,11 absent");
    }

    //--------------------------------------------------------------------------
    std::printf ("Spectrum range: hard-cut N octaves above the fundamental:\n");
    {
        SpectrumSettings s; s.baseN = 1; s.reach = 64; s.selection = Selection::All; s.curveSlope = 0.0f;
        s.spectrumRangeOct = 3; // keep ratio <= 2^3 = 8
        auto t = compileSpectrum (s);
        double maxR = 0.0; for (const auto& p : t.partials) maxR = std::max (maxR, (double) p.ratio);
        check (maxR <= 8.0 + 1e-3, "no partial above 2^3 = 8");
        check (hasRatio (t, 8.0), "ratio 8 kept (exactly at the edge)");
        check (! hasRatio (t, 9.0) && ! hasRatio (t, 16.0), "ratios 9 and 16 cut");

        SpectrumSettings full = s; full.spectrumRangeOct = 10; // full = no cut
        auto tf = compileSpectrum (full);
        check (hasRatio (tf, 16.0) && hasRatio (tf, 32.0), "range=10 (full) keeps 16 and 32");
    }

    //--------------------------------------------------------------------------
    std::printf ("Scala-as-timbre: degrees stacked over periods become the partials:\n");
    {
        // A 12-TET scale (12 degrees), 2 periods, flat slope, no fractal.
        SpectrumSettings s;
        s.source = Source::ScalaTimbre;
        s.timbrePeriod = 2.0f;
        s.timbrePeriodRepeats = 2;
        s.curveSlope = 0.0f;
        s.fractalLevel = 1.0f;
        s.timbreDegrees.clear();
        for (int k = 0; k < 12; ++k) s.timbreDegrees.push_back ((float) std::pow (2.0, k / 12.0));

        auto t = compileSpectrum (s);
        check (t.partials.size() == 24, "2 periods x 12 degrees = 24 partials");
        check (std::abs ((double) t.partials.front().ratio - 1.0) < 1e-4, "lowest partial = played pitch (1.0)");
        check (hasRatio (t, 2.0, 2e-3), "octave (period) present at ratio 2.0");
        check (hasRatio (t, std::pow (2.0, 7.0 / 12.0), 2e-3), "the fifth (2^{7/12}) present");
        check (! hasRatio (t, 1.5, 1e-3), "12-TET fifth is NOT a pure 3/2 (it is 2^{7/12})");
    }

    //--------------------------------------------------------------------------
    std::printf ("Scala-as-timbre: more period repeats reach higher (more partials):\n");
    {
        SpectrumSettings a; a.source = Source::ScalaTimbre; a.timbrePeriod = 2.0f;
        a.curveSlope = 0.0f; a.fractalLevel = 1.0f; a.timbrePeriodRepeats = 2;
        for (int k = 0; k < 5; ++k) a.timbreDegrees.push_back ((float) std::pow (2.0, k / 5.0)); // 5-EDO
        SpectrumSettings b = a; b.timbrePeriodRepeats = 4;
        auto ta = compileSpectrum (a);
        auto tb = compileSpectrum (b);
        check (tb.partials.size() > ta.partials.size(), "4 repeats has more partials than 2");
        check ((double) tb.partials.back().ratio > (double) ta.partials.back().ratio, "and reaches a higher top ratio");
    }

    //--------------------------------------------------------------------------
    std::printf ("Sethares dissonance curve (harmonic timbre -> minima at simple ratios):\n");
    {
        // Classic reference case: 6 harmonic partials, amplitudes 0.88^k.
        std::vector<dissonance::Partial> spec;
        for (int k = 0; k < 6; ++k)
            spec.push_back ({ 261.63 * (k + 1), std::pow (0.88, (double) k) });

        auto curve = dissonance::dissonanceCurve (spec, 2.0, 1.0);
        check ((int) curve.size() == 1200, "1-cent curve over an octave has 1200 points");

        // Unison-adjacent dissonance must RISE from near-zero, and the curve must
        // come back down near the octave (2/1 is consonant for harmonic timbres).
        check (curve.front().second < curve[80].second, "dissonance rises away from unison");
        check (curve.back().second < curve[1000].second, "dissonance falls again toward the octave");

        auto minima = dissonance::pickExtremes (spec, curve, 6, false, 2.0);
        check ((int) minima.size() == 6, "6 minima found");
        auto hasNear = [&] (double cents, double tol)
        {
            for (const auto& e : minima)
                if (std::abs (1200.0 * std::log2 (e.ratio) - cents) < tol) return true;
            return false;
        };
        // Sub-cent refinement: the fifth must land on the JUST 3/2 (701.955c), not
        // a rounded 702; likewise 4/3 (498.04c) and 5/4 (386.31c) must appear.
        check (hasNear (701.955, 0.2), "minimum on the JUST fifth 3/2 (701.955c, sub-cent precision)");
        check (hasNear (498.04, 2.0), "minimum on the just fourth 4/3 (498c)");
        check (hasNear (386.31, 3.0), "minimum on the just major third 5/4 (386c)");
        // Heights: every minimum's dissonance must be below the curve's maximum.
        {
            double maxC = 0.0; for (const auto& c : curve) maxC = std::max (maxC, c.second);
            for (const auto& e : minima)
                check (e.value < maxC, "minimum height below the curve maximum");
        }

        auto maxima = dissonance::pickExtremes (spec, curve, 4, true, 2.0);
        check ((int) maxima.size() >= 1, "reverse mode finds maxima");
        for (const auto& e : maxima)
            check (e.ratio > 1.0 && e.ratio < 2.0, "maxima strictly inside the period");
    }

    //--------------------------------------------------------------------------
    std::printf ("Just-interval labels (STRICT: fraction only on exact match, else cents):\n");
    {
        using dissonance::justIntervalName;
        check (justIntervalName (1.5) == "3/2", "1.5 -> exact 3/2");
        check (justIntervalName (5.0/4.0) == "5/4", "1.25 -> exact 5/4");
        // Labels are LITERAL: no octave folding (a tritave-scale degree of 5/2
        // must never be shown as 5/4).
        check (justIntervalName (2.0) == "2/1", "2.0 -> 2/1 (literal, not folded to 1/1)");
        check (justIntervalName (2.5) == "5/2", "2.5 -> 5/2 (non-octave degree, literal)");
        check (justIntervalName (3.0) == "3/1", "3.0 -> 3/1 (tritave period, literal)");
        // 12-TET intervals are NOT exact just ratios -> cents, never a fraction.
        check (justIntervalName (std::pow (2.0, 7.0/12.0)) == "700¢", "12-TET fifth -> 700 cent-sign (not 3/2)");
        check (justIntervalName (std::pow (2.0, 4.0/12.0)) == "400¢", "12-TET third -> 400 cent-sign (not 5/4)");
        {
            const auto s = justIntervalName (1.37); // ~545c, no exact simple ratio
            check (! s.empty() && (unsigned char) s.back() == 0xa2, "inexact interval -> cents label");
        }
        check (justIntervalName (0.0).empty() && justIntervalName (-1.0).empty(), "non-positive ratio -> blank");

        long long p = 0, q = 0;
        dissonance::bestFraction (1.5, p, q);
        check (p == 3 && q == 2, "bestFraction(1.5) = 3/2");
        dissonance::bestFraction (std::pow (2.0, 4.0/12.0), p, q);
        check (p == 5 && q == 4, "bestFraction(12-TET third) = 5/4 (for the hover hint)");
    }

    //--------------------------------------------------------------------------
    std::printf ("N-note chords (Sethares union measure, TTSS 6.8/App.E):\n");
    {
        // A harmonic timbre, 7 partials, saw-ish roll-off, anchored at middle C.
        std::vector<dissonance::Partial> spec;
        for (int k = 1; k <= 7; ++k)
            spec.push_back ({ 261.6255653 * k, 1.0 / k });

        // (1) The grid decomposition used by the search must equal the true union
        //     measure EXACTLY (they are the same pairs, just grouped).
        {
            const std::vector<double> tri { 1.0, 1.25, 1.5 };
            const double whole = dissonance::chordDissonance (spec, tri);
            double dec = 0.0;
            for (const double a : tri) dec += dissonance::selfDissonance (spec, a);
            dec += dissonance::crossDissonance (spec, 1.0, 1.25);
            dec += dissonance::crossDissonance (spec, 1.0, 1.5);
            dec += dissonance::crossDissonance (spec, 1.25, 1.5);
            check (std::abs (whole - dec) < 1.0e-9 * std::max (1.0, std::abs (whole)),
                   "self+cross decomposition == dissmeasure of the union (exact)");
        }

        // (2) Sanity of the measure itself: the just major triad must be far more
        //     consonant than a cluster of seconds for a harmonic timbre.
        {
            const double good = dissonance::chordDissonance (spec, { 1.0, 1.25, 1.5 });
            const double bad  = dissonance::chordDissonance (spec, { 1.0, std::pow (2.0, 1.0/12.0), std::pow (2.0, 2.0/12.0) });
            check (good < bad, "just major triad beats a semitone cluster (harmonic timbre)");
        }

        // (3) The full search (3 notes in an octave) must FIND recognisable just
        //     triads: some result within 12 cents of 5/4 or 3/2 style intervals.
        {
            auto res = dissonance::searchChords (spec, 2.0, 3, 8, false);
            check (! res.empty(), "chord search returns results");
            bool foundJustish = false;
            for (const auto& r : res)
                for (size_t k = 1; k < r.alphas.size(); ++k)
                {
                    const double cents = 1200.0 * std::log2 (r.alphas[k]);
                    for (const double just : { 386.31, 498.04, 701.955, 884.36 }) // 5/4 4/3 3/2 5/3
                        if (std::abs (cents - just) < 12.0) foundJustish = true;
                }
            check (foundJustish, "top consonant triads include just-interval notes (5/4, 4/3, 3/2, 5/3)");
            // Ranking is by ascending dissonance in minima mode.
            for (size_t i = 1; i < res.size(); ++i)
                check (res[i].value >= res[i-1].value - 1.0e-12, "minima ranking is ascending in dissonance");
            // Every note strictly inside (1, period).
            for (const auto& r : res)
                for (size_t k = 1; k < r.alphas.size(); ++k)
                    check (r.alphas[k] > 1.0 && r.alphas[k] < 2.0, "chord notes strictly inside the period");
        }

        // (4) Maxima mode ranks descending and returns something.
        {
            auto res = dissonance::searchChords (spec, 2.0, 3, 4, true);
            check (! res.empty(), "maxima chord search returns results");
            for (size_t i = 1; i < res.size(); ++i)
                check (res[i].value <= res[i-1].value + 1.0e-12, "maxima ranking is descending in dissonance");
        }

        // (5) SCOPE: "0 vs all" scores only self + beating against the root, so it is
        //     a genuinely different surface from "all vs all" - the two must not be
        //     the same search wearing two names.
        {
            auto all  = dissonance::searchChords (spec, 2.0, 4, 6, false, 33.0, false);
            auto root = dissonance::searchChords (spec, 2.0, 4, 6, false, 33.0, true);
            check (! root.empty(), "0-vs-all chord search returns results");
            // The reported value IS the root-only measure, not the full union one.
            if (! root.empty())
            {
                const auto& a = root[0].alphas;
                double expect = 0.0;
                for (size_t i = 0; i < a.size(); ++i)
                {
                    expect += dissonance::selfDissonance (spec, a[i]);
                    if (i > 0) expect += dissonance::crossDissonance (spec, a[0], a[i]);
                }
                check (std::abs (root[0].value - expect) < 1.0e-6,
                       "its value is the 0-vs-all measure (self + beating against the root)");
                check (std::abs (root[0].value - dissonance::chordDissonance (spec, a)) > 1.0e-6,
                       "which is NOT the all-vs-all measure of the same chord");
            }
            bool differs = all.size() != root.size();
            for (size_t i = 0; i < std::min (all.size(), root.size()) && ! differs; ++i)
                for (size_t k = 1; k < all[i].alphas.size() && ! differs; ++k)
                    if (std::abs (1200.0 * std::log2 (all[i].alphas[k] / root[i].alphas[k])) > 1.0)
                        differs = true;
            check (differs, "0 vs all and all vs all do NOT return the same chords");
        }
    }

    //--------------------------------------------------------------------------
    // THE INVERSE SEARCH: the notes are given and the PARTIALS move (adaptSpectrum).
    {
        std::printf ("\nadapting the spectrum to a fixed set of notes:\n");
        constexpr double f0 = 261.6255653;                 // C3, same anchor the plugin uses
        // A 31-EDO major triad: 0, 10, 18 steps. This is the case the panel advertises.
        const std::vector<double> triad { 1.0, std::pow (2.0, 10.0 / 31.0), std::pow (2.0, 18.0 / 31.0) };

        const int N = 8;
        const double minGap = 50.0, period = 2.0;
        std::vector<dissonance::Partial> seed;
        for (int i = 0; i < N; ++i)
        {
            const double r = std::exp2 (1200.0 * (double) i / (double) N / 1200.0);
            seed.push_back ({ f0 * r, std::pow (r, -1.0) });
        }

        const auto lo = dissonance::adaptSpectrum (seed, f0, triad, false, dissonance::Goal::Min,
                                                   0.0, 0.0, minGap, period, true, 24.0, 6);
        const auto hi = dissonance::adaptSpectrum (seed, f0, triad, false, dissonance::Goal::Max,
                                                   0.0, 0.0, minGap, period, true, 24.0, 6);
        std::printf ("  31-EDO major triad, %d partials:  seed %.3f   min %.3f   max %.3f\n",
                     N, lo.seedScore, lo.score, hi.score);
        check (lo.score < lo.seedScore * 0.9,
               "Min really lowers the beating of that chord (>10% under the harmonic seed)");
        check (hi.score > hi.seedScore * 1.05,
               "Max really raises it");
        check ((int) lo.spectrum.size() == N && (int) hi.spectrum.size() == N,
               "and both come back with exactly the partials they were given");

        // THE FIDELITY RULE, as a number: no partial may be merged into another or
        // squeezed past its neighbour. Without the clamp a minimisation stacks partials
        // (coincident partials beat with nothing), and the spectrum silently loses notes.
        double worstGap = 1.0e9;
        bool   ordered  = true;
        for (const auto& r : { lo, hi })
            for (size_t i = 1; i < r.spectrum.size(); ++i)
            {
                const double g = 1200.0 * std::log2 (r.spectrum[i].freq / r.spectrum[i - 1].freq);
                worstGap = std::min (worstGap, g);
                if (! (g > 0.0)) ordered = false;
            }
        std::printf ("  closest two partials in either result: %.2f cents (asked for %.0f)\n",
                     worstGap, minGap);
        check (ordered && worstGap >= minGap - 0.05,
               "no two partials come closer than the smallest interval - nothing merges");

        // ...and none of them is silenced: a muted partial is a deleted partial.
        double quietest = 1.0e9, loudest = 0.0;
        for (const auto& p : lo.spectrum) { quietest = std::min (quietest, p.amp); loudest = std::max (loudest, p.amp); }
        std::printf ("  amplitude spread after shaping: quietest %.5f, loudest %.5f\n", quietest, loudest);
        check (quietest > 0.0, "every adapted partial keeps a real amplitude");

        // AMPLITUDE SHAPING MUST BE SHAPE, NOT LEVEL. dissmeasure is min(ai,aj) summed over
        // pairs - homogeneous of degree 1 - so scaling the whole spectrum down scales the
        // dissonance down with it. That is a free win the search WILL take: before the
        // objective pinned the peak, every partial came back at exactly the bottom of its
        // dB bound with its shape untouched, reporting a huge improvement that was nothing
        // but a volume cut (and one the apply step renormalises straight back out).
        {
            double seedPeak = 0.0;
            for (const auto& p : seed) seedPeak = std::max (seedPeak, p.amp);
            check (std::abs (loudest - seedPeak) < 1.0e-9,
                   "the adapted spectrum keeps the seed's PEAK level (so the cost cannot be gamed by volume)");
            double worstShapeMove = 0.0;
            for (size_t i = 0; i < lo.spectrum.size() && i < seed.size(); ++i)
                worstShapeMove = std::max (worstShapeMove,
                                           std::abs (lo.spectrum[i].amp / loudest - seed[i].amp / seedPeak));
            std::printf ("  biggest change to the amplitude SHAPE: %.4f of full scale\n", worstShapeMove);
            check (worstShapeMove > 0.05, "and it really reshaped them rather than just turning them down");
        }

        // Every partial stays inside (1, period]: above it, the Custom Engine's own period
        // stacking would bring it back as a duplicate of a lower one.
        bool inBand = true;
        for (const auto& p : lo.spectrum)
            if (p.freq < f0 - 1.0e-9 || p.freq > f0 * period + 1.0e-6) inBand = false;
        check (inBand && std::abs (lo.spectrum[0].freq - f0) < 1.0e-9,
               "partial 0 stays at the played pitch and none escapes the period");

        // A RANGE target is a real third option, not min or max wearing a hat: ask for a
        // beating value between the two extremes and it must land nearer to it than either.
        {
            // A band the SEED is not already in, or the search would land in it by doing
            // nothing at all and the test could never fail.
            const double bLo = lo.score * 4.0, bHi = lo.score * 6.0;
            const auto rg = dissonance::adaptSpectrum (seed, f0, triad, false, dissonance::Goal::Range,
                                                       bLo, bHi, minGap, period, true, 24.0, 6);
            std::printf ("  range [%.3f, %.3f] -> %.3f   (seed %.3f, min %.3f, max %.3f)\n",
                         bLo, bHi, rg.score, rg.seedScore, lo.score, hi.score);
            check (rg.seedScore < bLo || rg.seedScore > bHi,
                   "the range band starts out of reach of the seed (so the test can fail)");
            check (rg.score >= bLo - 1.0e-6 && rg.score <= bHi + 1.0e-6,
                   "Range lands INSIDE the band it was given");
            check (rg.score > lo.score * 1.5,
                   "and stops short of the minimum instead of sailing past it");
        }

        // Shaping amplitudes is not decoration: with it allowed the search must reach at
        // least as low as with positions alone (it is a strict superset of the moves).
        {
            const auto posOnly = dissonance::adaptSpectrum (seed, f0, triad, false, dissonance::Goal::Min,
                                                            0.0, 0.0, minGap, period, false, 24.0, 6);
            std::printf ("  positions only %.3f   positions + amplitudes %.3f\n", posOnly.score, lo.score);
            check (lo.score <= posOnly.score + 1.0e-9,
                   "letting it move amplitudes too never does worse than positions alone");
            bool ampsMoved = false;
            for (size_t i = 0; i < posOnly.spectrum.size() && i < lo.spectrum.size(); ++i)
                if (std::abs (posOnly.spectrum[i].amp - lo.spectrum[i].amp) > 1.0e-6) ampsMoved = true;
            check (ampsMoved, "and it really does move them");
        }

        // The SCOPE means the same thing here as in the forward search.
        {
            const std::vector<double> a { 1.0, 1.25, 1.5 };
            check (std::abs (dissonance::targetScore (seed, a, false)
                             - dissonance::chordDissonance (seed, a)) < 1.0e-9,
                   "all-vs-all target score IS the union measure");
            double expect = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                expect += dissonance::selfDissonance (seed, a[i]);
                if (i > 0) expect += dissonance::crossDissonance (seed, a[0], a[i]);
            }
            check (std::abs (dissonance::targetScore (seed, a, true) - expect) < 1.0e-9,
                   "0-vs-all target score is self + beating against the root");
        }

        // goalCost's contract, since the whole search minimises it.
        check (dissonance::goalCost (3.0, dissonance::Goal::Min, 0, 0) == 3.0
               && dissonance::goalCost (3.0, dissonance::Goal::Max, 0, 0) == -3.0
               && dissonance::goalCost (3.0, dissonance::Goal::Range, 2.0, 4.0) == 0.0
               && std::abs (dissonance::goalCost (5.0, dissonance::Goal::Range, 2.0, 4.0) - 1.0) < 1.0e-12
               && std::abs (dissonance::goalCost (1.0, dissonance::Goal::Range, 2.0, 4.0) - 1.0) < 1.0e-12,
               "goalCost: min, max, and zero cost anywhere inside the range");
    }

    //--------------------------------------------------------------------------
    // Fractal must be CONTINUOUS: nudging it off "off" is a nudge, not a new timbre.
    // "Raising fractal from 0 to a little more changed the sound irreversibly."
    // The fader quantises to 0.01, so 1.01 is the smallest move a user can make; it
    // used to admit ~1% of the next level's partials at FULL amplitude, and the RMS
    // normalisation then pushed everything else down ~20 dB.
    {
        std::printf ("\nfractal continuity (a 1%% move must be a 1%% change):\n");
        auto tmplAt = [] (float level, int baseN)
        {
            SpectrumSettings s;
            s.baseN        = baseN;
            s.fractalLevel = level;
            s.curveSlope   = 1.0f;
            return compileSpectrum (s);
        };
        // Cosine similarity of the two amplitude spectra, matched by ~1 cent bins.
        // Both templates are RMS-normalised, so 1.0 = identical timbre, 0 = disjoint.
        auto similarity = [] (const SpectrumTemplate& a, const SpectrumTemplate& b)
        {
            const auto key = [] (float r)
            { return (long long) std::llround (1200.0 * std::log2 (std::max (1.0e-9, (double) r))); };
            std::map<long long, double> A, B;
            for (const auto& p : a.partials) A[key (p.ratio)] += (double) p.amp;
            for (const auto& p : b.partials) B[key (p.ratio)] += (double) p.amp;
            double dot = 0.0, na = 0.0, nb = 0.0;
            for (const auto& kv : A) { na += kv.second * kv.second;
                                       const auto it = B.find (kv.first);
                                       if (it != B.end()) dot += kv.second * it->second; }
            for (const auto& kv : B) nb += kv.second * kv.second;
            return (na > 0.0 && nb > 0.0) ? dot / std::sqrt (na * nb) : 0.0;
        };

        for (const int N : { 1, 16 })
        {
            const auto off  = tmplAt (1.00f, N);
            const auto nudge= tmplAt (1.01f, N);
            const auto half = tmplAt (1.50f, N);
            const auto full = tmplAt (2.00f, N);
            const double sNudge = similarity (off, nudge);
            const double sHalf  = similarity (off, half);
            const double sFull  = similarity (off, full);
            std::printf ("  Base %-3d  sim(1.00,1.01)=%.4f  sim(1.00,1.50)=%.4f  sim(1.00,2.00)=%.4f\n",
                         N, sNudge, sHalf, sFull);
            std::printf ("            partials: off=%d nudge=%d full=%d   truncated: off=%d nudge=%d full=%d\n",
                         (int) off.partials.size(), (int) nudge.partials.size(), (int) full.partials.size(),
                         (int) off.truncated, (int) nudge.truncated, (int) full.truncated);
            check (sNudge > 0.97,
                   "the smallest possible fractal move barely changes the timbre");
            check (sNudge > sHalf && sHalf >= sFull - 1.0e-9,
                   "and the timbre moves away monotonically as fractal rises");
        }
        // The endpoints must still be EXACT - the ramp must not smear level 2 into 1.
        {
            SpectrumSettings s; s.baseN = 4; s.curveSlope = 1.0f;
            s.fractalLevel = 2.0f;  const auto exact = compileSpectrum (s);
            s.fractalLevel = 1.999f; const auto near_ = compileSpectrum (s);
            check (similarity (exact, near_) > 0.99,
                   "and fractal just below an integer level matches that level");
        }
    }

    //--------------------------------------------------------------------------
    // Per-partial amplitude edits: they must stay attached to the PARTIAL the user
    // edited, not to a slot number, because every rebuild re-sorts and reshapes the
    // set. An edit that drifted onto a neighbour would be worse than no feature.
    {
        std::printf ("\npartial-gain identity:\n");

        check (gainKey (1.0) == 0 && gainKey (2.0) == 120000,
               "the key grid is hundredths of a cent, anchored at ratio 1");
        {
            // Half a bin is 0.005 cents, i.e. ~2.9e-6 in relative terms, so the round
            // trip can never be tighter than that - the grid IS the quantisation.
            bool roundTrips = true;
            for (double r : { 1.0, 1.5, 3.0, 7.0, 1023.5 })
                if (std::abs (gainKeyRatio (gainKey (r)) - r) > 1.0e-5 * r) roundTrips = false;
            check (roundTrips, "a key converts back to the ratio it came from, to within half a bin");
        }

        auto spectrumOf = [] (int baseN, float level, BaseMode mode, int primeLimit)
        {
            SpectrumSettings s;
            s.baseN = baseN; s.reach = 16; s.selection = Selection::All;
            s.curveSlope = 1.0f; s.fractalLevel = level; s.baseMode = mode;
            s.primeLimit = primeLimit;
            return compileSpectrum (s);
        };
        auto indexOfRatio = [] (const SpectrumTemplate& t, double r)
        {
            for (size_t i = 0; i < t.partials.size(); ++i)
                if (std::abs ((double) t.partials[i].ratio - r) < 1.0e-3) return (int) i;
            return -1;
        };

        const auto plain = spectrumOf (1, 1.0f, BaseMode::Transposed, 0);
        std::vector<float> scale;

        // Edits land on exactly the partials asked for, and on nothing else.
        {
            std::vector<PartialGain> ov;
            setPartialGain (ov, gainKey (2.0), -6.0f);
            setPartialGain (ov, gainKey (3.0), -6.0f);
            setPartialGain (ov, gainKey (5.0), -6.0f);
            const int m = resolveGains (plain.partials, ov, scale);
            const int i2 = indexOfRatio (plain, 2.0), i3 = indexOfRatio (plain, 3.0),
                      i5 = indexOfRatio (plain, 5.0);
            bool onlyThose = (m == 3 && i2 >= 0 && i3 >= 0 && i5 >= 0);
            for (size_t i = 0; i < scale.size() && onlyThose; ++i)
            {
                const bool edited = ((int) i == i2 || (int) i == i3 || (int) i == i5);
                if (edited ? std::abs (scale[i] - gainScaleOf (-6.0f)) > 1.0e-6f
                           : scale[i] != 1.0f)
                    onlyThose = false;
            }
            check (onlyThose, "three edits touch exactly their three partials, nothing else");
        }

        // THE point of keying by ratio: the same edit survives a rebuild that moves
        // every index - a deeper fractal, a different Base, a different base mode.
        {
            std::vector<PartialGain> ov;
            setPartialGain (ov, gainKey (4.0), -12.0f);
            bool stable = true;
            for (const auto& t : { plain,
                                   spectrumOf (1, 2.0f, BaseMode::Transposed, 0),
                                   spectrumOf (2, 1.0f, BaseMode::Transposed, 0),
                                   spectrumOf (4, 1.0f, BaseMode::Original,   0) })
            {
                const int m = resolveGains (t.partials, ov, scale);
                const int i4 = indexOfRatio (t, 4.0);
                if (m != 1 || i4 < 0 || std::abs (scale[(size_t) i4] - gainScaleOf (-12.0f)) > 1.0e-6f)
                    stable = false;
            }
            check (stable, "an edit stays on ITS partial across fractal, Base and base-mode rebuilds");
        }

        // Tolerance: a couple of hundredths of a cent of float noise still binds, a
        // twentieth of a cent is a different partial and must not.
        {
            std::vector<PartialGain> near_ { { gainKey (3.0) + 2, -6.0f } };
            std::vector<PartialGain> far_  { { gainKey (3.0) + 5, -6.0f } };
            check (resolveGains (plain.partials, near_, scale) == 1,
                   "an edit within the float-noise tolerance still finds its partial");
            check (resolveGains (plain.partials, far_, scale) == 0,
                   "an edit further off binds to nothing rather than to a neighbour");
        }

        // One-to-one: two edits a hair apart must not BOTH multiply the same partial.
        {
            std::vector<PartialGain> ov { { gainKey (3.0), -6.0f }, { gainKey (3.0) + 1, -6.0f } };
            const int m = resolveGains (plain.partials, ov, scale);
            const int i3 = indexOfRatio (plain, 3.0);
            check (m == 1 && i3 >= 0
                   && std::abs (scale[(size_t) i3] - gainScaleOf (-6.0f)) < 1.0e-6f,
                   "two edits on one partial do not compound - the second is left orphaned");
        }

        // An orphan is kept, not pruned: the partial may come back.
        {
            std::vector<PartialGain> ov;
            setPartialGain (ov, gainKey (7.0), -6.0f);
            const auto no7 = spectrumOf (1, 1.0f, BaseMode::Transposed, 5); // 5-limit drops 7
            const size_t before = ov.size();
            const int m = resolveGains (no7.partials, ov, scale);
            bool allUnity = true;
            for (float v : scale) if (v != 1.0f) allUnity = false;
            check (m == 0 && allUnity && ov.size() == before,
                   "an edit whose partial is not in the spectrum applies to nothing and is kept");
        }

        // bindGains is what the amplitude EDITOR reads to decide which knob owns which
        // stored edit; resolveGains is what the AUDIO applies. They are the same pass, so
        // they can never disagree about the pairing - which is the whole point of it
        // being one function. If they ever could, a knob would show an edit that a
        // different partial was actually getting.
        {
            std::vector<PartialGain> ov;
            setPartialGain (ov, gainKey (2.0), -9.0f);
            setPartialGain (ov, gainKey (7.0), +4.5f);           // present
            setPartialGain (ov, gainKey (3.0) + 40, -6.0f);      // 0.4 c off: homeless
            std::vector<int> where;
            const int m = bindGains (plain.partials, ov, where);
            const int i2 = indexOfRatio (plain, 2.0), i7 = indexOfRatio (plain, 7.0);
            check (m == 2 && where.size() == 3
                   && where[0] == i2 && where[1] == -1 && where[2] == i7,
                   "bindGains reports the exact partial index of each edit, -1 when homeless");

            resolveGains (plain.partials, ov, scale);
            bool agree = true;
            for (size_t j = 0; j < ov.size(); ++j)
                if (where[j] >= 0
                    && std::abs (scale[(size_t) where[j]] - gainScaleOf (ov[j].dB)) > 1.0e-6f)
                    agree = false;
            check (agree, "the index bindGains reports is the one resolveGains multiplied");
        }
    }

    //--------------------------------------------------------------------------
    // Knob CAPTIONS. The editor labels each knob with the partial number, and a
    // denominator of 1 is noise there ("3", not "3/1"). Nothing else may change: a
    // caption that dropped the /2 of 3/2 would name a partial that does not exist.
    std::printf ("partial-number captions:\n");
    {
        using dissonance::partialNumberName;
        check (partialNumberName (1.0) == "1" && partialNumberName (3.0) == "3"
               && partialNumberName (16.0) == "16",
               "whole ratios lose the /1: 3, not 3/1");
        check (partialNumberName (1.5) == "3/2" && partialNumberName (7.0 / 4.0) == "7/4",
               "a real fraction keeps its denominator");
        check (partialNumberName (11.0) == "11" && dissonance::justIntervalName (11.0) == "11/1",
               "only the caption is shortened - the strict label still says 11/1");
        // 100 cents is not a ratio this engine can name exactly: it must stay in cents,
        // or a tempered partial would be captioned as a just interval it is not.
        check (partialNumberName (std::pow (2.0, 100.0 / 1200.0)).find ("/") == std::string::npos,
               "an inexact ratio still shows cents, never an invented fraction");
    }

    //--------------------------------------------------------------------------
    // The amplitude editor's selection + drag arithmetic (PartialAmpEdit.h). These are
    // the rules a user notices immediately when they break, and the window itself is
    // not reachable from any test target - so they live outside the Component.
    std::printf ("partial amplitude editor, selection and drag:\n");
    {
        auto build = [] (int n, bool duplicateKey = false)
        {
            partialamp::Model m;
            std::vector<partialamp::Cell> c;
            for (int i = 0; i < n; ++i)
            {
                const double ratio = (double) (i + 1);
                c.push_back ({ ratio, 0.5f, gainKey (ratio), 0.0f, false });
            }
            // Two partials inside one 0.01 c key bin: only the lower can hold an edit.
            if (duplicateKey && n >= 3) c[2].key = c[1].key;
            m.setCells (std::move (c));
            return m;
        };

        {
            auto m = build (10);
            m.shiftClick (2);
            check (m.selectionCount() == 1 && m.isSelected (2) && m.anchor == 2,
                   "the first SHIFT+click selects one knob and arms the anchor");
            m.shiftClick (6);
            bool range = m.selectionCount() == 5;
            for (int k = 2; k <= 6; ++k) range = range && m.isSelected (k);
            check (range && m.anchor == 2,
                   "the second SHIFT+click selects both ends and everything between them");
            m.shiftClick (0);
            bool back = m.selectionCount() == 3 && m.isSelected (0) && m.isSelected (1)
                     && m.isSelected (2) && ! m.isSelected (3);
            check (back, "the anchor stays put, so a further SHIFT+click re-stretches the same range");
        }

        // A drag on a knob INSIDE the selection moves all of it; on one outside, only it.
        {
            auto m = build (10);
            m.shiftClick (3); m.shiftClick (5);
            check (m.dragSetFor (4).size() == 3, "dragging inside the selection moves the whole selection");
            check (m.dragSetFor (8).size() == 1, "dragging outside it moves that knob alone");
        }

        // One delta, clamped PER knob and measured from each knob's own start: the knob
        // that hits the ceiling must not freeze the others, and dragging back must
        // restore the original spacing instead of leaving them collapsed on the limit.
        {
            auto m = build (4);
            m.cells[0].dB = 20.0f;
            m.cells[1].dB = 0.0f;
            m.sel[0] = m.sel[1] = 1;
            m.anchor = 0;
            m.beginDrag (0);
            m.applyDragDelta (10.0f);
            check (m.cells[0].dB == partialamp::kMaxDb && m.cells[1].dB == 10.0f,
                   "one knob clamping at +24 dB does not stop the rest of the selection");
            m.applyDragDelta (0.0f);
            check (m.cells[0].dB == 20.0f && m.cells[1].dB == 0.0f,
                   "dragging back restores the selection's original spacing exactly");
            m.applyDragDelta (-500.0f);
            check (m.cells[0].dB == partialamp::kMinDb && partialamp::isMuted (m.cells[0].dB)
                   && m.cells[1].dB == partialamp::kMinDb,
                   "the bottom of the travel is the engine's mute, and it is reachable");
        }

        // A partial sharing a key with its neighbour cannot receive an edit (the binding
        // is one-to-one), so it must be locked rather than allowed to move and be ignored.
        {
            auto m = build (5, true);
            check (m.cells[2].locked && m.lockedCount() == 1,
                   "a partial inside its neighbour's 0.01 c key bin is locked, and counted");
            m.sel[1] = m.sel[2] = 1;
            check (m.dragSetFor (1).size() == 1, "a locked knob is left out of a selection drag");
            m.beginDrag (2);
            m.applyDragDelta (12.0f);
            check (m.cells[2].dB == 0.0f, "and dragging it directly does nothing at all");
        }

        // Writing the knobs back must never delete an edit whose partial is missing from
        // the CURRENT spectrum: that is a hand-drawn amplitude the user still owns.
        {
            auto m = build (4);
            m.cells[1].dB = -6.0f;
            m.cells[3].dB = +3.0f;
            std::vector<PartialGain> homeless { { gainKey (2.5), -12.0f }, { gainKey (9.0), +1.0f } };
            const auto out = partialamp::mergedGains (m, homeless);
            bool sorted = true;
            for (size_t i = 1; i < out.size(); ++i) sorted = sorted && out[i - 1].key < out[i].key;
            bool hasHomeless = false, hasEdits = false;
            for (const auto& g : out)
            {
                if (g.key == gainKey (2.5) && g.dB == -12.0f) hasHomeless = true;
                if (g.key == gainKey (4.0) && g.dB == 3.0f)   hasEdits = true;
            }
            check (out.size() == 4 && sorted && hasHomeless && hasEdits,
                   "the written list keeps the homeless edits and stays sorted by key");
        }
    }

    //--------------------------------------------------------------------------
    // The Custom Engine's interval box paints every value in its own colour, which
    // means rebuilding its text run by run. A run whose range is one character out
    // paints the wrong characters and NOTHING complains - the wrong colour is still
    // legible text - so the ranges are checked here rather than by eye.
    std::printf ("interval-token colouring:\n");
    {
        auto cps = [] (const char* s)
        {
            std::vector<int> v;
            for (const char* p = s; *p != '\0'; ++p) v.push_back ((int) (unsigned char) *p);
            return v;
        };
        auto piece = [&cps] (const char* s, const intervaltokens::Span& sp)
        {
            const auto v = cps (s);
            std::string out;
            for (int i = sp.start; i < sp.start + sp.length; ++i) out += (char) v[(size_t) i];
            return out;
        };

        {
            const char* t = "3/2 2 700c";
            const auto s = intervaltokens::spans (cps (t), 5);
            check (s.size() == 3, "three values are three spans");
            check (s.size() == 3 && piece (t, s[0]) == "3/2" && piece (t, s[1]) == "2"
                   && piece (t, s[2]) == "700c",
                   "each span covers exactly its own value, separators excluded");
            check (s.size() == 3 && s[0].colour != s[1].colour && s[1].colour != s[2].colour,
                   "and no value shares a colour with the one before or after it");
        }
        // Every separator loadTimbreText() splits on, several in a row, plus leading and
        // trailing ones: a run of spaces must not become an empty token wearing a colour.
        {
            const char* t = "  5/4,,9/8;\t2\r\n  ";
            const auto s = intervaltokens::spans (cps (t), 5);
            bool ok = s.size() == 3 && piece (t, s[0]) == "5/4" && piece (t, s[1]) == "9/8"
                   && piece (t, s[2]) == "2";
            for (const auto& sp : s) ok = ok && sp.length > 0;
            check (ok, "commas, semicolons, tabs, newlines and runs of them separate but never colour");
        }
        // The palette must WRAP without ever repeating across a boundary.
        {
            const auto s = intervaltokens::spans (cps ("1 2 3 4 5 6 7 8 9"), 5);
            bool ok = s.size() == 9;
            for (size_t i = 1; i < s.size(); ++i) ok = ok && s[i].colour != s[i - 1].colour;
            for (const auto& sp : s) ok = ok && sp.colour >= 0 && sp.colour < 5;
            check (ok, "a list longer than the palette keeps cycling, adjacent values still differ");
        }
        // A one-colour palette would make the whole feature a no-op, so it is refused.
        {
            const auto s = intervaltokens::spans (cps ("3/2 2"), 1);
            check (s.size() == 2 && s[0].colour != s[1].colour, "a degenerate palette is widened, not obeyed");
        }
        check (intervaltokens::spans ({}, 5).empty() && intervaltokens::spans (cps ("   "), 5).empty(),
               "empty and all-separator text produce no spans at all");
    }

    //--------------------------------------------------------------------------
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
