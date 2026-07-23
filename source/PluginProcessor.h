#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/AdditiveVoice.h"
#include "dsp/RhythmEngine.h"
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
    const juce::String getName() const override { return juce::String::fromUTF8 ("Fibl\xc3\xb3 Gal\xc3\xa0" "ctic"); }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; } // timbre->rhythm MIDI out (RhythmEngine)
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

    // MIDI range the colour keyboard should display, adapted to the active tuning
    // so wide-period scales (e.g. a period-5 scale, whose keys span sub-audio to
    // ultrasonic over a fixed 44-key window) only ever offer keys in a musical
    // band. Expands out from the tonic (key 60) while the fundamental stays in
    // [loHz, hiHz], then caps the span for playable cell widths.
    void getKeyboardKeyRange (int& lowKey, int& numKeys) const noexcept
    {
        constexpr double loHz = 40.0, hiHz = 4186.0; // ~E1 .. ~C8
        constexpr int tonic = 60, maxKeys = 52, minKeys = 8;
        int lo = tonic, hi = tonic;
        while (lo > 0   && tuning.frequencyForNote (lo - 1) >= loHz) --lo;
        while (hi < 127 && tuning.frequencyForNote (hi + 1) <= hiHz) ++hi;
        if (hi - lo + 1 > maxKeys) // 12-TET etc.: comfortable window centred on the tonic
        {
            lo = juce::jmax (lo, tonic - maxKeys / 2);
            hi = juce::jmin (hi, lo + maxKeys - 1);
        }
        if (hi - lo + 1 < minKeys) // ultra-wide period: still show a few keys
        {
            lo = juce::jlimit (0, 127 - minKeys + 1, tonic - minKeys / 2);
            hi = lo + minKeys - 1;
        }
        lowKey  = lo;
        numKeys = hi - lo + 1;
    }

    // Frequency ratio of scale degree `d` relative to degree 0 (the tonic on
    // key 60), for the keyboard's just-interval labels. Degree d is mapped to an
    // IN-RANGE key of the same degree class (walking down whole periods) rather
    // than clamping the MIDI note, so dense scales (72-EDO etc.) label correctly.
    double getDegreeRatio (int d) const
    {
        const int D = juce::jmax (1, tuning.degreesPerPeriod());
        int key = 60 + d;
        while (key > 127 && key - D >= 0) key -= D;
        while (key < 0   && key + D <= 127) key += D;
        key = juce::jlimit (0, 127, key);
        const double f0 = tuning.frequencyForNote (60);
        const double f  = tuning.frequencyForNote (key);
        return f0 > 0.0 ? f / f0 : 1.0;
    }

    //==========================================================================
    // Scala-as-timbre: a loaded .scl becomes the partial set instead of the
    // harmonic engine. Called from the editor.
    bool loadTimbreFile (const juce::File& f, juce::String& errorOut);
    // Build the timbre spectrum from typed intervals (cents "701.955c" / "700.0",
    // ratios "3/2", or integers "3"), space/comma/newline separated. The highest
    // interval becomes the period (the Reps repeat point).
    bool loadTimbreText (const juce::String& text, juce::String& errorOut);
    void clearTimbre();
    bool isTimbreActive() const noexcept { return timbreActive; }
    juce::String getTimbreStatus() const;

    // Recently-used .scl files (persisted across sessions, newest first, max 3).
    juce::StringArray getRecentSclPaths() const { return recentSclPaths; }

    //==========================================================================
    // Timbre -> rhythm MIDI out (see RhythmEngine.h). The editor edits the slots
    // directly (atomics) and reads the live pulse rates for its panel.
    RhythmEngine& getRhythm() noexcept { return rhythm; }
    const spectrum::SpectrumTemplate* getLiveTemplate() const noexcept { return spectrumEngine.getLive(); }
    juce::uint32 getTemplateGeneration() const noexcept { return spectrumEngine.getGeneration(); }

    //==========================================================================
    // Tuning bank: load as many .scl files as you like (effectively unlimited - the
    // automatable "tuningBankSlot" parameter just needs a bounded range, so the ceiling
    // is a generous 128). 0 = none, 1..N = slot; the switch is picked up on the message
    // thread (timerCallback) and loaded there. The UI only ever shows the loaded slots.
    static constexpr int kBankSlots = 128;
    void setTuningBankFile (int slot0, const juce::File& f);       // load a .scl into a slot + activate it
    juce::String getTuningBankName (int slot0) const { return juce::isPositiveAndBelow (slot0, kBankSlots) ? bankName[(size_t) slot0] : juce::String(); }
    int  getTuningBankSlotParam() const noexcept { return (int) apvts.getRawParameterValue ("tuningBankSlot")->load(); }
    void setTuningBankSlotParam (int slot);                        // 0 = none, 1..N = slot (drives automation + load)
    int  firstEmptyBankSlot() const noexcept;

    //==========================================================================
    // File presets (.gsting). Called from the editor.
    static juce::File getPresetFolder();
    juce::StringArray getPresetNames();
    juce::String savePreset (const juce::String& name); // returns the stored name, or empty on failure
    bool loadPreset (const juce::String& name);

    //==========================================================================
    // "Spectrum to Scale" (Sethares): analyses the CURRENT spectrum, builds a
    // .scl from the curve's minima (or maxima in reverse mode) over the chosen
    // period, loads it as the active scale and keeps the text for saving.
    bool generateDissonanceScale (int count, bool useMaxima, double periodRatio,
                                  const juce::String& periodSclLine, juce::String& errorOut);
    bool saveGeneratedScl (const juce::File& f, juce::String& errorOut);
    bool hasGeneratedScl() const noexcept { return lastDissScl.isNotEmpty(); }
    juce::String getGeneratedSclName() const { return lastDissName; }

    // Generated-scale degree list for the UI (sorted ascending by curve height).
    struct DissDegreeInfo { int degree; double ratio; double height; };
    const std::vector<DissDegreeInfo>& getDissDegrees() const noexcept { return dissDegrees; }
    double getDissGreenHeight() const noexcept { return dissGreenH; }
    double getDissRedHeight()   const noexcept { return dissRedH; }
    int    getDissRequested()   const noexcept { return dissRequested; }
    int    getDissFound()       const noexcept { return dissFound; }
    int    getDissGeneration()  const noexcept { return dissGeneration; }

    //==========================================================================
    // Shared with the editor:
    juce::MidiKeyboardState keyboardState;
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    spectrum::SpectrumSettings currentSettings() const;
    void timerCallback() override;
    void addRecentScl (const juce::File&);
    // tuningMode param -> TuningManager mode + synth zone layout. Index 3 = "MPE
    // (bend input)": no internal tuning (equal base) and a REAL MPE lower zone so
    // per-note bends from an MPE controller/host are honoured; other modes run the
    // legacy single-channel layout. Message thread; switching releases held notes.
    void applyTuningMode (int paramIndex);
    bool lastZoneMpe = false;

    juce::ValueTree captureState();
    void restoreState (const juce::ValueTree&);

    // Declared before `synth`: the AdditiveVoices owned by `synth` hold references
    // to spectrumEngine and tuning, so the voices must be destroyed before them.
    spectrum::SpectrumEngine    spectrumEngine;
    TuningManager               tuning;
    juce::MPESynthesiser        synth;
    spectrum::SpectrumSettings  lastPushed;
    RhythmEngine                rhythm { apvts, spectrumEngine, tuning };

    // Scala-as-timbre state (message thread; read into SpectrumSettings each poll).
    std::vector<float> timbreDegrees;
    float              timbrePeriod = 2.0f;
    bool               timbreActive = false;
    juce::String       timbreName, timbreSource;

    // Persistent recent-.scl list (global app settings, shared across instances).
    std::unique_ptr<juce::PropertiesFile> appProps;
    juce::StringArray                     recentSclPaths;

    // Tuning bank: per-instance .scl slots (name + raw source), persisted in state.
    std::array<juce::String, kBankSlots> bankName, bankSource;
    int lastBankSlot = 0; // last applied "tuningBankSlot" value (message thread)

    // Last dissonance-generated .scl (text + display name), for saving to disk,
    // plus the degree/height data shown in the Spectrum-to-Scale panel.
    juce::String lastDissScl, lastDissName;
    std::vector<DissDegreeInfo> dissDegrees;
    double dissGreenH = 0.0, dissRedH = 1.0;
    int    dissRequested = 0, dissFound = 0, dissGeneration = 0;

    float lastGain = 0.0f;
    int   lastBendApplied = 48; // matches enableLegacyMode(48) in the constructor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessor)
};
