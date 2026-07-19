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

    // The `count` most consonant local minima (or, reversed, the most dissonant
    // local maxima) of the curve, as interval ratios sorted ascending. Unison and
    // anything within `edgeCents` of it or of the period are excluded (the .scl
    // gets 1/1 implicitly and the period explicitly).
    inline std::vector<double> pickExtremes (const std::vector<std::pair<double, double>>& curve,
                                             int count, bool maxima, double period,
                                             double edgeCents = 5.0)
    {
        std::vector<std::pair<double, double>> found; // (alpha, value)
        for (size_t i = 1; i + 1 < curve.size(); ++i)
        {
            const double prev = curve[i - 1].second, cur = curve[i].second, next = curve[i + 1].second;
            const bool isExtreme = maxima ? (cur > prev && cur >= next)
                                          : (cur < prev && cur <= next);
            if (! isExtreme)
                continue;
            const double cents = 1200.0 * std::log2 (curve[i].first);
            const double periodCents = 1200.0 * std::log2 (period);
            if (cents < edgeCents || cents > periodCents - edgeCents)
                continue;
            found.emplace_back (curve[i].first, cur);
        }

        // Most consonant first (lowest dissonance) - or roughest first in reverse.
        std::sort (found.begin(), found.end(),
                   [maxima] (const auto& a, const auto& b)
                   { return maxima ? (a.second > b.second) : (a.second < b.second); });
        if ((int) found.size() > count)
            found.resize ((size_t) count);

        std::vector<double> ratios;
        ratios.reserve (found.size());
        for (const auto& f : found) ratios.push_back (f.first);
        std::sort (ratios.begin(), ratios.end());
        return ratios;
    }
}
