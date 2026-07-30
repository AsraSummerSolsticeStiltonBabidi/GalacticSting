#pragma once

//==============================================================================
// PartialGains: the user's per-partial amplitude edits, and how they stay attached
// to the partial they were made on.
//
// WHY THIS IS NOT PART OF THE COMPILER. An edit must never reach compileSpectrum,
// because three separate mechanisms in there would silently DELETE partials:
//   - the amplitude-floor cull, which is relative to the running maximum, so a boost
//     raises that maximum and drops OTHER, untouched partials,
//   - the keep-loudest cap,
//   - the whole-template RMS normalisation, which would renormalise the edit away.
// So an edit is stored here, resolved against a finished partial list, and applied
// downstream as one multiply per partial. The compiler never sees it, which also
// means an edit can never trigger a recompile.
//
// WHY EDITS ARE KEYED BY RATIO, NOT BY INDEX. A partial's index changes the moment
// the spectrum is rebuilt: templates are re-sorted by ratio and the set itself moves
// with Base, Fractal, the prime rules and everything else. "The 7th partial" would
// therefore mean something different a second later, and an edit made on one partial
// would reappear on a stranger. Keying by (quantised) log-frequency means "the
// partial at 3/2" stays the partial at 3/2 across a rebuild, an edit whose partial
// disappears simply stops applying until it comes back, and a brand-new partial
// arrives at its natural amplitude.
//
// Header-only and deliberately JUCE-free, so the unit tests can exercise it without
// a plugin.
//==============================================================================

#include "Spectrum.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace spectrum
{
    // Key grid: hundredths of a cent. Partial::ratio is a float, whose ulp near these
    // magnitudes is ~0.0002 cents, and recompiling the SAME mathematical ratio down a
    // different path (the transposed anchor divide, the double->float step in the
    // fractal blend) moves it by a few ulps. 0.01 c sits ~50x above that noise, while
    // the closest spacing the level-1 engine can produce - 1200*log2(1 + 1/16384) =
    // 0.106 c - is still ten bins wide, so neighbouring partials never share a bin.
    inline constexpr double kGainKeyCents   = 0.01;
    inline constexpr double kGainMatchCents = 0.02;   // bind tolerance: +/- 2 bins
    inline constexpr float  kGainMutedDb    = -120.0f; // at or below this the partial is silent
    inline constexpr float  kGainMaxDb      = 24.0f;

    inline long long gainKey (double ratio) noexcept
    {
        return (long long) std::llround (1200.0 / kGainKeyCents
                                         * std::log2 (std::max (1.0e-12, ratio)));
    }
    inline double gainKeyRatio (long long key) noexcept
    {
        return std::exp2 ((double) key * kGainKeyCents / 1200.0);
    }

    // One user edit. `dB` is relative to the partial's own compiled amplitude, so 0
    // means "leave it exactly as the compiler made it".
    struct PartialGain
    {
        long long key = 0;
        float     dB  = 0.0f;
        bool operator== (const PartialGain& o) const noexcept { return key == o.key && dB == o.dB; }
        bool operator!= (const PartialGain& o) const noexcept { return ! (*this == o); }
    };

    inline float gainScaleOf (float dB) noexcept
    {
        return dB <= kGainMutedDb ? 0.0f : (float) std::pow (10.0, (double) dB / 20.0);
    }

    //==========================================================================
    // Bind a set of edits to a finished partial list, and report WHERE each edit
    // landed: indexOut[j] is the index in `partials` that ov[j] claimed, or -1 when
    // that edit's partial is not in this spectrum at all.
    //
    // `partials` must be ascending by ratio (every template is) and `ov` ascending by
    // key with unique keys.
    //
    // The binding is strictly ONE-TO-ONE: an edit claims at most one partial and a
    // partial accepts at most one edit, nearest first, ties going to the lower index.
    // Without that rule two edits a fraction of a cent apart would both land on the
    // same partial and their gains would compound - the user would have moved one
    // control and heard two.
    //
    // Returns how many edits actually bound. An edit that binds to nothing is not an
    // error: its partial is simply not in the current spectrum, and it must be left in
    // the stored set so it comes back when the partial does. Never modifies `ov`.
    //
    // WHY THIS IS ITS OWN FUNCTION. The amplitude editor has to draw the user's dB on
    // the SAME knob the voice will multiply, and it needs the exact dB (not the scale
    // factor, which throws away everything below the mute threshold). Re-deriving the
    // pairing in the UI would be a second copy of this rule, free to drift out of step
    // with the audio - a knob showing an edit that some other partial is actually
    // getting. One binding pass, two readers.
    //==========================================================================
    inline int bindGains (const std::vector<Partial>& partials,
                          const std::vector<PartialGain>& ov,
                          std::vector<int>& indexOut)
    {
        indexOut.assign (ov.size(), -1);
        if (partials.empty() || ov.empty())
            return 0;

        const long long tol = (long long) std::llround (kGainMatchCents / kGainKeyCents);

        int    matched = 0;
        size_t p       = 0;   // first partial not yet claimed; only ever moves forward,
                              // which is what makes the pass one-to-one and O(n + m)
        for (size_t j = 0; j < ov.size(); ++j)
        {
            const auto& g = ov[j];
            while (p < partials.size() && gainKey ((double) partials[p].ratio) < g.key - tol)
                ++p;
            if (p >= partials.size())
                break;

            size_t    best  = partials.size();
            long long bestD = tol + 1;
            for (size_t q = p; q < partials.size(); ++q)
            {
                const long long k = gainKey ((double) partials[q].ratio);
                if (k > g.key + tol) break;
                const long long d = k > g.key ? k - g.key : g.key - k;
                if (d < bestD) { bestD = d; best = q; }
            }

            if (best < partials.size())
            {
                indexOut[j] = (int) best;
                ++matched;
                p = best + 1;   // claimed
            }
        }
        return matched;
    }

    // The audio-side form: scaleOut is resized to partials.size() and filled with 1.0f,
    // then each edit that found a home multiplies its partial. Same one-to-one pairing
    // as bindGains, because it IS bindGains.
    inline int resolveGains (const std::vector<Partial>& partials,
                             const std::vector<PartialGain>& ov,
                             std::vector<float>& scaleOut)
    {
        scaleOut.assign (partials.size(), 1.0f);
        std::vector<int> where;
        const int matched = bindGains (partials, ov, where);
        for (size_t j = 0; j < where.size(); ++j)
            if (where[j] >= 0)
                scaleOut[(size_t) where[j]] = gainScaleOf (ov[j].dB);
        return matched;
    }

    // Insert or replace an edit, keeping the list sorted and its keys unique.
    inline void setPartialGain (std::vector<PartialGain>& ov, long long key, float dB)
    {
        const auto it = std::lower_bound (ov.begin(), ov.end(), key,
                                          [] (const PartialGain& a, long long k) { return a.key < k; });
        if (it != ov.end() && it->key == key) it->dB = dB;
        else                                  ov.insert (it, PartialGain { key, dB });
    }

    // Drop an edit entirely (its partial returns to the compiled amplitude).
    inline void clearPartialGain (std::vector<PartialGain>& ov, long long key)
    {
        const auto it = std::lower_bound (ov.begin(), ov.end(), key,
                                          [] (const PartialGain& a, long long k) { return a.key < k; });
        if (it != ov.end() && it->key == key) ov.erase (it);
    }
}
