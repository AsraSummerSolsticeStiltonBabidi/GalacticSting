// Standalone unit tests for the spectrum compiler (no JUCE).
// Verifies the numeric examples from the adversarially-verified math design.
//
//   build\...\SpectrumTests.exe   -> exit code 0 = all passed, non-zero = failures.
//
// `reach` bounds the generated harmonic range (top ratio). In the plugin it is fixed
// high (reach to Nyquist); the tests set it small to check exact partial sets.

#include "dsp/Spectrum.h"
#include "dsp/Dissonance.h"

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
        s.curveSlope = 1.0f; s.fractalLevel = 2;
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
        s.fractalLevel = 1;
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
        a.curveSlope = 0.0f; a.fractalLevel = 1; a.timbrePeriodRepeats = 2;
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
    }

    //--------------------------------------------------------------------------
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
