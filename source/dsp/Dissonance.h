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

namespace dissonance
{
    struct Partial { double freq; double amp; };

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

    // The `count` most PROMINENT local minima (or, reversed, maxima) of the curve,
    // each refined to sub-cent precision, as interval ratios sorted ascending.
    //
    // Ranking is by PROMINENCE (how deep the notch is relative to the flanking
    // opposite extremes), NOT by absolute dissonance: the curve slopes down toward
    // the octave, so ranking by raw value would crowd the picks near the period
    // and skip clear notches like 5/4. Unison and the period edge are excluded
    // (1/1 is implicit in the .scl and the period is appended by the caller).
    inline std::vector<double> pickExtremes (const std::vector<Partial>& spectrum,
                                             const std::vector<std::pair<double, double>>& curve,
                                             int count, bool maxima, double period,
                                             double edgeCents = 6.0)
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

        std::vector<double> ratios;
        ratios.reserve (cands.size());
        for (const auto& cnd : cands)
            ratios.push_back (refineExtreme (spectrum, curve[cnd.idx - 1].first, curve[cnd.idx + 1].first, maxima));
        std::sort (ratios.begin(), ratios.end());
        return ratios;
    }
}
