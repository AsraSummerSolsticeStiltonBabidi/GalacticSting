#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Prime-limit choice index -> largest prime allowed (0 = off).
    constexpr int primeLimitValues[] = { 0, 2, 3, 5, 7, 11, 13, 17, 19, 23 };
}

//==============================================================================
FractalAdditiveAudioProcessor::FractalAdditiveAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    // Legacy mode lets the MPE engine also respond to ordinary MIDI, so the
    // computer keyboard and plain Ableton MIDI clips produce sound. Real MPE
    // controllers still work; we will add a proper MPE/legacy switch later.
    synth.enableLegacyMode();
    synth.setVoiceStealingEnabled (true);

    for (int i = 0; i < 32; ++i)
        synth.addVoice (new AdditiveVoice (apvts, spectrumEngine, tuning));

    // Global app settings (recent .scl files), shared across all instances.
    juce::PropertiesFile::Options o;
    o.applicationName     = "GalacticSting";
    o.filenameSuffix      = "settings";
    o.folderName          = "GalacticSting";
    o.osxLibrarySubFolder = "Application Support";
    appProps = std::make_unique<juce::PropertiesFile> (o);
    recentSclPaths.addLines (appProps->getValue ("recentScl"));
    recentSclPaths.removeEmptyStrings();
}

void FractalAdditiveAudioProcessor::addRecentScl (const juce::File& f)
{
    const auto path = f.getFullPathName();
    recentSclPaths.removeString (path);
    recentSclPaths.insert (0, path);
    while (recentSclPaths.size() > 3)
        recentSclPaths.remove (recentSclPaths.size() - 1);
    if (appProps != nullptr)
    {
        appProps->setValue ("recentScl", recentSclPaths.joinIntoString ("\n"));
        appProps->saveIfNeeded();
    }
}

FractalAdditiveAudioProcessor::~FractalAdditiveAudioProcessor()
{
    stopTimer();
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
FractalAdditiveAudioProcessor::createParameterLayout()
{
    using namespace juce;

    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    //-- Spectrum ---------------------------------------------------------------
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "baseN", 1 }, "Base Harmonic (N)", 1, 64, 1));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "baseMode", 1 }, "Base Pitch",
        StringArray { "Transposed down", "Original" }, 0));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "selection", 1 }, "Partial Whitelist",
        StringArray { "All", "Even", "Odd", "Prime" }, 0));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "curveSlope", 1 }, "Partial Amp Decay Slope",
        NormalisableRange<float> (0.0f, 4.0f, 0.001f), 1.0f));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "primeLimit", 1 }, "Prime Limit",
        StringArray { "Off", "2", "3", "5", "7", "11", "13", "17", "19", "23" }, 0));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "primeMode", 1 }, "Prime Mode",
        StringArray { "Limit", "Whitelist" }, 0));
    {
        const char* pn[] = { "2","3","5","7","11","13","17","19","23" };
        for (int i = 0; i < spectrum::primeListSize; ++i)
            params.push_back (std::make_unique<AudioParameterBool>(
                ParameterID { "primeWL" + String (i), 1 }, "Prime " + String (pn[i]), true));
    }
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "fractalLevel", 1 }, "Fractal Level", 1, 4, 1));
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "spectrumRange", 1 }, "Spectrum Range (oct)", 1, 10, 10));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "phaseMode", 1 }, "Phase",
        StringArray { "Aligned", "Scattered" }, 0));

    //-- Amp envelope -----------------------------------------------------------
    auto timeRange = [] (float maxSeconds)
    {
        NormalisableRange<float> r (0.001f, maxSeconds, 0.0001f);
        r.setSkewForCentre (0.2f);
        return r;
    };

    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "ampAttack", 1 }, "Amp Attack", timeRange (5.0f), 0.005f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "ampDecay", 1 }, "Amp Decay", timeRange (5.0f), 0.120f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "ampSustain", 1 }, "Amp Sustain",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.80f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "ampRelease", 1 }, "Amp Release", timeRange (8.0f), 0.300f));

    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "masterGain", 1 }, "Master Gain",
        NormalisableRange<float> (-60.0f, 6.0f, 0.01f), 0.0f));

    //-- Filter + filter envelope ----------------------------------------------
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterCutoff", 1 }, "Filter Cutoff",
        NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f), 20000.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterResonance", 1 }, "Filter Resonance",
        NormalisableRange<float> (0.5f, 20.0f, 0.01f, 0.5f), 0.7f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterEnvAmount", 1 }, "Filter Env Amount (oct)",
        NormalisableRange<float> (-8.0f, 8.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterAttack", 1 }, "Filter Attack", timeRange (5.0f), 0.010f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterDecay", 1 }, "Filter Decay", timeRange (5.0f), 0.200f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterSustain", 1 }, "Filter Sustain",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 1.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterRelease", 1 }, "Filter Release", timeRange (8.0f), 0.300f));

    //-- Tuning ----------------------------------------------------------------
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "tuningMode", 1 }, "Tuning",
        StringArray { "12-TET", "MTS-ESP", "Scala (.scl)" }, 0));

    //-- Scala-as-timbre --------------------------------------------------------
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "timbreRepeats", 1 }, "Timbre Period Repeats", 1, 8, 3));

    return { params.begin(), params.end() };
}

//==============================================================================
spectrum::SpectrumSettings FractalAdditiveAudioProcessor::currentSettings() const
{
    spectrum::SpectrumSettings s;
    s.baseN         = (int) apvts.getRawParameterValue ("baseN")->load();
    s.baseMode      = (spectrum::BaseMode) (int) apvts.getRawParameterValue ("baseMode")->load();
    s.selection     = (spectrum::Selection) (int) apvts.getRawParameterValue ("selection")->load();
    s.curveSlope    = apvts.getRawParameterValue ("curveSlope")->load();

    const int plIdx = juce::jlimit (0, 9, (int) apvts.getRawParameterValue ("primeLimit")->load());
    s.primeLimit    = primeLimitValues[plIdx];

    s.primeUseWhitelist = apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
    unsigned mask = 0;
    for (int i = 0; i < spectrum::primeListSize; ++i)
        if (apvts.getRawParameterValue ("primeWL" + juce::String (i))->load() > 0.5f)
            mask |= (1u << i);
    s.primeWhitelistMask = mask;

    s.fractalLevel   = (int) apvts.getRawParameterValue ("fractalLevel")->load();
    s.spectrumRangeOct = (int) apvts.getRawParameterValue ("spectrumRange")->load();
    s.ampFloorDb     = -120.0f;
    s.maxPartials    = spectrum::kMaxPartials;

    // Scala-as-timbre overrides the harmonic engine; slope + fractal still apply.
    if (timbreActive && ! timbreDegrees.empty())
    {
        s.source              = spectrum::Source::ScalaTimbre;
        s.timbreDegrees       = timbreDegrees;
        s.timbrePeriod        = timbrePeriod;
        s.timbrePeriodRepeats = (int) apvts.getRawParameterValue ("timbreRepeats")->load();
    }
    else
    {
        s.source = spectrum::Source::Harmonic;
    }
    return s;
}

void FractalAdditiveAudioProcessor::timerCallback()
{
    const auto s = currentSettings();
    if (s != lastPushed)
    {
        spectrumEngine.requestRebuild (s);
        lastPushed = s;
    }

    tuning.setMode ((TuningManager::Mode) (int) apvts.getRawParameterValue ("tuningMode")->load());
}

//==============================================================================
bool FractalAdditiveAudioProcessor::loadScalaFile (const juce::File& f, juce::String& errorOut)
{
    if (! tuning.loadScalaFile (f, errorOut))
        return false;

    // Selecting a file implies the user wants to hear it.
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    tuning.setMode (TuningManager::Mode::Scala);
    addRecentScl (f);
    return true;
}

void FractalAdditiveAudioProcessor::clearScala()
{
    tuning.clearScala();
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Equal));
    tuning.setMode (TuningManager::Mode::Equal);
}

//==============================================================================
bool FractalAdditiveAudioProcessor::loadTimbreFile (const juce::File& f, juce::String& errorOut)
{
    if (! f.existsAsFile())
    {
        errorOut = "File not found.";
        return false;
    }

    const auto text = f.loadFileAsString();
    std::vector<double> perDeg;
    double period = 2.0;
    if (! TuningManager::parseScalaScale (text, perDeg, period, errorOut))
        return false;

    timbreDegrees.assign (perDeg.begin(), perDeg.end()); // double -> float
    timbrePeriod = (float) period;
    timbreName   = f.getFileNameWithoutExtension();
    timbreSource = text;
    timbreActive = true;
    addRecentScl (f);
    return true;
}

void FractalAdditiveAudioProcessor::clearTimbre()
{
    timbreActive = false;
    timbreDegrees.clear();
    timbreName.clear();
    timbreSource.clear();
}

juce::String FractalAdditiveAudioProcessor::getTimbreStatus() const
{
    if (timbreActive && ! timbreDegrees.empty())
        return "Timbre: " + timbreName + "  (" + juce::String ((int) timbreDegrees.size()) + " notes/period)";
    return "Timbre off (harmonic engine)";
}

//==============================================================================
void FractalAdditiveAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    synth.setCurrentPlaybackSampleRate (sampleRate);
    lastGain = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("masterGain")->load());

    // Build once synchronously so a template exists before the first note.
    lastPushed = currentSettings();
    spectrumEngine.buildNow (lastPushed);

    tuning.setMode ((TuningManager::Mode) (int) apvts.getRawParameterValue ("tuningMode")->load());

    startTimerHz (30); // poll spectrum params and rebuild off the audio thread
}

bool FractalAdditiveAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::stereo();
}

void FractalAdditiveAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                  juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();

    // Merge on-screen / computer-keyboard notes into the incoming MIDI stream.
    keyboardState.processNextMidiBuffer (midiMessages, 0, buffer.getNumSamples(), true);

    synth.renderNextBlock (buffer, midiMessages, 0, buffer.getNumSamples());

    const auto gain = juce::Decibels::decibelsToGain (
        apvts.getRawParameterValue ("masterGain")->load());
    buffer.applyGainRamp (0, buffer.getNumSamples(), lastGain, gain);
    lastGain = gain;
}

//==============================================================================
juce::AudioProcessorEditor* FractalAdditiveAudioProcessor::createEditor()
{
    return new FractalAdditiveAudioProcessorEditor (*this);
}

//==============================================================================
void FractalAdditiveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Persist a loaded Scala scale as a child of the parameter tree so reopening
    // a project restores the exact tuning (APVTS ignores non-parameter children).
    state.removeChild (state.getChildWithName ("SCALA"), nullptr);
    if (tuning.getScalaSource().isNotEmpty())
    {
        juce::ValueTree scl ("SCALA");
        scl.setProperty ("name",   tuning.getScalaName(),   nullptr);
        scl.setProperty ("source", tuning.getScalaSource(), nullptr);
        state.appendChild (scl, nullptr);
    }

    state.removeChild (state.getChildWithName ("TIMBRE"), nullptr);
    if (timbreActive && timbreSource.isNotEmpty())
    {
        juce::ValueTree tim ("TIMBRE");
        tim.setProperty ("name",   timbreName,   nullptr);
        tim.setProperty ("source", timbreSource, nullptr);
        state.appendChild (tim, nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FractalAdditiveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        apvts.replaceState (tree);

        if (auto scl = tree.getChildWithName ("SCALA"); scl.isValid())
        {
            juce::String err;
            tuning.loadScalaString (scl.getProperty ("source").toString(),
                                    scl.getProperty ("name").toString(), err);
        }
        else
        {
            tuning.clearScala();
        }

        tuning.setMode ((TuningManager::Mode) (int) apvts.getRawParameterValue ("tuningMode")->load());

        clearTimbre();
        if (auto tim = tree.getChildWithName ("TIMBRE"); tim.isValid())
        {
            const auto src = tim.getProperty ("source").toString();
            std::vector<double> perDeg; double period = 2.0; juce::String err;
            if (src.isNotEmpty() && TuningManager::parseScalaScale (src, perDeg, period, err))
            {
                timbreDegrees.assign (perDeg.begin(), perDeg.end());
                timbrePeriod = (float) period;
                timbreName   = tim.getProperty ("name").toString();
                timbreSource = src;
                timbreActive = true;
            }
        }
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FractalAdditiveAudioProcessor();
}
