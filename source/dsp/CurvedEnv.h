#pragma once

//==============================================================================
// CurvedEnv: an ADSR whose attack, decay and release each carry their own SHAPE.
//
//   curve = 0  -> a straight line (what juce::ADSR does)
//   curve > 0  -> convex: falls fast, then a long tail (how a natural decay goes)
//   curve < 0  -> concave: holds, then drops
//
// value = from + (to - from) * t^k , k = 2^(-3*curve)
//
// It is the SAME formula the envelope editor draws with, so the curve on screen is
// exactly the signal - and the same one ModEngine uses for the modulators, so
// every envelope in the plugin behaves identically.
//
// getNextSample() advances one sample (voices); advanceBlock() jumps a whole block
// (the block-rate modulators). No allocation, no locking - safe on the audio thread.
//==============================================================================

#include <algorithm>
#include <cmath>

namespace spectrum
{
    class CurvedEnv
    {
    public:
        void setSampleRate (double newSr) noexcept
        { sr = newSr > 0.0 ? newSr : 44100.0; }

        void setParameters (float attack, float decay, float sustain, float release,
                            float curveA = 0.0f, float curveD = 0.0f, float curveR = 0.0f) noexcept
        {
            a = std::max (1.0e-4f, attack);
            d = std::max (1.0e-4f, decay);
            s = std::min (1.0f, std::max (0.0f, sustain));
            r = std::max (1.0e-4f, release);
            cA = curveA; cD = curveD; cR = curveR;
            // THE EXPONENT IS A PARAMETER, SO IT IS COMPUTED WHERE THE PARAMETERS ARRIVE.
            //
            // k = 2^(-3c) depends on the CURVE and on nothing else - not on t, not on the
            // stage's position, not on the sample - and shape() was recomputing it inside
            // the per-sample call: two std::pow per sample per envelope where one is the
            // whole job. Three envelopes per voice (amp, filter, pitch) times 32 voices
            // makes that a per-block cost, and pow is not cheap: measured on this box at
            // ~40 ns for the pair against ~20 ns for the survivor, so hoisting it gives
            // the attack/decay/release legs back roughly half of what they were spending.
            // (Sustain is unaffected - that branch calls neither.)
            //
            // BIT-IDENTICAL, and asserted rather than asserted-in-prose: std::pow with the
            // same argument returns the same double, so the k stored here is the k the old
            // call would have produced at every sample. The envelope test at the end of
            // RhythmVerify renders A/D/R against a reference built on the OLD per-sample
            // spelling and demands the two agree to the last bit.
            kA = curveExponent (cA); kD = curveExponent (cD); kR = curveExponent (cR);
        }

        // A note-on restarts the attack from ZERO by default: retriggering from
        // wherever the envelope happened to be left a second note with no attack and
        // no decay left to travel.
        // fromCurrent = start the attack from the level the envelope is ALREADY at.
        // Only for a voice STEAL, where the previous note is still sounding: dropping
        // its gain to zero in one sample is a click, and it also threw away the
        // glided velocity/pressure targets and the preserved fast-kill gain that the
        // steal path goes to the trouble of keeping continuous. The Attack branch
        // already interpolates from `from`, so the ramp simply starts higher.
        void noteOn (bool fromCurrent = false) noexcept
        {
            stage = Attack; pos = 0.0;
            from  = fromCurrent ? value : 0.0;
            value = from;
        }
        void noteOff() noexcept { if (stage != Idle) { stage = Release; pos = 0.0; from = value; } }
        void reset() noexcept   { stage = Idle; pos = 0.0; value = 0.0; from = 0.0; }
        bool isActive() const noexcept { return stage != Idle; }
        float getCurrentValue() const noexcept { return (float) value; }

        float getNextSample() noexcept { return advance (1.0 / sr); }
        float advanceBlock (int numSamples) noexcept
        { return advance ((double) std::max (0, numSamples) / sr); }

        // THE CURVE'S EXPONENT, in ONE place. The editor and ModEngine spell the same
        // formula for their own envelopes, and this is the copy the signal is made of.
        static double curveExponent (float c) noexcept
        { return std::pow (2.0, -3.0 * (double) std::min (1.0f, std::max (-1.0f, c))); }

        // The curve, given the exponent that has already been worked out. This is what
        // the render path calls; nothing about the arithmetic moved, only where the
        // invariant half of it happens.
        static double shapeK (double t, double k) noexcept
        {
            t = std::min (1.0, std::max (0.0, t));
            return std::pow (t, k);
        }

        // The old spelling, kept EXACTLY as it was for any caller outside this class -
        // one call, curve in, value out - and now expressed in terms of the two halves
        // above so there is a single definition of the curve rather than two that agree
        // until one of them is edited. Nothing in the plugin calls it (ModEngine.h:651
        // and PluginEditor.h:3485 each carry their own copy for their own reasons); it
        // stays because a public static that a verifier might reach is not something to
        // delete quietly.
        static double shape (double t, float c) noexcept { return shapeK (t, curveExponent (c)); }

    private:
        float advance (double dt) noexcept
        {
            switch (stage)
            {
                case Attack:
                    pos += dt / (double) a;
                    if (pos >= 1.0) { stage = Decay; pos = 0.0; value = 1.0; }
                    else            value = from + (1.0 - from) * shapeK (pos, kA);
                    break;
                case Decay:
                    pos += dt / (double) d;
                    if (pos >= 1.0) { stage = Sustain; pos = 0.0; value = s; }
                    else            value = 1.0 + ((double) s - 1.0) * shapeK (pos, kD);
                    break;
                case Sustain:
                    value = s;
                    break;
                case Release:
                    pos += dt / (double) r;
                    if (pos >= 1.0) { stage = Idle; pos = 0.0; value = 0.0; }
                    else            value = from * (1.0 - shapeK (pos, kR));
                    break;
                case Idle:
                default:
                    value = 0.0;
                    break;
            }
            return (float) std::min (1.0, std::max (0.0, value));
        }

        enum Stage { Idle, Attack, Decay, Sustain, Release };
        Stage  stage = Idle;
        double sr = 44100.0, pos = 0.0, value = 0.0, from = 0.0;
        float  a = 0.01f, d = 0.1f, s = 0.8f, r = 0.3f, cA = 0.0f, cD = 0.0f, cR = 0.0f;
        // 2^(-3 * 0) == 1, so these are the exponents the three curve defaults above ask
        // for. Written out rather than left to setParameters, because an envelope that is
        // advanced before its parameters are pushed must sound like a straight line and
        // not like whatever an uninitialised exponent makes of pow().
        double kA = 1.0, kD = 1.0, kR = 1.0;
    };
}
