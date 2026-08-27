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

    // The ratio one full rise step covers. `tuningPeriod` is only consulted for
    // RisePeriod, and only when it is a usable period.
    inline double riseRatio (int choice, double tuningPeriod) noexcept
    {
        if (choice == RisePeriod && std::isfinite (tuningPeriod) && tuningPeriod > 1.0001)
            return juce::jlimit (1.0001, 16.0, tuningPeriod);
        return 2.0;
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
    inline double riseSeconds (bool sync, double freeMs, int divIdx, double bpm) noexcept
    {
        if (sync && bpm > 1.0) return beatsForDiv (divIdx) * 60.0 / bpm;
        return juce::jlimit (0.05, 600.0, (std::isfinite (freeMs) ? freeMs : 8000.0) * 0.001);
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

    // How wide (in octaves) the near-Nyquist taper should be. The floor is a
    // musical constant; the second term is what keeps a FAST rise smooth - the
    // taper is refreshed once per block, so it must be several blocks' worth of
    // movement wide or a partial would leave in one step anyway.
    inline double taperWidth (double blockLogMove) noexcept
    {
        return juce::jlimit (kTaperOct, 1.0, juce::jmax (kTaperOct, 24.0 * std::abs (blockLogMove)));
    }
}
