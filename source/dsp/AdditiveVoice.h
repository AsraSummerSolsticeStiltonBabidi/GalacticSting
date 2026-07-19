#pragma once

//==============================================================================
// Additive MPE voice: renders the shared spectrum template as a bank of sine
// partials.
//
// Click-free design:
//   - Note-on from an IDLE voice: scatter phases, reset the envelope to 0, snap
//     the partial amplitudes to the template (crisp attack).
//   - Note-on that REUSES an active voice (voice stealing): MPESynthesiser calls
//     noteStarted() directly without a noteStopped(false), so we must NOT reset
//     here. We keep phases and envelope continuous, so the steal is click-free.
//   - Template swap while a note is held (the user edits a spectrum parameter):
//     partial FREQUENCIES snap (a frequency jump with continuous phase does not
//     click) but partial AMPLITUDES slew toward their new values over a few ms, so
//     the spectrum morphs smoothly instead of stepping (no zipper). Partials that
//     appear fade in from 0; partials that disappear fade out to 0.
//
// Fidelity: the ONLY partials skipped are those above Nyquist at the current pitch
// (they would alias). No audible partial is dropped here.
//==============================================================================

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "Spectrum.h"
#include "SpectrumEngine.h"
#include "SineTable.h"
#include "TuningManager.h"

#include <array>
#include <cmath>

class AdditiveVoice : public juce::MPESynthesiserVoice
{
public:
    AdditiveVoice (juce::AudioProcessorValueTreeState& state, const spectrum::SpectrumEngine& eng,
                   const TuningManager& tuningManager)
        : apvts (state), engine (eng), tuning (tuningManager), sine (spectrum::sineTable())
    {
        attackParam  = apvts.getRawParameterValue ("ampAttack");
        decayParam   = apvts.getRawParameterValue ("ampDecay");
        sustainParam = apvts.getRawParameterValue ("ampSustain");
        releaseParam = apvts.getRawParameterValue ("ampRelease");
        phaseModeParam = apvts.getRawParameterValue ("phaseMode");

        fCutoffParam = apvts.getRawParameterValue ("filterCutoff");
        fResParam    = apvts.getRawParameterValue ("filterResonance");
        fEnvAmtParam = apvts.getRawParameterValue ("filterEnvAmount");
        fAttackParam = apvts.getRawParameterValue ("filterAttack");
        fDecayParam  = apvts.getRawParameterValue ("filterDecay");
        fSustainParam= apvts.getRawParameterValue ("filterSustain");
        fReleaseParam= apvts.getRawParameterValue ("filterRelease");

        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    //==========================================================================
    void noteStarted() override
    {
        // An MTS-ESP master may leave this key unmapped -> it must not sound.
        if (tuning.shouldFilterNote ((int) currentlyPlayingNote.initialNote))
        {
            clearCurrentNote();
            return;
        }

        // Deterministic note-on (same note twice = identical). Two phase modes:
        //   Aligned  - every partial starts at its peak (cosine), so they form a coherent
        //              pulse at the difference-tone rate -> a defined tone, not noise. The
        //              summed peak is then exactly sum|amp|, which the render normalises
        //              against so it never clips (dense patches just get quieter).
        //   Scattered- pseudo-random phases decorrelate the partials into a soft/noisy bed.
        const bool aligned = phaseModeParam->load() < 0.5f;
        for (int i = 0; i < spectrum::kMaxPartials; ++i)
        {
            if (aligned)
            {
                phase[(size_t) i] = 0.25; // sin(2*pi*0.25) = 1 -> all partials at their peak
            }
            else
            {
                const double h = std::sin ((double) i * 12.9898) * 43758.5453;
                phase[(size_t) i] = h - std::floor (h);
            }
        }

        velocityLevel = 0.10f + 0.90f * currentlyPlayingNote.noteOnVelocity.asUnsignedFloat();
        pressure      = currentlyPlayingNote.pressure.asUnsignedFloat();

        fastKill     = false;
        fastKillGain = 1.0f;
        snapAmps     = true;
        snapNorm     = true;

        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        updateAdsrParameters();
        adsr.setSampleRate (sr);
        adsr.reset();     // start from silence -> click-free onset, deterministic
        adsr.noteOn();

        updateFilterAdsrParameters();
        filterAdsr.setSampleRate (sr);
        filterAdsr.reset();
        filterAdsr.noteOn();
        filter.reset();
        filterEnvValue = 0.0f;
    }

    void noteStopped (bool allowTailOff) override
    {
        if (allowTailOff) { adsr.noteOff(); filterAdsr.noteOff(); }
        else              beginFastKill();
    }

    void notePressureChanged() override  { pressure = currentlyPlayingNote.pressure.asUnsignedFloat(); }
    void notePitchbendChanged() override {}
    void noteTimbreChanged() override    {}
    void noteKeyStateChanged() override  {}

    void setCurrentSampleRate (double newRate) override
    {
        if (currentSampleRate != newRate)
        {
            currentSampleRate = newRate;
            const double sr = newRate > 0.0 ? newRate : 44100.0;
            adsr.setSampleRate (sr);
            filterAdsr.setSampleRate (sr);
            filter.prepare ({ sr, 8192, 1 });
        }
    }

    //==========================================================================
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr.isActive() && ! fastKill)
            return;

        updateAdsrParameters();
        updateFilterAdsrParameters();

        // Snapshot the live template: FREQUENCIES snap, AMPLITUDES are the slew target.
        // Pitch comes from the active tuning (12-TET / MTS-ESP / Scala) for the base
        // note, with any MPE per-note pitch bend riding on top.
        int n = 0;
        const auto* tmpl = engine.getLive();
        const double baseFreq = tuning.frequencyForNote ((int) currentlyPlayingNote.initialNote);
        const double f0 = baseFreq * std::pow (2.0, (double) currentlyPlayingNote.totalPitchbendInSemitones / 12.0);

        if (tmpl != nullptr && currentSampleRate > 0.0 && f0 > 0.0)
        {
            const double maxRatio = (0.5 * currentSampleRate) / f0; // Nyquist / f0
            const auto&  ps       = tmpl->partials;
            const int    count    = std::min ((int) ps.size(), spectrum::kMaxPartials);

            for (int i = 0; i < count; ++i)
            {
                if (ps[(size_t) i].ratio > maxRatio) break; // sorted ascending -> rest are higher
                inc[(size_t) i]    = (double) ps[(size_t) i].ratio * f0 / currentSampleRate;
                tgtAmp[(size_t) i] = ps[(size_t) i].amp;
                n = i + 1;
            }

            engine.reportRendered (n); // audible partial count for the meter
        }

        if (snapAmps)
        {
            for (int i = 0; i < n; ++i)               curAmp[(size_t) i] = tgtAmp[(size_t) i];
            for (int i = n; i < activeCount; ++i)     curAmp[(size_t) i] = 0.0;
            activeCount = n;
            snapAmps = false;
        }

        // Per-voice normalisation over the ACTUALLY-RENDERED (sub-Nyquist) partials, so
        // every note/patch has a consistent level no matter how many partials survive the
        // Nyquist cull, AND so the summed peak never clips.
        double sumAmp = 0.0, sumSq = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double a = tgtAmp[(size_t) i]; // amps are non-negative
            sumAmp += a;
            sumSq  += a * a;
        }
        const bool aligned = phaseModeParam->load() < 0.5f;
        // Aligned: the pulse peak is sum|amp|, so normalise against it -> guaranteed no clip
        // (dense patches simply get quieter). Scattered: RMS-normalise for even loudness.
        const float tgtNorm = aligned ? (sumAmp > 1.0e-9  ? (float) (0.5  / sumAmp)            : 0.0f)
                                      : (sumSq  > 1.0e-12 ? (float) (0.25 / std::sqrt (sumSq)) : 0.0f);
        if (snapNorm) { normGain = tgtNorm; snapNorm = false; }

        const double sr   = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        const int    span = std::max (n, activeCount);
        const double slew     = 1.0 - std::exp (-1.0 / (0.007 * sr));
        const double normSlew = 1.0 - std::exp (-1.0 / (0.020 * sr));

        // Filter cutoff = base * 2^(envAmount * filterEnv). Updated per block; the filter
        // envelope itself advances every sample below.
        {
            const double octs = (double) fEnvAmtParam->load() * (double) filterEnvValue;
            const double co = (double) fCutoffParam->load() * std::pow (2.0, octs);
            filter.setCutoffFrequency ((float) juce::jlimit (20.0, 0.49 * sr, co));
            filter.setResonance (juce::jlimit (0.1f, 40.0f, fResParam->load()));
        }

        const int    numChannels = output.getNumChannels();

        while (--numSamples >= 0)
        {
            normGain += (float) ((tgtNorm - normGain) * normSlew);

            double acc = 0.0;
            for (int i = 0; i < span; ++i)
            {
                const double target = (i < n) ? tgtAmp[(size_t) i] : 0.0;
                curAmp[(size_t) i] += (target - curAmp[(size_t) i]) * slew;

                double p = phase[(size_t) i] + inc[(size_t) i];
                if (p >= 1.0) p -= 1.0;
                phase[(size_t) i] = p;

                acc += curAmp[(size_t) i] * (double) sine.lookup (p);
            }

            const float env    = adsr.getNextSample();
            const float killG  = fastKill ? fastKillGain : 1.0f;
            const float gain   = env * killG * velocityLevel * (0.35f + 0.65f * pressure) * normGain;
            const float s      = filter.processSample (0, (float) acc * gain);
            filterEnvValue     = filterAdsr.getNextSample();

            for (int ch = 0; ch < numChannels; ++ch)
                output.addSample (ch, startSample, s);

            ++startSample;

            if (fastKill)
            {
                fastKillGain -= fastKillDelta;
                if (fastKillGain <= 0.0f) { finishNote(); break; }
            }
            else if (! adsr.isActive())
            {
                finishNote();
                break;
            }
        }

        // Keep fading-out tails alive until they are inaudible.
        int newActive = n;
        for (int i = n; i < span; ++i)
            if (curAmp[(size_t) i] > 1.0e-5)
                newActive = i + 1;
        activeCount = newActive;
    }

private:
    //==========================================================================
    void updateAdsrParameters()
    {
        juce::ADSR::Parameters p;
        p.attack  = attackParam->load();
        p.decay   = decayParam->load();
        p.sustain = sustainParam->load();
        p.release = releaseParam->load();
        adsr.setParameters (p);
    }

    void updateFilterAdsrParameters()
    {
        juce::ADSR::Parameters p;
        p.attack  = fAttackParam->load();
        p.decay   = fDecayParam->load();
        p.sustain = fSustainParam->load();
        p.release = fReleaseParam->load();
        filterAdsr.setParameters (p);
    }

    void beginFastKill()
    {
        constexpr double fadeSeconds = 0.005; // 5 ms
        const auto sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        fastKill      = true;
        fastKillDelta = (float) (1.0 / (fadeSeconds * sr));
    }

    void finishNote()
    {
        adsr.reset();
        filterAdsr.reset();
        fastKill     = false;
        fastKillGain = 1.0f;
        clearCurrentNote();
    }

    juce::AudioProcessorValueTreeState& apvts;
    const spectrum::SpectrumEngine&     engine;
    const TuningManager&                tuning;
    const spectrum::SineTable&          sine;

    std::atomic<float>* attackParam    = nullptr;
    std::atomic<float>* decayParam     = nullptr;
    std::atomic<float>* sustainParam   = nullptr;
    std::atomic<float>* releaseParam   = nullptr;
    std::atomic<float>* phaseModeParam = nullptr;
    std::atomic<float>* fCutoffParam   = nullptr;
    std::atomic<float>* fResParam      = nullptr;
    std::atomic<float>* fEnvAmtParam   = nullptr;
    std::atomic<float>* fAttackParam   = nullptr;
    std::atomic<float>* fDecayParam    = nullptr;
    std::atomic<float>* fSustainParam  = nullptr;
    std::atomic<float>* fReleaseParam  = nullptr;

    juce::ADSR adsr, filterAdsr;
    juce::dsp::StateVariableTPTFilter<float> filter;
    float filterEnvValue = 0.0f;

    std::array<double, spectrum::kMaxPartials> phase  {};
    std::array<double, spectrum::kMaxPartials> inc    {}; // snapped frequency (cycles/sample)
    std::array<double, spectrum::kMaxPartials> tgtAmp {}; // target amplitude (from template)
    std::array<double, spectrum::kMaxPartials> curAmp {}; // slewed amplitude actually rendered

    int   activeCount = 0;     // slots to process (includes fading-out tails)
    bool  snapAmps    = true;  // snap amps to target this block (cold note start)
    float normGain    = 1.0f;  // per-voice RMS-normalisation gain (smoothed)
    bool  snapNorm    = true;  // snap the normalisation gain on note start

    float velocityLevel = 0.0f;
    float pressure = 0.0f;

    bool  fastKill = false;
    float fastKillGain = 1.0f;
    float fastKillDelta = 0.0f;
};
