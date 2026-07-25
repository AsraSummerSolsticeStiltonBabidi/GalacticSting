#pragma once

//==============================================================================
// Sensory-dissonance analysis after William Sethares ("Tuning, Timbre, Spectrum,
// Scale"). Faithful port of the reference `dissmeasure` from
// https://sethares.engr.wisc.edu/comprog.html :
//
//   Dstar=0.24; S1=0.0207; S2=18.96; C1=5; C2=-5; A1=-3.51; A2=-5.75;
//   for each pair (i,j):  S = Dstar / (S1*Fmin + S2);  Fdif = |Fj - Fi|
//     D += min(ai,aj) * ( C1*exp(A1*S*Fdif) + C2*exp(A2*S*Fdif) )
//
// The dissonance curve of a spectrum is dissmeasure of the union of the spectrum
// and a copy of itself scaled by an interval ratio alpha, swept from 1 to the
// period. Its local MINIMA are the intervals that sound most consonant with this
// exact timbre - the basis of the "tune by dissonance curve" feature; its MAXIMA
// are the roughest intervals (the "reverse" mode).
//
// Pure C++ (no JUCE), message-thread only, unit-tested in SpectrumTests.
//==============================================================================

#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <functional>

namespace dissonance
{
    struct Partial { double freq; double amp; };

    // Reduce a ratio into [1, 2). Returns 0 for invalid input.
    inline double octaveReduce (double ratio)
    {
        if (! std::isfinite (ratio) || ratio <= 0.0)
            return 0.0;
        while (ratio >= 2.0 - 1.0e-9) ratio *= 0.5;
        while (ratio < 1.0)           ratio *= 2.0;
        return ratio;
    }

    // Simplest integer fraction approximating `ratio` (continued-fraction
    // convergents, denominator <= maxDen). Returns p/q.
    inline void bestFraction (double ratio, long long& pOut, long long& qOut, long long maxDen = 16)
    {
        long long pPrev = 1, p = (long long) std::floor (ratio);
        long long qPrev = 0, q = 1;
        double frac = ratio - std::floor (ratio);
        while (frac > 1.0e-9)
        {
            const double y = 1.0 / frac;
            const long long a = (long long) std::floor (y);
            const long long pN = a * p + pPrev, qN = a * q + qPrev;
            if (qN > maxDen) break;
            pPrev = p; p = pN; qPrev = q; q = qN;
            frac = y - std::floor (y);
        }
        pOut = p; qOut = q;
    }

    // STRICT, LITERAL label for a scale-degree ratio: an integer fraction ONLY
    // when the ratio matches it EXACTLY (within `exactTolCents`, a hair's width
    // that only absorbs floating-point/file rounding); anything else shows its
    // interval in cents ("317c"). The ratio is NOT folded into an octave - a
    // degree of 5/2 in a tritave scale labels "5/2" (or its true cents), never
    // "5/4" - so every label can be trusted literally. Unit-tested.
    inline std::string justIntervalName (double ratio, double exactTolCents = 0.5)
    {
        if (! std::isfinite (ratio) || ratio <= 0.0)
            return {};

        long long p = 1, q = 1;
        bestFraction (ratio, p, q);

        const double centsR  = 1200.0 * std::log2 (ratio);
        const double centsPQ = (p > 0 && q > 0) ? 1200.0 * std::log2 ((double) p / (double) q) : 1.0e9;
        if (std::abs (centsR - centsPQ) < exactTolCents)
            return std::to_string (p) + "/" + std::to_string (q); // exact match only
        return std::to_string ((long long) std::llround (centsR)) + "\xc2\xa2"; // cent sign
    }

    // Sethares' pairwise sensory-dissonance measure (verbatim constants).
    inline double dissmeasure (std::vector<Partial> p)
    {
        constexpr double Dstar = 0.24, S1 = 0.0207, S2 = 18.96;
        constexpr double C1 = 5.0, C2 = -5.0, A1 = -3.51, A2 = -5.75;

        std::sort (p.begin(), p.end(), [] (const Partial& a, const Partial& b) { return a.freq < b.freq; });

        double d = 0.0;
        const int n = (int) p.size();
        for (int i = 0; i < n; ++i)
        {
            for (int j = i + 1; j < n; ++j)
            {
                const double fmin = p[(size_t) i].freq;           // sorted -> the lower of the pair
                const double s    = Dstar / (S1 * fmin + S2);
                const double fdif = p[(size_t) j].freq - fmin;
                const double a    = std::min (p[(size_t) i].amp, p[(size_t) j].amp);
                d += a * (C1 * std::exp (A1 * s * fdif) + C2 * std::exp (A2 * s * fdif));
            }
        }
        return d;
    }

    // Dissonance of the spectrum sounded together with itself transposed by `alpha`.
    inline double dissonanceAt (const std::vector<Partial>& spectrum, double alpha)
    {
        std::vector<Partial> both;
        both.reserve (spectrum.size() * 2);
        for (const auto& p : spectrum) both.push_back (p);
        for (const auto& p : spectrum) both.push_back ({ p.freq * alpha, p.amp });
        return dissmeasure (std::move (both));
    }

    // The curve sampled at `stepsPerCent^-1`-cent resolution over (1, period].
    // Returns pairs (alpha, dissonance), first entry just above unison.
    inline std::vector<std::pair<double, double>> dissonanceCurve (const std::vector<Partial>& spectrum,
                                                                   double period, double centsStep = 1.0)
    {
        std::vector<std::pair<double, double>> curve;
        if (spectrum.empty() || period <= 1.0001)
            return curve;

        const double totalCents = 1200.0 * std::log2 (period);
        const int    steps      = (int) std::ceil (totalCents / centsStep);
        curve.reserve ((size_t) steps);
        for (int i = 1; i <= steps; ++i)
        {
            const double cents = totalCents * (double) i / (double) steps;
            const double alpha = std::pow (2.0, cents / 1200.0);
            curve.emplace_back (alpha, dissonanceAt (spectrum, alpha));
        }
        return curve;
    }

    // Refine one located extreme to sub-cent precision by golden-section search
    // on the bracketing curve segment (locally unimodal). The 1-cent sampling
    // only LOCATES an extreme; this pins it exactly, so a harmonic timbre's fifth
    // comes out at 701.955 c (ratio 1.49994...) - just-intonation precision - not
    // a rounded "702".
    inline double refineExtreme (const std::vector<Partial>& spectrum,
                                 double alphaLo, double alphaHi, bool maxima)
    {
        constexpr double invPhi = 0.6180339887498949;
        double a = std::log2 (alphaLo), b = std::log2 (alphaHi);
        const auto eval = [&] (double x)
        {
            const double v = dissonanceAt (spectrum, std::pow (2.0, x));
            return maxima ? -v : v; // always minimise
        };
        double c = b - (b - a) * invPhi, d = a + (b - a) * invPhi;
        double fc = eval (c), fd = eval (d);
        for (int it = 0; it < 80 && (b - a) > 5.0e-9; ++it)
        {
            if (fc < fd) { b = d; d = c; fd = fc; c = b - (b - a) * invPhi; fc = eval (c); }
            else         { a = c; c = d; fc = fd; d = a + (b - a) * invPhi; fd = eval (d); }
        }
        return std::pow (2.0, 0.5 * (a + b));
    }

    //==========================================================================
    // N-NOTE CHORDS - faithful Sethares generalization (TTSS 2nd ed. §6.8 p.127 and
    // App. E p.347: "Generalizations to m sounds ... follow the same philosophy of
    // calculating the sum of the dissonances between all simultaneously sounding
    // partials"). Chord dissonance = dissmeasure of the UNION of the spectrum
    // transposed to each chord note - the same convention as dissonanceAt (each
    // note's intrinsic dissonance counted ONCE, matching his MATLAB reference and
    // his triadic dissonance surfaces). Sethares locates surface minima by grid
    // evaluation + inspection (his own dyad grid is ~17 cents); the search below is
    // a finer grid + exact refinement, i.e. the same method done more carefully.

    // Total Sethares dissonance of the chord {alphas} (include the root 1.0).
    inline double chordDissonance (const std::vector<Partial>& spectrum, const std::vector<double>& alphas)
    {
        std::vector<Partial> all;
        all.reserve (spectrum.size() * alphas.size());
        for (const double a : alphas)
            for (const auto& p : spectrum)
                all.push_back ({ p.freq * a, p.amp });
        return dissmeasure (std::move (all));
    }

    // Cross-dissonance between two transposed copies (one partial from EACH copy).
    // Absolute frequencies matter (s depends on fmin in Hz) - no ratio-only shortcut.
    inline double crossDissonance (const std::vector<Partial>& sp, double a, double b)
    {
        constexpr double Dstar = 0.24, S1 = 0.0207, S2 = 18.96;
        constexpr double C1 = 5.0, C2 = -5.0, A1 = -3.51, A2 = -5.75;
        double d = 0.0;
        for (const auto& p : sp)
            for (const auto& q : sp)
            {
                const double f1 = p.freq * a, f2 = q.freq * b;
                const double fmin = f1 < f2 ? f1 : f2;
                const double fdif = f1 < f2 ? f2 - f1 : f1 - f2;
                const double s    = Dstar / (S1 * fmin + S2);
                const double w    = p.amp < q.amp ? p.amp : q.amp;
                d += w * (C1 * std::exp (A1 * s * fdif) + C2 * std::exp (A2 * s * fdif));
            }
        return d;
    }

    // Intrinsic (within-one-copy) dissonance of the spectrum transposed by `a`.
    inline double selfDissonance (const std::vector<Partial>& sp, double a)
    {
        std::vector<Partial> one;
        one.reserve (sp.size());
        for (const auto& p : sp) one.push_back ({ p.freq * a, p.amp });
        return dissmeasure (std::move (one));
    }

    struct ChordResult { std::vector<double> alphas; double value; };

    // Ranked n-note chord search: the root is fixed at 1/1; the remaining notes-1
    // degrees sweep a cents grid inside (0, period); every combination's TOTAL
    // Sethares dissonance is evaluated EXACTLY at grid points via the pair
    // decomposition  D = sum_k Dself(a_k) + sum_{k<l} Dcross(a_k, a_l)  (identical to
    // dissmeasure of the union - the pairs of the union are exactly those pairs);
    // the best combinations (most consonant, or most dissonant with `maxima`) are
    // deduped and refined per-coordinate by golden section on the TRUE union measure.
    // `progress` (0..1) is polled for cancellation (return false to abort).
    // `minGapCents`: no two chord notes (nor a note and the root/period) may sit
    // closer than this - keeps results playable as scales (default 33 cents).
    inline std::vector<ChordResult> searchChords (const std::vector<Partial>& spectrum,
                                                  double period, int notes, int count, bool maxima,
                                                  double minGapCents = 33.0,
                                                  const std::function<bool(float)>& progress = {})
    {
        std::vector<ChordResult> out;
        if (spectrum.empty() || ! (period > 1.02) || notes < 2)
            return out;
        if (notes > 8) notes = 8;                      // idx arrays support up to 8 notes
        const int m = notes - 1;                       // free notes above the fixed root
        const double totalCents = 1200.0 * std::log2 (period);

        // Adaptive grid: the finest step whose combination count stays tractable
        // (~12M) - n=3 gets ~5c, n=6 ~20c; the coarse tail keeps wide periods with
        // many notes feasible instead of silently returning nothing.
        double step = 50.0;
        long long combosEst = 0;
        for (const double cand : { 5.0, 10.0, 15.0, 20.0, 25.0, 30.0, 40.0, 50.0,
                                   75.0, 100.0, 125.0, 150.0, 200.0 })
        {
            const int gPts = (int) std::floor (totalCents / cand) - 1;
            if (gPts < m) continue;
            long long c = 1;
            for (int i = 0; i < m; ++i) c = c * (long long) (gPts - i) / (long long) (i + 1);
            if (c <= 12'000'000LL) { step = cand; combosEst = c; break; }
        }
        const int G = (int) std::floor (totalCents / step) - 1; // interior grid points 1..G
        if (G < m || combosEst <= 0)
            return out;

        // Grid alphas: index 0 = the root (1/1), 1..G = interior cents steps.
        std::vector<double> alpha ((size_t) G + 1);
        alpha[0] = 1.0;
        for (int i = 1; i <= G; ++i) alpha[(size_t) i] = std::pow (2.0, (double) i * step / 1200.0);

        // Exact tables: Dself per grid point, Dcross per unordered pair (incl. root).
        std::vector<double> dSelf ((size_t) G + 1);
        for (int i = 0; i <= G; ++i) dSelf[(size_t) i] = selfDissonance (spectrum, alpha[(size_t) i]);
        std::vector<double> dCross ((size_t) (G + 1) * (size_t) (G + 1), 0.0);
        for (int i = 0; i <= G; ++i)
        {
            for (int j = i + 1; j <= G; ++j)
            {
                const double v = crossDissonance (spectrum, alpha[(size_t) i], alpha[(size_t) j]);
                dCross[(size_t) i * (size_t) (G + 1) + (size_t) j] = v;
                dCross[(size_t) j * (size_t) (G + 1) + (size_t) i] = v;
            }
            if (progress && ! progress (0.25f * (float) i / (float) std::max (1, G)))
                return out;
        }

        // Enumerate combinations of m strictly-increasing interior indices. Keep the
        // best K (with headroom for dedup) in a simple bounded worst-out list.
        const int keep = std::max (count * 6, 48);
        struct Cand { double value; int idx[7]; };     // supports notes <= 8
        std::vector<Cand> best;
        best.reserve ((size_t) keep + 1);
        auto worseThan = [maxima] (double a, double b) { return maxima ? a < b : a > b; };

        int c[7] = {};
        for (int i = 0; i < m; ++i) c[i] = i + 1;
        long long done = 0;
        const double rootTerm = dSelf[0];
        const double minGap = std::max (0.0, minGapCents);
        while (true)
        {
            // Minimum-interval rule: every adjacent pair (incl. root->first and
            // last->period) must be at least minGap cents apart.
            bool gapOk = (double) c[0] * step >= minGap
                      && totalCents - (double) c[m - 1] * step >= minGap;
            for (int i = 1; i < m && gapOk; ++i)
                if ((double) (c[i] - c[i - 1]) * step < minGap) gapOk = false;
            if (! gapOk)
            {
                if (++done % 65536 == 0 && progress
                    && ! progress (0.25f + 0.65f * (float) done / (float) combosEst))
                    return out;
                int i = m - 1;
                while (i >= 0 && c[i] == G - (m - 1 - i)) --i;
                if (i < 0) break;
                ++c[i];
                for (int j = i + 1; j < m; ++j) c[j] = c[j - 1] + 1;
                continue;
            }

            // Total = union-of-copies dissmeasure, decomposed exactly into the tables.
            double v = rootTerm;
            for (int i = 0; i < m; ++i)
            {
                v += dSelf[(size_t) c[i]] + dCross[(size_t) c[i]]; // cross with the root (index 0)
                for (int j = i + 1; j < m; ++j)
                    v += dCross[(size_t) c[i] * (size_t) (G + 1) + (size_t) c[j]];
            }

            if ((int) best.size() < keep || ! worseThan (v, best.back().value))
            {
                Cand nc; nc.value = v;
                for (int i = 0; i < 7; ++i) nc.idx[i] = i < m ? c[i] : 0;
                const auto pos = std::lower_bound (best.begin(), best.end(), nc,
                    [&] (const Cand& x, const Cand& y) { return worseThan (y.value, x.value); });
                best.insert (pos, nc);
                if ((int) best.size() > keep) best.pop_back();
            }

            if (++done % 65536 == 0 && progress
                && ! progress (0.25f + 0.65f * (float) done / (float) combosEst))
                return out;

            // next combination (lexicographic)
            int i = m - 1;
            while (i >= 0 && c[i] == G - (m - 1 - i)) --i;
            if (i < 0) break;
            ++c[i];
            for (int j = i + 1; j < m; ++j) c[j] = c[j - 1] + 1;
        }

        // Dedupe then refine each survivor per-coordinate by golden section on the
        // TRUE union measure. The refinement window is +-0.45 grid steps so two
        // NEIGHBOURING coordinates (>= 1 step apart on the grid) can never cross or
        // collide into duplicate notes; dedupe compares REFINED positions, so two
        // grid seeds converging onto the same extreme collapse into one result.
        const double dedupCents = step * 0.9;
        const double win        = step * 0.45;
        std::vector<ChordResult> finals;
        bool cancelled = false;
        for (const auto& cand : best)
        {
            if (cancelled) break;
            std::vector<double> cents ((size_t) m);
            for (int i = 0; i < m; ++i) cents[(size_t) i] = (double) cand.idx[i] * step;

            // Coordinate refinement: golden section per note over +-win, two sweeps.
            std::vector<double> a ((size_t) notes);
            a[0] = 1.0;
            for (int i = 0; i < m; ++i) a[(size_t) i + 1] = std::pow (2.0, cents[(size_t) i] / 1200.0);
            const double seedValue = chordDissonance (spectrum, a);
            constexpr double invPhi = 0.6180339887498949;
            for (int sweep = 0; sweep < 2 && ! cancelled; ++sweep)
                for (int i = 1; i <= m; ++i)
                {
                    const double c0 = 1200.0 * std::log2 (a[(size_t) i]);
                    // The minimum interval applies to EVERY pair of notes, not just to
                    // the root: the refinement window is clamped so this note can never
                    // come closer than minGap to its neighbours (and, since the notes
                    // stay ordered, to any other note), the root, or the period.
                    const double prevC = i == 1 ? 0.0 : 1200.0 * std::log2 (a[(size_t) i - 1]);
                    const double nextC = i == m ? totalCents : 1200.0 * std::log2 (a[(size_t) i + 1]);
                    double lo = std::max (std::max (1.0, prevC + minGap), c0 - win);
                    double hi = std::min (std::min (totalCents - 1.0, nextC - minGap), c0 + win);
                    if (! (hi > lo)) continue; // no legal room: leave the note where it is
                    auto eval = [&] (double cts)
                    {
                        auto t = a; t[(size_t) i] = std::pow (2.0, cts / 1200.0);
                        const double v = chordDissonance (spectrum, t);
                        return maxima ? -v : v;
                    };
                    double x1 = hi - (hi - lo) * invPhi, x2 = lo + (hi - lo) * invPhi;
                    double f1 = eval (x1), f2 = eval (x2);
                    for (int it = 0; it < 32 && (hi - lo) > 0.05; ++it)
                    {
                        if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - (hi - lo) * invPhi; f1 = eval (x1); }
                        else         { lo = x1; x1 = x2; f1 = f2; x2 = lo + (hi - lo) * invPhi; f2 = eval (x2); }
                    }
                    a[(size_t) i] = std::pow (2.0, 0.5 * (lo + hi) / 1200.0);
                    if (progress && ! progress (0.9f + 0.1f * (float) finals.size() / (float) count))
                    { cancelled = true; break; } // fine-grained cancel (short worker joins)
                }
            std::sort (a.begin() + 1, a.end());

            // Keep the better of refined vs its grid seed (a clamped/multimodal window
            // must never make a result WORSE than the grid point that seeded it).
            double v = chordDissonance (spectrum, a);
            if (maxima ? (v < seedValue) : (v > seedValue))
            {
                a[0] = 1.0;
                for (int i = 0; i < m; ++i) a[(size_t) i + 1] = std::pow (2.0, cents[(size_t) i] / 1200.0);
                v = seedValue;
            }

            // Post-refinement dedupe: identical extremes reached from different seeds.
            bool dup = false;
            for (const auto& f : finals)
            {
                bool near = true;
                for (int i = 1; i <= m; ++i)
                    if (std::abs (1200.0 * std::log2 (f.alphas[(size_t) i] / a[(size_t) i])) > dedupCents)
                    { near = false; break; }
                if (near) { dup = true; break; }
            }
            if (dup) continue;

            finals.push_back ({ std::move (a), v });
            if ((int) finals.size() >= count) break;
        }

        std::sort (finals.begin(), finals.end(), [maxima] (const ChordResult& x, const ChordResult& y)
                   { return maxima ? x.value > y.value : x.value < y.value; });
        return finals;
    }

    struct ExtremeInfo { double ratio; double value; };

    // The `count` most PROMINENT local minima (or, reversed, maxima) of the curve,
    // each refined to sub-cent precision, with its dissonance height, sorted
    // ascending by ratio.
    //
    // Ranking is by PROMINENCE (how deep the notch is relative to the flanking
    // opposite extremes), NOT by absolute dissonance: the curve slopes down toward
    // the period, so ranking by raw value would crowd the picks near it and skip
    // clear notches like 5/4. Only a sliver around unison and the period itself is
    // excluded (1/1 is implicit in the .scl and the period is appended by the
    // caller) - every other strict local extreme is usable, however wild.
    inline std::vector<ExtremeInfo> pickExtremes (const std::vector<Partial>& spectrum,
                                                  const std::vector<std::pair<double, double>>& curve,
                                                  int count, bool maxima, double period,
                                                  double edgeCents = 1.5)
    {
        const size_t n = curve.size();
        if (n < 3)
            return {};

        std::vector<size_t> minIdx, maxIdx;
        for (size_t i = 1; i + 1 < n; ++i)
        {
            const double prev = curve[i - 1].second, cur = curve[i].second, next = curve[i + 1].second;
            if (cur < prev && cur <= next) minIdx.push_back (i);
            if (cur > prev && cur >= next) maxIdx.push_back (i);
        }

        const auto& own      = maxima ? maxIdx : minIdx;
        const auto& opposite = maxima ? minIdx : maxIdx;
        const double periodCents = 1200.0 * std::log2 (period);

        struct Cand { size_t idx; double prominence; };
        std::vector<Cand> cands;
        for (const size_t i : own)
        {
            const double cents = 1200.0 * std::log2 (curve[i].first);
            if (cents < edgeCents || cents > periodCents - edgeCents)
                continue;

            // Nearest opposite extreme on each side (curve endpoints as fallback).
            double left = curve.front().second, right = curve.back().second;
            for (auto it = opposite.rbegin(); it != opposite.rend(); ++it)
                if (*it < i) { left = curve[*it].second; break; }
            for (const size_t j : opposite)
                if (j > i) { right = curve[j].second; break; }

            const double cur = curve[i].second;
            const double prom = maxima ? cur - std::max (left, right)
                                       : std::min (left, right) - cur;
            cands.push_back ({ i, prom });
        }

        std::sort (cands.begin(), cands.end(),
                   [] (const Cand& a, const Cand& b) { return a.prominence > b.prominence; });
        if ((int) cands.size() > count)
            cands.resize ((size_t) count);

        std::vector<ExtremeInfo> out;
        out.reserve (cands.size());
        for (const auto& cnd : cands)
        {
            const double r = refineExtreme (spectrum, curve[cnd.idx - 1].first, curve[cnd.idx + 1].first, maxima);
            out.push_back ({ r, dissonanceAt (spectrum, r) });
        }
        std::sort (out.begin(), out.end(),
                   [] (const ExtremeInfo& a, const ExtremeInfo& b) { return a.ratio < b.ratio; });
        return out;
    }
}
