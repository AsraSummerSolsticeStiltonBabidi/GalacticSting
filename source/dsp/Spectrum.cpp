#include "Spectrum.h"

#include <unordered_map>
#include <algorithm>

namespace spectrum
{
    // The played harmonic k==N (the fundamental) is ALWAYS kept: otherwise, in
    // Transposed mode, "Even" (2,4,6...) would be transposed down onto the fundamental
    // and become an ordinary 1,2,3,4 series - i.e. IDENTICAL to "All" (even harmonics
    // ARE a full series an octave up). Keeping the fundamental makes Even = fundamental
    // + even (1,2,4,6...), a genuinely hollow, distinct timbre. Even N already keeps
    // itself; the fix matters most for the default N=1.
    static bool selectionPass (int k, int N, Selection sel)
    {
        if (k == N) return true;
        switch (sel)
        {
            case Selection::All:   return true;
            case Selection::Even:  return (k % 2) == 0;
            case Selection::Odd:   return (k % 2) == 1;
            case Selection::Prime: return isPrime (k);
        }
        return true;
    }

    // Spectrum range: hard-cut every partial more than 2^N octaves above the played
    // pitch (ratio 1). N == 10 (or more) reaches the full template, so it is a no-op.
    static void applyRangeCut (std::vector<Partial>& parts, const SpectrumSettings& s)
    {
        if (s.spectrumRangeOct >= 10)
            return;
        const double maxR = std::pow (2.0, (double) s.spectrumRangeOct);
        parts.erase (std::remove_if (parts.begin(), parts.end(),
                                     [maxR] (const Partial& p) { return (double) p.ratio > maxR; }),
                     parts.end());
    }

    // Shared tail: keep-loudest cap (always keeping the lowest-ratio partial so the
    // played pitch survives), ascending sort, RMS normalisation. Used by both engines.
    static void finalisePartials (std::vector<Partial>& parts, const SpectrumSettings& s,
                                  SpectrumTemplate& out)
    {
        applyRangeCut (parts, s);
        out.rawCount = (int) parts.size();

        const int cap = std::min (s.maxPartials, kMaxPartials);
        if ((int) parts.size() > cap)
        {
            auto lowestIt = std::min_element (parts.begin(), parts.end(),
                                              [] (const Partial& a, const Partial& b) { return a.ratio < b.ratio; });
            std::iter_swap (parts.begin(), lowestIt);
            std::nth_element (parts.begin() + 1, parts.begin() + cap, parts.end(),
                              [] (const Partial& a, const Partial& b) { return a.amp > b.amp; });
            parts.resize ((size_t) cap);
            out.truncated = true;
        }

        std::sort (parts.begin(), parts.end(),
                   [] (const Partial& a, const Partial& b) { return a.ratio < b.ratio; });

        double sumSq = 0.0;
        for (const auto& p : parts) sumSq += (double) p.amp * (double) p.amp;
        if (sumSq > 0.0)
        {
            const float inv = (float) (1.0 / std::sqrt (sumSq));
            for (auto& p : parts) p.amp *= inv;
        }

        out.partials = std::move (parts);
    }

    //==========================================================================
    // Scala-as-timbre engine: the loaded scale's degree ratios become the partials,
    // stacked over `timbrePeriodRepeats` periods (more periods -> reaches higher).
    // Slope rolls off the highs; fractal L-fold self-convolves (float ratios merged
    // by quantised cents). The lowest partial is always 1.0 = the played pitch.
    static SpectrumTemplate compileScalaTimbre (const SpectrumSettings& s, const std::atomic<bool>* abort)
    {
        SpectrumTemplate out;

        const auto aborted = [abort]() noexcept
        { return abort != nullptr && abort->load (std::memory_order_relaxed); };

        if (s.timbreDegrees.empty())
            return out;

        const double P     = std::max (1.0001, (double) s.timbrePeriod);
        const int    R     = std::max (1, s.timbrePeriodRepeats);
        const int    L     = std::max (1, s.fractalLevel);
        const double slope = (double) s.curveSlope;
        const double reach = (double) std::max (1, s.reach);
        const double floorLin = std::pow (10.0, (double) s.ampFloorDb / 20.0);

        // Base partial set: degree ratio * period^j, amplitude = ratio^(-slope).
        std::vector<std::pair<double, double>> base; // (ratio, amp)
        base.reserve (s.timbreDegrees.size() * (size_t) R);
        for (int j = 0; j < R; ++j)
        {
            const double pj = std::pow (P, (double) j);
            for (float d : s.timbreDegrees)
            {
                const double r = (double) d * pj;
                if (r < 0.999 || r > reach) continue;     // below the fundamental or past the reach
                base.emplace_back (r, std::pow (r, -slope));
            }
        }
        if (base.empty())
            return out;

        // Merge coincident partials by quantised cents (~0.5 c) during convolution.
        constexpr double mergeCents = 0.5;
        const auto keyOf = [mergeCents] (double r) { return (long long) std::llround (1200.0 * std::log2 (r) / mergeCents); };

        std::unordered_map<long long, std::pair<double, double>> cur; // key -> (ratio, amp)
        cur.reserve (base.size() * 2);
        for (const auto& b : base) { auto& e = cur[keyOf (b.first)]; e.first = b.first; e.second += b.second; }

        const size_t workingCap = 200000;
        bool hitWorkingCap = false;

        for (int level = 2; level <= L; ++level)
        {
            if (aborted()) return {};
            std::unordered_map<long long, std::pair<double, double>> next;
            next.reserve (cur.size() * 2);

            double maxAmp = 0.0; int tick = 0;
            for (const auto& c : cur)
            {
                if ((++tick & 8191) == 0 && aborted()) return {};
                for (const auto& b : base)
                {
                    const double r = c.second.first * b.first;
                    if (r > reach) continue;
                    auto& e = next[keyOf (r)];
                    e.first = r;
                    e.second += c.second.second * b.second;
                    if (e.second > maxAmp) maxAmp = e.second;
                }
            }

            const double prune = maxAmp * floorLin;
            for (auto it = next.begin(); it != next.end(); )
                it = (it->second.second < prune) ? next.erase (it) : std::next (it);

            if (next.size() > workingCap)
            {
                std::vector<std::pair<long long, std::pair<double, double>>> v (next.begin(), next.end());
                std::nth_element (v.begin(), v.begin() + workingCap, v.end(),
                                  [] (const auto& a, const auto& b) { return a.second.second > b.second.second; });
                v.resize (workingCap);
                next.clear();
                for (const auto& e : v) next[e.first] = e.second;
                hitWorkingCap = true;
            }

            cur.swap (next);
        }

        double maxA = 0.0;
        for (const auto& c : cur) maxA = std::max (maxA, c.second.second);
        const double cullThresh = maxA * floorLin;

        std::vector<Partial> parts;
        parts.reserve (cur.size());
        for (const auto& c : cur)
            if (c.second.second >= cullThresh)
                parts.push_back ({ (float) c.second.first, (float) c.second.second });

        // Anchor the lowest surviving partial to the played pitch (ratio 1.0).
        if (! parts.empty())
        {
            double minRatio = 1e18;
            for (const auto& p : parts) minRatio = std::min (minRatio, (double) p.ratio);
            if (minRatio > 0.0)
                for (auto& p : parts) p.ratio = (float) ((double) p.ratio / minRatio);
        }

        finalisePartials (parts, s, out);
        out.truncated = out.truncated || hitWorkingCap;
        return out;
    }

    SpectrumTemplate compileSpectrum (const SpectrumSettings& s, const std::atomic<bool>* abort)
    {
        if (s.source == Source::ScalaTimbre)
            return compileScalaTimbre (s, abort);

        SpectrumTemplate out;

        const auto aborted = [abort]() noexcept
        {
            return abort != nullptr && abort->load (std::memory_order_relaxed);
        };

        const int N = std::max (1, s.baseN);
        const int L = std::max (1, s.fractalLevel);

        // Generate up to `kSpectrumReach` in final ratio (relative to the played note)
        // so the spectrum reaches past the audible ceiling for the whole musical range;
        // the per-voice Nyquist cull removes whatever is inaudible at the actual pitch.
        // There is no musical "max harmonic" - only the physical Nyquist limit.
        const int reach = std::max (1, s.reach);
        const long long reachK = (s.baseMode == BaseMode::Transposed)
                                     ? (long long) N * reach
                                     : (long long) reach;
        int Kmax = (int) std::min<long long> (std::max<long long> (N, reachK),
                                              (long long) kMaxPartials);

        // For fractal (L>=2) bound the BASE harmonic COUNT so the convolution is fast. The
        // self-similar products still fill higher ratios up to the reach (via the kappa cap
        // below), so raising the level no longer shrinks the partial count.
        if (L >= 2)
            Kmax = std::min (Kmax, std::max (N, 1024));

        //----------------------------------------------------------------------
        // Stages A-C: base harmonic set (integer k).
        //   selection AND prime-limit are both keep-predicates (intersection);
        //   any active rule that excludes k silences it -> prime-limit "rules".
        std::vector<std::pair<int, double>> base; // (harmonic number k, amplitude), ascending in k
        base.reserve ((size_t) (Kmax - N + 1));

        for (int k = N; k <= Kmax; ++k)
        {
            if (! selectionPass (k, N, s.selection))   continue;
            const bool primeOk = s.primeUseWhitelist ? primeWhitelistOk (k, s.primeWhitelistMask)
                                                      : primeLimitOk (k, s.primeLimit);
            if (! primeOk) continue;
            base.emplace_back (k, 0.0);
        }

        if (base.empty())
            return out; // strict: nothing survives -> the note is silent

        const int kMin = base.front().first; // smallest surviving harmonic number

        // Stage D: amplitude curve, relative to the LOWEST present harmonic so the
        // played-pitch partial is always the reference (amp 1) and the roll-off fills
        // the band. This makes a higher base sound richer (more partials under the same
        // envelope), not quieter.
        for (auto& b : base)
            b.second = std::pow ((double) b.first / (double) kMin, - (double) s.curveSlope);

        // Ratio->partial mapping. Ratio relative to the played note:
        //   Transposed : R = kappa / (N * kMin^(L-1)) -> lowest partial at kMin/N (played pitch)
        //   Original   : R = kappa / (kMin^(L-1))     -> lowest partial at kMin (natural position)
        // Products whose final ratio would exceed the reach are dropped during the
        // convolution (kappaCap): they are above the template range and keep the working set
        // bounded, so the fractal is fast AND its partial count does not shrink with level.
        const double baseFactor = (s.baseMode == BaseMode::Transposed) ? (double) N : 1.0;
        const double denom      = baseFactor * std::pow ((double) kMin, L - 1);
        // Products above the template's ratio ceiling (kSpectrumReach) are dropped.
        const long long kappaCap = (long long) std::llround ((double) kSpectrumReach * denom);

        //----------------------------------------------------------------------
        // Stage F: L-fold multiplicative self-convolution.
        //   Working set keyed by integer kappa = product of L base harmonic
        //   numbers -> coincident partials merge EXACTLY (same kappa). Amplitudes
        //   multiply then sum on coincidence. Inaudible entries are pruned each
        //   level to bound the combinatorial explosion (they can only shrink).
        const double floorLin   = std::pow (10.0, (double) s.ampFloorDb / 20.0);
        const size_t workingCap = 200000;

        bool hitWorkingCap = false;

        std::unordered_map<long long, double> cur;
        cur.reserve (base.size() * 2);
        for (const auto& b : base)
            cur[(long long) b.first] += b.second;

        for (int level = 2; level <= L; ++level)
        {
            if (aborted()) return {};

            std::unordered_map<long long, double> next;
            next.reserve (cur.size() * 2);

            double maxAmp = 0.0;
            int    abortTick = 0;
            for (const auto& c : cur)
            {
                if ((++abortTick & 8191) == 0 && aborted()) return {};

                for (const auto& b : base)
                {
                    const long long kappa = c.first * (long long) b.first;
                    if (kappa > kappaCap) continue; // beyond the reach -> would be culled anyway
                    double& slot = next[kappa];
                    slot += c.second * b.second;
                    if (slot > maxAmp) maxAmp = slot;
                }
            }

            // prune entries that are inaudible relative to this level's peak
            const double prune = maxAmp * floorLin;
            for (auto it = next.begin(); it != next.end(); )
                it = (it->second < prune) ? next.erase (it) : std::next (it);

            // last-resort bound if a flat spectrum defeats amplitude pruning
            if (next.size() > workingCap)
            {
                std::vector<std::pair<long long, double>> v (next.begin(), next.end());
                std::nth_element (v.begin(), v.begin() + workingCap, v.end(),
                                  [] (const auto& a, const auto& b) { return a.second > b.second; });
                v.resize (workingCap);
                next.clear();
                for (const auto& e : v) next[e.first] = e.second;
                hitWorkingCap = true; // surfaced via out.truncated below (honesty)
            }

            cur.swap (next);
        }

        //----------------------------------------------------------------------
        // Convert to partials (denom / baseFactor were computed above).
        double maxA = 0.0;
        for (const auto& c : cur) maxA = std::max (maxA, c.second);
        const double cullThresh = maxA * floorLin;

        std::vector<Partial> parts;
        parts.reserve (cur.size());
        for (const auto& c : cur)
        {
            if (c.second < cullThresh) continue;
            parts.push_back ({ (float) ((double) c.first / denom), (float) c.second });
        }

        // Transposed mode = "transposed down to the fundamental": the lowest surviving
        // partial always lands on the played pitch, whatever the whitelist / prime-limit
        // removed. (When the played harmonic k=N survives, minRatio is already 1, so this
        // is a no-op.) Original mode leaves harmonics at their natural positions.
        if (s.baseMode == BaseMode::Transposed && ! parts.empty())
        {
            double minRatio = 1e18;
            for (const auto& p : parts) minRatio = std::min (minRatio, (double) p.ratio);
            if (minRatio > 0.0)
                for (auto& p : parts) p.ratio = (float) ((double) p.ratio / minRatio);
        }

        applyRangeCut (parts, s); // hard spectrum-range cut, relative to the played pitch

        out.rawCount = (int) parts.size();

        // Safety cap: keep the loudest maxPartials (transparent -> flagged for the meter).
        // ALWAYS keep the lowest-ratio partial first: it carries the played pitch (after the
        // Transposed anchor), so dropping it would shift the perceived pitch on dense patches.
        const int cap = std::min (s.maxPartials, kMaxPartials);
        if ((int) parts.size() > cap)
        {
            auto lowestIt = std::min_element (parts.begin(), parts.end(),
                                              [] (const Partial& a, const Partial& b) { return a.ratio < b.ratio; });
            std::iter_swap (parts.begin(), lowestIt);
            std::nth_element (parts.begin() + 1, parts.begin() + cap, parts.end(),
                              [] (const Partial& a, const Partial& b) { return a.amp > b.amp; });
            parts.resize ((size_t) cap);
            out.truncated = true;
        }

        // Voice relies on ascending-ratio order for the Nyquist break.
        std::sort (parts.begin(), parts.end(),
                   [] (const Partial& a, const Partial& b) { return a.ratio < b.ratio; });

        // RMS normalisation (scales all partials equally -> timbre preserved): set the
        // sum of amp^2 to 1, so the rendered voice has a constant RMS (~0.707 before
        // headroom) regardless of how many partials there are. This keeps loudness
        // consistent across patches. Crucially it does NOT let the time-domain peak run
        // away the way peak-normalisation did (that pushed dense/flat patches to +20 dBFS
        // and clipped the output); the per-voice headroom keeps even high-crest sums below
        // full scale. Any non-empty spectrum has amp>0, so a tiny-but-valid patch is still
        // scaled up.
        double sumSq = 0.0;
        for (const auto& p : parts) sumSq += (double) p.amp * (double) p.amp;
        if (sumSq > 0.0)
        {
            const float inv = (float) (1.0 / std::sqrt (sumSq));
            for (auto& p : parts) p.amp *= inv;
        }

        out.truncated = out.truncated || hitWorkingCap;
        out.partials = std::move (parts);
        return out;
    }
}
