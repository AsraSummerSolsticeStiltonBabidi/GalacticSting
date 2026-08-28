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
    static double juce_selClamp (float v)
    { return v < (float) Selection::Even ? (double) Selection::Even
           : v > (float) Selection::Prime ? (double) Selection::Prime : (double) v; }

    static bool inSelectionSet (int k, int which)
    {
        switch (which)
        {
            case Selection::Even:  return (k % 2) == 0;
            case Selection::Odd:   return (k % 2) == 1;
            case Selection::Prime: return isPrime (k);
            default:               return true;   // All
        }
    }

    // Continuous selector: whole values are the classic sets, values in between
    // CROSSFADE them - a harmonic that is only in one of the two neighbouring sets
    // comes in at partial amplitude, so the fader sweeps smoothly from Even through
    // All and Odd to Prime instead of stepping.
    static double selectionWeight (int k, int N, float sel)
    {
        if (k == N) return 1.0;   // the played harmonic is always kept
        const double x  = (double) juce_selClamp (sel);
        const int    lo = (int) std::floor (x);
        const int    hi = std::min ((int) Selection::Prime, lo + 1);
        const double t  = x - (double) lo;
        const double a  = inSelectionSet (k, lo) ? 1.0 : 0.0;
        const double b  = inSelectionSet (k, hi) ? 1.0 : 0.0;
        return a + t * (b - a);
    }

    // Spectrum range: hard-cut every partial more than 2^N octaves above the played
    // pitch (ratio 1). N == 10 (or more) reaches the full template, so it is a no-op.
    //
    // THE LOWEST-RATIO PARTIAL ALWAYS SURVIVES, and that is not a nicety - without it
    // this cut could delete the ENTIRE spectrum and leave a silent note with no warning
    // anywhere. "Ratio 1" is only the bottom of the spectrum in Transposed mode; in
    // ORIGINAL mode the harmonics keep their natural positions, so the lowest partial
    // sits at ratio N (the base-harmonic shift), not at 1. Measured on the shipping
    // control, whose minimum is 1 octave: Original N=4 at range 1, and Original N=7 at
    // ranges 1 and 2, all compiled to ZERO partials - every partial was above the cut,
    // remove_if took the lot, and the note simply did not sound. A control that can
    // silence the instrument while claiming to narrow its bandwidth is exactly the
    // "silent limit" this project forbids.
    //
    // The rule is the one the keep-loudest cap below already uses for the same reason:
    // whatever else goes, the partial carrying the played pitch stays. The result is a
    // pure sine at the base harmonic - visibly "1 partial" in the amplitude window's
    // counter and in the meter - instead of silence. Nothing else changes: when any
    // partial is at or under the ceiling this branch is never taken, so the cut is
    // bit-for-bit what it was on every patch that was not being emptied.
    static void applyRangeCut (std::vector<Partial>& parts, const SpectrumSettings& s)
    {
        if (s.spectrumRangeOct >= 10 || parts.empty())
            return;
        const double maxR = std::pow (2.0, (double) s.spectrumRangeOct);
        const auto lowestIt = std::min_element (parts.begin(), parts.end(),
                                                [] (const Partial& a, const Partial& b) { return a.ratio < b.ratio; });
        const Partial lowest = *lowestIt;   // copied BEFORE the erase invalidates the iterator
        parts.erase (std::remove_if (parts.begin(), parts.end(),
                                     [maxR] (const Partial& p) { return (double) p.ratio > maxR; }),
                     parts.end());
        if (parts.empty())
            parts.push_back (lowest);
    }

    // Shared tail: keep-loudest cap (always keeping the lowest-ratio partial so the
    // played pitch survives), ascending sort, RMS normalisation. Used by both engines.
    static void finalisePartials (std::vector<Partial>& parts, const SpectrumSettings& s,
                                  SpectrumTemplate& out)
    {
        out.curveSlope = s.curveSlope; // what a per-voice slope modulation scales FROM
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
        // Stamp the slope on EVERY exit path, including the early returns below. The
        // harmonic engine has its own inline tail and never reaches finalisePartials, so
        // its template kept the default 1.0 - and every voice then believed it had to
        // rescale from 1.0 to the real slope, pushing the upper partials far enough down
        // that FftVerify reported them dropped.
        out.curveSlope = s.curveSlope;

        const auto aborted = [abort]() noexcept
        { return abort != nullptr && abort->load (std::memory_order_relaxed); };

        if (s.timbreDegrees.empty())
            return out;

        const double P     = std::max (1.0001, (double) s.timbrePeriod);
        const int    R     = std::max (1, s.timbrePeriodRepeats);
        const int    L     = std::max (1, (int) std::lround ((double) s.fractalLevel));
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

    //==========================================================================
    // The fractional-fractality reveal schedule. Both numbers are MEASURED, not tuned
    // by ear, and the note on blendFractalTemplates below says what they buy.
    //
    //   kRevealRampW  - the NARROWEST a single new partial's fade-in is ever allowed to
    //                   be, as a fraction of one crossing. The Fractal fader steps 0.01,
    //                   so half a crossing is 50 clicks: no partial can ever change by
    //                   more than ~1/50 of its ramp in one click. The old schedule gave
    //                   each partial 1/fresh.size() of the crossing - 0.0033 with 304 new
    //                   partials - i.e. THREE TIMES FINER than the fader itself, which is
    //                   why one click switched whole partials on at full height.
    //                   The ramp actually used is WIDER than this whenever the fresh set
    //                   is small enough that the last ramp would otherwise finish early
    //                   (see "THE RAMP WIDENS" in blendFractalTemplates). Widening only
    //                   ever buys MORE clicks per ramp, so this bound survives it.
    //   kRevealFloorDb- where that fade starts, relative to the partial's own final
    //                   amplitude. It has to be below hearing at the moment the partial
    //                   appears: the loudest new partial measured over every config
    //                   swept sits at -2.9 dB re the loudest partial in the template
    //                   (Prime, slope 0.3), so -72 dB puts even that one's first
    //                   appearance at -74.9 dB re the loudest partial. It also has to be
    //                   shallow enough that the ramp itself is gentle: 72 dB over half a
    //                   crossing is 1.44 dB per fader click.
    static constexpr double kRevealRampW    = 0.5;
    static constexpr double kRevealFloorDb  = -72.0;
    static constexpr double kDbPerLog2      = 6.020599913279624; // 20*log10(2)

    //==========================================================================
    // Fractional fractality: blend the integer level below (lo) with the one above
    // (hi). Partials present in BOTH crossfade their amplitudes; partials that only
    // exist at the higher level enter progressively from LOW to HIGH ratio as the
    // fraction rises (the newest one ramps in, so nothing pops); lo-only partials
    // fade out. At t=0 this is exactly lo.
    //
    // WHY THE REVEAL LOOKS THE WAY IT DOES. The first version admitted new partials by
    // COUNT - partial i was fully in once t passed i/fresh.size() - in ascending ratio
    // order. Amplitudes fall with ratio, so ascending ratio is DESCENDING loudness, and
    // fresh.size() is large: at Harmonics=Prime, level 1->2 there are 304 new partials,
    // so the first fader click (t=0.01) admitted the three LOUDEST of them at FULL
    // height. Measured on 1/12-octave band energy in dB, that one click moved the
    // spectrum 10.157 dB - 49% of the entire 1->2 crossing, and 70x a click taken in the
    // middle of the same crossing (0.145 dB). At 2->3 it was 67% of the crossing, at
    // 3->4 73%. That is the "raising Fractal a hair off an integer jumps, but moving it
    // in between does nothing" report.
    //
    // Two things fix it, and both are needed:
    //   * each new partial fades in over kRevealRampW of the crossing along a STRAIGHT
    //     LINE IN dB (geometric in amplitude), which is what an ear calls gradual;
    //   * the fade STARTS are staggered by the geometric mean of the partial's position
    //     by count and its position by cumulative energy. Position by count alone leaves
    //     the loud low partials bunched at the start; position by energy alone is
    //     fragile when one new partial owns most of the energy (measured on a synthetic
    //     set where partial 0 carries 99%: energy-only max/mean step 3.2, geometric mean
    //     2.1, count-only 1.8) and it wanders furthest from the fader's designed feel
    //     (spectral centroid at mid-crossing 0.78 octaves against the old 1.00; the
    //     geometric mean holds 0.84). The geometric mean takes both.
    // Order is untouched - low ratios still lead - so detail still grows upward; only
    // the SPEED at which each one arrives changed. Measured max-step/mean-step over the
    // 0.01 fader grid, Prime mode: 14.2/27.3/39.9 before, 1.8/1.7/2.0 after.
    static SpectrumTemplate blendFractalTemplates (const SpectrumTemplate& lo,
                                                   const SpectrumTemplate& hi,
                                                   double t, const SpectrumSettings& s)
    {
        if (lo.partials.empty()) return hi;
        if (hi.partials.empty()) return lo;

        // Match partials across levels by quantised cents (~1 c).
        const auto key = [] (float r)
        { return (long long) std::llround (1200.0 * std::log2 (std::max (1.0e-9, (double) r))); };

        // ONE PARTIAL AT A TIME, NEVER A BIN AT A TIME. Both lists leave their compile
        // sorted ascending by ratio, so they are sorted by key too and a two-pointer merge
        // finds the coincidences without a map. Inside a bin that both levels occupy the
        // partials are PAIRED ONE FOR ONE in ascending-ratio order; a bin holding more
        // upper-level partials than lower-level ones leaves the surplus as genuinely NEW,
        // at their OWN ratios.
        //
        // What this replaces summed each level's bin into a single amplitude and pushed
        // the whole upper-level bin energy onto the LOWER level's partials. k upper
        // partials sharing a bin with one lower partial were FOLDED onto that one ratio
        // with their k amplitudes added, and two failures came out of that:
        //   * PARTIALS DISAPPEARED. Measured at Base 4 Odd, level 2->3: the blend at 0.999
        //     held 7119 partials against level 3's 14480. 7361 of the user's partials
        //     merged away with nothing said - which this synth does not do.
        //   * A UNIFORM GAIN ERROR AT THE TOP OF EVERY CROSSING. Folding k partials of
        //     amplitude a into one of amplitude k*a multiplies that bin's sum of squares
        //     by k, so the RMS normalisation in finalisePartials divided the WHOLE
        //     template by too much. Everything that did NOT fold - the loud low partials
        //     the ear is actually following - came out a uniform gain below the upper
        //     level: measured -0.427 dB (Base 4 Odd 2->3), -0.332 dB (Base 8 1->2),
        //     -0.914 dB (Base 16 1->2), spread only 0.010-0.025 dB over the 32 loudest
        //     partials, which is what makes it a GAIN error and not a timbre error.
        //     The fader paid it all back in ONE click at the handover from x.99 to the
        //     single-level compile at x+1.00: 1.480 dB at Base 16 1->2, 5.7x the average
        //     click of that crossing and the biggest click left anywhere on the fader.
        // Pairing one for one fixes both at the root - every upper-level partial keeps its
        // own ratio, so t=1 IS the upper level and there is no gain left to pay back.
        // Measured handover click after: 0.046 / 0.144 / 0.419 dB, each one SMALLER than
        // the click before it. The alternative considered - keep the fold and undo its
        // effect on the normaliser with a uniform gain - was rejected on measurement: it
        // does correct the loud partials (-0.427 dB -> +0.003 dB) but it scales the
        // already-too-loud folded bins up as well, and the handover click got WORSE, not
        // better (0.740 -> 0.930, 0.993 -> 1.092, 1.480 -> 1.684 dB). It also left the
        // merged partials merged.
        //
        // WHAT IS LEFT. Where the upper level itself overflows the keep-loudest cap
        // (Base 16 All 1->2 wants 113432 partials and kMaxPartials is 16384) the blend and
        // the single-level compile keep DIFFERENT 16384 of them, because a fresh partial
        // still short of full amplitude ranks differently, and 0.419 dB of handover
        // survives there. That is the cap, not the blend: at a reach where nothing is
        // truncated the same config's handover is 0.046-0.105 dB in every config swept,
        // and `truncated` is set on both sides so the meter says so.
        //
        // Dropping the two hash maps for a merge walk also made the blend cheaper, not
        // dearer, despite emitting more partials: measured median blend cost 53.99 ->
        // 45.32 ms (Base 4 All L3->L4, 16k+16k) and 20.40 -> 0.56 ms (Base 16 All L1->L2).
        //
        // A partial the upper level KILLS fades LINEARLY, not geometrically: a geometric
        // path can never reach zero (pow(0, t) would kill it outright on the first click
        // instead of fading it out).
        std::vector<Partial> parts, fresh;
        parts.reserve (lo.partials.size() + hi.partials.size());
        fresh.reserve (hi.partials.size());

        const auto dying = [&parts, t] (const Partial& p)
        { parts.push_back ({ p.ratio, (float) ((double) p.amp * (1.0 - t)) }); };

        // A partial that exists at BOTH levels walks a GEOMETRIC path from its lower
        // amplitude to its upper one - lA^(1-t) * hA^t, a straight line in dB. Both forms
        // agree exactly at t = 0 (scale 1) and t = 1 (scale hA/lA), so no patch parked on
        // an integer level moves; only the shape in between does, and a straight line in
        // dB is what an ear hears as an even fade. It is worth little on its own -
        // isolated, the survivor rescale moves the spectrum 0.030 dB over the first fader
        // click against the reveal's 10.157 dB - but it costs nothing and it is the ONLY
        // mechanism at all when the base harmonic set is closed under multiplication
        // (Harmonics = All/Odd/Even, or a pure prime LIMIT), where nothing is fresh.
        //
        // exp2/log2 rather than pow: measured 1.6x faster over the 16k-partial worst case
        // (0.97 ms -> 0.59 ms for the two blend loops at Base 4, L3->L4).
        const auto surviving = [&parts, t] (const Partial& l, const Partial& h)
        {
            const double lA = (double) l.amp, hA = (double) h.amp;
            const double scale = (lA > 0.0 && hA > 0.0) ? std::exp2 (t * std::log2 (hA / lA))
                                                        : 1.0 - t;
            parts.push_back ({ l.ratio, (float) (lA * scale) });
        };

        const size_t nLo = lo.partials.size(), nHi = hi.partials.size();
        size_t i = 0, j = 0;
        while (i < nLo && j < nHi)
        {
            const long long kl = key (lo.partials[i].ratio);
            const long long kh = key (hi.partials[j].ratio);
            if (kl < kh) { dying (lo.partials[i++]); continue; }
            if (kh < kl) { fresh.push_back (hi.partials[j++]); continue; }

            size_t i2 = i, j2 = j;
            while (i2 < nLo && key (lo.partials[i2].ratio) == kl) ++i2;
            while (j2 < nHi && key (hi.partials[j2].ratio) == kl) ++j2;
            const size_t pairs = std::min (i2 - i, j2 - j);
            for (size_t p = 0;     p < pairs;   ++p) surviving (lo.partials[i + p], hi.partials[j + p]);
            for (size_t p = pairs; p < i2 - i;  ++p) dying (lo.partials[i + p]);
            for (size_t p = pairs; p < j2 - j;  ++p) fresh.push_back (hi.partials[j + p]);
            i = i2; j = j2;
        }
        for (; i < nLo; ++i) dying (lo.partials[i]);
        for (; j < nHi; ++j) fresh.push_back (hi.partials[j]);

        // `fresh` is filled in hi's own order, which is already ascending by ratio, so it
        // needs no sort - and the reveal below depends on that order.
        //
        // Partials that exist ONLY at the upper level enter from low to high: raising
        // Fractal adds detail progressively rather than crossfading a whole level in.
        // Each one gets its own dB ramp; the ramps START in ascending-ratio order, spaced
        // by the geometric mean of "how many partials are below me" and "how much of the
        // new ENERGY is below me" (see the header note).
        if (! fresh.empty())
        {
            double totalEnergy = 0.0;
            for (const auto& f : fresh) totalEnergy += (double) f.amp * (double) f.amp;

            const double n     = (double) fresh.size();
            const double slope = kRevealFloorDb / kDbPerLog2; // dB floor in log2 units

            // THE RAMP WIDENS WHEN THE FRESH SET IS SMALL, so that the LAST new partial
            // finishes exactly at t = 1 instead of somewhere in the middle of the fader.
            //
            // The stagger below places start(i) in [0, 1-kRevealRampW]; the largest of
            // those, `lastStart`, is reached by the last partial, and a fixed ramp width
            // of kRevealRampW then ended the reveal at lastStart + kRevealRampW. With a
            // big fresh set lastStart -> 1-kRevealRampW and that is 1.0, as intended. With
            // a TINY fresh set both stagger factors collapse: with exactly ONE new partial
            // (Harmonics = Prime at prime limit 2, or a 2-degree Scala timbre at Reps 1)
            // byCount is 0, lastStart is 0, and the whole reveal was over by t = 0.5.
            // Measured on 1/12-octave band energy, clicks 51-100 of those crossings then
            // carried 3.2% / 5.9% / 3.1% of the loudness path against clicks 1-50: the top
            // half of the fader did nothing it was designed to do.
            //
            // So the ramp is widened by exactly the gap: rampW = 1 - lastStart. Because
            // lastStart <= 1-kRevealRampW always, rampW >= kRevealRampW ALWAYS - it can
            // only ever buy MORE fader clicks per ramp, never fewer, so the per-click
            // bound kRevealRampW was chosen for survives this untouched. Large fresh sets
            // barely notice (Prime 1->2, 304 fresh: 0.5000 -> 0.5008).
            //
            // fresh is ascending in ratio and amplitude falls with ratio, so the last
            // partial is the quietest one and both factors are at their maximum there.
            const double eLast     = (double) fresh.back().amp * (double) fresh.back().amp;
            const double cLast     = (n - 1.0) / n;
            const double gLast     = totalEnergy > 0.0 ? (totalEnergy - eLast) / totalEnergy : cLast;
            const double lastStart = std::sqrt (cLast * gLast) * (1.0 - kRevealRampW);
            const double rampW     = 1.0 - lastStart;   // >= kRevealRampW by construction

            double belowEnergy = 0.0;
            for (size_t f = 0; f < fresh.size(); ++f)
            {
                const double byCount  = (double) f / n;
                const double byEnergy = totalEnergy > 0.0 ? belowEnergy / totalEnergy : byCount;
                belowEnergy += (double) fresh[f].amp * (double) fresh[f].amp;

                // Both factors are non-decreasing in f, so the start is too, so the loop
                // can stop at the first partial whose ramp has not opened yet.
                const double start = std::sqrt (byCount * byEnergy) * (1.0 - kRevealRampW);
                const double u     = (t - start) / rampW;   // 0 -> silence, 1 -> full
                if (u <= 0.0) break;

                const double w = (u >= 1.0) ? 1.0 : std::exp2 (slope * (1.0 - u));
                parts.push_back ({ fresh[f].ratio, (float) ((double) fresh[f].amp * w) });
            }
        }

        SpectrumTemplate out;
        out.truncated = lo.truncated || hi.truncated;
        finalisePartials (parts, s, out); // cap + sort + RMS-normalise + moves into out
        return out;
    }

    //==========================================================================
    // Crossfade two compiled templates (see the contract in Spectrum.h).
    SpectrumTemplate blendTemplates (const SpectrumTemplate& a, const SpectrumTemplate& b, double t)
    {
        if (! (t > 0.0)) return a;   // also catches NaN -> "stay where you were"
        if (! (t < 1.0)) return b;

        const auto key = [] (float r)
        { return (long long) std::llround (1200.0 * std::log2 (std::max (1.0e-9, (double) r))); };

        // ONE bin map fed by BOTH sides - see the energy-duplication note in the header.
        struct Bin { double aAmp = 0.0, bAmp = 0.0; };
        std::unordered_map<long long, Bin> bins;
        bins.reserve ((a.partials.size() + b.partials.size()) * 2);
        for (const auto& p : a.partials) bins[key (p.ratio)].aAmp += (double) p.amp;
        for (const auto& p : b.partials) bins[key (p.ratio)].bAmp += (double) p.amp;

        std::vector<Partial> parts;
        parts.reserve (a.partials.size() + b.partials.size());

        // Every partial of `a` keeps its OWN ratio and takes its share of the bin's
        // blended energy. A bin holding three of a's partials and one of b's splits b's
        // contribution between the three in proportion instead of handing each the lot.
        for (const auto& p : a.partials)
        {
            const auto it = bins.find (key (p.ratio));
            const double aA = (it != bins.end()) ? it->second.aAmp : (double) p.amp;
            const double bA = (it != bins.end()) ? it->second.bAmp : 0.0;
            const double share = aA > 0.0 ? (double) p.amp / aA : 0.0;
            const double amp = share * ((1.0 - t) * aA + t * bA);
            if (amp > 0.0) parts.push_back ({ p.ratio, (float) amp });
        }
        // Partials `a` has no counterpart for are the genuinely new ones: they fade in
        // from silence at t*amp rather than appearing at full height, which is what
        // keeps a drag across the pad free of pops.
        for (const auto& p : b.partials)
        {
            const auto it = bins.find (key (p.ratio));
            if (it != bins.end() && it->second.aAmp > 0.0) continue;   // already carried above
            const double amp = t * (double) p.amp;
            if (amp > 0.0) parts.push_back ({ p.ratio, (float) amp });
        }

        std::sort (parts.begin(), parts.end(),
                   [] (const Partial& x, const Partial& y) { return x.ratio < y.ratio; });

        SpectrumTemplate out;
        out.truncated = a.truncated || b.truncated;
        out.rawCount  = (int) parts.size();
        // The one honest limit: the voices' partial arrays are kMaxPartials long. Keep
        // the loudest (and always the lowest ratio, so the played pitch survives) and
        // SAY so through the flag the meter already reads.
        if ((int) parts.size() > kMaxPartials)
        {
            auto lowestIt = std::min_element (parts.begin(), parts.end(),
                                              [] (const Partial& x, const Partial& y) { return x.ratio < y.ratio; });
            std::iter_swap (parts.begin(), lowestIt);
            std::nth_element (parts.begin() + 1, parts.begin() + kMaxPartials, parts.end(),
                              [] (const Partial& x, const Partial& y) { return x.amp > y.amp; });
            parts.resize ((size_t) kMaxPartials);
            std::sort (parts.begin(), parts.end(),
                       [] (const Partial& x, const Partial& y) { return x.ratio < y.ratio; });
            out.truncated = true;
        }

        double sumSq = 0.0;
        for (const auto& p : parts) sumSq += (double) p.amp * (double) p.amp;
        if (sumSq > 0.0)
        {
            const float inv = (float) (1.0 / std::sqrt (sumSq));
            for (auto& p : parts) p.amp *= inv;
        }

        // A linear blend of the two compile-time slopes. The caller (the playground)
        // overwrites this with the exact value it wrote to the curveSlope PARAMETER, so
        // an unmodulated voice computes dS = 0 and renders the blended amplitudes as
        // they are instead of re-sloping the whole crossfade.
        out.curveSlope = (float) ((1.0 - t) * (double) a.curveSlope + t * (double) b.curveSlope);
        out.partials   = std::move (parts);
        return out;
    }

    SpectrumTemplate compileSpectrum (const SpectrumSettings& s, const std::atomic<bool>* abort)
    {
        // Decimal fractal level -> compile the two neighbouring integer levels and blend.
        // Below the threshold the blend is skipped and the single integer level compiled
        // instead, which saves the second compile for a fraction nobody can hear.
        //
        // That shortcut used to be a CLIFF. With the old count-based reveal the first
        // blended value the automation lane could reach, L+0.0011, already put the
        // lowest new partial in at a third of its final height: 4.881 dB away from the
        // integer level in Prime mode (7.068 dB at Base 4) against 0.000 dB at L+0.0009.
        // The slider itself snaps to 0.01 and so could never land in there, but host
        // automation and modulators do not snap and could. With the dB reveal ramp the
        // same step now measures 0.0033 dB (worst over every config swept: 0.0118 dB at
        // Base 4) - smaller than the smallest step the fader can produce anywhere
        // (0.023 dB) - so the threshold can stay where it is and keep saving the compile.
        {
            const double Lf  = std::min (8.0, std::max (1.0, (double) s.fractalLevel));
            const int    Llo = (int) std::floor (Lf);
            const double t   = Lf - (double) Llo;
            if (t > 1.0e-3)
            {
                SpectrumSettings a = s, b = s;
                a.fractalLevel = (float) Llo;
                b.fractalLevel = (float) (Llo + 1);
                const auto tLo = compileSpectrum (a, abort);
                const auto tHi = compileSpectrum (b, abort);
                if (abort != nullptr && abort->load (std::memory_order_relaxed)) return {};
                return blendFractalTemplates (tLo, tHi, t, s);
            }
        }

        if (s.source == Source::ScalaTimbre)
            return compileScalaTimbre (s, abort);

        SpectrumTemplate out;
        // Stamp the slope on EVERY exit path, including the early returns below. The
        // harmonic engine has its own inline tail and never reaches finalisePartials, so
        // its template kept the default 1.0 - and every voice then believed it had to
        // rescale from 1.0 to the real slope, pushing the upper partials far enough down
        // that FftVerify reported them dropped.
        out.curveSlope = s.curveSlope;

        const auto aborted = [abort]() noexcept
        {
            return abort != nullptr && abort->load (std::memory_order_relaxed);
        };

        const int N = std::max (1, s.baseN);
        const int L = std::max (1, (int) std::lround ((double) s.fractalLevel));

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

        std::vector<double> selW; // selection weight (1, 0, or a blend in between)
        selW.reserve ((size_t) (Kmax - N + 1));
        // How far between this prime-limit stop and the NEXT (narrower) one the fader
        // sits: 0 = exactly on this stop, 1 = on the next. In between, a harmonic the
        // next stop would remove is still there but fading down - so the fader sweeps
        // continuously instead of jumping from one prime limit to the next.
        const double primeMix = std::min (1.0, std::max (0.0, (double) s.primeLimitPos - std::floor ((double) s.primeLimitPos)));
        for (int k = N; k <= Kmax; ++k)
        {
            const double wSel = selectionWeight (k, N, s.selection);
            if (wSel <= 1.0e-6) continue;
            double wPrime = 1.0;
            if (s.primeUseWhitelist)
            {
                if (! primeWhitelistOk (k, s.primeWhitelistMask)) continue;
            }
            else
            {
                if (! primeLimitOk (k, s.primeLimit)) continue;      // this stop already vetoes it
                if (primeMix > 1.0e-6 && s.primeLimitNext != s.primeLimit
                    && ! primeLimitOk (k, s.primeLimitNext))
                    wPrime = 1.0 - primeMix;                          // the NEXT stop drops it: fade
            }
            if (wPrime <= 1.0e-6) continue;
            base.emplace_back (k, 0.0);
            selW.push_back (wSel * wPrime);
        }

        if (base.empty())
            return out; // strict: nothing survives -> the note is silent

        const int kMin = base.front().first; // smallest surviving harmonic number

        // Stage D: amplitude curve, relative to the LOWEST present harmonic so the
        // played-pitch partial is always the reference (amp 1) and the roll-off fills
        // the band. This makes a higher base sound richer (more partials under the same
        // envelope), not quieter. The selection weight rides on top of it, so a
        // half-way Harmonics setting fades its extra harmonics in instead of adding
        // them at once.
        for (size_t i = 0; i < base.size(); ++i)
            base[i].second = std::pow ((double) base[i].first / (double) kMin, - (double) s.curveSlope)
                           * selW[i];

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

        // TEMPER mode: snap every harmonic-engine partial onto the loaded scale's
        // pitch grid (degree * period^k, nearest in log-frequency) - the engine keeps
        // generating structure while the .scl decides where partials may sit.
        if (s.temperToTimbre && ! s.timbreDegrees.empty() && s.timbrePeriod > 1.0001f && ! parts.empty())
        {
            // Work in LOG space with a sorted grid: one binary search per partial
            // instead of a 3xD scan with a log() per candidate (a 200-degree scale
            // over thousands of partials used to add seconds to every rebuild).
            const double logPer = std::log ((double) s.timbrePeriod);
            std::vector<double> logDeg;
            logDeg.reserve (s.timbreDegrees.size());
            for (const float dg : s.timbreDegrees)
                if (dg > 0.0f) logDeg.push_back (std::log ((double) dg));
            std::sort (logDeg.begin(), logDeg.end());
            // Fold every degree into [0, logPer) so the grid is periodic and complete.
            for (auto& l : logDeg) l -= logPer * std::floor (l / logPer);
            std::sort (logDeg.begin(), logDeg.end());
            logDeg.erase (std::unique (logDeg.begin(), logDeg.end(),
                                       [] (double a, double b) { return std::abs (a - b) < 1.0e-9; }),
                          logDeg.end());

            if (! logDeg.empty())
            {
                int tick = 0;
                for (auto& p : parts)
                {
                    if ((++tick & 4095) == 0 && aborted()) return {};
                    const double r = (double) p.ratio;
                    if (! (r > 0.0)) continue;
                    const double lr = std::log (r);
                    const double k  = std::floor (lr / logPer);
                    const double x  = lr - k * logPer;             // position inside the period
                    // Nearest grid point: the two neighbours of x, plus the wrapped
                    // ends (first degree of the next period / last of this one).
                    const auto it = std::lower_bound (logDeg.begin(), logDeg.end(), x);
                    double best = 0.0, bestErr = 1.0e30;
                    const auto consider = [&] (double cand)
                    { const double e = std::abs (cand - lr); if (e < bestErr) { bestErr = e; best = cand; } };
                    if (it != logDeg.end())    consider (k * logPer + *it);
                    if (it != logDeg.begin())  consider (k * logPer + *(it - 1));
                    consider ((k + 1.0) * logPer + logDeg.front());
                    consider ((k - 1.0) * logPer + logDeg.back());
                    p.ratio = (float) std::exp (best);
                }
            }

            // Snapping can land several partials on the same grid point: merge them
            // (amp-sum, the same 0.5 c convention as the other coincidence paths) so
            // Aligned phase does not stack coherent duplicates past the RMS model and
            // the partial meter counts distinct frequencies.
            std::sort (parts.begin(), parts.end(),
                       [] (const Partial& a, const Partial& b) { return a.ratio < b.ratio; });
            std::vector<Partial> merged;
            merged.reserve (parts.size());
            for (const auto& p : parts)
            {
                if (! merged.empty()
                    && std::abs (1200.0 * std::log2 ((double) p.ratio / (double) merged.back().ratio)) < 0.5)
                    merged.back().amp += p.amp;
                else
                    merged.push_back (p);
            }
            parts = std::move (merged);
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
