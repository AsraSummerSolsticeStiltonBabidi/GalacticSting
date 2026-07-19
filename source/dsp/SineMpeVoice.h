#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>

//==============================================================================
/**
    Milestone 1 voice: a single sine oscillator with an amplitude ADSR, driven by
    an MPE note. Seed of the future additive engine — the single sine later becomes
    a bank of partials compiled from the spectrum rules, but the MPE plumbing and
    the click-free note handling below stay the same.

    Click-free design:
      - The ADSR is reset to zero on note-on, so a note (or a retriggered voice)
        always starts from silence: the phase reset can never cause a discontinuity.
      - A hard stop (voice stealing, allowTailOff == false) does a very short fade
        instead of an instant cut, which removes voice-steal clicks.

    MPE mapping (v1 defaults):
      - pitch bend -> frequency  (via MPENote::getFrequencyInHertz)
      - pressure   -> level
      - velocity   -> level
*/
class SineMpeVoice : public juce::MPESynthesiserVoice
{
public:
    explicit SineMpeVoice (juce::AudioProcessorValueTreeState& state)
        : apvts (state)
    {
        attackParam  = apvts.getRawParameterValue ("ampAttack");
        decayParam   = apvts.getRawParameterValue ("ampDecay");
        sustainParam = apvts.getRawParameterValue ("ampSustain");
        releaseParam = apvts.getRawParameterValue ("ampRelease");
    }

    //==========================================================================
    void noteStarted() override
    {
        phase = 0.0;
        updateFrequency();

        velocityLevel = 0.10f + 0.90f * currentlyPlayingNote.noteOnVelocity.asUnsignedFloat();
        pressure      = currentlyPlayingNote.pressure.asUnsignedFloat();

        fastKill     = false;
        fastKillGain = 1.0f;

        updateAdsrParameters();
        adsr.setSampleRate (currentSampleRate > 0.0 ? currentSampleRate : 44100.0);
        adsr.reset();     // start attack from true silence -> click-free (even on reuse)
        adsr.noteOn();
    }

    void noteStopped (bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            // Voice being stolen: fade out fast instead of cutting (removes clicks).
            beginFastKill();
        }
    }

    void notePressureChanged() override  { pressure = currentlyPlayingNote.pressure.asUnsignedFloat(); }
    void notePitchbendChanged() override { updateFrequency(); }
    void noteTimbreChanged() override    {}
    void noteKeyStateChanged() override  {}

    void setCurrentSampleRate (double newRate) override
    {
        if (currentSampleRate != newRate)
        {
            currentSampleRate = newRate;
            adsr.setSampleRate (newRate > 0.0 ? newRate : 44100.0);
            updateFrequency();
        }
    }

    //==========================================================================
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr.isActive() && ! fastKill)
            return;

        updateAdsrParameters();

        const int numChannels = output.getNumChannels();

        while (--numSamples >= 0)
        {
            const auto env     = adsr.getNextSample();
            const auto killG   = fastKill ? fastKillGain : 1.0f;
            const auto gain    = velocityLevel * env * killG * (0.35f + 0.65f * pressure);
            const auto sample  = (float) std::sin (phase) * gain;

            for (int ch = 0; ch < numChannels; ++ch)
                output.addSample (ch, startSample, sample);

            phase += phaseIncrement;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            ++startSample;

            if (fastKill)
            {
                fastKillGain -= fastKillDelta;
                if (fastKillGain <= 0.0f)
                {
                    finishNote();
                    break;
                }
            }
            else if (! adsr.isActive())
            {
                finishNote();
                break;
            }
        }
    }

private:
    //==========================================================================
    void updateFrequency()
    {
        if (currentSampleRate > 0.0)
        {
            const auto freq = currentlyPlayingNote.getFrequencyInHertz();
            phaseIncrement = juce::MathConstants<double>::twoPi * freq / currentSampleRate;
        }
    }

    void updateAdsrParameters()
    {
        juce::ADSR::Parameters p;
        p.attack  = attackParam->load();
        p.decay   = decayParam->load();
        p.sustain = sustainParam->load();
        p.release = releaseParam->load();
        adsr.setParameters (p);
    }

    void beginFastKill()
    {
        constexpr double fadeSeconds = 0.005; // 5 ms
        const auto sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        fastKill     = true;
        fastKillDelta = (float) (1.0 / (fadeSeconds * sr));
        // keep current fastKillGain (usually 1.0) so the fade starts from the live level
    }

    void finishNote()
    {
        adsr.reset();
        fastKill     = false;
        fastKillGain = 1.0f;
        clearCurrentNote();
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float>* attackParam  = nullptr;
    std::atomic<float>* decayParam   = nullptr;
    std::atomic<float>* sustainParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;

    juce::ADSR adsr;
    double phase = 0.0;
    double phaseIncrement = 0.0;
    float  velocityLevel = 0.0f;
    float  pressure = 0.0f;

    bool  fastKill = false;
    float fastKillGain = 1.0f;
    float fastKillDelta = 0.0f;
};
