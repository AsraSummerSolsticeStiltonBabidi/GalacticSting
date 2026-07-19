#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/AdditiveVoice.h"
#include "dsp/SpectrumEngine.h"
#include "dsp/TuningManager.h"

//==============================================================================
class FractalAdditiveAudioProcessor : public juce::AudioProcessor,
                                      private juce::Timer
{
public:
    FractalAdditiveAudioProcessor();
    ~FractalAdditiveAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    const juce::String getName() const override { return "Galactic Sting"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    //==========================================================================
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // For the editor's partial/CPU meter.
    int  getPartialCount() const noexcept { return spectrumEngine.getRenderedCount(); }
    int  getTemplatePartialCount() const noexcept { return spectrumEngine.getPartialCount(); }
    int  getRawPartialCount() const noexcept { return spectrumEngine.getRawCount(); }
    bool isSpectrumTruncated() const noexcept { return spectrumEngine.isTruncated(); }

    //==========================================================================
    // Microtuning (12-TET / MTS-ESP / Scala). Called from the editor.
    bool loadScalaFile (const juce::File& f, juce::String& errorOut);
    void clearScala();
    juce::String getTuningStatus() const { return tuning.getStatusText(); }

    // Raw tuning/timbre state so the editor can format localised status strings.
    int  getTuningMode()    const noexcept { return (int) apvts.getRawParameterValue ("tuningMode")->load(); }
    bool tuningHasMaster()  const noexcept { return tuning.hasMtsMaster(); }
    bool tuningHasScala()   const noexcept { return tuning.hasScala(); }
    juce::String tuningScaleName() const { return tuning.currentScaleName(); }
    juce::String getTimbreName()   const { return timbreName; }
    int  getTimbreNoteCount() const noexcept { return (int) timbreDegrees.size(); }

    // Degrees per period of the active tuning (for the colour keyboard).
    int getScaleDegrees() const noexcept { return tuning.degreesPerPeriod(); }

    //==========================================================================
    // Scala-as-timbre: a loaded .scl becomes the partial set instead of the
    // harmonic engine. Called from the editor.
    bool loadTimbreFile (const juce::File& f, juce::String& errorOut);
    void clearTimbre();
    bool isTimbreActive() const noexcept { return timbreActive; }
    juce::String getTimbreStatus() const;

    // Recently-used .scl files (persisted across sessions, newest first, max 3).
    juce::StringArray getRecentSclPaths() const { return recentSclPaths; }

    //==========================================================================
    // File presets (.gsting). Called from the editor.
    static juce::File getPresetFolder();
    juce::StringArray getPresetNames();
    juce::String savePreset (const juce::String& name); // returns the stored name, or empty on failure
    bool loadPreset (const juce::String& name);

    //==========================================================================
    // "Tune by dissonance curve" (Sethares): analyses the CURRENT spectrum,
    // builds a .scl from the curve's minima (or maxima in reverse mode), loads
    // it as the active scale and keeps the text for saving to disk.
    bool generateDissonanceScale (int count, bool useMaxima, juce::String& errorOut);
    bool saveGeneratedScl (const juce::File& f, juce::String& errorOut);
    bool hasGeneratedScl() const noexcept { return lastDissScl.isNotEmpty(); }
    juce::String getGeneratedSclName() const { return lastDissName; }

    //==========================================================================
    // Shared with the editor:
    juce::MidiKeyboardState keyboardState;
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    spectrum::SpectrumSettings currentSettings() const;
    void timerCallback() override;
    void addRecentScl (const juce::File&);

    juce::ValueTree captureState();
    void restoreState (const juce::ValueTree&);

    // Declared before `synth`: the AdditiveVoices owned by `synth` hold references
    // to spectrumEngine and tuning, so the voices must be destroyed before them.
    spectrum::SpectrumEngine    spectrumEngine;
    TuningManager               tuning;
    juce::MPESynthesiser        synth;
    spectrum::SpectrumSettings  lastPushed;

    // Scala-as-timbre state (message thread; read into SpectrumSettings each poll).
    std::vector<float> timbreDegrees;
    float              timbrePeriod = 2.0f;
    bool               timbreActive = false;
    juce::String       timbreName, timbreSource;

    // Persistent recent-.scl list (global app settings, shared across instances).
    std::unique_ptr<juce::PropertiesFile> appProps;
    juce::StringArray                     recentSclPaths;

    // Last dissonance-generated .scl (text + display name), for saving to disk.
    juce::String lastDissScl, lastDissName;

    float lastGain = 0.0f;
    int   lastBendApplied = 48; // matches enableLegacyMode(48) in the constructor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessor)
};
