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
// Pure C++ (no JUCE), unit-tested in SpectrumTests. searchChords spawns short-lived
// worker threads for its table build (see buildGridTables) and joins them before it
// returns, so a caller still sees one plain blocking call.
//==============================================================================

#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>

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

    // The same label with the "/1" left off a whole-number ratio: the amplitude
    // editor's knob captions are read as PARTIAL NUMBERS - "3", not "3/1" - and a
    // denominator that is always 1 is noise on a knob 40 px wide. Nothing else is
    // touched: 3/2 stays 3/2 and an inexact ratio still shows its cents, so the
    // caption never claims a ratio the partial does not have.
    inline std::string partialNumberName (double ratio, double exactTolCents = 0.5)
    {
        auto s = justIntervalName (ratio, exactTolCents);
        if (s.size() > 2 && s.compare (s.size() - 2, 2, "/1") == 0)
            s.erase (s.size() - 2);
        return s;
    }

    // A cents VALUE as display/round-trip text, without redundant zeros: "700",
    // "701.955", "200.00002" - never "700.00000". Fixed maxDecimals first (5 keeps
    // the full precision the adapted-spectrum text always carried), then the trailing
    // zeros (and a bare trailing dot) go. FOR TEXT THE USER READS ONLY - .scl writers
    // must NOT use it: the Scala format reads a line as cents only when it contains a
    // decimal point, so "700" in a .scl would silently become the ratio 700/1. The
    // interval-text parser is untouched and needs no decimals: its 'c' suffix alone
    // marks cents ("700c" parses identically before and after). Unit-tested.
    inline std::string centsText (double cents, int maxDecimals = 5)
    {
        char buf[64];
        std::snprintf (buf, sizeof (buf), "%.*f", maxDecimals, cents);
        std::string s (buf);
        if (s.find ('.') != std::string::npos)
        {
            while (! s.empty() && s.back() == '0') s.pop_back();
            if (! s.empty() && s.back() == '.') s.pop_back();
        }
        if (s == "-0") s = "0"; // -0.000001 rounds to "-0", which is not a value
        return s;
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

    // THE ANALYSIS SPECTRUM every Sethares readout starts from: a template's partials
    // at their SOUNDING amplitudes (compiled amp x the user's per-partial gain edit,
    // when one is bound - `gain` is index-aligned, empty = no edits), anchored in
    // absolute Hz at `f0`, capped to the `cap` loudest partials. `PartialLike` is
    // anything with .ratio and .amp (spectrum::Partial), kept generic so this header
    // stays JUCE- and Spectrum-free for the unit tests.
    //
    // Two rules here are load-bearing:
    //   - The gain multiplies BEFORE the cap sorts. The cap keeps the loudest
    //     SOUNDING partials; every call site used to sort on the compiled amplitude
    //     and ignore the gains, so a MUTED partial (gain 0) could hold a cap slot
    //     and beat against everything in the analysis while contributing silence to
    //     the ear, and a +24 dB-boosted quiet partial - possibly the loudest thing
    //     actually sounding - could be dropped from the analysis entirely: the user
    //     heard one spectrum and the search scored another.
    //   - The cap is ANALYSIS-ONLY (the curve's shape is set by the strongest
    //     partials and it keeps the sweeps instant). The SOUND is never capped.
    template <typename PartialLike>
    inline std::vector<Partial> analysisSpectrum (const std::vector<PartialLike>& partials,
                                                  const std::vector<float>& gain,
                                                  double f0, size_t cap = 64)
    {
        std::vector<Partial> spec;
        spec.reserve (partials.size());
        for (size_t i = 0; i < partials.size(); ++i)
        {
            const double g = i < gain.size() ? (double) gain[i] : 1.0;
            spec.push_back ({ (double) partials[i].ratio * f0, (double) partials[i].amp * g });
        }
        if (spec.size() > cap)
        {
            std::sort (spec.begin(), spec.end(),
                       [] (const Partial& a, const Partial& b) { return a.amp > b.amp; });
            spec.resize (cap);
        }
        return spec;
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

    //==========================================================================
    // THE GRID TABLES, and why they are the whole cost of a chord search.
    //
    // The enumeration below scores a combination from two exact tables - Dself per grid
    // point and Dcross per pair of grid points. Building dCross is O(G^2) calls of
    // crossDissonance, each O(P^2) partial pairs: at the 64-partial analysis cap, a 5-cent
    // grid over 2/1 is ~28,700 pairs x 4,096 partial pairs x 2 exp = ~235 million exp
    // calls, which measured 1.4 s single-threaded. The realtime "tuning follows the
    // spectrum" link re-runs this on every spectrum change, so that number IS the
    // feature's latency. Three things cut it without touching a single result:
    //
    //   1. BUILD ONLY WHAT IS READ. Every grid index in a legal combination sits at least
    //      minGap cents from the root AND from the period, and every PAIR of them at least
    //      minGap apart - so the band outside that, and every cell closer than minGap to
    //      the diagonal, is never looked at. In "0 vs all" the enumeration only ever reads
    //      row 0, so the other 99.6% of the matrix was pure waste.
    //   2. SPREAD IT OVER THE CORES. The cells are independent; each worker claims whole
    //      rows from one atomic counter (row i costs G-i pairs, so a static split would
    //      leave half the cores idle). Every cell is written by exactly one thread and read
    //      by none until the join, so the table is bit-identical to the serial one.
    //   3. REUSE IT. The tables depend on the spectrum, the period and the grid, and on
    //      nothing else - not on the note count, not on min/max, not on how many results
    //      are wanted. Re-searching the same spectrum (a second Find, a note count changed,
    //      Min flipped to Max) therefore costs the enumeration only.
    struct GridTables
    {
        std::vector<Partial> spectrum;      // exactly what these were built from
        double period = 0.0, step = 0.0, minGap = -1.0;
        int    G = 0;
        bool   full = false;                // every pair, not just the root row
        std::vector<double> dSelf, dCross;
        bool   valid = false;
    };

    inline std::mutex& gridCacheMutex() { static std::mutex m; return m; }
    inline GridTables& gridCache()      { static GridTables c; return c; }

    // Drop the cached tables (frees ~0.5 MB; the tests use it to prove a cold search and
    // a cached one return the same thing).
    inline void clearGridCache()
    {
        std::unique_lock<std::mutex> lk (gridCacheMutex());
        gridCache() = GridTables {};
    }

    // Do these cached tables answer THIS question exactly? The spectrum is compared value
    // by value (a spectrum that differs in the last bit is a different spectrum), and a
    // table built for a SMALLER minimum interval is a superset of one built for a larger
    // one - it has every cell this search can read, and then some.
    inline bool gridTablesMatch (const GridTables& c, const std::vector<Partial>& sp,
                                 double period, double step, int G, double minGap, bool needFull)
    {
        if (! c.valid || c.G != G || c.period != period || c.step != step
            || (needFull && ! c.full) || c.minGap > minGap
            || c.spectrum.size() != sp.size())
            return false;
        for (size_t i = 0; i < sp.size(); ++i)
            if (c.spectrum[i].freq != sp[i].freq || c.spectrum[i].amp != sp[i].amp)
                return false;
        return true;
    }

    // Fill dSelf/dCross for the indices a legal combination can actually use. Returns
    // false if `progress` asked to stop. `progress` is only ever called from THIS thread.
    inline bool buildGridTables (const std::vector<Partial>& sp, const std::vector<double>& alpha,
                                 int G, int iLo, int iHi, double step, double minGap, bool full,
                                 std::vector<double>& dSelf, std::vector<double>& dCross,
                                 const std::function<bool(float)>& progress, float p0, float p1)
    {
        const int stride = G + 1;
        dSelf.assign ((size_t) stride, 0.0);
        dCross.assign ((size_t) stride * (size_t) stride, 0.0);
        dSelf[0] = selfDissonance (sp, alpha[0]);            // the root's own term
        for (int i = iLo; i <= iHi; ++i)
            dSelf[(size_t) i] = selfDissonance (sp, alpha[(size_t) i]);

        // Row 0 is the beating against the ROOT - the one row "0 vs all" needs. The rest
        // of the matrix is only built for "all vs all".
        const int rows = full ? (iHi - iLo + 2) : 1;         // row 0, then iLo..iHi
        std::atomic<int>  next { 0 };
        std::atomic<int>  done { 0 };
        std::atomic<bool> stop { false };
        bool cancelled = false;

        const auto worker = [&] (bool main)
        {
            for (;;)
            {
                if (stop.load (std::memory_order_relaxed)) return;
                const int k = next.fetch_add (1, std::memory_order_relaxed);
                if (k >= rows) return;
                const int i = k == 0 ? 0 : iLo + k - 1;
                for (int j = std::max (i + 1, iLo); j <= iHi; ++j)
                {
                    // The SAME comparison the enumeration's gap rule makes, so a cell is
                    // skipped exactly when no combination can ever read it.
                    if ((double) (j - i) * step < minGap) continue;
                    const double v = crossDissonance (sp, alpha[(size_t) i], alpha[(size_t) j]);
                    dCross[(size_t) i * (size_t) stride + (size_t) j] = v;
                    dCross[(size_t) j * (size_t) stride + (size_t) i] = v;
                }
                const int d = done.fetch_add (1, std::memory_order_relaxed) + 1;
                if (main && progress
                    && ! progress (p0 + (p1 - p0) * (float) d / (float) std::max (1, rows)))
                {
                    stop.store (true, std::memory_order_relaxed);
                    cancelled = true;
                    return;
                }
            }
        };

        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        const int helpers = std::max (0, std::min ((int) hw, rows) - 1);
        std::vector<std::thread> pool;
        pool.reserve ((size_t) helpers);
        for (int t = 0; t < helpers; ++t)
        {
            // A machine that cannot spawn a thread must still finish the search: the
            // calling thread already does the same work, so it just goes slower.
            try { pool.emplace_back ([&worker] { worker (false); }); }
            catch (...) { break; }
        }
        worker (true);
        for (auto& t : pool) t.join();
        return ! cancelled;
    }

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
                                                  double minGapCents = 33.0, bool rootOnly = false,
                                                  const std::function<bool(float)>& progress = {})
    {
        std::vector<ChordResult> out;
        if (spectrum.empty() || ! (period > 1.02) || notes < 2)
            return out;
        if (notes > 8) notes = 8;                      // idx arrays support up to 8 notes
        const int m = notes - 1;                       // free notes above the fixed root
        const double totalCents = 1200.0 * std::log2 (period);

        // SCOPE: "all vs all" is the full union measure; "0 vs all" judges each note
        // only by its own roughness plus its beating against the root. The grid pass
        // and the refinement below MUST score the same way, or the refinement would
        // walk towards an extreme of a different surface than the one enumerated.
        const auto score = [&spectrum, rootOnly] (const std::vector<double>& al)
        {
            if (! rootOnly) return chordDissonance (spectrum, al);
            double v = 0.0;
            for (size_t i = 0; i < al.size(); ++i)
            {
                v += selfDissonance (spectrum, al[i]);
                if (i > 0) v += crossDissonance (spectrum, al[0], al[i]);
            }
            return v;
        };

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
        // Only the indices a legal combination can hold are built (see GridTables): every
        // note sits at least minGap from the root and from the period, so the band outside
        // [iLo, iHi] is unreachable and is never written NOR read.
        const double minGap = std::max (0.0, minGapCents);
        int iLo = 1, iHi = G;
        while (iLo <= G && (double) iLo * step < minGap) ++iLo;
        while (iHi >= 1 && totalCents - (double) iHi * step < minGap) --iHi;
        if (iHi - iLo + 1 < m)
            return out;                        // no room for m notes: the gap rule kills them all

        std::vector<double> dSelf, dCross;
        {
            std::unique_lock<std::mutex> lk (gridCacheMutex());
            const auto& c = gridCache();
            if (gridTablesMatch (c, spectrum, period, step, G, minGap, ! rootOnly))
            {
                dSelf = c.dSelf;               // ~460 KB memcpy against ~1.4 s of exp calls
                dCross = c.dCross;
            }
        }
        if (dCross.empty())
        {
            if (! buildGridTables (spectrum, alpha, G, iLo, iHi, step, minGap, ! rootOnly,
                                   dSelf, dCross, progress, 0.0f, 0.25f))
                return out;                    // cancelled mid-build
            std::unique_lock<std::mutex> lk (gridCacheMutex());
            auto& c = gridCache();
            c.spectrum = spectrum; c.period = period; c.step = step; c.G = G;
            c.minGap = minGap;     c.full = ! rootOnly;
            c.dSelf = dSelf;       c.dCross = dCross;
            c.valid = true;
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
        // BRANCH AND BOUND. Every term of the decomposition is >= 0 - Sethares' pair term
        // is min(ai,aj) * (C1*e^(A1 s d) + C2*e^(A2 s d)) with C1 = -C2 and A1 > A2, which
        // is 0 at d = 0 and positive for every d > 0 - so the running sum over the first
        // few notes is a LOWER BOUND on the whole combination, and on every combination
        // that shares those notes as a prefix. Once the keep-list is full, a prefix already
        // worse than its worst entry can be dropped WITH ITS ENTIRE SUBTREE: those
        // combinations could only have been rejected anyway, so the results are identical
        // and the work is not done. Only for minima (a maximum needs an upper bound, which
        // a partial sum is not), and only if no amplitude is negative, which would break
        // the sign argument.
        bool canPrune = ! maxima;
        for (const auto& p : spectrum) if (! (p.amp >= 0.0)) canPrune = false;
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
            // Accumulated as a PREFIX (each note against the root and against the notes
            // already added) so the running value is a bound on the whole subtree.
            const double cut = (int) best.size() >= keep ? best.back().value : 0.0;
            const bool   prune = canPrune && (int) best.size() >= keep;
            double v = rootTerm;
            int cutLevel = -1;
            for (int i = 0; i < m; ++i)
            {
                v += dSelf[(size_t) c[i]] + dCross[(size_t) c[i]]; // cross with the root (index 0)
                if (! rootOnly)                                    // "all vs all" only
                    for (int j = 0; j < i; ++j)
                        v += dCross[(size_t) c[j] * (size_t) (G + 1) + (size_t) c[i]];
                if (prune && v > cut) { cutLevel = i; break; }
            }
            if (cutLevel >= 0)
            {
                // Skip every combination that starts with this prefix: index cutLevel
                // moves on and everything after it is reset, instead of the usual
                // last-index step.
                if (++done % 65536 == 0 && progress
                    && ! progress (0.25f + 0.65f * (float) done / (float) combosEst))
                    return out;
                int i = cutLevel;
                while (i >= 0 && c[i] == G - (m - 1 - i)) --i;
                if (i < 0) break;
                ++c[i];
                for (int j = i + 1; j < m; ++j) c[j] = c[j - 1] + 1;
                continue;
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
            const double seedValue = score (a);
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
                    // At least 1 cent even when the user sets 0, so two notes can
                    // never converge onto the same pitch (that would be a duplicate,
                    // not a chord).
                    const double gap = std::max (1.0, minGap);
                    double lo = std::max (std::max (1.0, prevC + gap), c0 - win);
                    double hi = std::min (std::min (totalCents - 1.0, nextC - gap), c0 + win);
                    if (! (hi > lo)) continue; // no legal room: leave the note where it is
                    auto eval = [&] (double cts)
                    {
                        auto t = a; t[(size_t) i] = std::pow (2.0, cts / 1200.0);
                        const double v = score (t);
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
            double v = score (a);
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

    //==========================================================================
    // THE INVERSE PROBLEM: adapt the SPECTRUM to a fixed set of notes.
    //
    // searchChords above holds the timbre still and moves the notes. Sethares' own
    // point (TTSS ch. 6-7, "related scales and spectra") is that the relation goes
    // both ways: for any set of intervals there are timbres whose partials beat least
    // (or most) against them. That is what this does - the notes are given (a loaded
    // tuning's degrees, or the chord under the player's fingers) and the PARTIALS move.
    //
    // Coordinate descent, exactly the refinement machinery searchChords already trusts
    // (golden section per coordinate, several sweeps), over two kinds of coordinate:
    //   - each partial's RATIO, in log space, clamped between its neighbours
    //   - optionally each partial's AMPLITUDE, in dB
    // The ratio of partial 0 is FIXED at 1: it is the played pitch, the thing every
    // other partial is a ratio OF.
    //
    // WHY THE CLAMP IS NOT COSMETIC. Every partial keeps at least `minGapCents` of air
    // from its neighbours, so two partials can never converge onto one pitch. Without
    // that, a minimisation would happily stack partials on top of each other (coincident
    // partials beat with nothing) and the spectrum the user asked for - N partials -
    // would come back with fewer, merged by the search itself. Partials are never
    // dropped here, only moved.
    //
    // Amplitudes are bounded to +-`ampRangeDb` around their seed and NEVER reach zero
    // for the same reason: silencing a partial is deleting it, which this is not allowed
    // to do. The floor is a bound, not a mute.

    enum class Goal { Min = 0, Max = 1, Range = 2 };

    // Total Sethares dissonance of `sp` sounded at every alpha in `alphas`.
    //   rootOnly == true  -> the DYADS only: each note against the root, plus each
    //                        note's own intrinsic roughness (the panel's "0 vs all").
    //   rootOnly == false -> the whole union, every pair of notes ("all vs all").
    // The absolute frequencies in `sp` matter (Sethares' s depends on register), so the
    // caller anchors the spectrum at a real f0 before calling.
    inline double targetScore (const std::vector<Partial>& sp, const std::vector<double>& alphas,
                               bool rootOnly)
    {
        if (sp.empty() || alphas.empty())
            return 0.0;
        if (! rootOnly)
            return chordDissonance (sp, alphas);
        double v = 0.0;
        for (size_t i = 0; i < alphas.size(); ++i)
        {
            v += selfDissonance (sp, alphas[i]);
            if (i > 0) v += crossDissonance (sp, alphas[0], alphas[i]);
        }
        return v;
    }

    // What the search MINIMISES, for each goal. Range is a two-sided target: zero cost
    // anywhere inside [lo, hi], growing with the distance outside it - so "a specific
    // amount of beating" is a real target and not just min or max in disguise.
    inline double goalCost (double d, Goal g, double lo, double hi)
    {
        switch (g)
        {
            case Goal::Max:   return -d;
            case Goal::Range:
            {
                const double a = std::min (lo, hi), b = std::max (lo, hi);
                if (d < a) return a - d;
                if (d > b) return d - b;
                return 0.0;
            }
            case Goal::Min:
            default:          return d;
        }
    }

    struct AdaptResult
    {
        std::vector<Partial> spectrum;   // freq = ratio * f0, ascending; [0] is the played pitch
        double score = 0.0;              // the dissonance it achieves against the target
        double seedScore = 0.0;          // what the SEED spectrum scored, for honest before/after
        int    sweepsDone = 0;
    };

    // `seed` must be ascending by freq with seed[0].freq == f0 (the played pitch).
    // `maxRatio` is the highest ratio a partial may reach; `minGapCents` the air every
    // pair of partials keeps. `progress` (0..1) returns false to abort.
    inline AdaptResult adaptSpectrum (std::vector<Partial> seed, double f0,
                                      const std::vector<double>& alphas, bool rootOnly,
                                      Goal goal, double rangeLo, double rangeHi,
                                      double minGapCents, double maxRatio,
                                      bool shapeAmps, double ampRangeDb, int sweeps,
                                      const std::function<bool(float)>& progress = {})
    {
        AdaptResult out;
        if (seed.empty() || alphas.empty() || ! (f0 > 0.0))
            return out;

        const int n = (int) seed.size();
        std::vector<double> cents ((size_t) n), amp ((size_t) n), seedAmp ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            cents[(size_t) i] = 1200.0 * std::log2 (std::max (1.0e-12, seed[(size_t) i].freq / f0));
            amp[(size_t) i]   = seed[(size_t) i].amp;
            seedAmp[(size_t) i] = seed[(size_t) i].amp;
        }
        const double topCents = 1200.0 * std::log2 (std::max (1.0001, maxRatio));
        const double gap      = std::max (1.0, minGapCents); // at least 1 cent: never two partials on one pitch

        // The loudest seed partial's amplitude: every candidate is rescaled to it (below).
        double seedPeak = 0.0;
        for (const double a : seedAmp) seedPeak = std::max (seedPeak, a);
        if (! (seedPeak > 0.0)) seedPeak = 1.0;

        std::vector<Partial> work ((size_t) n);
        // WHY THE PEAK IS PINNED. dissmeasure is min(ai, aj) summed over pairs, which is
        // homogeneous of degree 1 in amplitude: multiply the whole spectrum by g and the
        // dissonance multiplies by g too. A free amplitude search therefore has a trivial
        // winning move - turn EVERYTHING down to the bottom of its bound - and it took it:
        // every partial came back at exactly -24 dB with its shape completely unchanged,
        // an enormous "improvement" that was nothing but a volume cut (and one the apply
        // step renormalises straight back out again). Pinning the peak makes the cost
        // invariant to overall level, so the only way to move it is to change the SHAPE -
        // which is the whole point of shaping amplitudes. It is also what makes a Range
        // target mean anything: the numbers now sit on a fixed scale.
        const auto build = [&]
        {
            double peak = 0.0;
            for (int i = 0; i < n; ++i) peak = std::max (peak, amp[(size_t) i]);
            const double g = peak > 0.0 ? seedPeak / peak : 1.0;
            for (int i = 0; i < n; ++i)
                work[(size_t) i] = { f0 * std::exp2 (cents[(size_t) i] / 1200.0), amp[(size_t) i] * g };
        };
        const auto cost = [&]
        {
            build();
            return goalCost (targetScore (work, alphas, rootOnly), goal, rangeLo, rangeHi);
        };

        build();
        out.seedScore = targetScore (work, alphas, rootOnly);
        double best = goalCost (out.seedScore, goal, rangeLo, rangeHi);

        constexpr double invPhi = 0.6180339887498949;
        // One golden-section line search on coordinate `set`, over [lo, hi]. `restore` is
        // the coordinate's current value: a clamped or multimodal window must never leave
        // the coordinate WORSE than it found it, so anything that fails to beat `best`
        // is rolled back. `best` is therefore monotone across the whole descent.
        const auto lineSearch = [&] (const std::function<void(double)>& set, double restore,
                                     double lo, double hi)
        {
            if (! (hi > lo + 1.0e-9)) return;
            double x1 = hi - (hi - lo) * invPhi, x2 = lo + (hi - lo) * invPhi;
            set (x1); double f1 = cost();
            set (x2); double f2 = cost();
            double bx = f1 < f2 ? x1 : x2, bf = std::min (f1, f2);
            for (int it = 0; it < 26 && (hi - lo) > 1.0e-4; ++it)
            {
                if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - (hi - lo) * invPhi; set (x1); f1 = cost(); }
                else         { lo = x1; x1 = x2; f1 = f2; x2 = lo + (hi - lo) * invPhi; set (x2); f2 = cost(); }
                if (f1 < bf) { bf = f1; bx = x1; }
                if (f2 < bf) { bf = f2; bx = x2; }
            }
            const double mid = 0.5 * (lo + hi);
            set (mid);
            const double fm = cost();
            if (fm < bf) { bf = fm; bx = mid; }
            if (bf < best) { best = bf; set (bx); }
            else           set (restore);
        };

        const int nSweeps = std::max (1, sweeps);
        for (int s = 0; s < nSweeps; ++s)
        {
            const double before = best;
            for (int i = 1; i < n; ++i)   // partial 0 IS the played pitch: it does not move
            {
                const double keep = cents[(size_t) i];
                const double lo = cents[(size_t) i - 1] + gap;
                const double hi = i + 1 < n ? cents[(size_t) i + 1] - gap : topCents;
                lineSearch ([&] (double c) { cents[(size_t) i] = c; }, keep, lo, std::min (hi, topCents));
                if (! (cents[(size_t) i] > cents[(size_t) i - 1] + gap * 0.999))
                    cents[(size_t) i] = keep;   // the window had no legal room: leave it put
                if (progress && ! progress (0.05f + 0.9f * (float) (s * n + i) / (float) (nSweeps * n)))
                { out.spectrum = work; out.score = targetScore (work, alphas, rootOnly); return out; }
            }
            if (shapeAmps)
                for (int i = 0; i < n; ++i)
                {
                    // dB around the SEED amplitude, floored well above silence: a bound,
                    // never a mute (a muted partial is a deleted partial).
                    const double a0 = std::max (1.0e-9, seedAmp[(size_t) i]);
                    const double now = 20.0 * std::log10 (std::max (1.0e-12, amp[(size_t) i] / a0));
                    lineSearch ([&] (double dB) { amp[(size_t) i] = a0 * std::pow (10.0, dB / 20.0); },
                                now, -std::abs (ampRangeDb), std::abs (ampRangeDb));
                }
            out.sweepsDone = s + 1;
            if (std::abs (before - best) < 1.0e-9) break;  // converged: further sweeps buy nothing
        }

        build();
        out.spectrum = work;
        out.score    = targetScore (work, alphas, rootOnly);
        return out;
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
