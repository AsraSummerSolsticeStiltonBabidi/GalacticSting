#pragma once

//==============================================================================
// SHEPARD MODE - the rules, shared by the voice that renders it, the parameter
// list, the editor and the verifiers.
//
// WHAT IT IS NOW (v2). Version 1 was a separate audio engine: a free-running
// stack of SINES mixed under the synth, deaf to the keyboard. It is gone. The
// Shepard is now a PLAYED MODE of the ordinary voice:
//
//   - the root is THE KEY YOU PRESS, at its tuned pitch (frequencyForNote);
//   - the building block is THE SYNTH'S OWN SPECTRUM - the live template with
//     everything on it (fractal, manual amps, per-voice slope, morph gliding,
//     the realtime-adaptive stream), not a sine;
//   - the voice renders that spectrum at several RISE-INTERVAL-spaced
//     transpositions of the key's pitch at once, all rising together;
//   - the modulators, the three envelopes and the filter are untouched: the
//     Shepard MULTIPLIES the voice's partial set, it does not bypass the voice.
//     Releasing the key releases the Shepard through the amp envelope.
//
// THE CONSTRUCTION (and why it cannot click). The copies live on a grid of
// log2-frequencies anchored on the played pitch:
//     L_m = log2(f0) + (g + m) * P,     m integer,  g in [0,1) rising
// with P = log2(riseRatio) - ONE OCTAVE by default in every tuning, or the
// active tuning's own period when the user asks for it (see kRiseChoices).
// Anchoring on f0 is what makes the tone "start from the key": every copy is a
// whole number of rise intervals from the note that was actually pressed.
//
// The amplitude window is a raised cosine over a FIXED band of ABSOLUTE
// log-frequency. It does NOT travel with the stack - a window that moves with
// the copies makes the pitch really rise instead of only seeming to. Because
// the window reaches EXACTLY ZERO at both edges, the copy that drifts off the
// top leaves silent and the one entering at the bottom enters silent: there is
// no reset to hide because no reset exists.
//
// THE VISIBLE BOUNDS (never silent ones):
//   - at most kMaxCopies transpositions sound at once. With an octave rise that
//     covers 8 of the ~10 audible octaves; the band is CENTRED in the audible
//     range and narrows rather than any copy being dropped from inside it.
//   - each copy renders up to kStride partials. The stack reuses the voice's
//     own oscillator arrays (kSlots * kStride == spectrum::kMaxPartials), so a
//     Shepard voice can never cost more oscillators than the same voice already
//     could without it. If a spectrum is dense enough that the LOWEST copy would
//     need more than that, the window's bottom edge lifts (the band narrows,
//     glided, so copies leave through a zero) instead of any partial being cut.
//   - everything above the copy's own Nyquist is culled exactly as the ordinary
//     voice culls it, with a taper (kTaperMax partials wide) so a partial leaves
//     by fading rather than by vanishing between two samples.
//   - ONE RISE STEP IS AT MOST kMaxRiseRatio (16/1, four octaves), and that bound
//     was missing from this list while the code enforced it - which is the whole
//     reason the list exists. A tuning may STATE a wider period than that: an .scl
//     period line is unbounded and MTS-ESP's MTS_GetPeriodRatio is a float, so this
//     is not a hypothetical. MEASURED: a one-note .scl whose period line reads 32/1
//     loads, periodRatio() answers 32, and with "Period pitch bend" chosen the rise
//     step came out 16 - half of what the file says - with nothing on screen about
//     it. The bound stays (see kMaxRiseRatio for why 16 is where it stops); what
//     changed is that riseRatioIsLimited() now answers it, so the panel can say
//     which interval is actually rising.
//
// RT contract: no allocation, no locks, no strings. Every array the voice needs
// already exists; this header only holds the constants and the pure maths.
//==============================================================================

#include <juce_core/juce_core.h>
#include "Spectrum.h"

#include <cmath>

namespace shepard
{
    //== The stack's size, expressed once ======================================
    // kSlots is the HASH MODULUS for a copy's grid index (slot = m mod kSlots),
    // exactly as v1 hashed its sine components. Two spare slots over kMaxCopies
    // are what let the copy leaving one edge and the copy entering the other
    // hold different slots for the block they overlap.
    inline constexpr int kSlots     = 10;
    inline constexpr int kMaxCopies = kSlots - 2;                     // 8
    // Partials per copy. kSlots * kStride <= kMaxPartials by construction, which
    // is the promise that Shepard mode reuses the voice's existing oscillator
    // arrays and adds no per-voice memory at all.
    inline constexpr int kStride    = spectrum::kMaxPartials / kSlots; // 1638
    static_assert (kSlots * kStride <= spectrum::kMaxPartials, "the stack must fit the voice's arrays");
    // How far below a copy's own Nyquist the anti-alias taper starts, and the
    // most partials it may span. See taperWidth().
    inline constexpr int    kTaperMax   = 64;
    inline constexpr double kTaperOct   = 1.0 / 32.0;

    // TWO density floors, both expressed as an index into the (ascending) template.
    //   kDensityGuard - the COMFORTABLE one the window's bottom edge glides towards,
    //                   at half a slot, so a glide has a whole slot's worth of slack;
    //   kDensityHard  - the one applied INSTANTLY after the glide, so a template that
    //                   suddenly turns dense cannot spend 150 ms needing more partials
    //                   than a slot holds. A morph's gliding ratios can sit below the
    //                   template's own, which is what the slack is for.
    inline constexpr int kDensityGuard = kStride / 2;
    inline constexpr int kDensityHard  = kStride * 7 / 8;

    //== The rise interval choice (the "octave / period" dropdown) =============
    // 2/1 ALWAYS by default, in every tuning: an octave is what a listener hears
    // as "the same note again", and a Shepard that silently rose by 3/1 because a
    // Bohlen-Pierce scale was loaded would be a tuning decision nobody asked for.
    // The tuning's own period is offered - and only offered - when it differs.
    enum RiseChoice { RiseOctave = 0, RisePeriod = 1 };

    // THE WIDEST RISE STEP, and the narrowest period that counts as one at all.
    //
    // kMinRiseRatio is the same threshold TuningManager::hasUsablePeriod() uses, and it
    // is a GUARD rather than a clamp here: a period at or below it is not a repeat
    // interval going up, so the choice falls back to the octave instead of being
    // stretched up to 1.0001. Written once so the two cannot drift apart.
    //
    // kMaxRiseRatio is where a rise step stops. Four octaves is already most of what a
    // Shepard has to work with: bandFor centres the copies in the audible range, which
    // is 9.95 octaves wide at 44.1 kHz, so at 16/1 the stack is down to 2.49 copies and
    // at 32/1 it would be 1.99 - and at two copies the raised-cosine window is no longer
    // a smooth crossfade between neighbours but two humps taking turns. The bound is a
    // real one and is NOT to be widened to make a warning go away; it is to be SAID.
    inline constexpr double kMinRiseRatio = 1.0001;
    inline constexpr double kMaxRiseRatio = 16.0;

    // The ratio one full rise step covers. `tuningPeriod` is only consulted for
    // RisePeriod, and only when it is a usable period.
    //
    // jmin, not jlimit: the guard above already demands tuningPeriod > kMinRiseRatio, so
    // a lower clamp at the same number could never fire. It read as a second bound and
    // would have made riseRatioIsLimited() below answer for a case that does not exist.
    inline double riseRatio (int choice, double tuningPeriod) noexcept
    {
        if (choice == RisePeriod && std::isfinite (tuningPeriod) && tuningPeriod > kMinRiseRatio)
            return juce::jmin (kMaxRiseRatio, tuningPeriod);
        return 2.0;
    }

    // IS THE STEP THE USER ASKED FOR THE STEP THEY ARE GETTING? The one question a panel
    // needs in order not to let a four-octave rise pass for the five-octave one a 32/1
    // scale states. Pure, cheap and lock-free - the caller already holds the tuning's
    // period (the processor publishes it), so there is no state to keep.
    //
    // Only the upper bound can answer yes. RiseOctave never consults the period at all,
    // and a period that fails the guard is not narrowed to fit - the choice falls back
    // to a plain octave, which the dropdown already says out loud.
    inline bool riseRatioIsLimited (int choice, double tuningPeriod) noexcept
    {
        return choice == RisePeriod && std::isfinite (tuningPeriod)
               && tuningPeriod > kMaxRiseRatio;
    }

    // Is there anything to choose? (The dropdown is hidden when there is not.)
    inline bool periodDiffersFromOctave (double tuningPeriod) noexcept
    {
        return std::isfinite (tuningPeriod) && std::abs (tuningPeriod - 2.0) > 1.0e-4;
    }

    //== Rate ==================================================================
    // The quantized-mode choices, in beats: the rise of ONE interval takes this
    // many host beats. One table, shared by the parameter list, the voice's
    // beats->seconds conversion and the tests, so they can never disagree.
    inline constexpr double kDivBeats[] = { 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0,
                                            1.0 / 2.0, 2.0 / 3.0, 1.0, 2.0, 3.0, 4.0, 8.0, 16.0 };
    inline constexpr int kNumDivs = (int) (sizeof (kDivBeats) / sizeof (kDivBeats[0]));
    inline double beatsForDiv (int idx) noexcept
    { return kDivBeats[juce::jlimit (0, kNumDivs - 1, idx)]; }

    // Seconds per rise interval, from the two rate dresses plus the host clock.
    // With Sync chosen and NO bpm on offer the FREE value takes over - the same
    // honesty rule v1 had, and the editor still says so.
    // The FASTEST rise there is, expressed ONCE. Both dresses land on it: the free
    // fader cannot go below it and a sync division at an absurd BPM is clamped to it.
    // It has to live here rather than at the two use sites because the block splitter
    // below sizes a plan from it, and a splitter working from a faster rate than the
    // plan really uses would cut every offline buffer into needless chunks.
    inline constexpr double kMinRiseSec = 0.05;
    inline double riseSeconds (bool sync, double freeMs, int divIdx, double bpm) noexcept
    {
        if (sync && bpm > 1.0) return juce::jmax (kMinRiseSec, beatsForDiv (divIdx) * 60.0 / bpm);
        return juce::jlimit (kMinRiseSec, 600.0, (std::isfinite (freeMs) ? freeMs : 8000.0) * 0.001);
    }

    //== How much audio ONE plan may cover =====================================
    // A plan (planShepard) hands every grid index whose window is non-zero ANYWHERE
    // inside it its own slot, and a slot cannot hold two grid indices at once. Over a
    // stretch in which the grid advances by `d` intervals the plan needs
    //     d + (span / period) + 1   slots,
    // and span/period is at most kMaxCopies by construction (bandFor caps it there, and
    // the glide is a convex combination of two values that both obey the cap, so it
    // obeys it too). With kSlots slots available that leaves
    //     d <= kSlots - kMaxCopies - 1 == 1 interval.
    //
    // WHY THIS EXISTS. The clamp in planShepard that trims the list to kSlots used to
    // carry the comment "defensive; never fires". At a 48 kHz 8192-sample OFFLINE
    // RENDER block and the fastest rise the grid advances 3.4 intervals in one buffer,
    // the plan asks for 12 slots, and the clamp deletes a copy whose window is still
    // 0.269 of full scale: a partial dropped in silence and a step in the same event.
    // renderNextBlock splits the block on this instead, so the sentence is now true
    // because the code makes it true - and run 53 asserts the clamp never fires.
    inline int maxPlanSamples (double sr, double riseSec) noexcept
    {
        constexpr double intervals = (double) (kSlots - kMaxCopies - 1);
        static_assert (kSlots - kMaxCopies - 1 >= 1, "a plan must be able to cover at least one interval");
        const double s = intervals * juce::jmax (1.0e-4, riseSec) * juce::jmax (1.0, sr);
        // The floor keeps a pathological rate from splitting a block into single samples;
        // it is far below any rate the panel can ask for (the fastest is 0.05 s per
        // interval, which at 48 kHz is 2400 samples - a plan longer than any live buffer).
        return (int) juce::jlimit (64.0, 1.0e9, std::floor (s));
    }

    //== The window ============================================================
    // THE WINDOW: a raised cosine in normalised position u, EXACTLY zero at both
    // ends (and with zero slope there), which is the whole silent-wrap guarantee -
    // gating it to 0 outside [0,1] cannot introduce a step.
    //
    // It takes cos(2*pi*u) as an argument rather than computing it, because the
    // voice advances that cosine by a complex rotation once per sample (a cos()
    // per copy per sample would not be affordable) and this must be the ONE
    // definition of the window's shape. Written as a second formula over there, a
    // sabotage of the rule would leave every test green.
    inline double windowFromCos (double u, double cosTwoPiU) noexcept
    {
        if (u <= 0.0 || u >= 1.0) return 0.0;
        return 0.5 - 0.5 * cosTwoPiU;
    }

    // The band the copies live in, in log2 Hz.
    //   sr        - sample rate (the top edge never reaches the anti-alias filter)
    //   periodLog - log2 of the rise ratio
    //   loDense   - the density floor: the lowest log2 root at which the template
    //               still fits kStride partials under Nyquist (-1e9 when the
    //               template is small enough that it never binds)
    // Returns the bottom edge and the span; both are CENTRED in the audible band
    // so a narrow band sits where the ear is, not at one end.
    struct Band { double lo, span; };
    inline Band bandFor (double sr, double periodLog, double loDense) noexcept
    {
        const double lTop = std::log2 (juce::jmin (20000.0, sr * 0.45));
        const double lBot = juce::jmax (std::log2 (20.0), loDense);
        // A degenerate band (a density floor above the top edge) still has to
        // produce ONE usable copy rather than a NaN: give it the top edge and a
        // span of one interval.
        if (lTop - lBot < periodLog)
            return { lTop - periodLog, juce::jmax (1.0e-6, periodLog) };
        const double span = juce::jmin (lTop - lBot, (double) kMaxCopies * periodLog);
        return { 0.5 * (lTop + lBot) - 0.5 * span, span };
    }

    //== What a Shepard voice is normalised BY ==================================
    // THE RULE: the divisor is a property of the BAND, never of where the window
    // happens to be standing. A normaliser that divides by the window's CURRENT sum
    // divides the window straight back out, and the window reaching exactly zero at
    // both edges is the entire silent-wrap guarantee - so that normaliser makes the
    // copy leaving the top leave at FULL LEVEL, and the illusion is void.
    //
    // Nobody noticed because the two agree exactly in the ordinary case. On a uniform
    // grid of q = span/period copies the raised cosine sums to exactly q/2, and its
    // square to exactly 3q/8, for every integer q >= 3 - and the band is 8 intervals
    // wide whenever the audible range and the density floor allow it. It is only when
    // a dense spectrum or a wide rise interval NARROWS the band that the instantaneous
    // sum starts to ripple, and at one copy it does not ripple, it IS the window:
    // measured at a 16/1-period tuning with Base N at 64, the voice was as loud at a
    // window sum of 0.098 as at 0.95.
    //
    // Two references because the two laws want different things, and both are
    // geometry-only so neither can undo the window:
    //   PEAK law (0.5 / sum a_j): must never UNDERSTATE the sum, or the all-in-phase
    //     peak it is holding down at 0.5 is exceeded. jmax with 1.0 is what a single
    //     copy can reach on its own; it is exact for every integer q and for q <= 2,
    //     and no more than 5% low in between (measured across q = 1 .. 8).
    //   RMS law (0.25 / sqrt(sum a_j^2)): wants the MEAN, because RMS is an average
    //     measure and 3q/8 is that mean exactly, at every q.
    inline double windowSumPeak (double q) noexcept { return juce::jmax (1.0, 0.5 * q); }
    inline double windowSumMeanSq (double q) noexcept { return juce::jmax (1.0e-6, 0.375 * q); }

    // How wide (in octaves) the near-Nyquist taper should be. The floor is a
    // musical constant; the second term is what keeps a FAST rise smooth - the
    // taper is refreshed once per block, so it must be several blocks' worth of
    // movement wide or a partial would leave in one step anyway.
    inline double taperWidth (double blockLogMove) noexcept
    {
        return juce::jlimit (kTaperOct, 1.0, juce::jmax (kTaperOct, 24.0 * std::abs (blockLogMove)));
    }
}
