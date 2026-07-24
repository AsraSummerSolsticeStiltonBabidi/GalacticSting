#pragma once

//==============================================================================
// ModEngine: 6 modulators (3 custom envelopes + 3 LFOs), each routable to ANY
// number of parameters at once. Stepped ONCE PER AUDIO BLOCK on the audio thread
// and written into the parameters' RAW VALUE ATOMICS (the values the DSP reads),
// NOT through setValueNotifyingHost:
//   - smooth (block-rate, not 30 Hz timer steps), no zipper at UI level
//   - no host-automation spam, no undo pollution, RT-safe (atomic stores only)
//   - the parameter itself keeps the user's BASE setting; modulation rides on it
//     (base + value * range, clamped to the parameter's range) and the base is
//     restored when a modulator is switched off or unmapped.
//
// Envelopes: ADSR, retriggered by EVERY note-on (so each new key restarts the
// movement), released when no note is held.
// LFOs: sine / triangle / saw up / saw down / square / custom drawn wave; rate
// modes: BPM-divided, fundamental-divided (default; follows the lowest held
// note's frequency), or free Hz; phase modes: restart at each note-on (default)
// or locked to the host transport.
//==============================================================================

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>

class ModEngine
{
public:
    static constexpr int kEnvs = 3, kLfos = 3, kMods = kEnvs + kLfos; // 0..2 env, 3..5 lfo
    static constexpr int kMaxTargets = 8;    // params one modulator can drive
    static constexpr int kWaveN = 256;       // custom-wave table resolution

    ModEngine (juce::AudioProcessorValueTreeState& s) : apvts (s) {}

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (auto& e : env) { e.adsr.setSampleRate (sr); e.adsr.reset(); }
        noteSeen = 0;
    }

    // Message thread: routing management. Targets are cached RangedAudioParameter
    // pointers + their raw atomics (both stable for the processor's lifetime).
    void addTarget (int mod, const juce::String& pid)
    {
        if (! juce::isPositiveAndBelow (mod, kMods) || pid.startsWith ("mod")) return;
        auto& m = mods[(size_t) mod];
        for (int i = 0; i < m.numTargets.load(); ++i)
            if (m.tgtId[(size_t) i] == pid) return;
        const int n = m.numTargets.load();
        if (n >= kMaxTargets) return;
        auto* p   = apvts.getParameter (pid);
        auto* raw = apvts.getRawParameterValue (pid);
        if (p == nullptr || raw == nullptr) return;
        m.tgtId[(size_t) n]    = pid;
        m.tgtParam[(size_t) n] = p;
        m.tgtRaw[(size_t) n]   = raw;
        m.numTargets.store (n + 1);
    }
    void removeTarget (int mod, const juce::String& pid)
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return;
        auto& m = mods[(size_t) mod];
        const int n = m.numTargets.load();
        for (int i = 0; i < n; ++i)
            if (m.tgtId[(size_t) i] == pid)
            {
                restoreTarget (m, i);
                m.numTargets.store (n - 1);      // audio thread stops using the tail first
                m.tgtId[(size_t) i]    = m.tgtId[(size_t) (n - 1)];
                m.tgtParam[(size_t) i] = m.tgtParam[(size_t) (n - 1)];
                m.tgtRaw[(size_t) i]   = m.tgtRaw[(size_t) (n - 1)];
                return;
            }
    }
    void unmapAll (int mod)
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return;
        auto& m = mods[(size_t) mod];
        for (int i = 0; i < m.numTargets.load(); ++i) restoreTarget (m, i);
        m.numTargets.store (0);
    }
    int  numTargets (int mod) const { return juce::isPositiveAndBelow (mod, kMods) ? mods[(size_t) mod].numTargets.load() : 0; }
    juce::String targetId (int mod, int i) const
    {
        return juce::isPositiveAndBelow (mod, kMods) && juce::isPositiveAndBelow (i, mods[(size_t) mod].numTargets.load())
                   ? mods[(size_t) mod].tgtId[(size_t) i] : juce::String();
    }
    bool isMappedTo (const juce::String& pid, int mod) const
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return false;
        const auto& m = mods[(size_t) mod];
        for (int i = 0; i < m.numTargets.load(); ++i)
            if (m.tgtId[(size_t) i] == pid) return true;
        return false;
    }

    // Custom LFO wave table (message thread writes; audio reads - float tearing is
    // transient and inaudible).
    void setCustomWave (int lfo, const float* table256)
    {
        if (! juce::isPositiveAndBelow (lfo, kLfos)) return;
        std::copy (table256, table256 + kWaveN, customWave[(size_t) lfo].begin());
    }
    const float* getCustomWave (int lfo) const
    { return juce::isPositiveAndBelow (lfo, kLfos) ? customWave[(size_t) lfo].data() : nullptr; }

    // Audio thread: called once per block from processBlock BEFORE the synth renders.
    //   noteOnsSoFar - running note-on counter (retrigger detection)
    //   anyHeld      - is any note currently held
    //   fundamental  - lowest held note's Hz (0 if none)
    //   bpm/ppq      - host transport (bpm <= 0 = unknown; ppq < 0 = not playing)
    void processBlock (int numSamples, juce::uint32 noteOnsSoFar, bool anyHeld,
                       double fundamental, double bpm, double ppq)
    {
        const bool newNote = noteOnsSoFar != noteSeen;
        noteSeen = noteOnsSoFar;
        const double dt = (double) numSamples / sr;

        for (int i = 0; i < kMods; ++i)
        {
            auto& m = mods[(size_t) i];
            const bool isEnv = i < kEnvs;
            const auto id = [i, isEnv] (const char* suffix)
            { return (isEnv ? "modE" : "modL") + juce::String (isEnv ? i + 1 : i - kEnvs + 1) + suffix; };

            const bool on = apvts.getRawParameterValue (id ("On"))->load() > 0.5f;
            if (! on || m.numTargets.load() == 0)
            {
                if (m.wasActive) { for (int t = 0; t < m.numTargets.load(); ++t) restoreTarget (m, t); m.wasActive = false; }
                continue;
            }
            m.wasActive = true;

            double v = 0.0; // modulator output 0..1
            if (isEnv)
            {
                auto& e = env[(size_t) i];
                juce::ADSR::Parameters ap;
                ap.attack  = apvts.getRawParameterValue (id ("A"))->load();
                ap.decay   = apvts.getRawParameterValue (id ("D"))->load();
                ap.sustain = apvts.getRawParameterValue (id ("S"))->load();
                ap.release = apvts.getRawParameterValue (id ("R"))->load();
                e.adsr.setParameters (ap);
                if (newNote)              e.adsr.noteOn();   // EVERY key restarts the envelope
                else if (! anyHeld && e.held) e.adsr.noteOff();
                e.held = anyHeld;
                // advance block-wise (values in between are not observable anyway)
                for (int k = 0; k < numSamples; ++k) v = (double) e.adsr.getNextSample();
            }
            else
            {
                auto& l = lfo[(size_t) (i - kEnvs)];
                const int  rateMode  = (int) apvts.getRawParameterValue (id ("RateMode"))->load();
                const int  divN      = (int) apvts.getRawParameterValue (id ("Div"))->load();
                const int  phaseMode = (int) apvts.getRawParameterValue (id ("PhaseMode"))->load();
                const double phOff   = (double) apvts.getRawParameterValue (id ("Phase"))->load();
                double rate = 0.0;
                if (rateMode == 0)      rate = bpm > 1.0 ? (bpm / 60.0) / std::exp2 ((double) divN) : 0.0;
                else if (rateMode == 1) rate = fundamental > 0.0 ? fundamental / std::exp2 ((double) divN) : 0.0;
                else                    rate = (double) apvts.getRawParameterValue (id ("Hz"))->load();
                rate = juce::jlimit (0.0, 40.0, rate);

                if (phaseMode == 1 && ppq >= 0.0 && bpm > 1.0 && rateMode == 0)
                    l.phase = std::fmod (ppq / 60.0 * 60.0 / std::exp2 ((double) divN) + phOff, 1.0); // transport-locked (beats)
                else
                {
                    if (newNote && phaseMode == 0) l.phase = phOff; // note-retrigger (default)
                    l.phase = std::fmod (l.phase + rate * dt, 1.0);
                }
                v = evalWave (i - kEnvs, (int) apvts.getRawParameterValue (id ("Wave"))->load(), l.phase);
            }

            const float range = apvts.getRawParameterValue (id ("Range"))->load();
            for (int t = 0; t < m.numTargets.load(); ++t)
            {
                auto* p = m.tgtParam[(size_t) t];
                if (p == nullptr) continue;
                // base = the parameter's OWN setting; modulation rides on top of it.
                const float norm = juce::jlimit (0.0f, 1.0f, p->getValue() + (float) v * range);
                m.tgtRaw[(size_t) t]->store (p->convertFrom0to1 (norm));
            }
        }
    }

private:
    struct Mod
    {
        juce::String               tgtId[kMaxTargets];
        juce::RangedAudioParameter* tgtParam[kMaxTargets] = {};
        std::atomic<float>*         tgtRaw[kMaxTargets]   = {};
        std::atomic<int>            numTargets { 0 };
        bool                        wasActive = false;
    };
    struct Env { juce::ADSR adsr; bool held = false; };
    struct Lfo { double phase = 0.0; };

    void restoreTarget (Mod& m, int i)
    {
        if (m.tgtParam[(size_t) i] != nullptr && m.tgtRaw[(size_t) i] != nullptr)
            m.tgtRaw[(size_t) i]->store (m.tgtParam[(size_t) i]->convertFrom0to1 (m.tgtParam[(size_t) i]->getValue()));
    }

    double evalWave (int lfoIdx, int wave, double ph) const
    {
        switch (wave)
        {
            case 0:  return 0.5 + 0.5 * std::sin (ph * juce::MathConstants<double>::twoPi); // sine
            case 1:  return ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0;                            // triangle
            case 2:  return ph;                                                              // saw up
            case 3:  return 1.0 - ph;                                                        // saw down
            case 4:  return ph < 0.5 ? 1.0 : 0.0;                                            // square
            default: // custom drawn wave (linear interp over the 256-point table)
            {
                const auto& w = customWave[(size_t) lfoIdx];
                const double x = ph * (kWaveN - 1);
                const int i0 = (int) x;
                const double f = x - i0;
                return (1.0 - f) * w[(size_t) i0] + f * w[(size_t) juce::jmin (kWaveN - 1, i0 + 1)];
            }
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    Mod  mods[kMods];
    Env  env[kEnvs];
    Lfo  lfo[kLfos];
    std::array<std::array<float, kWaveN>, kLfos> customWave {};
    double sr = 44100.0;
    juce::uint32 noteSeen = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModEngine)
};
