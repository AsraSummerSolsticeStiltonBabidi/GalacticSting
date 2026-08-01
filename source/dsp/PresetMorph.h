#pragma once

//==============================================================================
// THE PRESET PLAYGROUND'S RULES, as pure C++ (no JUCE) so they can be unit-tested
// offline by SpectrumTests without building a plugin or a window.
//
// The playground puts up to four presets on the corners of a rectangle and a
// draggable point inside it. Two automatable parameters (pgX, pgY, both 0..1) ARE
// that point, so the DAW records and plays the movement back whether the window is
// open or shut.
//
// This header answers three questions and nothing else:
//
//   1. HOW MUCH of each corner is in force at (x, y)?          -> bilinearWeights()
//   2. In WHICH SPACE is a given parameter averaged?           -> morphSpaceFor()
//   3. WHICH corner wins for a setting that cannot be averaged? -> nearestCorner()
//
// The compiled SPECTRUM is NOT morphed here - two corners can be running different
// ENGINES (harmonic vs a custom scale), and their parameter sets do not correspond,
// so averaging "baseN" against "timbreRepeats" is meaningless. The partial LISTS are
// blended instead; see spectrum::blendTemplates in Spectrum.h.
//==============================================================================

#include <string>
#include <cmath>
#include <algorithm>

namespace presetmorph
{
    // Corner order, and it is the order the UI draws them in:
    //   0 = top-left (A)   1 = top-right (B)
    //   2 = bottom-left(C) 3 = bottom-right(D)
    inline constexpr int kCorners = 4;

    //==========================================================================
    // 1. WEIGHTS.
    //
    // Plain bilinear interpolation over the unit square: corner i's weight is the
    // area of the sub-rectangle DIAGONALLY OPPOSITE it. They sum to 1, each is 1 at
    // its own corner and 0 at the other three, and along an edge the two corners of
    // that edge share it linearly - which is what makes a two-preset playground a
    // straight line and a corner an exact preset.
    //
    // UNASSIGNED corners contribute NOTHING: their weight is forced to 0 and the rest
    // are renormalised. Two assigned corners therefore give a line, three give a
    // triangle-ish region, and an empty corner never quietly drags the sound towards
    // "no preset" (which would be silence, or worse, the patch that happened to be
    // loaded when the playground opened).
    //
    // The renormalisation has ONE singular point per empty corner: the point sitting
    // EXACTLY on an unassigned corner, where every assigned corner's raw weight is 0
    // and the ratio 0/0 has no answer. Rather than snap (which would make a sound
    // change discontinuously at the one place the user is most likely to park the
    // point - a corner), the position is pulled a hair towards the centre and the
    // weights recomputed: the common factor that was going to zero cancels in the
    // renormalisation, so a two-preset TOP edge still reads (1-x) : x all the way
    // along the BOTTOM edge, and a diagonal pair reads 50/50 at the empty corners.
    inline void bilinearWeights (double x, double y, const bool assigned[kCorners],
                                 double wOut[kCorners])
    {
        const auto fill = [&] (double px, double py)
        {
            const double raw[kCorners] = { (1.0 - px) * (1.0 - py), px * (1.0 - py),
                                           (1.0 - px) * py,         px * py };
            double sum = 0.0;
            for (int i = 0; i < kCorners; ++i)
            {
                wOut[i] = assigned[i] ? raw[i] : 0.0;
                sum += wOut[i];
            }
            if (sum <= 1.0e-12) return false;
            for (int i = 0; i < kCorners; ++i) wOut[i] /= sum;
            return true;
        };

        x = std::min (1.0, std::max (0.0, x));
        y = std::min (1.0, std::max (0.0, y));
        if (fill (x, y)) return;
        // Singular: shrink towards the centre (see above). RELATIVE, not a fixed step,
        // so the non-degenerate axis keeps its ratio to one part in 10^7 - a fixed 1e-4
        // nudge moved a two-preset line's 70/30 to 69.99/30.01, which is a real answer
        // being quietly rounded rather than a singularity being resolved.
        constexpr double shrink = 1.0 - 1.0e-6;
        if (fill (0.5 + (x - 0.5) * shrink, 0.5 + (y - 0.5) * shrink)) return;
        for (int i = 0; i < kCorners; ++i) wOut[i] = 0.0;   // nothing assigned at all
    }

    //==========================================================================
    // 2. THE SPACE A PARAMETER IS AVERAGED IN.
    //
    // Getting this wrong is the whole difference between a real intermediate and a
    // smear. The rule is: average in whatever space the parameter is PERCEIVED in.
    //
    //   Log    - anything whose perceptual distance is a RATIO: times, frequencies,
    //            resonance Q, and the base-harmonic index. Halfway between a 20 ms
    //            decay and a 2 s decay is 200 ms; the linear answer, 1.01 s, is
    //            musically nowhere near the middle - it is "the long one, slightly
    //            shortened". Same for Base Harmonic: in Transposed mode the played
    //            pitch is -log2(N) octaves, so halfway from N=1 to N=16 is N=4 (two
    //            octaves down), not N=8.5 (three octaves down and a bit).
    //   Linear - anything ALREADY in a logarithmic or perceptual unit (cents,
    //            semitones, octaves, dB), any normalised 0..1 control, any exponent
    //            (amp-decay slope, the 2^n divider exponents), any fraction, and the
    //            continuous selector axes (Harmonics, Prime Limit, Fractal Level),
    //            whose whole point is that the position BETWEEN two stops is the
    //            blend of them - so the position is what must be averaged.
    //   Discrete - see rule 3.
    //   Excluded - see the list below.
    //
    // EXCLUDED, and why. The playground changes the SOUND, never what the keys mean:
    //   tuningMode      the tuning system in force
    //   bendRange       per-note bend range - part of how MIDI maps to pitch
    //   refA            the master reference pitch
    //   octaveShift     whole-instrument transpose
    //   tuningBankSlot  loading a slot LOADS A SCALE
    // (The loaded .scl itself, the absolute 128-key table and its period are not
    // parameters at all and are likewise never touched - the playground never calls
    // anything that writes them, exactly as loadPreset() already refuses to.)
    //   specRandom      a TRIGGER, not a state. Its rising edge generates a brand-new
    //                   random spectrum; averaging a trigger, or handing it to a
    //                   nearest-corner rule, would fire fresh random timbres as the
    //                   point crossed a boundary and there is no continuum between
    //                   one random spectrum and the next to morph along anyway.
    //   pgX, pgY        the point itself. Morphing the coordinates from the corners
    //                   would make the playground drive its own input.
    //   pgMode          morph vs mix - the playground's OWN mode. Left to the discrete
    //                   rule it would be written from the winner corner's saved value,
    //                   so dropping a preset that happened to be saved in mix mode
    //                   would flip the playground the user is standing in.
    //   pgVoices        the mix's per-corner voice budget - playground configuration,
    //                   like pgMode: morphing it from the corners would let a preset
    //                   quietly change how much CPU the pad the user is dragging costs.
    //
    // A FUTURE PARAMETER lands in Linear (or Discrete, if it is a choice/bool) by
    // default. That is deliberate: the default must be "morphs, in the most
    // conservative space", never "silently swept into the tuning".
    enum class MorphSpace { Excluded, Discrete, Linear, Log };

    inline bool endsWith (const std::string& s, const char* suffix)
    {
        const std::string t (suffix);
        return s.size() >= t.size() && s.compare (s.size() - t.size(), t.size(), t) == 0;
    }

    // `isDiscrete` must be true for every choice and boolean parameter. It is asked of
    // the PARAMETER OBJECT rather than pattern-matched off the id, so a choice added
    // later is discrete automatically instead of being averaged into a value between
    // two modes that means nothing.
    inline MorphSpace morphSpaceFor (const std::string& id, bool isDiscrete)
    {
        // --- excluded (see the list above) ---
        if (id == "tuningMode" || id == "bendRange" || id == "refA"
            || id == "octaveShift" || id == "tuningBankSlot"
            || id == "specRandom"
            || id == "pgX" || id == "pgY" || id == "pgMode" || id == "pgVoices")
            return MorphSpace::Excluded;

        if (isDiscrete)
            return MorphSpace::Discrete;

        // --- log space ---
        // A FRACTION that merely ends in "Decay". Checked first, or the suffix rule
        // below would take it for a time and average 0.05 and 0.95 as 0.22.
        if (id == "rhythmFeedbackDecay")
            return MorphSpace::Linear;
        // Envelope segment CURVES are shapes in -1..+1 (0 = straight), not times.
        if (endsWith (id, "CurveA") || endsWith (id, "CurveD") || endsWith (id, "CurveR"))
            return MorphSpace::Linear;
        // Every envelope time: ampAttack/Decay/Release, filter*, pitch*, and Morph Time.
        if (endsWith (id, "Attack") || endsWith (id, "Decay") || endsWith (id, "Release")
            || id == "morphTime")
            return MorphSpace::Log;
        // The modulator envelopes' one-letter segments (modE1A, modE2D, modE3R...) are
        // times too. "S" is a sustain LEVEL (0..1) and is deliberately not in the set.
        if (id.size() == 6 && id.compare (0, 4, "modE") == 0
            && (id[5] == 'A' || id[5] == 'D' || id[5] == 'R'))
            return MorphSpace::Log;
        if (endsWith (id, "Hz"))            return MorphSpace::Log;  // LFO free rate
        if (id == "filterResonance")        return MorphSpace::Log;  // a Q
        if (id == "baseN")                  return MorphSpace::Log;  // see the note above
        if (id == "rhythmDivX")             return MorphSpace::Log;  // the BASE of x^n

        return MorphSpace::Linear;
    }

    // Average `v` under `w` in the given space. Weights need not be normalised.
    // Log space guards against a non-positive value: every parameter routed here has a
    // strictly positive minimum (0.001 s, 0.01 Hz, Q 0.5, N 1, x 2), so the guard only
    // ever fires on corrupt input, and it fires towards "very small" rather than NaN.
    inline double blendScalar (const double* v, const double* w, int n, MorphSpace space)
    {
        double sw = 0.0;
        for (int i = 0; i < n; ++i) sw += w[i];
        if (sw <= 1.0e-12) return n > 0 ? v[0] : 0.0;

        if (space == MorphSpace::Log)
        {
            double s = 0.0;
            for (int i = 0; i < n; ++i)
                s += w[i] * std::log (std::max (1.0e-9, v[i]));
            return std::exp (s / sw);
        }
        double s = 0.0;
        for (int i = 0; i < n; ++i) s += w[i] * v[i];
        return s / sw;
    }

    //==========================================================================
    // 3. NEAREST CORNER, WITH HYSTERESIS.
    //
    // An engine choice, a phase mode, a prime mode, manual amps, morph on/off and the
    // realtime-spectrum arming cannot be averaged: there is no state half-way between
    // "harmonic engine" and "custom scale", and a parameter parked between two choice
    // indices is simply the wrong choice. The corner with the highest weight wins.
    //
    // ONE winner serves EVERY discrete setting, not one vote per parameter. Two
    // discrete settings that disagreed about which corner they came from would produce
    // a state no preset has - the classic being "Custom Engine ON" from one corner
    // over a scale loaded from another.
    //
    // Hysteresis: the challenger must beat the incumbent by `margin`, not merely tie
    // it. Without it a point resting on a boundary (where two weights are equal to
    // within float noise, and automation only has to jitter by 1e-7) flips the engine,
    // the phase mode and the whole custom scale back and forth every 16 ms - a
    // machine-gun of spectrum republishes and, on a boundary between engines, an
    // audible flutter. The margin is a WEIGHT difference, so it means the same thing
    // however many corners are assigned.
    //
    // Returns `current` unchanged when nothing has any weight (nothing assigned).
    inline int nearestCorner (const double w[kCorners], int current, double margin)
    {
        int best = -1;
        double bw = -1.0;
        for (int i = 0; i < kCorners; ++i)
            if (w[i] > bw) { bw = w[i]; best = i; }

        if (best < 0 || bw <= 0.0)
            return current;
        // The incumbent has been un-assigned (or there never was one): adopt the
        // leader immediately - holding on to a corner that no longer holds a preset
        // would leave the discrete state pointing at nothing.
        if (current < 0 || current >= kCorners || w[current] <= 0.0)
            return best;
        return (bw > w[current] + margin) ? best : current;
    }

    // The default margin. 0.08 of a unit weight: on a two-corner line that is a dead
    // band of 8% of the travel around the middle, wide enough that neither a mouse
    // tremor nor an automation curve's own quantisation can rattle it, narrow enough
    // that the flip still lands where a listener would put "the middle".
    inline constexpr double kHysteresis = 0.08;

    //==========================================================================
    // 4. MIX GAINS (the playground's second mode).
    //
    // In MIX mode the four corner presets sound SIMULTANEOUSLY - four independent
    // engines summed in audio - and the point is a four-way volume balance. The
    // bilinear weights become per-corner GAINS under the EQUAL-POWER law:
    //
    //     gain[i] = sqrt(w[i])
    //
    // Why sqrt and not the weight itself: the corners are four INDEPENDENT signals
    // (different engines, different spectra, uncorrelated phases), so what the ear
    // hears as "how much of it" is its POWER, gain^2. The weights sum to 1, so
    // sum(gain^2) = 1 everywhere: constant total power across the whole pad. A
    // linear gain (gain = w) would instead sag to half power in the middle of a
    // two-corner edge (0.5^2 + 0.5^2 = 0.5, i.e. -3 dB) - the classic crossfade
    // hole. The law is still exact at a vertex: w = 1 there, sqrt(1) = 1, so only
    // that corner sounds and at full level.
    //
    // Weights <= 0 (unassigned corners, or the far corners at a vertex) give gain 0
    // exactly: a corner with no weight contributes silence, not a hiss floor.
    inline void mixGains (const double w[kCorners], double gOut[kCorners])
    {
        for (int i = 0; i < kCorners; ++i)
            gOut[i] = w[i] > 0.0 ? std::sqrt (w[i]) : 0.0;
    }
}
