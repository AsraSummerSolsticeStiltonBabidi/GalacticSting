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

        static double shape (double t, float c) noexcept
        {
            t = std::min (1.0, std::max (0.0, t));
            const double k = std::pow (2.0, -3.0 * (double) std::min (1.0f, std::max (-1.0f, c)));
            return std::pow (t, k);
        }

    private:
        float advance (double dt) noexcept
        {
            switch (stage)
            {
                case Attack:
                    pos += dt / (double) a;
                    if (pos >= 1.0) { stage = Decay; pos = 0.0; value = 1.0; }
                    else            value = from + (1.0 - from) * shape (pos, cA);
                    break;
                case Decay:
                    pos += dt / (double) d;
                    if (pos >= 1.0) { stage = Sustain; pos = 0.0; value = s; }
                    else            value = 1.0 + ((double) s - 1.0) * shape (pos, cD);
                    break;
                case Sustain:
                    value = s;
                    break;
                case Release:
                    pos += dt / (double) r;
                    if (pos >= 1.0) { stage = Idle; pos = 0.0; value = 0.0; }
                    else            value = from * (1.0 - shape (pos, cR));
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
    };
}
