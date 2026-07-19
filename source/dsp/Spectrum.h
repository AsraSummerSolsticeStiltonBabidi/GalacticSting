#pragma once

//==============================================================================
// The "spectrum compiler": pure C++ (no JUCE) that turns the patch settings into
// a list of partials (ratio relative to the played note + amplitude).
//
// Pipeline (canonical order):
//   A/B  index generation [N, maxHarmonic] + selection (all/even/odd/prime)
//   C    prime-limit veto (largest prime factor <= P)
//   D    amplitude curve  a(k) = k^(-slope)
//   F    fractal: L-fold multiplicative self-convolution (exact integer merge)
//   cull amplitude floor, keep-loudest cap, sort; per-voice Nyquist is applied later
//
// Everything that governs coincidence/merging is done on integers, so two fractal
// partials are the SAME iff their integer numerator kappa is equal (no tolerance).
//==============================================================================

#include <vector>
#include <cstdint>
#include <cmath>
#include <atomic>

namespace spectrum
{
    // One authoritative hard ceiling, shared by the compiler and the voice arrays.
    // High enough that realistic fractal patches are never truncated; the per-voice
    // Nyquist cull keeps the actually-rendered count much lower.
    inline constexpr int kMaxPartials = 16384;

    enum class Selection { All = 0, Even = 1, Odd = 2, Prime = 3 };

    // Where the partials come from:
    //   Harmonic    - the integer-harmonic engine (base-N, selection, prime, fractal).
    //   ScalaTimbre - the intervals of a loaded .scl become the partial ratios, stacked
    //                 over `timbrePeriodRepeats` periods; slope + fractal still apply.
    enum class Source { Harmonic = 0, ScalaTimbre = 1 };

    // How the base-harmonic shift N maps to frequencies:
    //   Transposed - the lowest partial is transposed down to the played pitch; the
    //                harmonics above it (N+1, N+2 ...) imply a low virtual fundamental,
    //                so raising N lowers the perceived pitch (missing-fundamental
    //                effect) and packs the band densely.
    //   Original   - the harmonics keep their original/natural positions (N, N+1 ...
    //                times the played pitch); the low harmonics are simply removed, so
    //                raising N makes the timbre brighter/thinner at the SAME pitch.
    enum class BaseMode { Transposed = 0, Original = 1 };

    // The spectrum reaches up to this ratio relative to the played note; the per-voice
    // Nyquist cull then removes whatever is inaudible at the actual pitch. This is a
    // "reach past the audible ceiling for the whole musical range", not a musical cap.
    inline constexpr int kSpectrumReach = 1024;

    struct Partial
    {
        float ratio; // frequency multiplier relative to the played note (lowest audible ~ 1.0)
        float amp;   // linear amplitude (uniformly normalised across the template)
    };

    struct SpectrumSettings
    {
        int       baseN         = 1;                 // played note = harmonic N of implied f0
        BaseMode  baseMode      = BaseMode::Transposed; // how N maps to frequency (see BaseMode)
        int       reach         = kSpectrumReach;    // top ratio generated (internal; not a UI knob)
        Selection selection     = Selection::All;
        float     curveSlope    = 1.0f;              // s in (k/kMin)^(-s); 1 -> saw-type roll-off
        int       primeLimit    = 0;                 // 0 = off, else largest prime allowed
        bool      primeUseWhitelist = false;         // true -> use the whitelist instead of the limit
        unsigned  primeWhitelistMask = 0xFFFFFFFFu;  // which listed primes are allowed
        int       fractalLevel  = 1;                 // 1 = normal, 2, 3, ...
        float     ampFloorDb    = -120.0f;           // partials below this (rel. to max) are dropped
        int       maxPartials   = kMaxPartials;      // safety cap (keep loudest)

        // Scala-as-timbre source (used only when source == ScalaTimbre).
        Source             source = Source::Harmonic;
        std::vector<float> timbreDegrees;            // scale degree ratios (index 0 = 1.0)
        float              timbrePeriod = 2.0f;      // period ratio (e.g. 2/1)
        int                timbrePeriodRepeats = 3;  // how many periods stack into the timbre

        bool operator== (const SpectrumSettings& o) const
        {
            return baseN == o.baseN
                && baseMode == o.baseMode
                && reach == o.reach
                && selection == o.selection
                && curveSlope == o.curveSlope
                && primeLimit == o.primeLimit
                && primeUseWhitelist == o.primeUseWhitelist
                && primeWhitelistMask == o.primeWhitelistMask
                && fractalLevel == o.fractalLevel
                && ampFloorDb == o.ampFloorDb
                && maxPartials == o.maxPartials
                && source == o.source
                && timbrePeriod == o.timbrePeriod
                && timbrePeriodRepeats == o.timbrePeriodRepeats
                && timbreDegrees == o.timbreDegrees;
        }
        bool operator!= (const SpectrumSettings& o) const { return ! (*this == o); }
    };

    struct SpectrumTemplate
    {
        std::vector<Partial> partials; // sorted ascending by ratio
        bool truncated = false;        // hit the maxPartials cap (audible partials dropped)
        int  rawCount  = 0;            // audible partial count before the cap
    };

    //==========================================================================
    // Prime helpers (exposed for unit tests).
    inline bool isPrime (int k)
    {
        if (k < 2)  return false;
        if (k < 4)  return true;          // 2, 3
        if (k % 2 == 0) return false;
        for (int d = 3; (long long) d * d <= k; d += 2)
            if (k % d == 0) return false;
        return true;
    }

    inline long long largestPrimeFactor (long long k)
    {
        if (k <= 1) return 1;             // 1 has no prime factors -> passes any limit
        long long largest = 1;
        for (long long d = 2; d * d <= k; ++d)
            while (k % d == 0) { largest = d; k /= d; }
        if (k > 1) largest = k;           // remaining prime
        return largest;
    }

    inline bool primeLimitOk (int k, int P)
    {
        return P <= 0 || largestPrimeFactor (k) <= (long long) P;
    }

    // The 9 primes the whitelist can toggle (bit i of the mask = primeList[i] allowed).
    inline constexpr int primeListSize = 9;
    inline const int* primeList() { static const int p[primeListSize] = { 2,3,5,7,11,13,17,19,23 }; return p; }

    // Keep harmonic k iff EVERY prime factor of k is a whitelisted prime. A factor that is a
    // prime outside the list (e.g. 29) or a listed prime whose bit is 0 excludes k.
    inline bool primeWhitelistOk (int k, unsigned mask)
    {
        if (k <= 1) return true;
        int n = k;
        const int* P = primeList();
        for (int i = 0; i < primeListSize; ++i)
            while (n % P[i] == 0)
            {
                if ((mask & (1u << i)) == 0) return false;
                n /= P[i];
            }
        return n == 1; // leftover > 1 -> a prime factor not in the list
    }

    //==========================================================================
    // The one entry point. Pure; safe to call from a background thread.
    // If `abort` is non-null and becomes true mid-build (e.g. the engine is being
    // destroyed), the build bails out early and returns an empty template.
    SpectrumTemplate compileSpectrum (const SpectrumSettings&,
                                      const std::atomic<bool>* abort = nullptr);
}
