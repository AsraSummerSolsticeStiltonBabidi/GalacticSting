#pragma once

//==============================================================================
// ShepardEngine: an endlessly rising Shepard tone, mixed into the synth's output
// from the TUNING panel.
//
// THE CONSTRUCTION (and why it cannot click):
// The components live on the ACTIVE TUNING's period grid: log2-frequencies
//   L_m = log2(anchor) + (g + m) * P,   m integer,  g in [0,1) rising
// where P = log2(periodRatio) - the tuning's own repeat interval (2/1 in octave
// tunings, 3/1 in Bohlen-Pierce), NOT a hard-coded octave - and `anchor` is the
// pitch key 60 really sounds at (frequencyForNote(60): reference-A, octave shift
// and a .tun's own anchor included). Anchoring there keeps every component a
// whole number of tuning periods from the sounding tonic, so the tone is
// consonant with the keyboard in whatever tuning is loaded.
//
// The amplitude window is a raised cosine over a FIXED band of log-frequency
// (it does NOT move with the stack - a window that travels with the components
// is the classic implementation mistake, and it makes the pitch really rise
// instead of only seeming to). Because the window reaches EXACTLY ZERO at both
// edges, the component that drifts off the top exits silent and the one that
// enters at the bottom enters silent: nothing discontinuous ever happens, which
// is the whole trick - there is no "reset" to hide because no reset exists.
//
// THE VISIBLE BOUND (never a silent one): the stack uses at most kMaxComponents
// sine components. With any period of ~187 cents or more that covers the whole
// 20 Hz..20 kHz band; for a smaller period the covered band narrows to
// kMaxComponents periods (centred in the audible range) rather than any
// component being dropped from inside the window. The Tuning panel's tooltip
// states this bound.
//
// SMOOTHNESS, mechanically:
//  - every per-sample value (each component's amplitude and phase increment,
//    the master gain) is linearly interpolated inside each 32-sample chunk;
//  - the period spacing P, the anchor and the rise rate glide with ~150 ms
//    one-poles, so a tuning change or a rate/mode change bends the trajectory
//    instead of stepping it (the grid phase g itself is never jumped);
//  - a slot is only (re)claimed for a grid index whose window value is ~0, so
//    no oscillator is ever hard-reset while audible.
//
// RT contract: fixed-size arrays, no allocation, no locks, no strings. render()
// is called once per block from processBlock; every parameter arrives as a
// plain value the caller read from its own atomics.
//==============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include "SineTable.h"

#include <array>
#include <cmath>

class ShepardEngine
{
public:
    static constexpr int kMaxComponents = 64;      // the stated bound (see header comment)
    static constexpr int kSlots         = kMaxComponents + 2;
    static constexpr int kChunk         = 32;      // samples per parameter update

    // The quantized-mode choices, in beats: the rise of ONE period takes this many
    // host beats. One table, shared by the parameter list, the processor's
    // beats->seconds conversion and the tests, so they can never disagree.
    static constexpr double kDivBeats[] = { 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0,
                                            1.0 / 2.0, 2.0 / 3.0, 1.0, 2.0, 3.0, 4.0, 8.0, 16.0 };
    static constexpr int kNumDivs = (int) (sizeof (kDivBeats) / sizeof (kDivBeats[0]));
    static double beatsForDiv (int idx) noexcept
    { return kDivBeats[juce::jlimit (0, kNumDivs - 1, idx)]; }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        resetRunning();
    }

    // Audio thread. Adds the tone into every channel of `buffer`.
    //   on          - the user's switch (off fades out over ~30 ms, then fully idles)
    //   levelDb     - output level of the tone
    //   riseSeconds - time to rise one PERIOD (already resolved from free ms or
    //                 host beats by the caller, which owns the transport)
    //   periodRatio - the ACTIVE tuning's period (TuningManager::periodRatio())
    //   anchorHz    - the active tuning's key-60 pitch (frequencyForNote (60))
    void render (juce::AudioBuffer<float>& buffer, int numSamples,
                 bool on, float levelDb, double riseSeconds,
                 double periodRatio, double anchorHz) noexcept
    {
        if (numSamples <= 0) return;

        const double gainTarget = on ? (double) juce::Decibels::decibelsToGain (levelDb) : 0.0;
        if (! running)
        {
            if (! on) return;                 // fully off: not even the fade-out tail is left
            startRunning (periodRatio, anchorHz, riseSeconds);
        }

        // Guard the inputs once per block. The clamps keep a degenerate tuning
        // (period ~1) or a wild anchor from producing NaNs; they never fire for
        // anything TuningManager::periodRatio() actually returns.
        const double pTarget = std::log2 (juce::jlimit (1.0001, 16.0,
                                   std::isfinite (periodRatio) ? periodRatio : 2.0));
        const double aTarget = std::log2 (juce::jlimit (1.0, sr * 0.45,
                                   (anchorHz > 0.0 && std::isfinite (anchorHz)) ? anchorHz : 261.6255653));
        const double vTarget = 1.0 / juce::jlimit (0.05, 600.0,
                                   std::isfinite (riseSeconds) ? riseSeconds : 8.0); // periods per second

        const int numCh = buffer.getNumChannels();
        int done = 0;
        while (done < numSamples)
        {
            const int n = juce::jmin (kChunk, numSamples - done);
            const double dt = (double) n / sr;

            // ~150 ms glides for spacing/anchor/rate, ~30 ms for the on/off+level gain.
            const double aSlow = 1.0 - std::exp (-dt / 0.15);
            const double aFast = 1.0 - std::exp (-dt / 0.03);
            smP += aSlow * (pTarget - smP);
            smA += aSlow * (aTarget - smA);
            smV += aSlow * (vTarget - smV);
            const double gainPrev = smGain;
            smGain += aFast * (gainTarget - smGain);

            // The grid phase rises; whole steps are renumbered away in multiples of
            // kSlots so slot hashing is untouched (see the renumber comment below).
            g += smV * dt;
            if (g >= 4096.0)
            {
                const long long d = (long long) kSlots * (long long) (g / (double) kSlots);
                g -= (double) d;
                for (auto& s : slots) if (s.active) s.m += d;
            }

            // FIXED window in log2-frequency: the audible band, narrowed (still centred)
            // when the period is so small that kMaxComponents cannot span it. Lo/Hi move
            // only when the sample rate or the period does - never with the stack.
            const double lTop = std::log2 (juce::jmin (20000.0, sr * 0.45));
            const double lBot = std::log2 (20.0);
            const double span = juce::jmin (lTop - lBot, (double) kMaxComponents * smP);
            const double lc   = 0.5 * (lTop + lBot);
            const double lo   = lc - 0.5 * span;

            // Which grid indices sit inside the window this chunk.
            const long long mLo = (long long) std::ceil  ((lo        - smA) / smP - g);
            const long long mHi = (long long) std::floor ((lo + span - smA) / smP - g);

            // Deactivate BEFORE claiming: with the window exactly kSlots wide, a slot
            // whose component just left one edge can hash to the same cell as the one
            // entering the other edge, and claiming first would orphan a live slot.
            for (auto& s : slots)
                if (s.active && (s.m < mLo || s.m > mHi))
                    s.active = false;

            // Normalisation: keep the SUM of squared window values ~constant so the
            // tone's loudness does not depend on how many components the period needs.
            // Computed from the end-of-chunk window values, then glided.
            double sumSq = 0.0;

            struct Line { double a0, a1, p, inc0, inc1; };
            Line lines[kSlots];
            int  nLines = 0;

            const double gEnd = g;
            for (long long m = mLo; m <= mHi && nLines < kSlots; ++m)
            {
                auto& s = slots[(size_t) (((m % kSlots) + kSlots) % kSlots)];
                const double L = smA + (gEnd + (double) m) * smP;
                const double u = juce::jlimit (0.0, 1.0, (L - lo) / juce::jmax (1.0e-9, span));
                const double w = 0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * u));
                const double f = std::exp2 (L);
                const double inc = juce::jlimit (0.0, 0.5, f / sr);
                if (! s.active || s.m != m)
                {
                    // Claimed at the window edge, where w ~ 0: the re-entering
                    // component starts SILENT - this is the no-reset guarantee.
                    s.active = true; s.m = m; s.phase = 0.0; s.amp = 0.0; s.inc = inc;
                }
                sumSq += w * w;
                lines[nLines++] = { s.amp, w, s.phase, s.inc, inc };
                s.amp = w; s.inc = inc;   // phase written back after the sample loop
            }
            const double normTarget = 1.0 / std::sqrt (juce::jmax (0.25, sumSq));
            smNorm += aSlow * (normTarget - smNorm);

            // Render the chunk: every per-sample value is a linear ramp.
            float chunk[kChunk];
            for (int i = 0; i < n; ++i) chunk[i] = 0.0f;
            const auto& sine = spectrum::sineTable();
            for (int k = 0; k < nLines; ++k)
            {
                auto& ln = lines[k];
                double ph = ln.p;
                for (int i = 0; i < n; ++i)
                {
                    const double t   = (double) (i + 1) / (double) n;
                    const double amp = ln.a0 + (ln.a1 - ln.a0) * t;
                    ph += ln.inc0 + (ln.inc1 - ln.inc0) * t;
                    ph -= std::floor (ph);
                    chunk[i] += (float) (amp * sine.lookup (ph));
                }
                ln.p = ph;
            }
            // Write the advanced phases back to their slots (same order as built).
            {
                int k = 0;
                for (long long m = mLo; m <= mHi && k < nLines; ++m, ++k)
                    slots[(size_t) (((m % kSlots) + kSlots) % kSlots)].phase = lines[k].p;
            }

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) (i + 1) / (double) n;
                chunk[i] = (float) ((double) chunk[i] * smNorm
                                    * (gainPrev + (smGain - gainPrev) * t));
            }
            for (int ch = 0; ch < numCh; ++ch)
            {
                float* out = buffer.getWritePointer (ch) + done;
                for (int i = 0; i < n; ++i) out[i] += chunk[i];
            }
            done += n;
        }

        // Fully faded out and switched off: return to cold idle so the next switch-on
        // starts a fresh, clean fade-in instead of resuming a stale stack.
        if (! on && smGain < 1.0e-5)
            resetRunning();
    }

    //== Test probe (single-threaded verifiers only) ===========================
    // The live components: grid index, frequency and WINDOW amplitude (0..1, before
    // normalisation and level). The window value is the number the silent-wrap rule
    // is stated in: a component may only appear or disappear with ~0 here.
    int probe (long long* ms, double* freqs, double* amps, int maxN) const noexcept
    {
        int n = 0;
        for (const auto& s : slots)
            if (s.active && n < maxN)
            {
                ms[n]    = s.m;
                freqs[n] = std::exp2 (smA + (g + (double) s.m) * smP);
                amps[n]  = s.amp;
                ++n;
            }
        return n;
    }
    bool   isRunning()       const noexcept { return running; }
    double currentSpacing()  const noexcept { return std::exp2 (smP); } // ratio between neighbours
    // The scale a probed window amplitude is rendered at (normalisation x on/off+level
    // gain): output sample = sum of amp_i * scale * sin(...). The click check derives
    // the largest per-sample step a SMOOTH render of these components can produce and
    // holds the measured waveform to it - a bound computed outside the engine would
    // drift the first time the normalisation rule moved.
    double probeOutScale()   const noexcept { return smNorm * smGain; }

private:
    void resetRunning() noexcept
    {
        running = false;
        g = 0.0; smGain = 0.0; smNorm = 1.0;
        for (auto& s : slots) s = {};
    }
    void startRunning (double periodRatio, double anchorHz, double riseSeconds) noexcept
    {
        // Adopt the targets INSTANTLY on a cold start: the glides exist to bend a
        // RUNNING tone, and a cold tone gliding up from stale values would be an
        // audible pitch swoop out of nothing. The gain still starts at 0 (fade-in).
        running = true;
        g      = 0.0;
        smP    = std::log2 (juce::jlimit (1.0001, 16.0, std::isfinite (periodRatio) ? periodRatio : 2.0));
        smA    = std::log2 (juce::jlimit (1.0, sr * 0.45,
                             (anchorHz > 0.0 && std::isfinite (anchorHz)) ? anchorHz : 261.6255653));
        smV    = 1.0 / juce::jlimit (0.05, 600.0, std::isfinite (riseSeconds) ? riseSeconds : 8.0);
        smGain = 0.0;
        smNorm = 1.0;
        for (auto& s : slots) s = {};
    }

    struct Slot
    {
        bool      active = false;
        long long m      = 0;     // grid index (component sits at anchor * period^(g+m))
        double    phase  = 0.0;   // oscillator phase [0,1)
        double    amp    = 0.0;   // window value at the last chunk boundary
        double    inc    = 0.0;   // phase increment at the last chunk boundary
    };

    double sr = 44100.0;
    bool   running = false;
    double g = 0.0;              // rising grid phase (in periods)
    double smP = 1.0;            // smoothed log2 (period)
    double smA = 8.0;            // smoothed log2 (anchor Hz)
    double smV = 0.125;          // smoothed rise rate (periods / second)
    double smGain = 0.0;         // on/off + level, one-pole (~30 ms)
    double smNorm = 1.0;         // loudness normalisation, one-pole
    std::array<Slot, kSlots> slots {};
};
