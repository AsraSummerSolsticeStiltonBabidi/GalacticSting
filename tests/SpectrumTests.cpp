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
#include "dsp/PresetMorph.h"
#include "PartialAmpEdit.h"
#include "IntervalTokens.h"
#include "GalacticPalette.h"

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
        // Low->high entry is now measured as REVEAL PROGRESS, not as presence. Each new
        // partial fades in along its own dB ramp, and the ramps start in ascending-ratio
        // order and overlap, so a higher new partial can be on the list while still 60 dB
        // down. Presence stopped being the right question the moment the reveal stopped
        // switching partials on at full height (which is exactly what made the first
        // hundredth of a crossing carry half of it). What must hold at every fader
        // position is that a LOWER new partial is always further along its ramp.
        auto revealed = [&t2] (const SpectrumTemplate& blend, double ratio)
        {
            const float now = ampAtRatio (blend, ratio), full = ampAtRatio (t2, ratio);
            if (full <= 0.0f) return -1.0;
            return now < 0.0f ? 0.0 : (double) now / (double) full;
        };
        bool orderOk = true, growsOk = true;
        double prev4 = -1.0, prev6 = -1.0, prev9 = -1.0;
        for (int k = 1; k <= 99; ++k)
        {
            s.fractalLevel = 1.0f + 0.01f * (float) k;
            const auto bl = compileSpectrum (s);
            // ORDER: compare each new partial against its own level-2 amplitude. Both
            // templates are RMS-normalised by a factor common to all three, so the
            // factor cancels in the comparison BETWEEN them and what is left is pure
            // reveal progress.
            const double r4 = revealed (bl, 4.0), r6 = revealed (bl, 6.0), r9 = revealed (bl, 9.0);
            if (! (r4 >= r6 - 1e-6 && r6 >= r9 - 1e-6)) orderOk = false;
            // GROWTH: measured against the loudest partial of the SAME template, i.e.
            // how loud the new partial actually sounds. Reveal progress on its own is
            // NOT monotone and must not be asserted to be: the whole template is
            // RMS-renormalised as new energy arrives, so a new partial momentarily
            // overshoots its level-2 amplitude by ~3% around mid-crossing and settles
            // back. Its level relative to the fundamental never falls - measured, 0
            // violations across the sweep.
            const double a1 = std::max (1.0e-12, (double) ampAtRatio (bl, 1.0));
            const double n4 = std::max (0.0f, ampAtRatio (bl, 4.0)) / a1;
            const double n6 = std::max (0.0f, ampAtRatio (bl, 6.0)) / a1;
            const double n9 = std::max (0.0f, ampAtRatio (bl, 9.0)) / a1;
            if (n4 < prev4 - 1e-9 || n6 < prev6 - 1e-9 || n9 < prev9 - 1e-9) growsOk = false;
            prev4 = n4; prev6 = n6; prev9 = n9;
        }
        check (orderOk, "at every fader position the lower new partial is further in than the higher (low->high)");
        check (growsOk, "and every new partial only ever grows against the fundamental as the level rises");
        // ...and they are genuinely STAGGERED, not merely ordered: half way across the
        // crossing the three are decades apart, measured 1.035 / 0.065 / 0.0015 of their
        // final amplitudes. Without the stagger every new partial would fade in together
        // and the fader would be an ordinary crossfade of the two levels - uniform, but
        // with the low-to-high growth that gives the control its meaning thrown away.
        {
            s.fractalLevel = 1.5f;
            const auto mid = compileSpectrum (s);
            const double m4 = revealed (mid, 4.0), m6 = revealed (mid, 6.0), m9 = revealed (mid, 9.0);
            std::printf ("  mid-crossing reveal: partial 4 %.4f, partial 6 %.4f, partial 9 %.4f of final\n", m4, m6, m9);
            check (m4 > 2.0 * m6 && m6 > 2.0 * m9,
                   "the new partials arrive STAGGERED low to high, not all at once");
        }
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
    // THE FRACTAL FADER MUST SPEND ITS CHANGE EVENLY.
    //
    // The report: "solo subir un poco el lvl por encima de uno de los escalones da un
    // cambio brusco, nada que ver con subirlo si ya esta entremedio". Measured, the
    // first hundredth of the 1->2 crossing in Prime mode moved the spectrum 10.157 dB
    // and the whole crossing 20.736 dB - 49% of the journey in one click, 70x what the
    // same click costs in the middle of the crossing. At 2->3 it was 67%, at 3->4 73%.
    //
    // The yardstick below is per-band energy in dB over 1/12-octave bands from ratio 1
    // to 1024, floored at -80 dB re the (unit) total energy of an RMS-normalised
    // template. Equal weight per band = log-frequency weighting, and the dB floor is
    // what makes "a band lit up from silence" the large event it is to an ear. Every
    // template compared is finalised the same way - range cut, keep-loudest cap,
    // ascending sort, RMS normalisation - by whichever path built it, so the numbers are
    // directly comparable and nothing has to be undone before comparing.
    std::printf ("fractal fader uniformity:\n");
    {
        constexpr int kBands = 120;   // 10 octaves at 1/12 octave = ratio 1 .. 1024
        auto bandsDb = [kBands] (const SpectrumTemplate& t)
        {
            std::vector<double> e ((size_t) kBands, 0.0);
            for (const auto& p : t.partials)
            {
                const int b = (int) std::floor (std::log2 (std::max (1.0e-9, (double) p.ratio)) * 12.0);
                if (b >= 0 && b < kBands) e[(size_t) b] += (double) p.amp * (double) p.amp;
            }
            std::vector<double> d ((size_t) kBands);
            for (int i = 0; i < kBands; ++i)
                d[(size_t) i] = 10.0 * std::log10 (std::max (e[(size_t) i], 1.0e-8)); // -80 dB floor
            return d;
        };
        auto dist = [] (const std::vector<double>& a, const std::vector<double>& b)
        {
            double s = 0.0;
            for (size_t i = 0; i < a.size(); ++i) { const double x = a[i] - b[i]; s += x * x; }
            return std::sqrt (s / (double) a.size());
        };
        auto centroidOct = [] (const SpectrumTemplate& t)
        {
            double num = 0.0, den = 0.0;
            for (const auto& p : t.partials)
            {
                const double e = (double) p.amp * (double) p.amp;
                num += e * std::log2 (std::max (1.0e-9, (double) p.ratio));
                den += e;
            }
            return den > 0.0 ? num / den : 0.0;
        };

        // One crossing, walked on the fader's OWN grid: NormalisableRange(1, 4, 0.01)
        // snaps the slider, so 1.00, 1.01 ... 2.00 are exactly the values a user can dial.
        struct Walk
        {
            double maxStep = 0.0, meanStep = 0.0, firstStep = 0.0, total = 0.0, worstBackward = 0.0;
            int    argMax = 0;
            bool   centroidRises = true;
            double centroidLo = 0.0, centroidHi = 0.0;
        };
        auto walk = [&] (SpectrumSettings s, int Llo)
        {
            std::vector<std::vector<double>> B;
            std::vector<double> C;
            for (int k = 0; k <= 100; ++k)
            {
                s.fractalLevel = (float) Llo + 0.01f * (float) k;
                const auto x = compileSpectrum (s);
                B.push_back (bandsDb (x));
                C.push_back (centroidOct (x));
            }
            Walk w;
            for (int k = 1; k <= 100; ++k)
            {
                const double d = dist (B[(size_t) k - 1], B[(size_t) k]);
                if (d > w.maxStep) { w.maxStep = d; w.argMax = k; }
                w.meanStep += d;
                if (k == 1) w.firstStep = d;
            }
            w.meanStep /= 100.0;
            w.total = dist (B[0], B[100]);
            double prev = 0.0;
            for (int k = 1; k <= 100; ++k)
            {
                const double d = dist (B[0], B[(size_t) k]);
                w.worstBackward = std::max (w.worstBackward, prev - d);
                prev = d;
            }
            for (int k = 1; k <= 100; ++k)
                if (C[(size_t) k] < C[(size_t) k - 1] - 1.0e-9) w.centroidRises = false;
            w.centroidLo = C.front(); w.centroidHi = C.back();
            return w;
        };

        // Harmonics = Prime is the worst case and the mode the report names, because
        // {1} u {primes} is the least multiplicatively closed of the four sets: 4, 6 and
        // 9 are all NEW at level 2, so there is a large fresh set to reveal. With
        // Harmonics = All/Odd/Even, or a pure prime LIMIT, the set is closed, there is
        // nothing fresh, and the fader was always smooth - which is why the user hears
        // this in prime mode and nowhere else.
        SpectrumSettings prime; prime.baseN = 1; prime.selection = (float) Selection::Prime;
        prime.curveSlope = 1.0f;

        // K: the worst single click, as a multiple of the average click. Measured on the
        // shipping code this was 14.4 / 27.6 / 40.2 for 1->2 / 2->3 / 3->4; with the dB
        // reveal ramp it is 1.8 / 1.8 / 2.0. 4 sits with 2x headroom over the fix and a
        // factor of 3.6 below the smallest failure the old code produced.
        constexpr double kMaxStepRatio = 4.0;
        // The first click of a crossing must not carry vastly more than its 1% share.
        // Measured: 0.15% / 0.34% / 0.63% now, 49% / 67% / 73% before.
        constexpr double kMaxFirstShare = 0.04;

        for (int Llo = 1; Llo <= 3; ++Llo)
        {
            const auto w = walk (prime, Llo);
            const double K = w.meanStep > 0.0 ? w.maxStep / w.meanStep : 0.0;
            std::printf ("  Prime %d->%d  worst click %.3f dB @%d, mean %.4f dB, K=%.1f;"
                         " first click %.2f%% of the crossing (%.3f dB total); centroid %.2f -> %.2f oct\n",
                         Llo, Llo + 1, w.maxStep, w.argMax, w.meanStep, K,
                         100.0 * w.firstStep / std::max (1.0e-12, w.total), w.total,
                         w.centroidLo, w.centroidHi);

            check (K <= kMaxStepRatio,
                   "UNIFORMITY: no single fader click costs more than 4x the average click");
            check (w.firstStep <= kMaxFirstShare * w.total,
                   "NO EARLY CLIFF: the first 1% of a crossing delivers at most 4% of it");
            // Measured worst backward wobble with the fix in place: 0.0101 dB, and it
            // comes from the RMS renormalisation, not from the reveal. 0.05 dB is a
            // twentieth of the smallest step the fader can produce.
            check (w.worstBackward < 0.05,
                   "MONOTONICITY: the spectrum only ever moves further from where the crossing started");
            check (w.centroidRises && w.centroidHi > w.centroidLo + 0.2,
                   "CHARACTER: the spectral centroid still climbs across the crossing - detail grows upward");
        }

        // Not just prime mode. The closed-set configs never stepped, and they must not
        // start; the open ones must all come into line. This sweep walks the crossing
        // INTERIOR (1.00 ... 1.99) only: the last click, from x.99 onto the integer
        // level, is governed by a separate and OLDER effect - several upper-level
        // partials inside one ~1-cent bin collapse onto the surviving lower-level
        // partial, so blend(t->1) carries their summed amplitude on one ratio instead of
        // three. That is why Base 4 measures a 0.797 dB last click against a 0.186 dB
        // average (it was 0.820 dB before this change, i.e. untouched by it) while Prime,
        // whose bins hold one partial each, measures 0.030 dB.
        auto walkInterior = [&] (SpectrumSettings s, int Llo)
        {
            std::vector<std::vector<double>> B;
            for (int k = 0; k <= 99; ++k)
            {
                s.fractalLevel = (float) Llo + 0.01f * (float) k;
                B.push_back (bandsDb (compileSpectrum (s)));
            }
            double mx = 0.0, mean = 0.0;
            for (int k = 1; k <= 99; ++k)
            { const double d = dist (B[(size_t) k - 1], B[(size_t) k]); mx = std::max (mx, d); mean += d; }
            mean /= 99.0;
            return mean > 0.0 ? mx / mean : 0.0;
        };
        {
            struct Cfg { const char* name; SpectrumSettings s; };
            std::vector<Cfg> cfgs;
            { SpectrumSettings s; s.selection = (float) Selection::Prime; s.reach = 256; cfgs.push_back ({ "Prime", s }); }
            { SpectrumSettings s; s.selection = (float) Selection::All;   s.reach = 256; cfgs.push_back ({ "All (closed set)", s }); }
            { SpectrumSettings s; s.selection = (float) Selection::Odd;   s.reach = 256; cfgs.push_back ({ "Odd (closed set)", s }); }
            { SpectrumSettings s; s.selection = 2.5f;                     s.reach = 256; cfgs.push_back ({ "Harmonics 2.5", s }); }
            { SpectrumSettings s; s.selection = (float) Selection::Prime; s.reach = 256;
              s.primeLimit = 13; s.primeLimitPos = 1.0f; cfgs.push_back ({ "Prime + 13-limit", s }); }
            { SpectrumSettings s; s.selection = (float) Selection::Prime; s.reach = 256;
              s.curveSlope = 2.5f; cfgs.push_back ({ "Prime, slope 2.5", s }); }
            { SpectrumSettings s; s.selection = (float) Selection::Prime; s.reach = 256;
              s.curveSlope = 0.3f; cfgs.push_back ({ "Prime, slope 0.3", s }); }
            { SpectrumSettings s; s.baseN = 3; s.selection = (float) Selection::Prime; s.reach = 256;
              cfgs.push_back ({ "Base 3, Prime", s }); }
            bool allEven = true;
            double worst = 0.0; const char* worstAt = "";
            for (const auto& c : cfgs)
                for (int Llo = 1; Llo <= 3; ++Llo)
                {
                    const double K = walkInterior (c.s, Llo);
                    if (K > worst) { worst = K; worstAt = c.name; }
                    if (K > 4.0) { allEven = false; std::printf ("    %s %d->%d K=%.1f\n", c.name, Llo, Llo + 1, K); }
                }
            std::printf ("  worst max/mean click over 8 configs x 3 crossings: %.1f (%s)\n", worst, worstAt);
            check (allEven, "UNIFORMITY holds across harmonics/prime/slope/base settings, not just prime mode");
        }

        // THE CLOSED-SET CASE, which is what the geometric survivor is for. When the base
        // harmonic set is closed under multiplication - Harmonics = All or Odd, or a
        // prime whitelist that keeps a closed set - the upper level introduces NO new
        // ratios at all, there is nothing to reveal, and the only thing the fader does is
        // rescale the partials both levels share. Linearly in amplitude that front-loads:
        // measured max/mean click 1.8 (All), 1.7 (Odd), 2.4 (whitelist) with the worst
        // click always the FIRST one. On a straight line in dB it is 1.1 / 1.0 / 1.0 and
        // the worst click is no longer at the start.
        //
        // At the PLUGIN's reach (kSpectrumReach), not a cut-down one: the two levels of a
        // closed set differ only in how far the products reach, so shortening the reach
        // shrinks the difference until neither form of the fade is distinguishable
        // (measured at reach 256: 1.55 linear against 1.58 geometric, a wash).
        {
            struct Cfg { const char* name; SpectrumSettings s; };
            std::vector<Cfg> closed;
            { SpectrumSettings s; s.selection = (float) Selection::All; closed.push_back ({ "All", s }); }
            { SpectrumSettings s; s.selection = (float) Selection::Odd; closed.push_back ({ "Odd", s }); }
            { SpectrumSettings s; s.primeUseWhitelist = true;
              s.primeWhitelistMask = 0x1FCu; closed.push_back ({ "whitelist without 2 and 3", s }); }
            bool flat = true;
            for (const auto& c : closed)
            {
                const double K = walkInterior (c.s, 1);
                std::printf ("  closed set, %-26s max/mean click %.2f\n", c.name, K);
                if (K > 1.4) flat = false;
            }
            check (flat, "with nothing new to reveal the fader is FLAT - the shared partials fade in dB, not in amplitude");
        }

        // ENDPOINT EXACTNESS, bottom. compileSpectrum takes the single-level path for a
        // fraction of 1e-3 or less, so the blend at t=0 itself is not reachable through
        // the public API; what IS reachable, and what matters, is that the blend
        // CONVERGES on the integer level as the fraction vanishes. Before the fix the
        // first blended value the automation lane could reach was 4.881 dB away from the
        // integer level in this config.
        {
            SpectrumSettings s = prime;
            s.fractalLevel = 1.0f;     const auto atL   = compileSpectrum (s);
            s.fractalLevel = 1.0011f;  const auto justAbove = compileSpectrum (s);
            const double d = dist (bandsDb (atL), bandsDb (justAbove));
            std::printf ("  crossing the 1e-3 blend threshold costs %.4f dB (was 4.881)\n", d);
            check (d < 0.02, "ENDPOINT: entering the blend is not a cliff - below the fader's own smallest step");
            // Every partial of the integer level is still there, at its own amplitude.
            bool same = justAbove.partials.size() >= atL.partials.size();
            double worstAmp = 0.0;
            for (const auto& p : atL.partials)
            {
                const float a = ampAtRatio (justAbove, (double) p.ratio, 1.0e-4 * (double) p.ratio);
                if (a < 0.0f) { same = false; break; }
                worstAmp = std::max (worstAmp, std::abs ((double) a - (double) p.amp));
            }
            check (same && worstAmp < 1.0e-3,
                   "ENDPOINT: and every partial of the integer level survives it unchanged");
        }

        // ENDPOINT EXACTNESS, top. At a fraction of 0.99 the reveal is complete (the
        // last ramp starts at 0.5 at the latest and is a half-crossing wide) and every
        // survivor carries the upper level's bin energy, so the template IS the upper
        // level - partial set included. It cannot be bit-identical, because 1.99 is
        // genuinely one hundredth short of 2.00 and the survivors are still that far
        // along their fade; what has to be true is that the remaining gap shrinks in
        // proportion to the remaining fraction rather than sitting there as an offset,
        // which is what a blend that did NOT converge on the upper level would show.
        //
        // Prime is used because its ~1-cent bins hold exactly one partial each; where
        // they do not (Base 4) the bin collapse described above applies and the partial
        // COUNT at the top of a crossing differs from the upper level's. That is older
        // than this change and untouched by it.
        {
            SpectrumSettings s = prime;
            s.fractalLevel = 2.0f;    const auto exact  = compileSpectrum (s);
            s.fractalLevel = 1.99f;   const auto near1  = compileSpectrum (s);
            s.fractalLevel = 1.999f;  const auto near2  = compileSpectrum (s);
            auto gap = [&exact] (const SpectrumTemplate& n, bool& sameSet)
            {
                sameSet = exact.partials.size() == n.partials.size();
                double worst = 0.0;
                if (sameSet)
                    for (size_t i = 0; i < exact.partials.size(); ++i)
                    {
                        if (std::abs ((double) exact.partials[i].ratio - (double) n.partials[i].ratio)
                            > 1.0e-4 * (double) exact.partials[i].ratio) { sameSet = false; break; }
                        worst = std::max (worst, std::abs ((double) exact.partials[i].amp
                                                           - (double) n.partials[i].amp));
                    }
                return worst;
            };
            bool set1 = false, set2 = false;
            const double g1 = gap (near1, set1), g2 = gap (near2, set2);
            std::printf ("  level 2 has %d partials; 1.99 is %d partials and %.2e away, 1.999 is %d and %.2e\n",
                         (int) exact.partials.size(), (int) near1.partials.size(), g1,
                         (int) near2.partials.size(), g2);
            check (set1 && set2 && g1 < 5.0e-3,
                   "ENDPOINT: the top of a crossing is the upper integer level, partial for partial");
            check (g2 < 0.2 * g1,
                   "ENDPOINT: and the last of the gap closes with the fraction, so t=1 is the upper level exactly");
        }
    }

    //--------------------------------------------------------------------------
    // THE INTEGER HANDOVER, x.99 -> x+1.00.
    //
    // Going from 1.99 to 2.00 IS crossing a step, and it is the one step the blend never
    // takes itself: at x+1.00 compileSpectrum drops the blend and returns the single
    // integer level. Whatever gap the blend leaves at t->1 is paid in that one click, and
    // once the reveal ramps had shrunk every other click it was the biggest one left.
    //
    // The gap was a UNIFORM GAIN, and it came from a silent MERGE. blendFractalTemplates
    // used to sum each level's ~1-cent bin into a single amplitude and push the whole
    // upper-level bin onto the LOWER level's partials, so k upper partials sharing a bin
    // with one lower partial were folded onto that one ratio with their amplitudes added.
    // Folding k partials of amplitude a into one of amplitude k*a multiplies that bin's
    // sum of squares by k, so the RMS normalisation in finalisePartials divided the whole
    // template by too much, and everything that did NOT fold - the loud low partials the
    // ear follows - came out uniformly quiet. Measured over the 32 loudest partials of
    // the upper level, the blend at x.999 sat -0.427 dB (Base 4 Odd 2->3), -0.332 dB
    // (Base 8 All 1->2) and -0.914 dB (Base 16 All 1->2) below it, spread only
    // 0.010-0.025 dB across those partials - a gain error, not a timbre error - and the
    // handover click measured 0.740 / 0.993 / 1.480 dB, up to 5.7x the average click of
    // its crossing. At Base 4 Odd 2->3 the blend also held 7119 partials against level
    // 3's 14480: 7361 of the user's partials merged away with nothing said.
    //
    // The guard that existed could not see any of this. It compares COSINE SIMILARITY,
    // which normalises both spectra before comparing and is therefore blind to exactly
    // the quantity that was wrong. These assertions are in dB and in partial COUNTS,
    // which is what a uniform gain and a silent fold actually move.
    std::printf ("fractal fader: the integer handover (x.99 -> x+1.00):\n");
    {
        constexpr int kBands = 120;   // 10 octaves at 1/12 octave, as above
        auto bandsDb = [kBands] (const SpectrumTemplate& t)
        {
            std::vector<double> e ((size_t) kBands, 0.0);
            for (const auto& p : t.partials)
            {
                const int b = (int) std::floor (std::log2 (std::max (1.0e-9, (double) p.ratio)) * 12.0);
                if (b >= 0 && b < kBands) e[(size_t) b] += (double) p.amp * (double) p.amp;
            }
            std::vector<double> d ((size_t) kBands);
            for (int i = 0; i < kBands; ++i)
                d[(size_t) i] = 10.0 * std::log10 (std::max (e[(size_t) i], 1.0e-8));
            return d;
        };
        auto dist = [] (const std::vector<double>& a, const std::vector<double>& b)
        {
            double s = 0.0;
            for (size_t i = 0; i < a.size(); ++i) { const double x = a[i] - b[i]; s += x * x; }
            return std::sqrt (s / (double) a.size());
        };

        // Base N with N > 1 is where the bins genuinely collide: the ratio grid is k/N,
        // so at the top of the reach consecutive harmonics are a fraction of a cent apart
        // and several of them land in one bin. Prime mode's bins hold one partial each,
        // which is why the fold was invisible in the mode the original report named.
        struct HCfg { const char* name; SpectrumSettings s; int Llo; };
        std::vector<HCfg> cfgs;
        { SpectrumSettings s; s.baseN = 4;  s.selection = (float) Selection::Odd; s.reach = 256;
          cfgs.push_back ({ "Base 4 Odd",  s, 2 }); }
        { SpectrumSettings s; s.baseN = 8;  s.selection = (float) Selection::All; s.reach = 256;
          cfgs.push_back ({ "Base 8 All",  s, 1 }); }
        { SpectrumSettings s; s.baseN = 16; s.selection = (float) Selection::All; s.reach = 256;
          cfgs.push_back ({ "Base 16 All", s, 1 }); }

        for (const auto& c : cfgs)
        {
            SpectrumSettings s = c.s;
            s.fractalLevel = (float) (c.Llo + 1);    const auto up   = compileSpectrum (s);
            s.fractalLevel = (float) c.Llo + 0.999f; const auto top  = compileSpectrum (s);
            s.fractalLevel = (float) c.Llo + 0.99f;  const auto last = compileSpectrum (s);
            s.fractalLevel = (float) c.Llo + 0.98f;  const auto prev = compileSpectrum (s);

            // GAIN, read at 0.999 rather than on the fader's 0.01 grid so that what is
            // left of the survivors' own fade (1/1000 of a geometric path) cannot be
            // mistaken for the offset being hunted. The median over the 32 LOUDEST
            // partials, so neither one outlier nor the inaudible tail can invent or hide
            // an offset; those 32 are also the ones that never fold, which is precisely
            // why they carried the error.
            std::vector<Partial> loudest (up.partials);
            std::sort (loudest.begin(), loudest.end(),
                       [] (const Partial& a, const Partial& b) { return a.amp > b.amp; });
            if (loudest.size() > 32) loudest.resize (32);
            std::vector<double> off;
            for (const auto& p : loudest)
            {
                const float a = ampAtRatio (top, (double) p.ratio, 1.0e-4 * (double) p.ratio);
                if (a > 0.0f && p.amp > 0.0f)
                    off.push_back (20.0 * std::log10 ((double) a / (double) p.amp));
            }
            std::sort (off.begin(), off.end());
            const double med    = off.empty() ? 99.0 : off[off.size() / 2];
            const double spread = off.empty() ? 99.0 : off.back() - off.front();

            // The handover click itself, and the click immediately before it, on the
            // fader's own grid. A gain paid back in one step shows up as the last click
            // dwarfing its neighbour; an ordinary end-of-crossing click does not.
            const double lastClick = dist (bandsDb (last), bandsDb (up));
            const double prevClick = dist (bandsDb (prev), bandsDb (last));

            std::printf ("  %-12s %d->%d  gain at x.999 %+.4f dB (spread %.4f over %d partials);"
                         " handover click %.4f dB vs %.4f dB before it; partials upper %d, blend %d\n",
                         c.name, c.Llo, c.Llo + 1, med, spread, (int) off.size(),
                         lastClick, prevClick, (int) up.partials.size(), (int) top.partials.size());

            // 0.05 dB: a factor of 6 below the smallest failure the fold produced
            // (0.332 dB) and 50x above what pairing leaves (measured +0.0008 / +0.0009 /
            // +0.0000 dB). It is also about twice the smallest step the fader can make
            // anywhere (0.023 dB), so it cannot fail on the fader's own quantisation.
            check (std::abs (med) < 0.05,
                   "HANDOVER: the top of a crossing carries the upper level's own GAIN, not a fraction of it");
            // The count is the merge caught directly: every upper-level partial has to be
            // somewhere in the blend at its OWN ratio. (The blend may hold MORE - a lower
            // partial the upper level kills is still there, faded to nothing.)
            check ((int) top.partials.size() >= (int) up.partials.size(),
                   "HANDOVER: and no upper-level partial has been folded onto a neighbour's ratio");
            // Measured against the click before it, after pairing: 0.046 vs 0.094,
            // 0.144 vs 0.260, 0.419 vs 0.539 - every handover SMALLER than its
            // neighbour. Folding: 0.740 vs 0.061, 0.993 vs 0.233, 1.481 vs 0.483,
            // i.e. 12.2x, 4.3x and 3.1x the click before it.
            check (lastClick < 3.0 * prevClick + 0.02,
                   "HANDOVER: so the last click of a crossing is an ordinary click, not a step");
        }
    }

    //--------------------------------------------------------------------------
    // A SINGLE NEW PARTIAL MUST STILL USE THE WHOLE FADER.
    //
    // The reveal gives each new partial a dB ramp and staggers the ramp STARTS over
    // [0, 1-kRevealRampW] by the geometric mean of its position by count and by energy.
    // With a big fresh set the last start lands at 1-kRevealRampW and the last ramp
    // therefore ends at 1.0, which is the design. With a TINY fresh set both stagger
    // factors collapse towards zero: with exactly ONE new partial - Harmonics = Prime at
    // prime limit 2, so the base is {1,2}; or a 2-degree Scala timbre at Reps 1 - its
    // ramp started at 0 and, at a fixed width of 0.5, was over by the midpoint. Measured
    // on 1/12-octave band energy, clicks 51-100 then carried 3.2% / 5.9% / 3.1% of the
    // crossing's loudness path: the top half of the fader did nothing.
    //
    // The fix widens the ramp to 1 - lastStart, so the last new partial always lands on
    // t = 1. Because lastStart <= 1 - kRevealRampW always, the width can only ever GROW,
    // never shrink - so the per-click bound that kRevealRampW buys (at least 50 fader
    // clicks per ramp) survives it, and this block re-checks uniformity on the spot
    // rather than taking that on trust.
    std::printf ("fractal fader: a single new partial still uses the whole fader:\n");
    {
        constexpr int kBands = 120;
        auto bandsDb = [kBands] (const SpectrumTemplate& t)
        {
            std::vector<double> e ((size_t) kBands, 0.0);
            for (const auto& p : t.partials)
            {
                const int b = (int) std::floor (std::log2 (std::max (1.0e-9, (double) p.ratio)) * 12.0);
                if (b >= 0 && b < kBands) e[(size_t) b] += (double) p.amp * (double) p.amp;
            }
            std::vector<double> d ((size_t) kBands);
            for (int i = 0; i < kBands; ++i)
                d[(size_t) i] = 10.0 * std::log10 (std::max (e[(size_t) i], 1.0e-8));
            return d;
        };
        auto dist = [] (const std::vector<double>& a, const std::vector<double>& b)
        {
            double s = 0.0;
            for (size_t i = 0; i < a.size(); ++i) { const double x = a[i] - b[i]; s += x * x; }
            return std::sqrt (s / (double) a.size());
        };

        struct TCfg { const char* name; SpectrumSettings s; int Llo; };
        std::vector<TCfg> cfgs;
        { SpectrumSettings s; s.selection = (float) Selection::Prime;
          s.primeLimit = 2; s.primeLimitPos = 1.0f; cfgs.push_back ({ "Prime, 2-limit", s, 1 }); }
        { SpectrumSettings s; s.selection = (float) Selection::Prime;
          s.primeLimit = 2; s.primeLimitPos = 1.0f; cfgs.push_back ({ "Prime, 2-limit", s, 2 }); }
        { SpectrumSettings s; s.source = Source::ScalaTimbre; s.timbreDegrees = { 1.0f, 1.5f };
          s.timbrePeriod = 2.0f; s.timbrePeriodRepeats = 1;
          cfgs.push_back ({ "Scala 2 degrees", s, 1 }); }

        for (const auto& c : cfgs)
        {
            SpectrumSettings s = c.s;
            s.fractalLevel = (float) c.Llo;       const auto L = compileSpectrum (s);
            s.fractalLevel = (float) (c.Llo + 1); const auto H = compileSpectrum (s);

            // The PREMISE, asserted rather than assumed: if a settings change ever made
            // these configs bring two new partials the numbers below would still look
            // fine and the test would have stopped testing what it says it tests.
            int fresh = 0;
            for (const auto& p : H.partials)
                if (! hasRatio (L, (double) p.ratio, 1.0e-4 * (double) p.ratio)) ++fresh;

            std::vector<std::vector<double>> B;
            for (int k = 0; k <= 100; ++k)
            {
                s.fractalLevel = (float) c.Llo + 0.01f * (float) k;
                B.push_back (bandsDb (compileSpectrum (s)));
            }
            double firstHalf = 0.0, secondHalf = 0.0, mx = 0.0; int arg = 0;
            for (int k = 1; k <= 100; ++k)
            {
                const double d = dist (B[(size_t) k - 1], B[(size_t) k]);
                (k <= 50 ? firstHalf : secondHalf) += d;
                if (d > mx) { mx = d; arg = k; }
            }
            const double total = firstHalf + secondHalf;
            const double K     = total > 0.0 ? mx / (total / 100.0) : 0.0;
            std::printf ("  %-16s %d->%d  %d new partial(s); clicks 1-50 %.1f%%, 51-100 %.1f%%;"
                         " worst click %.4f dB @%d, K=%.2f\n",
                         c.name, c.Llo, c.Llo + 1, fresh,
                         100.0 * firstHalf / std::max (1.0e-12, total),
                         100.0 * secondHalf / std::max (1.0e-12, total), mx, arg, K);

            check (fresh == 1, "exactly one partial is new at the upper level (the premise of this test)");
            // 25%: half the fader's travel is asked to deliver at least half of a fair
            // share. Measured 54.7% / 62.0% / 52.0% with the adaptive width, 3.2% / 5.9%
            // / 3.1% with the fixed one - so this sits 2x under the fix and 4x over the
            // failure.
            check (secondHalf > 0.25 * total,
                   "TOP HALF: the second half of the crossing still carries the fader's work");
            // Same K <= 4 the twelve-config sweep above uses. Widening the ramp must not
            // buy the top half at the cost of a lumpier fader: measured 1.12 / 1.25 /
            // 1.07 after, against 2.15 / 2.38 / 2.06 before.
            check (K <= 4.0,
                   "TOP HALF: and the wider ramp does not make any single click stand out");
        }
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
    // ...AND THE RANGE CUT MUST NEVER EMPTY THE SPECTRUM.
    //
    // The cut is stated relative to ratio 1 = the played pitch, but ratio 1 is only the
    // bottom of the spectrum in TRANSPOSED mode. In ORIGINAL mode the harmonics keep
    // their natural positions, so with base N the lowest partial is at ratio N and a
    // narrow range can sit entirely BELOW every partial there is.
    //
    // Measured before the fix, at the shipping control's own minimum (spectrumRange = 1):
    //   Original N=4, range 1          ->  0 partials   (silent note, no warning)
    //   Original N=7, range 1 and 2    ->  0 partials   (idem)
    // Every (mode, N, range) the control can reach is swept here, both engines' entry
    // points, so a future change that re-introduces the hole anywhere is caught. The
    // rule asserted is the one applyRangeCut now implements: the LOWEST-ratio partial
    // survives whatever the ceiling is, so the played pitch always sounds.
    std::printf ("Spectrum range can never delete the whole spectrum:\n");
    {
        int emptied = 0, worst[3] = { 0, 0, 0 };
        for (int mode = 0; mode < 2; ++mode)
            for (int N = 1; N <= 16; ++N)
                for (int rng = 1; rng <= 10; ++rng)
                {
                    SpectrumSettings s;
                    s.baseN = N;
                    s.baseMode = mode == 0 ? BaseMode::Transposed : BaseMode::Original;
                    s.reach = 64;
                    s.selection = (float) Selection::All;
                    s.curveSlope = 1.0f;
                    s.spectrumRangeOct = rng;
                    const auto t = compileSpectrum (s);
                    if (t.partials.empty())
                    {
                        if (emptied == 0) { worst[0] = mode; worst[1] = N; worst[2] = rng; }
                        ++emptied;
                    }
                }
        if (emptied > 0)
            std::printf ("    %d of 320 (mode, N, range) combinations compiled to ZERO partials"
                         " (first: %s N=%d range=%d)\n",
                         emptied, worst[0] == 0 ? "Transposed" : "Original", worst[1], worst[2]);
        check (emptied == 0,
               "no (base mode, N, range) the controls can reach compiles to a silent spectrum");

        // The three combinations that were MEASURED at zero, named individually so the
        // regression report says which patch went silent rather than "one of 320".
        struct Case { int N, rng; };
        const Case zeroed[] = { { 4, 1 }, { 7, 1 }, { 7, 2 } };
        bool allSound = true;
        for (const auto& c : zeroed)
        {
            SpectrumSettings s;
            s.baseN = c.N; s.baseMode = BaseMode::Original; s.reach = 64;
            s.selection = (float) Selection::All; s.curveSlope = 1.0f;
            s.spectrumRangeOct = c.rng;
            const auto t = compileSpectrum (s);
            // The survivor has to be the LOWEST partial (the played pitch at ratio N),
            // not just any partial: keeping the loudest one instead would move the pitch.
            const bool ok = ! t.partials.empty()
                         && std::abs ((double) t.partials.front().ratio - (double) c.N) < 1e-3;
            std::printf ("    Original N=%d, range %d: %d partials, lowest ratio %.3f\n",
                         c.N, c.rng, (int) t.partials.size(),
                         t.partials.empty() ? 0.0 : (double) t.partials.front().ratio);
            if (! ok) allSound = false;
        }
        check (allSound,
               "the three configurations measured at 0 partials now keep their played pitch");

        // ...and the cut still cuts. Guaranteeing the lowest partial must not turn into
        // "the range control does nothing": a range that DOES contain partials keeps
        // exactly the ones under the ceiling and no more.
        {
            SpectrumSettings s;
            s.baseN = 1; s.baseMode = BaseMode::Transposed; s.reach = 64;
            s.selection = (float) Selection::All; s.curveSlope = 0.0f;
            s.spectrumRangeOct = 2;                     // keep ratio <= 4
            const auto t = compileSpectrum (s);
            double maxR = 0.0;
            for (const auto& p : t.partials) maxR = std::max (maxR, (double) p.ratio);
            check (t.partials.size() == 4 && maxR <= 4.0 + 1e-3,
                   "a range that does contain partials still cuts everything above it");
        }
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
    // The adapted-spectrum text used to print every value at a fixed 5 decimals
    // ("100.00000c 1200.00000c"), pure noise the user then had to edit around.
    // Precision must survive the trim: only REDUNDANT zeros go.
    std::printf ("Cents text (trailing zeros trimmed, precision kept):\n");
    {
        using dissonance::centsText;
        check (centsText (700.0)      == "700",       "700.00000 -> 700 (no redundant zeros)");
        check (centsText (701.955)    == "701.955",   "701.955 keeps its real decimals");
        check (centsText (200.00002)  == "200.00002", "200.00002 keeps ALL its 5 decimals");
        check (centsText (100.000004) == "100",       "sub-precision dust rounds away, then trims");
        check (centsText (1200.5)     == "1200.5",    "1200.50000 -> 1200.5");
        check (centsText (0.0)        == "0",         "zero is plain 0");
        check (centsText (-0.000001)  == "0",         "negative dust never prints as -0");
        check (centsText (-386.31371) == "-386.31371","negative values keep sign and precision");
    }

    //--------------------------------------------------------------------------
    // THE REFERENCE MODEL, PINNED. An independent brute-force dissmeasure, written
    // straight from Sethares' appendix (no sorting, no pair decomposition, no
    // tables, no cap): every optimised path in Dissonance.h is held to it at 1e-12,
    // and one dyad is pinned to a hand-computed CONSTANT so a wrong constant in
    // dissmeasure could not hide inside a reference that copied it.
    std::printf ("\nSethares reference model (brute force + hand-computed fixture):\n");
    {
        const auto refPair = [] (double f1, double a1, double f2, double a2)
        {
            const double fmin = std::min (f1, f2), fdif = std::abs (f1 - f2);
            const double s = 0.24 / (0.0207 * fmin + 18.96);
            return std::min (a1, a2) * (5.0 * std::exp (-3.51 * s * fdif)
                                        - 5.0 * std::exp (-5.75 * s * fdif));
        };
        const auto refDiss = [&refPair] (const std::vector<dissonance::Partial>& p)
        {
            double d = 0.0;
            for (size_t i = 0; i < p.size(); ++i)
                for (size_t j = i + 1; j < p.size(); ++j)
                    d += refPair (p[i].freq, p[i].amp, p[j].freq, p[j].amp);
            return d;
        };

        // FIXTURE, by hand: 440 + 450 Hz, unit amplitudes. s = 0.24/(0.0207*440 +
        // 18.96) = 0.00855066267635742; d = 10; D = 5(e^(-3.51 s d) - e^(-5.75 s d))
        // = 0.645574149314169. Pins Dstar, S1, S2, C1, C2, A1, A2 and the
        // lower-frequency rule all at once.
        {
            const double got = dissonance::dissmeasure ({ { 440.0, 1.0 }, { 450.0, 1.0 } });
            std::printf ("  440+450 Hz dyad: %.15f (hand value 0.645574149314169)\n", got);
            check (std::abs (got - 0.64557414931416934) < 1.0e-12,
                   "two sines 440+450 Hz, equal amps: matches the hand-computed roughness");
        }
        // s must come from the pair's LOWER frequency, not the global minimum: in
        // {100, 400, 410} the 400/410 pair uses s(400). Checked against the
        // reference, which applies the rule per pair by construction.
        {
            const std::vector<dissonance::Partial> tri { { 100.0, 1.0 }, { 400.0, 1.0 }, { 410.0, 1.0 } };
            check (std::abs (dissonance::dissmeasure (tri) - refDiss (tri)) < 1.0e-12,
                   "s is taken from each pair's own lower frequency (not the spectrum's minimum)");
        }
        // Edge cases the model must get exactly right.
        check (dissonance::dissmeasure ({}) == 0.0, "empty spectrum -> 0");
        check (dissonance::dissmeasure ({ { 440.0, 1.0 } }) == 0.0, "single partial -> 0 (nothing to beat)");
        check (dissonance::dissmeasure ({ { 440.0, 1.0 }, { 440.0, 1.0 } }) == 0.0,
               "unison pair (d = 0) -> exactly 0 (C1 = -C2)");
        check (dissonance::dissmeasure ({ { 440.0, 0.0 }, { 450.0, 1.0 } }) == 0.0,
               "a zero-amplitude partial contributes nothing (min-amplitude weighting)");

        // UNION SEMANTICS at 1e-12 on three spectra: harmonic, stretched (Sethares'
        // own example of a timbre wanting its own scale) and hand-reweighted (with a
        // zero-amp partial and a duplicated frequency in it), at alphas below AND
        // above 1 - dissonanceAt/chordDissonance/self+cross must all be exactly
        // dissmeasure of the union, however the copies land.
        {
            std::vector<std::vector<dissonance::Partial>> specs (3);
            for (int k = 1; k <= 7; ++k)
                specs[0].push_back ({ 261.6255653 * k, 1.0 / k });
            for (int k = 1; k <= 10; ++k)
                specs[1].push_back ({ 261.6255653 * std::pow ((double) k, 1.2), 1.0 / k });
            specs[2] = { { 220.0, 0.3 }, { 275.0, 1.0 }, { 275.0, 0.5 }, { 330.0, 0.0 }, { 495.0, 0.7 } };

            bool unionOk = true, chordOk = true, decompOk = true, symOk = true;
            for (const auto& sp : specs)
            {
                for (const double alpha : { 1.5, std::pow (2.0, 7.0 / 12.0), 0.75, 1.0 })
                {
                    std::vector<dissonance::Partial> u = sp;
                    for (const auto& p : sp) u.push_back ({ p.freq * alpha, p.amp });
                    const double want = refDiss (u);
                    const double tol  = 1.0e-12 * std::max (1.0, std::abs (want));
                    if (std::abs (dissonance::dissonanceAt (sp, alpha) - want) > tol) unionOk = false;
                    // self+cross grouping of the same union (the tables' identity):
                    const double dec = dissonance::selfDissonance (sp, 1.0)
                                     + dissonance::selfDissonance (sp, alpha)
                                     + dissonance::crossDissonance (sp, 1.0, alpha);
                    if (std::abs (dec - want) > tol) decompOk = false;
                    if (std::abs (dissonance::crossDissonance (sp, 1.0, alpha)
                                  - dissonance::crossDissonance (sp, alpha, 1.0)) > tol) symOk = false;
                }
                const std::vector<double> chord { 1.0, 0.8, 1.25, 1.5 }; // includes a note BELOW the root
                std::vector<dissonance::Partial> u;
                for (const double a : chord)
                    for (const auto& p : sp) u.push_back ({ p.freq * a, p.amp });
                const double want = refDiss (u);
                if (std::abs (dissonance::chordDissonance (sp, chord) - want)
                    > 1.0e-12 * std::max (1.0, std::abs (want))) chordOk = false;
            }
            check (unionOk,  "dissonanceAt == brute-force dissmeasure of the union (1e-12; alpha < 1 included)");
            check (decompOk, "self + self + cross == the same union, exactly (the grid tables' identity)");
            check (symOk,    "crossDissonance is symmetric in its two transpositions");
            check (chordOk,  "chordDissonance == brute-force union over all notes (1e-12, all three spectra)");
        }
    }

    //--------------------------------------------------------------------------
    // THE ANALYSIS SPECTRUM: what every dissonance readout scores. The user's
    // per-partial gain must multiply BEFORE the 64-cap sorts, or a muted partial
    // holds a cap slot (beating in the analysis, silent to the ear) while a boosted
    // one - the loudest thing sounding - is dropped from the analysis entirely.
    std::printf ("\nanalysis spectrum (sounding amplitudes, gains BEFORE the cap):\n");
    {
        std::vector<spectrum::Partial> parts;
        for (int k = 1; k <= 100; ++k)
            parts.push_back ({ (float) k, 1.0f / (float) k });
        const double f0 = 100.0;
        const auto ampAt = [] (const std::vector<dissonance::Partial>& s, double freq) -> double
        {
            for (const auto& p : s)
                if (std::abs (p.freq - freq) < 1.0e-6) return p.amp;
            return -1.0;
        };

        // No edits: the 64 loudest COMPILED partials, anchored at f0.
        {
            const auto spec = dissonance::analysisSpectrum (parts, {}, f0);
            check (spec.size() == 64, "no edits: exactly the 64-partial analysis cap");
            check (std::abs (ampAt (spec, 100.0) - 1.0) < 1.0e-9, "partial 1 kept at freq ratio*f0, amp untouched");
            check (ampAt (spec, 100.0 * 100.0) < 0.0, "the quietest compiled partial is the one dropped");
        }

        // Edits: mute the loudest (gain 0), boost the quietest 40 dB (x100 -> sounding amp 1.0).
        std::vector<float> gain (100, 1.0f);
        gain[0]  = 0.0f;
        gain[99] = 100.0f;
        const auto spec = dissonance::analysisSpectrum (parts, gain, f0);
        check (spec.size() == 64, "with edits: still exactly 64 partials");
        check (ampAt (spec, 100.0) < 0.0,
               "a MUTED partial (the loudest compiled one) is not in the analysis");
        check (std::abs (ampAt (spec, 100.0 * 100.0) - 1.0) < 1.0e-6,
               "the BOOSTED quiet partial is analysed at its sounding amplitude (amp x gain)");
        check (std::abs (ampAt (spec, 200.0) - 0.5) < 1.0e-6,
               "an unedited partial keeps its compiled amplitude exactly");
        // The cap keeps the loudest SOUNDING partials: kept minimum >= dropped maximum.
        double keptMin = 1.0e30;
        for (const auto& p : spec) keptMin = std::min (keptMin, p.amp);
        double dropMax = 0.0;
        for (int k = 1; k <= 100; ++k)
        {
            const double sound = (1.0 / (double) k) * (double) gain[(size_t) k - 1];
            if (ampAt (spec, f0 * (double) k) < 0.0) dropMax = std::max (dropMax, sound);
        }
        std::printf ("  kept quietest %.6f   dropped loudest %.6f\n", keptMin, dropMax);
        check (keptMin >= dropMax - 1.0e-9, "the cap keeps the loudest SOUNDING partials, none louder was dropped");
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
    // THE REALTIME BUDGET. The tuning-follows-spectrum link re-runs this exact search
    // every time the spectrum changes, so its cost is the feature's latency. Measured on
    // this machine (12 cores) at the 64-partial analysis cap, before and after:
    //     3 notes, all vs all   1374 ms -> ~300 ms cold, ~120 ms on the cached tables
    //     3 notes, 0 vs all     2376 ms -> ~30 ms   (it was building a whole matrix to
    //                                                read one row of it)
    //     4 notes, all vs all   3045 ms -> ~320 ms with those tables already built
    // The follower used to lag audibly behind the sound it was supposed to be following.
    //
    // Everything below the timings is a FENCE around the answer. The search was made
    // parallel, table-cached and pruned; none of that is allowed to move a single
    // result, so the top-4 chords and their dissonance values are pinned here to the
    // values the single-threaded, uncached, unpruned version produced. A speedup that
    // shifts a ranking is a bug, not an optimisation - this is what says so.
    {
        std::printf ("\nchord search: cost and an exact fence on its results\n");
        std::vector<dissonance::Partial> big;          // the analysis cap, saw roll-off
        for (int k = 1; k <= 64; ++k)
            big.push_back ({ 261.6255653 * (double) k, 1.0 / (double) k });

        const auto run = [&big] (int notes, int count, bool rootOnly, double& msOut)
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto r = dissonance::searchChords (big, 2.0, notes, count, false, 50.0, rootOnly);
            msOut = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
            return r;
        };

        double msAll = 0.0, msAll2 = 0.0, msRoot = 0.0, msQuad = 0.0;
        const auto all  = run (3, 4, false, msAll);
        const auto all2 = run (3, 4, false, msAll2);   // second run: the cached tables
        const auto root = run (3, 4, true,  msRoot);
        // FOUR notes: 2.2 million combinations, which is where the branch-and-bound does
        // its work. It has to reach the same eight chords as the exhaustive scan did.
        const auto quad = run (4, 8, false, msQuad);
        std::printf ("  3 notes all vs all: %.0f ms (again, same spectrum: %.0f ms)   0 vs all: %.0f ms"
                     "   4 notes all vs all: %.0f ms\n", msAll, msAll2, msRoot, msQuad);
        const auto dump = [] (const char* tag, const std::vector<dissonance::ChordResult>& v)
        {
            for (const auto& r : v)
            {
                std::printf ("    %-10s value %.15g  alphas", tag, r.value);
                for (const double a : r.alphas) std::printf (" %.15g", a);
                std::printf ("\n");
            }
        };
        dump ("all-vs-all", all);
        dump ("0-vs-all", root);
        dump ("4-note", quad);

        // The fence. Values captured from the pre-optimisation build (single thread, no
        // cache, no pruning) - they are what "unchanged" means from here on.
        struct Fence { double value; double a1, a2, a3; };
        static const Fence kAll[] = {
            { 19.3419858948448, 1.50000056084297, 1.8750000170461,  0.0 },
            { 19.4016229826057, 1.49999307729161, 1.94303671932295, 0.0 },
            { 19.4063962706288, 1.49999307729161, 1.9285599852982,  0.0 },
            { 19.5135834250506, 1.49999307729161, 1.91914459359882, 0.0 },
        };
        static const Fence kRoot[] = {
            { 14.1229789745861, 1.66667375059104, 1.74999331540571, 0.0 },
            { 14.1307750170015, 1.74999331540571, 1.83332750118558, 0.0 },
            { 14.1320326277097, 1.75000870882375, 1.80131306216873, 0.0 },
            { 14.1493449256536, 1.66667375059104, 1.83332750118558, 0.0 },
        };
        static const Fence kQuad[] = {
            { 36.6296014709117, 1.29536498039975, 1.72715266142539, 1.94303671932295 },
            { 36.6517883765966, 1.25047367501992, 1.66728415085272, 1.8757095314529  },
            { 36.6794680166313, 1.29165651437782, 1.72221663410612, 1.9374854959832  },
            { 36.6869045473911, 1.29536498039975, 1.66546706473225, 1.94303671932295 },
            { 36.7141852821162, 1.2873018080426,  1.71640176896889, 1.93094206851233 },
            { 36.785578435077,  1.2499995439951,  1.66666543550174, 1.94303671932295 },
            { 36.796759806717,  1.29536498039975, 1.61920563480691, 1.94303671932295 },
            { 36.8278914609482, 1.29162921688404, 1.66667375059104, 1.93743488375364 },
        };
        const auto fence = [] (const std::vector<dissonance::ChordResult>& got,
                               const Fence* want, int n, int notes, const char* what)
        {
            bool ok = (int) got.size() == n;
            for (int i = 0; i < n && ok; ++i)
            {
                if ((int) got[(size_t) i].alphas.size() != notes) { ok = false; break; }
                const double* w = &want[i].a1;
                ok = std::abs (got[(size_t) i].value - want[i].value) <= 1.0e-9 * std::abs (want[i].value);
                for (int k = 1; k < notes && ok; ++k)
                    ok = std::abs (1200.0 * std::log2 (got[(size_t) i].alphas[(size_t) k] / w[k - 1])) < 1.0e-6;
            }
            check (ok, what);
        };
        fence (all,  kAll,  (int) (sizeof (kAll)  / sizeof (Fence)), 3, "3-note all-vs-all top 4: same chords, same values as before the optimisation");
        fence (root, kRoot, (int) (sizeof (kRoot) / sizeof (Fence)), 3, "3-note 0-vs-all top 4: same chords, same values as before the optimisation");
        fence (quad, kQuad, (int) (sizeof (kQuad) / sizeof (Fence)), 4, "4-note top 8: the pruned search reaches the same chords as the exhaustive one");

        // The cache must be an EXACT reuse, not an approximation: the same spectrum
        // searched twice has to give bit-identical results, or a cached table is
        // quietly answering a question it was not built for.
        bool identical = all.size() == all2.size();
        for (size_t i = 0; i < all.size() && identical; ++i)
        {
            if (all[i].value != all2[i].value || all[i].alphas.size() != all2[i].alphas.size())
                identical = false;
            for (size_t k = 0; k < all[i].alphas.size() && identical; ++k)
                if (all[i].alphas[k] != all2[i].alphas[k]) identical = false;
        }
        check (identical, "the same search run twice is bit-identical (cached tables answer the same question)");

        // ...and a DIFFERENT spectrum must never be answered out of the previous one's
        // tables. That is the whole risk of caching them, and it takes TWO probes to close
        // it, each the same SIZE as the cached spectrum so only the values can tell them
        // apart: one that moves the frequencies (stretched partials - a stretched timbre
        // wants a stretched scale, Sethares' own example) and one that moves only the
        // amplitudes (a steeper roll-off). A cache that compared anything less than every
        // value would hand one of them the other's chords.
        for (const int probe : { 0, 1 })
        {
            std::vector<dissonance::Partial> other;
            for (int k = 1; k <= 64; ++k)
                other.push_back ({ 261.6255653 * (probe == 0 ? std::pow ((double) k, 1.2) : (double) k),
                                   probe == 0 ? 1.0 / (double) k : (double) k / 64.0 });

            dissonance::clearGridCache();
            const auto cold = dissonance::searchChords (other, 2.0, 3, 4, false, 50.0, false);
            dissonance::clearGridCache();
            (void) dissonance::searchChords (big, 2.0, 3, 4, false, 50.0, false); // the cache now holds ANOTHER spectrum
            const auto warm = dissonance::searchChords (other, 2.0, 3, 4, false, 50.0, false);

            bool same = cold.size() == warm.size() && ! cold.empty();
            for (size_t i = 0; i < cold.size() && same; ++i)
            {
                if (cold[i].value != warm[i].value || cold[i].alphas.size() != warm[i].alphas.size())
                    same = false;
                for (size_t k = 0; k < cold[i].alphas.size() && same; ++k)
                    if (cold[i].alphas[k] != warm[i].alphas[k]) same = false;
            }
            std::printf ("  %-22s cold best %.9f   after another spectrum was searched %.9f\n",
                         probe == 0 ? "stretched partials:" : "steeper roll-off:",
                         cold.empty() ? 0.0 : cold[0].value, warm.empty() ? 0.0 : warm[0].value);
            check (same, probe == 0
                       ? "a spectrum whose partials MOVED gets its own tables, not the cached ones"
                       : "and so does one where only the AMPLITUDES moved");
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

        //----------------------------------------------------------------------
        // A SINGLE HELD NOTE.
        //
        // The realtime "spectrum follows what you play" link used to refuse anything under
        // two notes: a monophonic line lit the REALTIME button and then never adapted once,
        // for ever. Nothing in the model required that. With one note the target set is
        // {1.0}, every cross-note term vanishes, and what is left is the spectrum's own
        // INTERNAL roughness - the partials beating against EACH OTHER - which is precisely
        // what "minimum beating between the partials" means. These checks fence that it is
        // a real, register-dependent objective and not a constant the search can only shrug
        // at, and that the number the panel prints is the number that was optimised.
        {
            std::printf ("\n  a SINGLE held note (target = {1.0}):\n");
            const std::vector<double> one { 1.0 };

            // (1) THE IDENTITY the whole single-note path rests on. Both scopes collapse to
            // selfDissonance, so "0 vs all" and "all vs all" cannot disagree - which is what
            // keeps the scope selector honest instead of silently meaning two things - and
            // AdaptResult's seedScore/score (the panel's "beating: X -> Y") ARE the internal
            // roughness they claim to be.
            const double self = dissonance::selfDissonance (seed, 1.0);
            check (std::abs (dissonance::targetScore (seed, one, false) - self) < 1.0e-12
                   && std::abs (dissonance::targetScore (seed, one, true) - self) < 1.0e-12,
                   "one note: BOTH scopes are exactly the spectrum's internal roughness");

            // (2) NOT FLAT. A constant objective would leave coordinate descent nothing to
            // descend and the partials would sit exactly where the seed put them - which is
            // the failure mode this whole feature would have had if the measure were
            // degenerate. Slide one partial across 300 cents and watch the measure move.
            double vLo = 1.0e18, vHi = -1.0e18;
            for (double c = 500.0; c <= 800.0; c += 50.0)
            {
                auto t = seed;
                t[3].freq = f0 * std::exp2 (c / 1200.0);
                const double v = dissonance::targetScore (t, one, false);
                vLo = std::min (vLo, v); vHi = std::max (vHi, v);
            }
            std::printf ("    sliding partial 3 over 300 cents: %.4f .. %.4f\n", vLo, vHi);
            check (vHi - vLo > 0.05 * vLo,
                   "the single-note objective really depends on where the partials sit");

            // (3) It ADAPTS, in two registers, on POSITIONS ALONE - amplitude shaping off, so
            // the improvement cannot be credited to a reshaped envelope.
            const double topRatio = std::exp2 ((1200.0 - minGap) / 1200.0); // the plugin's own cap
            const auto seedAt = [&] (double base)
            {
                std::vector<dissonance::Partial> s;
                for (int i = 0; i < N; ++i)
                {
                    const double r = std::exp2 (1200.0 * (double) i / (double) N / 1200.0);
                    s.push_back ({ base * r, std::pow (r, -1.0) });
                }
                return s;
            };
            struct Reg { double f; dissonance::AdaptResult mn, mx; };
            Reg regs[2] { { f0 }, { f0 * 0.25 } };            // C4, and C2 two octaves below
            for (auto& R : regs)
            {
                const auto s = seedAt (R.f);
                R.mn = dissonance::adaptSpectrum (s, R.f, one, false, dissonance::Goal::Min,
                                                  0.0, 0.0, minGap, topRatio, false, 24.0, 6);
                R.mx = dissonance::adaptSpectrum (s, R.f, one, false, dissonance::Goal::Max,
                                                  0.0, 0.0, minGap, topRatio, false, 24.0, 6);
                std::printf ("    %8.3f Hz: internal roughness  seed %.4f -> min %.4f / max %.4f\n",
                             R.f, R.mn.seedScore, R.mn.score, R.mx.score);
                std::printf ("      min lands at cents:");
                for (const auto& p : R.mn.spectrum)
                    std::printf (" %.1f", 1200.0 * std::log2 (p.freq / R.f));
                std::printf ("\n");

                check ((int) R.mn.spectrum.size() == N && (int) R.mx.spectrum.size() == N,
                       "a single-note adaptation returns every partial it was given");
                check (std::abs (R.mn.spectrum[0].freq - R.f) < 1.0e-9
                       && std::abs (R.mx.spectrum[0].freq - R.f) < 1.0e-9,
                       "anchored on the note ACTUALLY played - partial 0 sits exactly there");
                check (R.mn.score < R.mn.seedScore * 0.9,
                       "Min really lowers the internal roughness of one note (>10% under the harmonic seed)");
                check (R.mx.score > R.mx.seedScore * 1.01,
                       "Max really raises it, so the goal selector is live with one note too");
                // The read-out cannot lie: what adaptSpectrum reports IS selfDissonance of
                // the spectra it started from and ended with.
                check (std::abs (R.mn.seedScore - dissonance::selfDissonance (seedAt (R.f), 1.0)) < 1.0e-12
                       && std::abs (R.mn.score - dissonance::selfDissonance (R.mn.spectrum, 1.0)) < 1.0e-12,
                       "and the before/after it reports IS that same internal roughness");
                // Fidelity: moved, never merged.
                double worstGap = 1.0e9;
                for (const auto& r : { R.mn, R.mx })
                    for (size_t i = 1; i < r.spectrum.size(); ++i)
                        worstGap = std::min (worstGap, 1200.0 * std::log2 (r.spectrum[i].freq
                                                                           / r.spectrum[i - 1].freq));
                check (worstGap >= minGap - 0.05,
                       "and no two of its partials come closer than the smallest interval");
            }

            // (4) THE REGISTER MATTERS, which is the reason the target is anchored at the
            // note's REAL frequency and not at key 60. Sethares' critical band is a width in
            // Hz, so the same partial set is a different roughness an octave down - if these
            // two answers were equal, the anchoring would be doing nothing.
            // Measured as each partial's interval above ITS OWN partial 0, so a whole
            // spectrum shifted bodily to some other register cannot pass for two different
            // answers - only genuinely different intervals can.
            double worstMove = 0.0;
            for (int i = 0; i < N; ++i)
                worstMove = std::max (worstMove,
                                      std::abs (1200.0 * std::log2 (regs[0].mn.spectrum[(size_t) i].freq / regs[0].mn.spectrum[0].freq)
                                              - 1200.0 * std::log2 (regs[1].mn.spectrum[(size_t) i].freq / regs[1].mn.spectrum[0].freq)));
            std::printf ("    biggest disagreement between the two registers' answers: %.1f cents\n", worstMove);
            check (worstMove > minGap,
                   "the two registers get genuinely DIFFERENT partial positions");

            // Sharper still: each register's own answer must beat the other register's
            // answer transplanted onto it. Equal positions could be a coincidence; this
            // cannot be - it says the search is solving the problem it was actually given.
            const auto transplantedTo = [] (const dissonance::AdaptResult& r, double from, double to)
            {
                std::vector<dissonance::Partial> t;
                t.reserve (r.spectrum.size());
                for (const auto& p : r.spectrum) t.push_back ({ p.freq / from * to, p.amp });
                return dissonance::selfDissonance (t, 1.0);
            };
            const double guestAtLow  = transplantedTo (regs[0].mn, regs[0].f, regs[1].f);
            const double guestAtHigh = transplantedTo (regs[1].mn, regs[1].f, regs[0].f);
            std::printf ("    C2's own answer %.4f vs C4's answer moved down %.4f;"
                         "  C4's own %.4f vs C2's moved up %.4f\n",
                         regs[1].mn.score, guestAtLow, regs[0].mn.score, guestAtHigh);
            check (regs[1].mn.score < guestAtLow && regs[0].mn.score < guestAtHigh,
                   "each register's answer is better THERE than the other register's answer is");

            // (5) Range is still a real third option with one note: aim at a band between the
            // min and the max just measured, one the seed is not already sitting in.
            {
                const double span = regs[0].mx.score - regs[0].mn.score;
                const double bLo  = regs[0].mn.score + 0.6 * span;
                const double bHi  = regs[0].mn.score + 0.8 * span;
                const auto rg = dissonance::adaptSpectrum (seedAt (f0), f0, one, false,
                                                           dissonance::Goal::Range, bLo, bHi,
                                                           minGap, topRatio, false, 24.0, 6);
                std::printf ("    range [%.4f, %.4f] -> %.4f  (seed %.4f)\n",
                             bLo, bHi, rg.score, rg.seedScore);
                check (rg.seedScore < bLo || rg.seedScore > bHi,
                       "the single-note range band starts out of reach of the seed (so the test can fail)");
                check (rg.score >= bLo - 1.0e-6 && rg.score <= bHi + 1.0e-6,
                       "Range lands INSIDE its band with a single held note too");
            }
        }
    }

    //--------------------------------------------------------------------------
    // OPTIMISING FOR A REGISTER RANGE, not for one pitch.
    //
    // Sethares' s = Dstar/(S1*f_lower + S2) makes the critical band a width in HERTZ, so
    // the roughness of a fixed set of ratios depends on where it is played. The whole
    // section below is one argument in four parts: the arithmetic that says a single-pitch
    // objective must misbehave, the misbehaviour measured, the range objective beating it,
    // and the sampling rule earned rather than assumed.
    {
        std::printf ("\nadapting over a REGISTER RANGE:\n");
        constexpr double kLowHz = 32.7031956625748;      // key 24 at A=440; the plugin names it C0
        const auto atOct = [] (double base, double oct) { return base * std::exp2 (oct); };

        //---------------------------------------------------------------------
        // (1) THE ARITHMETIC, checked against dissmeasure itself rather than trusted.
        // One pair of ratio r at lower frequency f1 contributes
        //     x = s*d = 0.24 * f1 * (r-1) / (0.0207*f1 + 18.96)
        // to Sethares' pair term, which peaks at x = ln(28.75/17.55)/2.24 = 0.2204.
        {
            const auto pairX = [] (double f1, double r)
            { return 0.24 * f1 * (r - 1.0) / (0.0207 * f1 + 18.96); };
            const auto pairD = [] (double f1, double r)
            {
                // dissmeasure of exactly that pair, both partials at amplitude 1.
                return dissonance::dissmeasure ({ { f1, 1.0 }, { f1 * r, 1.0 } });
            };
            const double xPeak = std::log (28.75 / 17.55) / 2.24;
            std::printf ("  Sethares' pair term peaks at x = %.4f\n", xPeak);
            check (std::abs (xPeak - 0.2204) < 5.0e-4, "the roughness peak really is at x = 0.2204");

            // The peak, found by brute force on the pair measure, must sit where x = xPeak.
            double bestX = 0.0, bestV = -1.0;
            for (double x = 0.001; x < 2.0; x += 0.0002)
            {
                const double v = 5.0 * (std::exp (-3.51 * x) - std::exp (-5.75 * x));
                if (v > bestV) { bestV = v; bestX = x; }
            }
            check (std::abs (bestX - xPeak) < 1.0e-3, "and a brute-force sweep of the term agrees");

            // The roughest INTERVAL therefore shrinks fast with register. Solve x = xPeak
            // for r at three registers, and confirm the pair measure really peaks there.
            for (const double oct : { 0.0, 2.0, 4.0 })
            {
                const double f1 = atOct (kLowHz, oct);
                const double r  = 1.0 + xPeak * (0.0207 * f1 + 18.96) / (0.24 * f1);
                const double c  = 1200.0 * std::log2 (r);
                // Brute force: the widest-beating interval at this f1, to 1 cent.
                double bc = 0.0, bv = -1.0;
                for (double cc = 5.0; cc < 900.0; cc += 1.0)
                {
                    const double v = pairD (f1, std::exp2 (cc / 1200.0));
                    if (v > bv) { bv = v; bc = cc; }
                }
                std::printf ("    %7.1f Hz: roughest interval %6.1f c (brute force %6.1f c)\n", f1, c, bc);
                check (std::abs (c - bc) < 2.0, "the roughest interval at this register is where the algebra puts it");
            }

            // The 32.6-cent watershed: x -> 11.594*(r-1) as f1 -> infinity, so a pair
            // reaches the peak on the way up only if 11.594*(r-1) > 0.2204.
            const double watershed = 1200.0 * std::log2 (1.0 + 0.2204 * 0.0207 / 0.24);
            std::printf ("  pairs wider than %.1f c cross the peak and then improve; narrower ones never do\n",
                         watershed);
            check (std::abs (watershed - 32.6) < 0.5, "the watershed is 32.6 cents");
            // A FIFTH is far above it: nearly gone five octaves up. Twenty cents is far below it:
            // six times worse five octaves up. This is the whole reason a bass-only
            // optimiser is not merely incomplete but actively wrong.
            const double fifthLo = pairD (kLowHz, 1.5),          fifthHi = pairD (atOct (kLowHz, 5.0), 1.5);
            const double tightLo = pairD (kLowHz, std::exp2 (20.0 / 1200.0));
            const double tightHi = pairD (atOct (kLowHz, 5.0), std::exp2 (20.0 / 1200.0));
            std::printf ("    a fifth : %.4f at 33 Hz -> %.4f at 1047 Hz   (x %.3f -> %.3f)\n",
                         fifthLo, fifthHi, pairX (kLowHz, 1.5), pairX (atOct (kLowHz, 5.0), 1.5));
            std::printf ("    20 cents: %.4f at 33 Hz -> %.4f at 1047 Hz\n", tightLo, tightHi);
            check (fifthHi < fifthLo * 0.05, "a fifth all but vanishes five octaves up");
            check (tightHi > tightLo * 3.0,  "a 20-cent pair gets several times WORSE five octaves up");
        }

        //---------------------------------------------------------------------
        // The shared setup for (2)-(4): one held note (the objective is then the
        // spectrum's own internal roughness), 12 partials, 30 cents of air - deliberately
        // just under the 32.6-cent watershed, so the optimiser is ALLOWED to build the
        // clusters that only bite higher up.
        const int    Nr      = 12;
        const double gapR    = 30.0;
        const double topR    = std::exp2 ((1200.0 - gapR) / 1200.0);
        const std::vector<double> oneNote { 1.0 };
        const auto seedR = [&] (double base)
        {
            std::vector<dissonance::Partial> s;
            for (int i = 0; i < Nr; ++i)
            {
                const double r = std::exp2 (1200.0 * (double) i / (double) Nr / 1200.0);
                s.push_back ({ base * r, std::pow (r, -1.0) });
            }
            return s;
        };
        const auto adaptOver = [&] (double anchor, double lo, double hi, int pts)
        {
            dissonance::Registers rg;
            rg.f0 = dissonance::registerPoints (lo, hi, pts);
            return dissonance::adaptSpectrum (seedR (anchor), anchor, oneNote, true,
                                              dissonance::Goal::Min, 0.0, 0.0, gapR, topR,
                                              false, 24.0, 8, rg);
        };
        // The TRUTH a candidate is judged by: a dense sweep, never the sampling it was
        // optimised on (that would let a coarse sampling grade its own homework).
        const auto judge = [&] (const std::vector<dissonance::Partial>& sp, double spF0,
                                double lo, double hi, double& mean, double& worst, double& worstHz)
        {
            const auto pts = dissonance::registerPoints (lo, hi,
                                 1 + (int) std::lround (12.0 * std::log2 (hi / lo)));
            const auto rs = dissonance::scoreOverRegisters (sp, spF0, pts, oneNote, true,
                                                            dissonance::Goal::Min, 0.0, 0.0);
            mean = rs.mean; worst = rs.worst;
            worstHz = pts[(size_t) rs.worstIndex];
        };
        const double lo1 = kLowHz, hi7 = atOct (kLowHz, 6.0);   // 33..2093 Hz, the range being argued about

        //---------------------------------------------------------------------
        // (2) THE FAILURE MODE, demonstrated on the OLD objective. Optimise at one pitch
        // in the bass (exactly what the search did before this change - the single-pitch
        // overload is still that code path) and two things must show up: the answer is a
        // stack of partials sitting ON the minimum gap, and it is far rougher up top than
        // a spectrum optimised up there.
        const auto bassOnly = dissonance::adaptSpectrum (seedR (lo1), lo1, oneNote, true,
                                                         dissonance::Goal::Min, 0.0, 0.0,
                                                         gapR, topR, false, 24.0, 8);
        const auto trebleOnly = dissonance::adaptSpectrum (seedR (hi7), hi7, oneNote, true,
                                                           dissonance::Goal::Min, 0.0, 0.0,
                                                           gapR, topR, false, 24.0, 8);
        {
            std::printf ("  single-pitch answer at %.1f Hz, partial cents:", lo1);
            for (const auto& p : bassOnly.spectrum)
                std::printf (" %.0f", 1200.0 * std::log2 (p.freq / lo1));
            std::printf ("\n");

            // Tight clusters: how many neighbour gaps sit ON the 30-cent floor?
            int atFloor = 0;
            for (size_t i = 1; i < bassOnly.spectrum.size(); ++i)
                if (1200.0 * std::log2 (bassOnly.spectrum[i].freq / bassOnly.spectrum[i - 1].freq)
                    < gapR + 1.0) ++atFloor;
            std::printf ("    %d of its %d neighbour gaps are pinned at the %.0f-cent floor\n",
                         atFloor, Nr - 1, gapR);
            check (atFloor >= (Nr - 1) / 2,
                   "optimising in the BASS packs partials into clusters - they cost nearly nothing there");

            // ...and those clusters are what dominates in the treble.
            const auto atTopBass = dissonance::scoreOverRegisters (bassOnly.spectrum, lo1, { hi7 },
                                                                   oneNote, true, dissonance::Goal::Min,
                                                                   0.0, 0.0);
            const auto atTopTop  = dissonance::scoreOverRegisters (trebleOnly.spectrum, hi7, { hi7 },
                                                                   oneNote, true, dissonance::Goal::Min,
                                                                   0.0, 0.0);
            std::printf ("    judged at %.0f Hz: bass-optimised %.3f vs treble-optimised %.3f (%.2fx worse)\n",
                         hi7, atTopBass.perRegister[0], atTopTop.perRegister[0],
                         atTopBass.perRegister[0] / atTopTop.perRegister[0]);
            check (atTopBass.perRegister[0] > atTopTop.perRegister[0] * 2.0,
                   "and up in the treble that bass-optimised spectrum is more than TWICE as rough as one optimised there");
        }

        //---------------------------------------------------------------------
        // (3) THE FIX BEATS IT. Same seed, same partials, same gap - only the objective
        // changes. The range objective (the MEAN over the registers, which is now the only
        // one) must lower the average across the range AND improve the worst register the
        // single-pitch answer leaves behind.
        double bassM = 0, bassW = 0, bassWHz = 0;
        judge (bassOnly.spectrum, lo1, lo1, hi7, bassM, bassW, bassWHz);
        const auto meanRun = adaptOver (lo1, lo1, hi7, 7);
        double meanM = 0, meanW = 0, meanWHz = 0;
        judge (meanRun.spectrum, lo1, lo1, hi7, meanM, meanW, meanWHz);
        {
            std::printf ("  over 33..2093 Hz (dense sweep of the ANSWERS):\n");
            std::printf ("    single pitch (33 Hz): mean %.3f   worst %.3f at %.0f Hz\n", bassM, bassW, bassWHz);
            std::printf ("    range (mean of 7)   : mean %.3f   worst %.3f at %.0f Hz\n", meanM, meanW, meanWHz);
            std::printf ("    the range objective is %.1f%% better on average, and %.1f%% better"
                         " at the worst register\n",
                         100.0 * (bassM - meanM) / bassM, 100.0 * (bassW - meanW) / bassW);
            check (meanM < bassM * 0.98,
                   "the range objective lowers the AVERAGE across the range against the single-pitch answer");
            // ...and it does NOT buy that average by sacrificing the worst register. It does
            // not protect it either - that was minimax's job, and minimax bought its worst
            // register with about 20% of the average. What matters for dropping minimax is
            // not that the mean replaces it (it does not) but that minimax had stopped doing
            // it: anchored at the played note and running upward, the worst register IS the
            // anchor, so minimax optimised one pitch and the width bought nothing. The honest
            // claim for the aggregate that ships is this one - a big win on the average, and
            // no loss at the extreme.
            check (meanW <= bassW,
                   "and does not pay for it at the worst register (1.2% better here, not worse)");
        }

        //---------------------------------------------------------------------
        // (4) THE SAMPLING RULE, EARNED - on the objective that actually ships. One octave
        // between points. The test that matters is not "does a coarse sampling estimate the
        // aggregate" but "does a spectrum optimised on it hold up when judged densely".
        //
        // The rule was originally earned on MINIMAX, which hunts a peak: at two octaves a
        // point the peak fell between the points and the answer came out +20.6% worse. That
        // aggregate is gone (see Dissonance.h - anchored at the played note and running
        // upward, its worst register is always the anchor, so it optimised one pitch and the
        // width stopped changing anything: 10.742 at every width). The MEAN is far more
        // forgiving, but the rule still earns itself on it: one octave a point lands within
        // 0.5% of a four-times-denser sampling, and two octaves a point comes out +2.2%
        // worse - so the spacing is bought, not assumed.
        {
            const auto ref = adaptOver (lo1, lo1, hi7, 25);
            double refM = 0, refW = 0, refH = 0;
            judge (ref.spectrum, lo1, lo1, hi7, refM, refW, refH);
            std::printf ("  MEAN sampling (6 octaves), judged densely against a 0.25-octave reference:\n");
            double w4 = 0, w7 = 0, m4 = 0, m7 = 0, h = 0;
            judge (adaptOver (lo1, lo1, hi7, 4).spectrum, lo1, lo1, hi7, m4, w4, h);
            judge (meanRun.spectrum, lo1, lo1, hi7, m7, w7, h);
            std::printf ("    reference (0.25 oct): mean %.3f\n", refM);
            std::printf ("    4 points (2.00 oct) : mean %.3f (%+.1f%%)\n", m4, 100.0 * (m4 - refM) / refM);
            std::printf ("    7 points (1.00 oct) : mean %.3f (%+.1f%%)\n", m7, 100.0 * (m7 - refM) / refM);
            check (std::abs (m7 - refM) < 0.03 * refM,
                   "one octave a point lands within 3% of a four-times-denser sampling");
            check (m4 > m7 * 1.01,
                   "and two octaves a point is measurably worse (+2.2%) - the rule is earned, not assumed");
        }

        //---------------------------------------------------------------------
        // (5) THE POINT-COUNT RULE the panel prints, and the caps the two paths use.
        {
            check (dissonance::registerCountFor (kLowHz, kLowHz, 7) == 1,
                   "a degenerate range is ONE register, not a division by zero");
            check (dissonance::registerCountFor (kLowHz, atOct (kLowHz, 2.0), 7) == 3,
                   "two octaves want three points (one octave a point)");
            check (dissonance::registerCountFor (kLowHz, atOct (kLowHz, 6.0), 7) == 7,
                   "six octaves want seven");
            check (dissonance::registerCountFor (kLowHz, atOct (kLowHz, 10.0), 7) == 7
                   && dissonance::registerCountFor (kLowHz, atOct (kLowHz, 10.0), 3) == 3,
                   "and the cap binds - the realtime path takes 3 where the Adapt button takes 7");
            // The realtime cap is NOT a degradation at the panel's default range: an octave
            // either side of the played note is two octaves, which wants three points anyway.
            check (dissonance::registerCountFor (kLowHz, atOct (kLowHz, 2.0), 3)
                   == dissonance::registerCountFor (kLowHz, atOct (kLowHz, 2.0), 7),
                   "at the default two-octave range the realtime follower samples EXACTLY as densely as Adapt");
            const auto pts = dissonance::registerPoints (kLowHz, atOct (kLowHz, 4.0), 5);
            bool geometric = pts.size() == 5;
            for (size_t i = 1; i < pts.size() && geometric; ++i)
                if (std::abs (pts[i] / pts[i - 1] - 2.0) > 1.0e-9) geometric = false;
            check (geometric, "the sampling points are evenly spaced in LOG frequency");
        }

        //---------------------------------------------------------------------
        // (6) NO REGRESSION, and no partial lost. The single-pitch overload must still be
        // exactly the old objective (one register at the anchor), and a range run must
        // return every partial it was given, ordered, with its air intact and audible.
        {
            dissonance::Registers oneReg;
            oneReg.f0 = { lo1 };
            const auto viaRange = dissonance::adaptSpectrum (seedR (lo1), lo1, oneNote, true,
                                                             dissonance::Goal::Min, 0.0, 0.0,
                                                             gapR, topR, false, 24.0, 8, oneReg);
            bool identical = viaRange.spectrum.size() == bassOnly.spectrum.size()
                          && std::abs (viaRange.score - bassOnly.score) < 1.0e-12;
            for (size_t i = 0; i < viaRange.spectrum.size() && identical; ++i)
                if (viaRange.spectrum[i].freq != bassOnly.spectrum[i].freq
                    || viaRange.spectrum[i].amp != bassOnly.spectrum[i].amp) identical = false;
            check (identical,
                   "one register IS the old single-pitch objective, bit for bit - nothing else changed under it");
            // And with one register the reported score is still exactly the score there.
            check (std::abs (bassOnly.score - dissonance::selfDissonance (bassOnly.spectrum, 1.0)) < 1.0e-12,
                   "and its read-out is still that register's own roughness");

            double worstGap = 1.0e9, quietest = 1.0e9;
            bool ordered = true;
            for (const auto& r : { meanRun })
            {
                if ((int) r.spectrum.size() != Nr) ordered = false;
                for (size_t i = 0; i < r.spectrum.size(); ++i)
                {
                    quietest = std::min (quietest, r.spectrum[i].amp);
                    if (i == 0) continue;
                    const double g = 1200.0 * std::log2 (r.spectrum[i].freq / r.spectrum[i - 1].freq);
                    worstGap = std::min (worstGap, g);
                    if (! (g > 0.0)) ordered = false;
                }
            }
            std::printf ("  range-optimised results: %d partials each, closest pair %.2f c, quietest %.5f\n",
                         Nr, worstGap, quietest);
            check (ordered && worstGap >= gapR - 0.05 && quietest > 0.0,
                   "a range-optimised spectrum keeps every partial, its air and its amplitude - nothing merged, nothing muted");
        }

        //---------------------------------------------------------------------
        // (7) THE READ-OUT CANNOT LIE. What adaptSpectrum reports as `score` is the
        // aggregate it optimised - the MEAN over the registers - recomputed here from the
        // spectrum it returned. The panel prints these two numbers as "before -> after" and
        // they have to be the thing the search actually worked on, not middle C standing in
        // for six octaves and not the worst register standing in for the average.
        {
            const auto rm = dissonance::scoreOverRegisters (meanRun.spectrum, lo1, meanRun.registerF0,
                                                            oneNote, true, dissonance::Goal::Min,
                                                            0.0, 0.0);
            check (std::abs (meanRun.score - rm.mean) < 1.0e-12
                   && std::abs (meanRun.mean - rm.mean) < 1.0e-12,
                   "the reported score IS the average over the range");
            // `worst` survives as a READ-OUT and nothing else: it must still be the largest
            // per-register value, so a caller quoting it is quoting the real thing.
            double biggest = 0.0;
            for (const double d : rm.perRegister) biggest = std::max (biggest, d);
            check (std::abs (meanRun.worst - biggest) < 1.0e-12,
                   "and `worst` still names the register that really scores worst");
            check ((int) meanRun.perRegister.size() == (int) meanRun.registerF0.size()
                   && meanRun.registerF0.size() == 7,
                   "with one number per register, for every register it was asked for");
            check (meanRun.score < meanRun.seedScore,
                   "and it really came down from the harmonic seed");
        }

        //---------------------------------------------------------------------
        // (8) HOW WIDE SHOULD THE RANGE BE? The panel asks for ONE number - N PERIODS EACH
        // WAY from the note being played - so the default N is a real decision and neither
        // "two octaves" nor "the same total width as before" is one anybody had measured.
        // This measures it, in the form the control now has.
        //
        // The same seed, the same anchor (the note under the fingers), optimised over
        // HALF-WIDTHS of 0 .. 2 periods with the SHIPPING sampling rule, then judged on a
        // dense sweep at three places that matter:
        //   - AT THE ANCHOR      : the register actually being played - the one thing the
        //                          player is guaranteed to hear;
        //   - WORST INSIDE       : the ugliest register the range promised to cover;
        //   - ONE PERIOD OUTSIDE : the hole question, and it is now a hole at BOTH ends. A
        //                          narrow band says nothing about the register just outside
        //                          it, and a player who moves an octave lands exactly there
        //                          - in either direction.
        // The reference for every probe is the spectrum optimised AT that register on its
        // own: that is the best any spectrum could do there, so the ratio is how much the
        // width choice costs somebody who plays there.
        {
            const double anch = atOct (kLowHz, 3.0);           // 261.6 Hz: the register a player sits in
            const auto atOne = [&] (const std::vector<dissonance::Partial>& sp, double spF0, double f)
            {
                return dissonance::scoreOverRegisters (sp, spF0, { f }, oneNote, true,
                                                       dissonance::Goal::Min, 0.0, 0.0).perRegister[0];
            };
            // HALF-WIDTH 0 is the old single-pitch behaviour, kept in the table as the
            // baseline every other row has to beat somewhere.
            const double halves[] = { 0.0, 0.5, 1.0, 1.5, 2.0 };
            constexpr int kRows = 5;
            // The probes are FIXED absolute registers - two periods BELOW the anchor up to
            // three above it - so a column means the same pitch in every row, and the half
            // of the keyboard the upward-only band never had an opinion about is now half
            // the table. (Judging each width "one period outside ITS OWN edge" compares a
            // different register per row, which is exactly how a width can look good by
            // being asked an easier question.)
            const double probes[] = { -2.0, -1.0, 0.0, 1.0, 2.0, 3.0 };
            constexpr int kProbes = 6;
            double refAt[kProbes] = {};
            for (int i = 0; i < kProbes; ++i)
            {
                const double f = atOct (anch, probes[i]);
                refAt[i] = atOne (adaptOver (anch, f, f, 1).spectrum, anch, f);
            }
            // The band the previous design produced, kept as a row of its own: N periods
            // UP from the anchor and nothing below it. It is the thing "por arriba y por
            // abajo" is a complaint about, so the table has to contain it or the change is
            // an assertion rather than a measurement.
            const auto upwardRun = adaptOver (anch, anch, atOct (anch, 2.0),
                                              dissonance::registerCountFor (anch, atOct (anch, 2.0),
                                                                            dissonance::kAdaptRegisterCap));
            double symBelow = 0.0, upBelow = 0.0;
            {
                std::printf ("  range HALF-WIDTH (each way), anchored at %.1f Hz (dense-judged):\n", anch);
                std::printf ("    +-width   pts   at anchor  worst inside  1 per outside"
                             "   | penalty vs a spectrum optimised for that register alone\n");
                std::printf ("    %-56s|   -2     -1     +0     +1     +2     +3 periods\n", "");
                double anchorAt[kRows] = {}, penOut[kRows] = {}, meanPen[kRows] = {};
                int row = 0;
                for (const double w : halves)
                {
                    const double lo = atOct (anch, -w), hi = atOct (anch, w);
                    const int pts = dissonance::registerCountFor (lo, hi, dissonance::kAdaptRegisterCap);
                    const auto run = adaptOver (anch, lo, hi, pts);
                    double m = 0, worst = 0, wHz = 0;
                    judge (run.spectrum, anch, lo, hi, m, worst, wHz);
                    const double here = atOne (run.spectrum, anch, anch);
                    // "One period outside" is now the MEAN of the two shoulders, because a
                    // symmetric band has two of them and reporting only the upper one would
                    // hide exactly the half this redesign is about.
                    const double outHi = atOne (run.spectrum, anch, atOct (hi,  1.0));
                    const double outLo = atOne (run.spectrum, anch, atOct (lo, -1.0));
                    const double refHi = atOne (adaptOver (anch, atOct (hi, 1.0), atOct (hi, 1.0), 1).spectrum,
                                                anch, atOct (hi, 1.0));
                    const double refLo = atOne (adaptOver (anch, atOct (lo, -1.0), atOct (lo, -1.0), 1).spectrum,
                                                anch, atOct (lo, -1.0));
                    const double pOut = 0.5 * (outHi / std::max (1.0e-9, refHi)
                                             + outLo / std::max (1.0e-9, refLo));
                    std::printf ("    %.1f per %4d  %10.3f  %12.3f  %8.3f %5.2fx |",
                                 w, pts, here, worst, 0.5 * (outHi + outLo), pOut);
                    double sum = 0.0;
                    for (int i = 0; i < kProbes; ++i)
                    {
                        const double pen = atOne (run.spectrum, anch, atOct (anch, probes[i]))
                                         / std::max (1.0e-9, refAt[i]);
                        std::printf (" %5.2fx", pen);
                        sum += pen;
                    }
                    std::printf ("\n");
                    anchorAt[row] = here; penOut[row] = pOut;
                    meanPen[row] = sum / (double) kProbes;
                    if (std::abs (w - dissonance::kAdaptRegisterDefaultPeriods) < 1.0e-9)
                        symBelow = atOne (run.spectrum, anch, atOct (anch, -1.0)) / std::max (1.0e-9, refAt[1]);
                    ++row;
                }
                std::printf ("    mean penalty over -2..+3 periods:");
                for (int i = 0; i < row; ++i) std::printf ("  %.1f per %.2fx", halves[i], meanPen[i]);
                std::printf ("\n");
                // (i) THE POINT OF THE WHOLE CHANGE: the band now buys something BELOW the
                // played note. Measured against the band the panel used to build - two
                // periods UP from the anchor, nothing under it - one period below the
                // anchor is a register the old form never looked at, and it shows.
                upBelow = atOne (upwardRun.spectrum, anch, atOct (anch, -1.0))
                        / std::max (1.0e-9, refAt[1]);
                std::printf ("    one period BELOW the anchor: %.2fx under the old upward-only"
                             " 2-period band, %.2fx under the symmetric +-%.1f one\n",
                             upBelow, symBelow, dissonance::kAdaptRegisterDefaultPeriods);
                check (symBelow < upBelow,
                       "the symmetric band really does account for the register BELOW the note played -"
                       " which the anchor-and-up band it replaces never did");
                // (ii) WIDENING COSTS THE PLAYED REGISTER, and the cost is real but
                // bounded. If it were not bounded the honest default would be "no
                // range at all", because the anchor is the one register the player is
                // certain to hear.
                std::printf ("    at the anchor: %.3f with no range -> %.3f at +-1 period"
                             " -> %.3f at +-2 periods\n", anchorAt[0], anchorAt[2], anchorAt[4]);
                check (anchorAt[2] < anchorAt[0] * 1.35,
                       "a one-period-each-way range costs the register being played less than 35%"
                       " against no range at all");
                // (iii) WHAT THE COVERAGE AVERAGE DOES *NOT* DECIDE. Under the upward-only
                // band this column was the whole argument (a zero-width answer was 1.51x
                // over +0..+4 against 1.25x for a two-period band), because the probes ran
                // four periods UP, where a spectrum optimised at one pitch falls apart.
                // Symmetrically the same numbers are nearly flat - every half-width lands
                // within a few percent of every other - so the coverage average CANNOT be
                // what picks the default, and pretending it does would be fitting a
                // decision to noise. Printed, checked as the flat thing it is, and the
                // decision is made by (ii) above and (v) below instead.
                double penLo = 1.0e9, penHi = 0.0;
                for (int i = 0; i < kRows; ++i) { penLo = std::min (penLo, meanPen[i]); penHi = std::max (penHi, meanPen[i]); }
                std::printf ("    one period outside a ZERO-width range: %.2fx"
                             " vs %.2fx outside a +-1-period range; the coverage average spans"
                             " only %.2f..%.2f over every half-width (%.1f%%)\n",
                             penOut[0], penOut[2], penLo, penHi, 100.0 * (penHi / penLo - 1.0));
                check (penHi < penLo * 1.15,
                       "across a symmetric band the coverage average barely separates one half-width from"
                       " another - so it is not what the default can be argued from");
                // (iv) THE WIDTH FIELD ACTUALLY DOES SOMETHING. This is the check that
                // replaces the deleted minimax aggregate rather than merely deleting it.
                // Under minimax the upward band's worst register was ALWAYS the anchor (the
                // band started there and ran up), so "only the worst register counts" was
                // "only the anchor counts" and the width field could not change the result
                // at all. Under the mean it has to, or the control is decorative.
                std::printf ("    the anchor moves %.1f%% between a zero-width and a +-2-period range"
                             " (under minimax it moved 0.0%%: 10.742 at every width)\n",
                             100.0 * std::abs (anchorAt[4] - anchorAt[0]) / anchorAt[0]);
                check (std::abs (anchorAt[4] - anchorAt[0]) > 0.05 * anchorAt[0],
                       "and the WIDTH really changes the answer - the control the minimax aggregate made inert");
            }
            // ...and the same question asked from three different registers, because a
            // default fitted to middle C would be a default fitted to one measurement. Only
            // the summary is printed: the mean penalty over the five registers from two
            // periods below the anchor to two above it, which is the number the choice
            // turns on.
            {
                std::printf ("  the same sweep from three anchors (mean penalty over -2..+2 periods):\n");
                const double anchors[] = { atOct (kLowHz, 2.0), atOct (kLowHz, 3.0), atOct (kLowHz, 5.0) };
                int bestCount[kRows] = {};
                double worstGiveUp = 1.0;
                for (const double a : anchors)
                {
                    double defPen = 0.0;   // the shipped default's own penalty at this anchor
                    double refs[5] = {};
                    for (int i = 0; i < 5; ++i)
                    {
                        const double f = atOct (a, (double) i - 2.0);
                        refs[i] = atOne (adaptOver (a, f, f, 1).spectrum, a, f);
                    }
                    std::printf ("    anchor %7.1f Hz:", a);
                    int best = 0; double bestVal = 1.0e9;
                    for (int r = 0; r < kRows; ++r)
                    {
                        const double lo = atOct (a, -halves[r]), hi = atOct (a, halves[r]);
                        const auto run = adaptOver (a, lo, hi,
                                                    dissonance::registerCountFor (lo, hi, dissonance::kAdaptRegisterCap));
                        double sum = 0.0;
                        for (int i = 0; i < 5; ++i)
                            sum += atOne (run.spectrum, a, atOct (a, (double) i - 2.0)) / std::max (1.0e-9, refs[i]);
                        const double pen = sum / 5.0;
                        std::printf ("  %.1f per %.2fx", halves[r], pen);
                        if (pen < bestVal) { bestVal = pen; best = r; }
                        if (std::abs (halves[r] - dissonance::kAdaptRegisterDefaultPeriods) < 1.0e-9)
                            defPen = pen;
                    }
                    std::printf ("   (best: %.1f periods each way, %.2fx; the default gives up %.1f%%)\n",
                                 halves[best], bestVal, 100.0 * (defPen / bestVal - 1.0));
                    ++bestCount[best];
                    worstGiveUp = std::max (worstGiveUp, defPen / bestVal);
                }
                // The DEFAULT is not the best cell of that table and does not need to be:
                // the spread is a few percent (see (iii)), so what has to hold is that the
                // shipped default gives up almost NOTHING on coverage against the best
                // width at every anchor. If it ever did, the two reasons it is chosen for -
                // the cost at the played register, and exact sampling under the realtime
                // cap - would be buying something the player could hear losing.
                std::printf ("    worst case, the default gives up %.1f%% of the best width's coverage\n",
                             100.0 * (worstGiveUp - 1.0));
                check (worstGiveUp < 1.05,
                       "from every anchor, the shipped default is within 5% of the best half-width measured");
            }
            // (v) THE REASON THE DEFAULT IS THIS WIDTH AND NOT A WIDER ONE: it is the
            // widest half-width the REALTIME follower samples exactly. The objective is
            // evaluated at one point per octave and the follower's cap is 3 points
            // (kAdaptRegisterCapRt), so at +-1 period in an octave-periodic tuning the
            // follower and the Adapt button optimise over literally the same registers -
            // while at +-1.5 the follower is already sampling its own band more coarsely
            // than the rule asks for. Nothing about that is degraded silently (the panel
            // prints both counts), but it is a real difference and the default should not
            // start on the wrong side of it.
            {
                const auto sameBothCaps = [&] (double half)
                {
                    const double lo = atOct (anch, -half), hi = atOct (anch, half);
                    return dissonance::registerCountFor (lo, hi, dissonance::kAdaptRegisterCapRt)
                        == dissonance::registerCountFor (lo, hi, dissonance::kAdaptRegisterCap);
                };
                std::printf ("    exact under the realtime cap: +-1.0 %s, +-1.5 %s, +-2.0 %s\n",
                             sameBothCaps (1.0) ? "yes" : "no", sameBothCaps (1.5) ? "yes" : "no",
                             sameBothCaps (2.0) ? "yes" : "no");
                check (sameBothCaps (dissonance::kAdaptRegisterDefaultPeriods),
                       "the default band is sampled EXACTLY by the realtime follower, not just by the button");
                check (! sameBothCaps (dissonance::kAdaptRegisterDefaultPeriods + 0.5),
                       "and it is the widest one that is - half a period more and the follower under-samples");
            }
            // WHAT THE ANCHOR-AND-UP FORM COSTS IN TIME, which is not free and shows up in
            // the realtime follower's landing latency (RhythmVerify times it end to end).
            // Same width, same sampling, same seed - only the PLACEMENT changes: two periods
            // ABOVE the played note, as the panel now asks for, against the octave-either-
            // side band it used to use. The optimiser sweeps longer up there, and this is
            // the number the follower's budget is derived from rather than guessed at.
            {
                // The follower's own shape: 8 partials, a triad under the fingers, 50 cents
                // of air, amplitudes shaped, at the realtime register cap.
                const int Nrt = 8;
                const double gRt = 50.0, tRt = std::exp2 ((1200.0 - gRt) / 1200.0);
                const std::vector<double> triad { 1.0, std::exp2 (4.0 / 12.0), std::exp2 (7.0 / 12.0) };
                std::vector<dissonance::Partial> seed8;
                for (int i = 0; i < Nrt; ++i)
                {
                    const double r = std::exp2 ((double) i / (double) Nrt);
                    seed8.push_back ({ anch * r, std::pow (r, -1.0) });
                }
                const auto timeOne = [&] (const char* what, double lo, double hi)
                {
                    dissonance::Registers rg;
                    rg.f0 = dissonance::registerPoints (lo, hi,
                                dissonance::registerCountFor (lo, hi, dissonance::kAdaptRegisterCapRt));
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto r = dissonance::adaptSpectrum (seed8, anch, triad, true,
                                                              dissonance::Goal::Min, 0.0, 0.0, gRt, tRt,
                                                              true, 24.0, 8, rg);
                    const double ms = std::chrono::duration<double, std::milli> (
                                          std::chrono::steady_clock::now() - t0).count();
                    std::printf ("    %-28s %d points, %2d sweeps, %6.1f ms\n",
                                 what, (int) rg.f0.size(), r.sweepsDone, ms);
                    return ms;
                };
                std::printf ("  the SAME two-period band, placed three ways (the follower's own shape):\n");
                const double upward   = timeOne ("two periods up (old)",      anch, atOct (anch, 2.0));
                const double straddle = timeOne ("+-1 period (now)",          atOct (anch, -1.0), atOct (anch, 1.0));
                const double wide     = timeOne ("+-2 periods",               atOct (anch, -2.0), atOct (anch, 2.0));
                std::printf ("    the symmetric form costs %.2fx the time of the upward one at the same"
                             " total width, and %.2fx at twice it\n",
                             straddle / std::max (1.0e-9, upward), wide / std::max (1.0e-9, upward));
            }
            // ...and the DEFAULT the panel ships (kAdaptRegisterDefaultPeriods) has to be one
            // of the half-widths this table measured, or the table is not about the product.
            std::printf ("  the panel's default range is +-%.1f periods of the loaded tuning\n",
                         dissonance::kAdaptRegisterDefaultPeriods);
            bool measured = false;
            for (const double w : halves)
                if (std::abs (w - dissonance::kAdaptRegisterDefaultPeriods) < 1.0e-9) measured = true;
            check (measured, "and that default is a half-width this table actually measured");
        }
    }

    //--------------------------------------------------------------------------
    // THE ADAPTATION COST MODEL. The panel prints "Adapt ~14 s" on the button and only
    // refuses genuinely absurd requests, so that number has to MEAN something - a
    // made-up figure is worse than no figure, because a user plans around it.
    //
    // What is being checked is the SHAPE of dissonance::adaptTotalPairs, not the clock:
    // one nanoseconds-per-pair constant is fitted across configurations that differ by
    // more than 100x in work, in both scopes, with and without amplitude shaping, at 1
    // and at 7 registers. If the pair count were wrong in any of its factors - registers,
    // line searches, scope - no single constant could fit them all and the spread below
    // would blow up. (The previous refusal claimed "31 notes x 16 partials is minutes,
    // not milliseconds". Row 6 is exactly that shape and the table says what it is.)
    {
        std::printf ("\nadaptation cost model (predicted vs measured):\n");
        struct Cfg { int N, A, R, sweeps; bool rootOnly, amps; const char* what; };
        const Cfg cfgs[] = {
            {  8,  1, 1, 6, true,  false, "8 partials, 1 note, 0-vs-all, 1 reg" },
            {  8, 12, 3, 6, true,  true,  "8 partials, 12 notes, 0-vs-all, 3 reg, +amps" },
            { 12,  4, 1, 6, false, false, "12 partials, 4 notes, all-vs-all, 1 reg" },
            { 16,  7, 7, 2, true,  true,  "16 partials, 7 notes, 0-vs-all, 7 reg, +amps" },
            { 12, 31, 1, 2, true,  false, "12 partials, 31 notes, 0-vs-all, 1 reg" },
            { 16, 31, 1, 1, false, false, "16 partials, 31 notes, ALL-vs-all, 1 reg" } };

        double worstRatio = 1.0, bestRatio = 1.0e9, sumPairs = 0.0, sumMs = 0.0;
        std::vector<double> rowRatios;   // every timeable row, for the MEDIAN below
        for (const auto& c : cfgs)
        {
            const double f0 = 220.0;
            const double totalC = 1200.0 * std::log2 (4.0);   // a 4/1 period: room for 16 partials
            const double gap = 20.0;
            const double top = std::exp2 ((totalC - gap) / 1200.0);
            std::vector<dissonance::Partial> seed;
            for (int i = 0; i < c.N; ++i)
            {
                const double r = std::exp2 (totalC * (double) i / (double) c.N / 1200.0);
                seed.push_back ({ f0 * r, std::pow (r, -1.0) });
            }
            std::vector<double> alphas;
            for (int i = 0; i < c.A; ++i) alphas.push_back (std::exp2 ((double) i / 31.0));
            dissonance::Registers rg;
            rg.f0 = dissonance::registerPoints (f0 * 0.5, f0 * 4.0, c.R);

            const auto t0 = std::chrono::steady_clock::now();
            const auto res = dissonance::adaptSpectrum (seed, f0, alphas, c.rootOnly,
                                                        dissonance::Goal::Min, 0.0, 0.0, gap, top,
                                                        c.amps, 6.0, c.sweeps, rg);
            const double ms = std::chrono::duration<double, std::milli> (
                                  std::chrono::steady_clock::now() - t0).count();
            // Predicted against the sweeps that REALLY ran: adaptSpectrum stops early when
            // a sweep buys nothing, and charging the model for sweeps it never did would
            // be fitting the constant to the optimiser's luck instead of to the machine.
            const int    done = std::max (1, res.sweepsDone);
            const double pairs = dissonance::adaptTotalPairs (c.N, c.A, c.rootOnly, c.R, c.amps, done);
            const double pred  = dissonance::adaptEstimateMs (c.N, c.A, c.rootOnly, c.R, c.amps, done);
            const double ratio = pred / std::max (0.001, ms);
            sumPairs += pairs; sumMs += ms;
            // Sub-millisecond runs are excluded from the BAND (never from the fit): at
            // 0.5 ms the measurement is the allocator, the cold cache and the clock's own
            // resolution, and it swings between x0.38 and x0.61 run to run on an idle
            // machine. Holding the model to that would be testing the timer.
            if (ms >= 2.0)
            {
                worstRatio = std::max (worstRatio, ratio);
                bestRatio  = std::min (bestRatio, ratio);
                rowRatios.push_back (ratio);
            }
            std::printf ("    %-46s  %2d sweeps  %10.0f pairs   predicted %8.1f ms   measured %8.1f ms   x%.2f%s\n",
                         c.what, done, pairs, pred, ms, ratio, ms < 2.0 ? "  (below the timing floor)" : "");
            // Partials are never dropped, whatever the cost: the run that the old dialog
            // called impossible still comes back with every partial it was given.
            check ((int) res.spectrum.size() == c.N,
                   (std::string ("  ...and it returned all ") + std::to_string (c.N) + " partials").c_str());
        }
        const double fitted = sumMs * 1.0e6 / std::max (1.0, sumPairs);
        const double probed = dissonance::adaptNsPerPair();
        std::printf ("    fitted %.2f ns/pair over the whole table; the runtime probe said %.2f"
                     " (fallback %.2f); prediction band x%.2f .. x%.2f\n",
                     fitted, probed, dissonance::kAdaptNsPerPairFallback, bestRatio, worstRatio);
        // A single constant has to cover every timeable row within a factor of two either
        // way. That is loose enough for a cold cache and a busy machine and far too tight
        // for a wrong exponent: dropping the register factor alone puts row 4 out by 7x.
        // This is the SHAPE check - it fits its own constant and so says nothing about
        // whether the number on the button is right.
        check (bestRatio > 0.5 && worstRatio < 2.0,
               "one ns/pair constant predicts every configuration within a factor of 2");

        // ...AND THE NUMBER THE PANEL ACTUALLY PRINTS TRACKS THE CLOCK.
        //
        // This replaces "a literal in the header is close to a fit computed here", which
        // compared two constants and said nothing at all about the user's experience.
        // What matters is predicted/measured for the estimate the button really shows,
        // and in particular that it is not systematically LOW: with the hard-coded 9.8
        // EVERY timeable row came in under its prediction - x0.48 .. x0.67 across every
        // measurement taken - so every search took 1.5x to 2x longer than the plugin had
        // promised, and 2-3x on a loaded machine. Nobody notices a red test; they notice
        // waiting three times as long as the button said.
        //
        // Asserted on the MEDIAN row rather than the worst one. These rows take 35 ms to
        // 1 s, and a single one of them can lose a scheduling slice and swing 30% on its
        // own; the worst-of-six is therefore a measurement of the busiest millisecond of
        // the test run, not of the model. The median over five timeable rows moves only
        // if the model is really off. Measured over ~25 repeat runs on ONE machine, whose
        // own fitted cost was seen anywhere between 11.2 and 21.8 ns/pair depending on
        // how busy and how warm it was - which is itself the argument against any literal:
        //     hard-coded 9.8 ns/pair   ->  median row x0.52 .. x0.83  (0.52 while the
        //                                  machine ran slow, 0.83 while it ran fast: the
        //                                  constant is only ever right by coincidence)
        //     runtime-calibrated       ->  median row x0.83 .. x1.58, wherever the
        //                                  machine happened to be - it follows it
        // 0.80 is the line between them. The upper end is a loose 1.80 on purpose: the
        // probe runs once, at process start, where a cold clock still reads high, and
        // over-estimating is only a discourtesy - the run finishes early - while
        // under-estimating is the defect this exists to remove.
        std::sort (rowRatios.begin(), rowRatios.end());
        const double medianRatio = rowRatios.empty() ? 0.0 : rowRatios[rowRatios.size() / 2];
        std::printf ("    median row x%.2f over %d timeable rows;"
                     " the probe is x%.2f of the table's own fit\n",
                     medianRatio, (int) rowRatios.size(), probed / std::max (1.0e-9, fitted));
        check (medianRatio > 0.80 && medianRatio < 1.80,
               "the estimate the panel prints tracks the clock - it is not systematically optimistic");
        // ...and the probe's small configuration really does predict the big ones: it is
        // measured on ~0.3 million pair terms and used to price runs of 53 million. With
        // the old literal this ratio was 0.47 .. 0.87 (it drifts with the machine, not with the
        // problem); calibrated it is 0.81 .. 1.65.
        check (probed > fitted * 0.8 && probed < fitted * 2.0,
               "the ~0.3M-pair probe generalises to the 53M-pair run it is used to price");
        // THE HARD BOUND is the only thing left that refuses, and it must refuse only the
        // absurd: 16 partials against a 31-note scale in "all vs all" - the configuration
        // the old dialog rejected outright - has to be OFFERED, and a 24-partial spectrum
        // against a full 128-key table in the same scope has to be the thing that is not.
        //
        // Asserted on PAIR COUNTS against kAdaptAbsurdPairs, which is what the plugin now
        // compares too. This assertion used to compare calibrated ms estimates against
        // kAdaptAbsurdMs, and one run under heavy load showed why that was wrong twice
        // over: the whole table fitted at 176 ns/pair (11x nominal), the estimate
        // honestly said the 31x16 search would take over ten minutes THAT MINUTE - and
        // the check failed, i.e. the plugin would have refused a search it exists to
        // offer, because a load spike had been cached into the one-shot probe. The set
        // of offered searches is a property of the REQUEST; only its price follows the
        // machine.
        {
            const double offered = dissonance::adaptTotalPairs (16, 31, false, 7, true, 6);
            const double absurd  = dissonance::adaptTotalPairs (24, 128, false, 7, true, 6);
            std::printf ("    31 notes x 16 partials all-vs-all, 7 reg, +amps: %.2e pairs (~%.1f s here); bound %.2e pairs\n"
                         "    128 notes x 24 partials all-vs-all, 7 reg, +amps: %.2e pairs (~%.0f s here)\n",
                         offered, dissonance::adaptEstimateMs (16, 31, false, 7, true, 6) * 0.001,
                         dissonance::kAdaptAbsurdPairs,
                         absurd, dissonance::adaptEstimateMs (24, 128, false, 7, true, 6) * 0.001);
            check (offered < dissonance::kAdaptAbsurdPairs,
                   "the search the old dialog refused is under the hard bound - it is offered ON EVERY MACHINE");
            check (absurd > dissonance::kAdaptAbsurdPairs,
                   "and a 128-note table at 24 partials all-vs-all is over it - that one is refused");
        }
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
    // THE PRESET PLAYGROUND: the weights, the log-vs-linear choice, the exclusion
    // list, the nearest-corner hysteresis and the spectrum crossfade. Everything here
    // is checkable without a plugin, a window or a mouse - which is the point, because
    // "does the middle of the pad sound like the middle" is a numeric claim.
    {
        using namespace presetmorph;
        std::printf ("\npreset playground - weights:\n");

        const bool all4[4]  = { true, true, true, true };
        const bool topEdge[4] = { true, true, false, false };
        const bool diag[4]  = { true, false, false, true };
        const bool tri[4]   = { true, true, true, false };
        double w[4];

        {
            bool ok = true;
            const double corners[4][2] = { {0,0}, {1,0}, {0,1}, {1,1} };
            for (int i = 0; i < 4; ++i)
            {
                bilinearWeights (corners[i][0], corners[i][1], all4, w);
                for (int k = 0; k < 4; ++k)
                    ok = ok && std::abs (w[k] - (k == i ? 1.0 : 0.0)) < 1e-12;
            }
            check (ok, "each corner of a full playground is exactly its own preset, nothing else");
        }
        bilinearWeights (0.5, 0.5, all4, w);
        check (std::abs (w[0] - 0.25) < 1e-12 && std::abs (w[1] - 0.25) < 1e-12
               && std::abs (w[2] - 0.25) < 1e-12 && std::abs (w[3] - 0.25) < 1e-12,
               "the centre of a full playground is a quarter of each");
        bilinearWeights (0.25, 0.0, all4, w);
        check (std::abs (w[0] - 0.75) < 1e-12 && std::abs (w[1] - 0.25) < 1e-12
               && w[2] == 0.0 && w[3] == 0.0,
               "on an edge only that edge's two corners contribute, linearly");

        // TWO presets = a LINE. The unused axis must do nothing at all, INCLUDING on the
        // far edge where the raw bilinear weights all collapse to zero.
        {
            bool ok = true;
            for (double y : { 0.0, 0.3, 1.0 })
            {
                bilinearWeights (0.3, y, topEdge, w);
                ok = ok && std::abs (w[0] - 0.7) < 1e-6 && std::abs (w[1] - 0.3) < 1e-6
                        && w[2] == 0.0 && w[3] == 0.0;
            }
            check (ok, "with two corners on one edge the OTHER axis is inert - even against the empty edge");
        }
        bilinearWeights (1.0, 0.0, diag, w);
        check (std::abs (w[0] - 0.5) < 1e-6 && std::abs (w[3] - 0.5) < 1e-6,
               "a diagonal pair reads 50/50 on the empty corner instead of dividing by zero");
        bilinearWeights (1.0, 1.0, tri, w);
        check (w[3] == 0.0 && std::abs (w[0] + w[1] + w[2] - 1.0) < 1e-9,
               "an unassigned corner never contributes, and the rest still sum to one");
        {
            const bool none[4] = { false, false, false, false };
            bilinearWeights (0.5, 0.5, none, w);
            check (w[0] == 0.0 && w[1] == 0.0 && w[2] == 0.0 && w[3] == 0.0,
                   "nothing assigned = no weight anywhere (the morph must not drive)");
        }

        std::printf ("preset playground - log vs linear:\n");
        // The claim that makes the whole feature musical: halfway between a 20 ms decay
        // and a 2 s decay is 200 ms, not 1.01 s.
        {
            const double v[2] = { 0.02, 2.0 }, ww[2] = { 0.5, 0.5 };
            const double lg = blendScalar (v, ww, 2, MorphSpace::Log);
            const double ln = blendScalar (v, ww, 2, MorphSpace::Linear);
            std::printf ("  20 ms vs 2 s at the midpoint: log %.4f s, linear %.4f s\n", lg, ln);
            check (std::abs (lg - 0.2) < 1e-9, "log space puts the middle of 20 ms..2 s at 200 ms");
            check (std::abs (ln - 1.01) < 1e-9, "...and the linear answer really is the 1.01 s smear");
        }
        {
            // Base Harmonic: the played pitch is -log2(N) octaves, so halfway from 1 to
            // 16 must be 4 (two octaves), not 8.5.
            const double v[2] = { 1.0, 16.0 }, ww[2] = { 0.5, 0.5 };
            check (std::abs (blendScalar (v, ww, 2, MorphSpace::Log) - 4.0) < 1e-9,
                   "Base Harmonic 1 -> 16 passes through 4, the two-octave midpoint");
        }
        check (blendScalar (nullptr, nullptr, 0, MorphSpace::Linear) == 0.0,
               "no contributing corner is not a divide by zero");

        check (morphSpaceFor ("ampDecay", false) == MorphSpace::Log
               && morphSpaceFor ("filterAttack", false) == MorphSpace::Log
               && morphSpaceFor ("pitchRelease", false) == MorphSpace::Log
               && morphSpaceFor ("morphTime", false) == MorphSpace::Log
               && morphSpaceFor ("modE2D", false) == MorphSpace::Log
               && morphSpaceFor ("modL1Hz", false) == MorphSpace::Log
               && morphSpaceFor ("filterResonance", false) == MorphSpace::Log
               && morphSpaceFor ("baseN", false) == MorphSpace::Log
               && morphSpaceFor ("rhythmDivX", false) == MorphSpace::Log,
               "times, rates, Q, the harmonic index and the divider BASE average in log space");
        check (morphSpaceFor ("rhythmFeedbackDecay", false) == MorphSpace::Linear,
               "a FRACTION that merely ends in \"Decay\" is not a time");
        check (morphSpaceFor ("ampCurveD", false) == MorphSpace::Linear
               && morphSpaceFor ("modE1CurveR", false) == MorphSpace::Linear,
               "envelope segment CURVES are shapes in -1..+1, not times");
        check (morphSpaceFor ("modE1S", false) == MorphSpace::Linear,
               "an envelope SUSTAIN is a level, not a time");
        check (morphSpaceFor ("filterCutoff", false) == MorphSpace::Linear
               && morphSpaceFor ("masterGain", false) == MorphSpace::Linear
               && morphSpaceFor ("pitchEnvAmount", false) == MorphSpace::Linear
               && morphSpaceFor ("filterEnvAmount", false) == MorphSpace::Linear
               && morphSpaceFor ("curveSlope", false) == MorphSpace::Linear
               && morphSpaceFor ("harmonics", false) == MorphSpace::Linear
               && morphSpaceFor ("primeLimit", false) == MorphSpace::Linear
               && morphSpaceFor ("fractalLevel", false) == MorphSpace::Linear
               && morphSpaceFor ("rhythmDivN", false) == MorphSpace::Linear,
               "cents, dB, semitones, octaves, exponents and the continuous selectors stay linear");
        check (morphSpaceFor ("phaseMode", true) == MorphSpace::Discrete
               && morphSpaceFor ("timbreOn", true) == MorphSpace::Discrete
               && morphSpaceFor ("partialManual", true) == MorphSpace::Discrete
               && morphSpaceFor ("morph", true) == MorphSpace::Discrete
               && morphSpaceFor ("primeMode", true) == MorphSpace::Discrete,
               "every choice/bool is discrete, decided by the parameter's TYPE not its name");
        check (morphSpaceFor ("somethingInventedTomorrow", false) == MorphSpace::Linear
               && morphSpaceFor ("somethingInventedTomorrow", true) == MorphSpace::Discrete,
               "a parameter nobody has written yet morphs conservatively - it is never excluded by accident");

        std::printf ("preset playground - the exclusion list (TUNING IS NOT MORPHED):\n");
        {
            bool ok = true;
            for (const char* id : { "tuningMode", "bendRange", "refA", "octaveShift",
                                    "tuningBankSlot", "specRandom", "pgX", "pgY", "pgMode", "pgVoices" })
                ok = ok && morphSpaceFor (id, false) == MorphSpace::Excluded
                        && morphSpaceFor (id, true)  == MorphSpace::Excluded;
            check (ok, "tuning mode, bend range, reference A, octave, bank slot, the random TRIGGER, "
                       "the pad's own coordinates AND the playground's own mode + voice budget are "
                       "excluded - discrete or not (a corner saved in mix mode must not flip the pad)");
        }

        std::printf ("preset playground - MIX gains (equal power):\n");
        {
            // The law itself: gains are sqrt(weights), so total POWER (sum of gain^2)
            // is 1 wherever any corner has weight - no crossfade hole in the middle of
            // an edge, no bump at a corner.
            double g[4];
            bool okPower = true, okZero = true;
            for (int xi = 0; xi <= 10 && okPower; ++xi)
                for (int yi = 0; yi <= 10 && okPower; ++yi)
                {
                    bilinearWeights (xi / 10.0, yi / 10.0, all4, w);
                    mixGains (w, g);
                    double p = 0.0;
                    for (int i = 0; i < 4; ++i)
                    {
                        p += g[i] * g[i];
                        if (w[i] <= 0.0 && g[i] != 0.0) okZero = false;
                    }
                    if (std::abs (p - 1.0) > 1e-9) okPower = false;
                }
            check (okPower, "sum of squared gains is 1 across the whole pad - constant total power");
            check (okZero,  "a corner with no weight gets gain 0 exactly - it contributes silence");

            bilinearWeights (0.0, 0.0, all4, w);
            mixGains (w, g);
            check (g[0] == 1.0 && g[1] == 0.0 && g[2] == 0.0 && g[3] == 0.0,
                   "at a vertex only that corner sounds, at exactly full level");

            bilinearWeights (0.5, 0.0, topEdge, w);
            mixGains (w, g);
            check (std::abs (g[0] - std::sqrt (0.5)) < 1e-12
                   && std::abs (g[1] - std::sqrt (0.5)) < 1e-12,
                   "the middle of a two-corner edge is 0.707 each (-3 dB), not the 0.5 sag");
        }

        std::printf ("preset playground - nearest corner with hysteresis:\n");
        {
            // A two-corner line swept left to right and back. The flip must happen ONCE
            // each way, at different places, and never chatter in the middle.
            int cur = 0, flipsUp = 0, flipsDown = 0;
            double upAt = -1.0, downAt = -1.0;
            for (int i = 0; i <= 1000; ++i)
            {
                const double x = i / 1000.0;
                bilinearWeights (x, 0.0, topEdge, w);
                const int n = nearestCorner (w, cur, kHysteresis);
                if (n != cur) { ++flipsUp; upAt = x; cur = n; }
            }
            for (int i = 1000; i >= 0; --i)
            {
                const double x = i / 1000.0;
                bilinearWeights (x, 0.0, topEdge, w);
                const int n = nearestCorner (w, cur, kHysteresis);
                if (n != cur) { ++flipsDown; downAt = x; cur = n; }
            }
            std::printf ("  flips: %d going right (at x=%.3f), %d coming back (at x=%.3f)\n",
                         flipsUp, upAt, flipsDown, downAt);
            check (flipsUp == 1 && flipsDown == 1, "exactly one flip each way across the whole travel");
            check (upAt > 0.53 && upAt < 0.56 && downAt > 0.44 && downAt < 0.47,
                   "and the two thresholds are apart - that gap IS the hysteresis");
            check (cur == 0, "sweeping there and back ends on the corner it started on");
        }
        {
            // The boundary jitter that made this necessary: a point parked at dead centre
            // while automation wobbles it by a hair must not flip anything.
            int cur = 0, flips = 0;
            for (int i = 0; i < 2000; ++i)
            {
                const double x = 0.5 + ((i & 1) ? 1e-7 : -1e-7);
                bilinearWeights (x, 0.5, all4, w);
                const int n = nearestCorner (w, cur, kHysteresis);
                if (n != cur) { ++flips; cur = n; }
            }
            check (flips == 0, "a point resting on a boundary does not flap the engine 60 times a second");
        }
        {
            // An incumbent whose corner has just been emptied must be dropped at once,
            // or every discrete setting would keep pointing at a preset that is gone.
            const bool only23[4] = { false, false, true, true };
            bilinearWeights (0.0, 1.0, only23, w);
            check (nearestCorner (w, 0, kHysteresis) == 2, "an un-assigned incumbent is abandoned immediately");
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("\npreset playground - blending compiled spectra (across engines):\n");
    {
        SpectrumSettings a;                       // harmonic, base 1
        a.reach = 16; a.curveSlope = 1.0f;
        SpectrumSettings b = a;                   // a custom scale: nothing in common to average
        b.source = Source::ScalaTimbre;
        b.timbreDegrees = { 1.0f, 1.25f, 1.5f };
        b.timbrePeriod = 2.0f; b.timbrePeriodRepeats = 2;
        const auto tA = compileSpectrum (a);
        const auto tB = compileSpectrum (b);
        check (! tA.partials.empty() && ! tB.partials.empty(), "both corner spectra compile to something");

        const auto same = [] (const SpectrumTemplate& x, const SpectrumTemplate& y)
        {
            if (x.partials.size() != y.partials.size()) return false;
            for (size_t i = 0; i < x.partials.size(); ++i)
                if (x.partials[i].ratio != y.partials[i].ratio || x.partials[i].amp != y.partials[i].amp)
                    return false;
            return true;
        };
        check (same (blendTemplates (tA, tB, 0.0), tA) && same (blendTemplates (tA, tB, 1.0), tB),
               "AT A CORNER the blend is that corner's spectrum, partial for partial, bit for bit");

        // The intermediate is REAL: it has to contain partials from both, and it must
        // not be either end.
        // 7.0 is harmonic 7, which the custom scale (1, 5/4, 3/2 over two octaves:
        // 1, 1.25, 1.5, 2, 2.5, 3) does not contain; 1.25 is the 5/4 the harmonic engine
        // does not contain. Both chosen because they belong to ONE side only - a ratio
        // the two share is a blend of two amplitudes and says nothing about either.
        const auto mid = blendTemplates (tA, tB, 0.5);
        const bool hasA = hasRatio (mid, 7.0, 1e-3);
        const bool hasB = hasRatio (mid, 1.25, 1e-3);
        check (hasA && hasB, "the middle really contains partials from BOTH engines");
        check (! same (mid, tA) && ! same (mid, tB), "...and is neither of them");

        // The morph's fidelity promise: a partial that exists at either end is still
        // there in between - nothing is dropped for being quiet.
        {
            int lost = 0;
            for (const auto& p : tA.partials)
                if (! hasRatio (mid, (double) p.ratio, 1e-3)) ++lost;
            check (lost == 0, "no partial of the source spectrum goes missing half way across");
        }

        // MONOTONIC INTERMEDIATES: dragging must not jump. A partial present in A only
        // fades DOWN all the way, one present in B only fades UP all the way, measured
        // against the partial they are both being normalised with.
        {
            bool downOk = true, upOk = true;
            double prevDown = 1e18, prevUp = -1.0;
            for (int i = 0; i <= 20; ++i)
            {
                const double t = i / 20.0;
                const auto bl = blendTemplates (tA, tB, t);
                const double dA = (double) ampAtRatio (bl, 7.0, 1e-3);   // A only
                const double dB = (double) ampAtRatio (bl, 1.25, 1e-3);  // B only
                if (i > 0 && i < 20)
                {
                    if (dA > prevDown + 1e-9) downOk = false;
                    if (dB < prevUp - 1e-9)   upOk = false;
                }
                if (dA >= 0.0) prevDown = dA;
                if (dB >= 0.0) prevUp   = dB;
            }
            check (downOk, "a partial only the first preset has fades down monotonically across the drag");
            check (upOk,   "a partial only the second preset has fades up monotonically across the drag");
        }

        // THE ENERGY-DUPLICATION TRAP, the one blendFractalTemplates was fixed for.
        // Three partials of A share ONE ~1-cent bin with a single partial of B. Each of
        // them must take its SHARE of the bin's blended energy, not the whole of B's.
        {
            SpectrumTemplate x, y;
            x.partials = { { 1.0000f, 0.6f }, { 1.0002f, 0.8f }, { 1.0004f, 0.6f }, { 4.0f, 1.0f } };
            y.partials = { { 1.0000f, 1.0f }, { 4.0f, 1.0f } };
            const auto bl = blendTemplates (x, y, 0.5);
            double lowSum = 0.0, high = 0.0;
            for (const auto& p : bl.partials)
                (p.ratio < 2.0f ? lowSum : high) += (double) p.amp;
            // 1.0000 and 1.0002 land in the same ~1 c bin, 1.0004 in the next one up.
            // Correct: (0.5*1.4 + 0.5*1.0) + 0.5*0.6 = 1.5 against the 4.0 partial's 1.0.
            // Handing each of the shared bin's partials the whole of b's amplitude gives
            // 0.8 + 0.9 + 0.3 = 2.0 - measured, and that is what this number catches.
            std::printf ("  shared-bin energy vs the lone partial: %.4f (correct 1.5, duplicating 2.0)\n",
                         lowSum / high);
            check (std::abs (lowSum / high - 1.5) < 1e-4,
                   "several partials sharing one bin split its energy - they do not each take all of it");
            check (bl.partials.size() == 4,
                   "and every one of them survives at its own ratio; nothing is merged away");
        }

        // Equal power through the middle. Two disjoint spectra blended linearly and then
        // RMS-normalised come out at 1/sqrt(2) each, so the pad does not sag in the
        // middle - and the normalisation is the identity at both ends.
        {
            SpectrumTemplate x, y;
            x.partials = { { 1.0f, 1.0f } };
            y.partials = { { 3.0f, 1.0f } };
            const auto bl = blendTemplates (x, y, 0.5);
            check (bl.partials.size() == 2
                   && std::abs (bl.partials[0].amp - 0.70710678f) < 1e-5f
                   && std::abs (bl.partials[1].amp - 0.70710678f) < 1e-5f,
                   "two disjoint spectra cross at equal power, not at half amplitude each");
        }

        // Four corners the way the engine does it: two rows by x, then the rows by y.
        // At a corner that chain must still collapse to exactly that corner.
        {
            const auto top    = blendTemplates (tA, tB, 0.0);
            const auto bottom = blendTemplates (tB, tA, 0.0);
            const auto out    = blendTemplates (top, bottom, 0.0);
            check (same (out, tA), "the hierarchical four-corner chain is still exact at a corner");
        }
    }

    //--------------------------------------------------------------------------
    // THE COLOUR SYSTEM.
    //
    // "haz que el color de spectrum vs tuning sea una mezcla del color de spectrum y del
    //  color de tuning. haz que todas las demas relaciones de colores incluidas la que
    //  hay entre spectrum y tuning sean lo mas contrastadas y diferentes posibles."
    //
    // The palette is the answer to a maximin problem (tools/PaletteSearch.cpp), which
    // means it is exactly the kind of thing that decays: every future colour added by
    // hand lands somewhere, and "somewhere" is on average right next to something.
    // That is not a hypothetical - it is the history of this palette, which shipped with
    // the TUNING panel and the CUSTOM modulator tab 3.8 units apart and a panel at
    // 6.2:1 against the page. These checks live here rather than in RhythmVerify
    // because the palette is JUCE-free and so is this file: nothing has to build an
    // editor to ask whether two colours are the same colour.
    {
        std::printf ("\nThe colour system:\n");
        using namespace galactic;

        // (b) EVERY PAIR, AS FAR APART AS POSSIBLE. 15 is what the search reached under
        // the constraints; asserting it is what stops the next colour landing at 4.
        {
            const auto w = palette::worstPair();
            std::printf ("  closest pair: %s vs %s at %.1f (was 3.8: Tuning vs Custom modulator)\n",
                         w.a, w.b, w.distance);
            check (w.distance >= 15.0,
                   "no two identity colours are closer than 15 units of OKLab");
        }

        // ...and every one of them is legible on the page, and is a COLOUR - the two
        // ways an "optimal" separation cheats: park it on white, or park it on grey.
        {
            // ON THE SURFACE THE TEXT IS DRAWN ON, not on the easiest surface in the
            // window. The page is the DARKER ground, so for these light colours it gives
            // the HIGHER ratio; the labels and outlines that carry the colour sit inside
            // the panel, on kPanelFill tinted 5% towards that same colour. Measured
            // there, the shipped set floors at 6.01:1 - real WCAG AA for large text
            // (3:1) and for normal text (4.5:1), but NOT the AAA this check used to
            // claim, which it only ever measured against the page.
            //
            // The threshold is the honest measured floor rather than an aspiration:
            // 5.9 stops the palette getting worse (which is what the check is for) while
            // saying out loud what it actually is. Raising it to 7.0 needs a re-run of
            // tools/PaletteSearch.cpp with the panel surface in its constraint - it now
            // has that rule; the shipped hexes predate it.
            // The surface each colour is really read against, printed. This is what the
            // check below is measuring, and it is worth seeing: the blend is NOT a lerp
            // (see colour::blendSrgb) and kEssentials in particular borrows across byte
            // lanes, so its panel is not the colour a tidy formula would predict.
            for (const auto& e : palette::identitySet())
                std::printf ("    %-19s #%06x on page %.2f:1, on its panel #%06x %.2f:1\n",
                             e.name, e.rgb,
                             colour::contrastRatio (e.rgb, palette::kPage),
                             palette::panelSurface (e.rgb),
                             colour::contrastRatio (e.rgb, palette::panelSurface (e.rgb)));
            const auto c  = palette::worstContrast();
            const auto cp = palette::worstContrastOnPage();
            std::printf ("  least legible: %s at %.2f:1 on %s"
                         " (the page alone would have said %.2f:1 for %s)\n",
                         c.a, c.distance, c.b, cp.distance, cp.a);
            check (c.distance >= 5.9,
                   "every identity colour clears WCAG AA (4.5:1) on the surface it is drawn on");
            // ...AND THAT IT IS REALLY MEASURING THE PANEL. The threshold above cannot
            // tell: measure the page instead and every number goes UP, so the check
            // passes more easily and the palette is signed off against a surface no
            // coloured text is drawn on. That is the exact failure this replaced.
            //
            // So the direction is asserted too, and it is the claim the old comment had
            // backwards ("the page is the darker of the two and therefore the harder"):
            // these are LIGHT colours on a DARK ground, so the DARKER surface is the
            // EASIER one and the page flatters every colour in the set. If a future
            // palette goes dark-on-light this fails, which is right - the argument for
            // measuring the panel would then have to be made again rather than assumed.
            {
                bool pageAlwaysEasier = true; const char* odd = "";
                for (const auto& e : palette::identitySet())
                    if (! (colour::contrastRatio (e.rgb, palette::kPage)
                             > colour::contrastRatio (e.rgb, palette::panelSurface (e.rgb))))
                    { pageAlwaysEasier = false; odd = e.name; }
                check (pageAlwaysEasier, pageAlwaysEasier
                         ? "the page flatters every one of them - the panel is the binding surface"
                         : odd);
            }
            check (c.distance < cp.distance,
                   "...so the reported floor is the PANEL's, strictly below what the page would say");
            double leastChroma = 1.0e9; const char* greyest = "";
            for (const auto& e : palette::identitySet())
            {
                const double ch = colour::chroma (colour::toOklab (e.rgb));
                if (ch < leastChroma) { leastChroma = ch; greyest = e.name; }
            }
            std::printf ("  least chromatic: %s at %.3f\n", greyest, leastChroma);
            check (leastChroma >= 0.10,
                   "and none of them has drifted back towards the neutral steel");
        }

        // (a) SPECTRUM VS TUNING IS THE MIXTURE OF ITS PARENTS.
        //
        // Asserted as a property, not as a hex: recompute the mixture here and require
        // the palette to agree, then require it to actually LIE BETWEEN them. The second
        // half is the one that bites, because rule (b) is pushing the parents apart and
        // a mixture that has been quietly replaced by a hand-picked third colour would
        // still "agree" with nothing at all if only the first half were checked.
        {
            const auto blend = palette::specScale();
            check (blend == colour::mixOklch (palette::kHarmonic, palette::kTuning, 0.5),
                   "Spectrum vs Tuning IS the mixture of Spectrum and Tuning - not a third hex");

            const double toS = colour::deltaE (blend, palette::kHarmonic);
            const double toT = colour::deltaE (blend, palette::kTuning);
            const double parents = colour::deltaE (palette::kHarmonic, palette::kTuning);
            std::printf ("  blend #%06x: %.1f from Spectrum, %.1f from Tuning, parents %.1f apart\n",
                         blend, toS, toT, parents);
            check (toS < parents && toT < parents,
                   "and it sits BETWEEN them - closer to each parent than they are to each other");
            // Rule (b) serving rule (a): far-apart parents are what give the mixture room
            // to be its own colour. If they ever close up, the blend collapses onto both.
            check (toS >= 12.0 && toT >= 12.0,
                   "...and far enough from both to be a colour of its own, not a shade of either");
            // The hue is genuinely between them, which is what "a mixture" looks like.
            // Mixed on the straight line in (a,b) instead of round the arc, a saturated
            // pair averages through the neutral axis and comes out grey.
            check (colour::chroma (colour::toOklab (blend)) >= 0.10,
                   "...and the mixture kept its chroma (it is not a grey wash of the two)");
        }

        // COLOUR BLINDNESS. Reported unconditionally and asserted at a lower bar than
        // normal vision, because it is a lower bar honestly: dichromacy collapses most
        // of the hue circle onto one axis, so twelve colours cannot be as separable as
        // they are for a trichromat. The bar that matters is that no two of them become
        // THE SAME COLOUR, which is where this palette was (0.7 apart under deuteranopia
        // - literally indistinguishable).
        {
            const auto d = palette::worstPair (colour::Vision::deuteranopia);
            const auto p = palette::worstPair (colour::Vision::protanopia);
            std::printf ("  deuteranopia: closest %s vs %s at %.1f (was 0.7)\n", d.a, d.b, d.distance);
            std::printf ("  protanopia:   closest %s vs %s at %.1f (was 1.2)\n", p.a, p.b, p.distance);
            check (d.distance >= 5.0, "no two identity colours collapse onto each other under deuteranopia");
            check (p.distance >= 5.0, "nor under protanopia");
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
