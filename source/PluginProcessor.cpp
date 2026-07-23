#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Dissonance.h"

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
    // computer keyboard and plain Ableton MIDI clips produce sound. Each MIDI
    // channel is independent with its own pitch bend, which is exactly how
    // Ableton Live 12 delivers its tuning systems / Microtuner: per-note MPE
    // pitch bend with an assumed range of +-48 semitones (the MPE default).
    // The range is a parameter ("bendRange"); 48 is required for Live tuning.
    synth.enableLegacyMode (48);
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
    // Cutoff is an INTERVAL above the played note (cents, 1200 = one octave), not
    // absolute Hz: the filter tracks the note, so it always cuts the SAME partials
    // (a partial at ratio r sits at 1200*log2(r) cents) regardless of pitch.
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterCutoff", 1 }, "Filter Cutoff (cents above note)",
        NormalisableRange<float> (0.0f, 12000.0f, 1.0f), 12000.0f));
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

    //-- Pitch envelope (bends the played pitch, in semitones) ------------------
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchEnvAmount", 1 }, "Pitch Env Amount (st)",
        NormalisableRange<float> (-48.0f, 48.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchAttack", 1 }, "Pitch Attack", timeRange (5.0f), 0.005f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchDecay", 1 }, "Pitch Decay", timeRange (5.0f), 0.150f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchSustain", 1 }, "Pitch Sustain",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));   // min by default
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchRelease", 1 }, "Pitch Release", timeRange (8.0f), 0.001f)); // min by default

    //-- Spectral morphing (glide partials to their new positions on a spectrum change)
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "morph", 1 }, "Morphing", false));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "morphTime", 1 }, "Morph Time", timeRange (5.0f), 0.300f));

    //-- Tuning ----------------------------------------------------------------
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "tuningMode", 1 }, "Tuning",
        StringArray { "12ED2", "MTS-ESP", "Scala (.scl)", "MPE (bend input)" }, 0));
    // Per-note pitch-bend range. +-48 st is the MPE default and what Ableton
    // Live 12's tuning systems / Microtuner assume; +-2 suits classic keyboards.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "bendRange", 1 }, "Pitch Bend Range",
        StringArray { "+-2 st", "+-12 st", "+-48 st (MPE)" }, 2));
    // Master reference: A = <hz>; transposes the whole instrument in every mode.
    {
        NormalisableRange<float> r (220.0f, 880.0f, 0.01f);
        r.setSkewForCentre (440.0f);
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { "refA", 1 }, "Reference A (Hz)", r, 440.0f));
    }
    // Whole-instrument octave transpose (+-5 octaves), on top of the A reference.
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "octaveShift", 1 }, "Octave Transpose", -5, 5, 0));
    // Tuning bank slot (0 = none, 1..N = load that bank .scl). Automatable.
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "tuningBankSlot", 1 }, "Tuning Bank Slot", 0, kBankSlots, 0));

    //-- Spectrum -> rhythm MIDI out --------------------------------------------
    // Each selected partial pulses at f0 * ratio / x^n; all of these are automatable.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmOn", 1 }, "Rhythm MIDI On", false));
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "rhythmDivX", 1 }, "Rhythm Divide Base (x)", 1, 12, 1));
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "rhythmDivN", 1 }, "Rhythm Divide Times (n)", 1, 16, 8));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "rhythmMode", 1 }, "Rhythm Mode",
        StringArray { "Lowest note", "All notes" }, 0));
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmMpe", 1 }, "Rhythm MPE Out", false));

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

    // Tuning-bank slot switched (UI or host automation): load that slot's .scl.
    // Runs on the message thread (timerCallback), so the parse/load is safe here.
    {
        const int slot = getTuningBankSlotParam();
        if (slot != lastBankSlot)
        {
            lastBankSlot = slot;
            if (slot >= 1 && slot <= kBankSlots && bankSource[(size_t) (slot - 1)].isNotEmpty())
            {
                juce::String err;
                if (tuning.loadScalaString (bankSource[(size_t) (slot - 1)], bankName[(size_t) (slot - 1)], err))
                {
                    if (auto* pm = apvts.getParameter ("tuningMode"))
                        pm->setValueNotifyingHost (pm->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
                }
            }
        }
    }

    applyTuningMode ((int) apvts.getRawParameterValue ("tuningMode")->load());
    tuning.setReferenceA ((double) apvts.getRawParameterValue ("refA")->load());
    tuning.setOctaveShift ((int) apvts.getRawParameterValue ("octaveShift")->load());

    // Apply the bend-range choice only when it changes: setLegacyModePitchbendRange
    // releases all held notes, so it must never be called redundantly.
    static constexpr int bendValues[] = { 2, 12, 48 };
    const int bendIdx = juce::jlimit (0, 2, (int) apvts.getRawParameterValue ("bendRange")->load());
    if (bendValues[bendIdx] != lastBendApplied)
    {
        lastBendApplied = bendValues[bendIdx];
        synth.setLegacyModePitchbendRange (lastBendApplied);
    }
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
void FractalAdditiveAudioProcessor::applyTuningMode (int paramIndex)
{
    const bool mpe = paramIndex == 3;
    tuning.setMode (mpe ? TuningManager::Mode::Equal          // MPE: no internal tuning
                        : (TuningManager::Mode) paramIndex);
    if (mpe != lastZoneMpe)
    {
        lastZoneMpe = mpe;
        if (mpe)
        {
            juce::MPEZoneLayout zone;
            zone.setLowerZone (15, 48, 2); // 15 member channels, +-48 st per-note bend
            synth.setZoneLayout (zone);
        }
        else
        {
            synth.enableLegacyMode (lastBendApplied);
        }
    }
}

//==============================================================================
int FractalAdditiveAudioProcessor::firstEmptyBankSlot() const noexcept
{
    for (int i = 0; i < kBankSlots; ++i)
        if (bankSource[(size_t) i].isEmpty())
            return i;
    return 0; // all full -> reuse the first
}

void FractalAdditiveAudioProcessor::setTuningBankFile (int slot0, const juce::File& f)
{
    if (! juce::isPositiveAndBelow (slot0, kBankSlots) || ! f.existsAsFile())
        return;

    const auto text = f.loadFileAsString();
    std::vector<double> tmp; double per = 2.0; juce::String err;
    if (! TuningManager::parseScalaScale (text, tmp, per, err)) // validate before storing
        return;

    bankSource[(size_t) slot0] = text;
    bankName[(size_t) slot0]   = f.getFileNameWithoutExtension();
    addRecentScl (f);
    setTuningBankSlotParam (slot0 + 1); // select + load it now
}

void FractalAdditiveAudioProcessor::setTuningBankSlotParam (int slot)
{
    if (auto* p = apvts.getParameter ("tuningBankSlot"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (0, kBankSlots, slot)));
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

bool FractalAdditiveAudioProcessor::loadTimbreText (const juce::String& text, juce::String& errorOut)
{
    // Parse each token into a ratio. 'c' suffix or a decimal point = cents;
    // "a/b" = fraction; a bare integer = a/1.
    juce::StringArray tokens;
    tokens.addTokens (text, " \t\r\n,;", "");
    tokens.removeEmptyStrings();

    std::vector<double> ratios;
    for (auto tk : tokens)
    {
        tk = tk.trim();
        double r = 0.0;
        if (tk.endsWithIgnoreCase ("c") || tk.containsChar ('.'))
        {
            auto num = tk.endsWithIgnoreCase ("c") ? tk.dropLastCharacters (1) : tk;
            if (! num.containsOnly ("0123456789.+-") || num.isEmpty()
                || num.indexOfChar ('.') != num.lastIndexOfChar ('.'))
            { errorOut = "Bad cents value: \"" + tk + "\""; return false; }
            r = std::pow (2.0, num.getDoubleValue() / 1200.0);
        }
        else if (tk.containsChar ('/'))
        {
            const auto a = tk.upToFirstOccurrenceOf ("/", false, false);
            const auto b = tk.fromFirstOccurrenceOf ("/", false, false);
            if (! a.containsOnly ("0123456789") || ! b.containsOnly ("0123456789") || a.isEmpty() || b.isEmpty())
            { errorOut = "Bad ratio: \"" + tk + "\""; return false; }
            const double den = b.getDoubleValue();
            if (den <= 0.0) { errorOut = "Bad ratio: \"" + tk + "\""; return false; }
            r = a.getDoubleValue() / den;
        }
        else if (tk.containsOnly ("0123456789"))
        {
            r = tk.getDoubleValue();
        }
        else { errorOut = "Not an interval: \"" + tk + "\""; return false; }

        if (std::isfinite (r) && r > 1.0000001) // above the (implicit) 1/1 fundamental
            ratios.push_back (r);
    }

    std::sort (ratios.begin(), ratios.end());
    ratios.erase (std::unique (ratios.begin(), ratios.end(),
                               [] (double a, double b) { return std::abs (a - b) < 1.0e-6; }),
                  ratios.end());
    if (ratios.empty())
    {
        errorOut = "Type at least one interval above 1/1 (e.g. \"3/2 2\" or \"700c 1200c\").";
        return false;
    }

    // Highest interval = the period; the rest (plus the implicit 1/1) are the partials.
    const double period = ratios.back();
    std::vector<float> deg;
    deg.reserve (ratios.size());
    deg.push_back (1.0f);
    for (size_t i = 0; i + 1 < ratios.size(); ++i)
        deg.push_back ((float) ratios[i]);

    timbreDegrees = deg;
    timbrePeriod  = (float) period;
    timbreName    = "typed";
    timbreSource  = text;
    timbreActive  = true;
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
bool FractalAdditiveAudioProcessor::generateDissonanceScale (int count, bool useMaxima,
                                                             double periodRatio,
                                                             const juce::String& periodSclLine,
                                                             juce::String& errorOut)
{
    count = juce::jlimit (1, 24, count);
    if (! (periodRatio > 1.02) || ! std::isfinite (periodRatio))
    {
        errorOut = "Invalid scale period.";
        return false;
    }

    // Analyse the spectrum the synth is REALLY producing right now (pure recompute
    // of the current settings; message thread, no audio-thread interaction).
    const auto tmpl = spectrum::compileSpectrum (currentSettings());
    if (tmpl.partials.empty())
    {
        errorOut = "The current settings produce no partials - nothing to analyse.";
        return false;
    }

    // Anchor at middle C times the A-reference, like the audible tuning itself.
    const double f0 = TuningManager::kRefFreq * tuning.referenceScale();
    std::vector<dissonance::Partial> spec;
    spec.reserve (tmpl.partials.size());
    for (const auto& p : tmpl.partials)
        spec.push_back ({ (double) p.ratio * f0, (double) p.amp });

    // Analysis cap: the curve's shape is set by the strongest partials; capping
    // keeps the 1-cent sweep instant. (Analysis only - the SOUND is never capped.)
    if (spec.size() > 64)
    {
        std::sort (spec.begin(), spec.end(),
                   [] (const dissonance::Partial& a, const dissonance::Partial& b) { return a.amp > b.amp; });
        spec.resize (64);
    }

    const auto curve = dissonance::dissonanceCurve (spec, periodRatio, 1.0);
    const auto found = dissonance::pickExtremes (spec, curve, count, useMaxima, periodRatio);
    if (found.empty())
    {
        errorOut = juce::String ("This timbre's dissonance curve has no interior ")
                 + (useMaxima ? "maxima" : "minima")
                 + " in the chosen period: with very few interacting partials (a pure sine is the "
                   "extreme case) roughness just rises once and falls again, forming no notches. "
                   "Add partials (higher N, fractal, flatter slope) or widen the period.";
        return false;
    }

    const juce::String kind = useMaxima ? "maxima" : "minima";
    juce::String scl;
    scl << "! Generated by Galactic Sting - Sethares dissonance " << kind << "\n"
        << "Dissonance " << kind << " of the current timbre\n"
        << " " << juce::String ((int) found.size() + 1) << "\n!\n";
    for (const auto& e : found)
        scl << " " << juce::String (1200.0 * std::log2 (e.ratio), 5) << "\n";
    scl << " " << periodSclLine << "\n"; // the chosen period, exact

    // Name by the number of extremes found (the .scl note-count is one higher
    // because it also lists the period as the last interval).
    const auto name = "diss-" + kind + "-" + juce::String ((int) found.size());
    if (! tuning.loadScalaString (scl, name, errorOut))
        return false;

    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    tuning.setMode (TuningManager::Mode::Scala);

    lastDissScl  = scl;
    lastDissName = name;

    // Degree list for the UI: degree i+1 = i-th interval ascending; plus the
    // period as degree 0 (its height anchors the green end of the gradient).
    dissDegrees.clear();
    double maxCurve = 0.0;
    for (const auto& c : curve) maxCurve = std::max (maxCurve, c.second);
    const double periodH = dissonance::dissonanceAt (spec, periodRatio);
    for (int i = 0; i < (int) found.size(); ++i)
        dissDegrees.push_back ({ i + 1, found[(size_t) i].ratio, found[(size_t) i].value });
    dissDegrees.push_back ({ 0, periodRatio, periodH });
    std::sort (dissDegrees.begin(), dissDegrees.end(),
               [] (const DissDegreeInfo& a, const DissDegreeInfo& b) { return a.height < b.height; });
    // Green anchors at the ACTUALLY most-consonant degree, NOT unconditionally at
    // the period. The period is a structural degree (the scale's repeat interval)
    // but is only consonant if the curve says so - for a non-harmonic timbre a
    // wide period (3, 5, ...) can be quite rough, and several interior minima can
    // be far more consonant than it. Forcing green = periodH used to paint the
    // period as "maximum consonance" even then, which is why the same ratio looked
    // consonant as a period yet unremarkable as an interior interval. Now every
    // degree - the period included - is coloured by its true curve height.
    dissGreenH    = dissDegrees.empty() ? 0.0 : dissDegrees.front().height; // sorted asc -> most consonant
    dissRedH      = std::max (maxCurve, dissGreenH + 1.0e-9);
    dissRequested = count;
    dissFound     = (int) found.size();
    ++dissGeneration;
    return true;
}

bool FractalAdditiveAudioProcessor::saveGeneratedScl (const juce::File& f, juce::String& errorOut)
{
    if (lastDissScl.isEmpty())
    {
        errorOut = "No generated scale to save yet.";
        return false;
    }
    if (! f.replaceWithText (lastDissScl))
    {
        errorOut = "Could not write the file.";
        return false;
    }
    addRecentScl (f);
    return true;
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

    applyTuningMode ((int) apvts.getRawParameterValue ("tuningMode")->load());
    rhythm.prepare (sampleRate);

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

    // Timbre -> rhythm MIDI out: generated in this very block with exact sample
    // stamps, from the same f0/tuning maths as the voices -> sample-locked to the
    // audio. Consumes the input events; only the pulse MIDI leaves the plugin.
    rhythm.process (midiMessages, buffer.getNumSamples());
}

//==============================================================================
juce::AudioProcessorEditor* FractalAdditiveAudioProcessor::createEditor()
{
    return new FractalAdditiveAudioProcessorEditor (*this);
}

//==============================================================================
// The full patch as a ValueTree: the parameters plus the loaded Scala scale and
// Scala-timbre (as child trees). Shared by host state save/load AND file presets.
juce::ValueTree FractalAdditiveAudioProcessor::captureState()
{
    auto state = apvts.copyState();

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

    state.removeChild (state.getChildWithName ("BANK"), nullptr);
    {
        juce::ValueTree bank ("BANK");
        for (int i = 0; i < kBankSlots; ++i)
            if (bankSource[(size_t) i].isNotEmpty())
            {
                juce::ValueTree slot ("SLOT");
                slot.setProperty ("i",      i,                      nullptr);
                slot.setProperty ("name",   bankName[(size_t) i],   nullptr);
                slot.setProperty ("source", bankSource[(size_t) i], nullptr);
                bank.appendChild (slot, nullptr);
            }
        if (bank.getNumChildren() > 0)
            state.appendChild (bank, nullptr);
    }

    state.removeChild (state.getChildWithName ("RHYTHM"), nullptr);
    {
        juce::ValueTree ry ("RHYTHM");
        for (int i = 0; i < RhythmEngine::kSlots; ++i)
        {
            const auto& s = rhythm.slots[i];
            if (s.partial.load() >= 0)
            {
                juce::ValueTree slot ("SLOT");
                slot.setProperty ("i",        i,                    nullptr);
                slot.setProperty ("partial",  s.partial.load(),     nullptr);
                slot.setProperty ("steps",    s.steps.load(),       nullptr);
                slot.setProperty ("useRatio", s.useRatio.load(),    nullptr);
                slot.setProperty ("ratio",    s.ratio.load(),       nullptr);
                slot.setProperty ("durMs",    (double) s.durMs.load(), nullptr);
                slot.setProperty ("on",       s.enabled.load(),     nullptr);
                ry.appendChild (slot, nullptr);
            }
        }
        if (ry.getNumChildren() > 0)
            state.appendChild (ry, nullptr);
    }
    return state;
}

void FractalAdditiveAudioProcessor::restoreState (const juce::ValueTree& tree)
{
    if (! tree.isValid() || ! tree.hasType (apvts.state.getType()))
        return;

    apvts.replaceState (tree);

    // Rhythm (spectrum->MIDI) slots.
    for (auto& s : rhythm.slots)
    { s.partial = -1; s.enabled = false; s.steps = 0; s.useRatio = false; s.ratio = 1.0; s.durMs = 60.0f; }
    if (auto ry = tree.getChildWithName ("RHYTHM"); ry.isValid())
        for (auto slot : ry)
        {
            const int i = (int) slot.getProperty ("i", -1);
            if (juce::isPositiveAndBelow (i, RhythmEngine::kSlots))
            {
                rhythm.slots[i].partial  = (int)    slot.getProperty ("partial", -1);
                rhythm.slots[i].steps    = (int)    slot.getProperty ("steps", 0);
                rhythm.slots[i].useRatio = (bool)   slot.getProperty ("useRatio", false);
                rhythm.slots[i].ratio    = (double) slot.getProperty ("ratio", 1.0);
                rhythm.slots[i].durMs    = (float) (double) slot.getProperty ("durMs", 60.0);
                rhythm.slots[i].enabled  = (bool)   slot.getProperty ("on", false);
            }
        }

    // Tuning bank slots.
    for (auto& s : bankName)   s.clear();
    for (auto& s : bankSource) s.clear();
    if (auto bank = tree.getChildWithName ("BANK"); bank.isValid())
        for (auto slot : bank)
        {
            const int i = (int) slot.getProperty ("i", -1);
            if (juce::isPositiveAndBelow (i, kBankSlots))
            {
                bankName[(size_t) i]   = slot.getProperty ("name").toString();
                bankSource[(size_t) i] = slot.getProperty ("source").toString();
            }
        }
    lastBankSlot = getTuningBankSlotParam(); // don't re-load on restore; state's SCALA already has it

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

    applyTuningMode ((int) apvts.getRawParameterValue ("tuningMode")->load());

    clearTimbre();
    if (auto tim = tree.getChildWithName ("TIMBRE"); tim.isValid())
    {
        const auto src  = tim.getProperty ("source").toString();
        const auto name = tim.getProperty ("name").toString();
        std::vector<double> perDeg; double period = 2.0; juce::String err;
        if (src.isNotEmpty() && name != "typed"
            && TuningManager::parseScalaScale (src, perDeg, period, err))
        {
            timbreDegrees.assign (perDeg.begin(), perDeg.end());
            timbrePeriod = (float) period;
            timbreName   = name;
            timbreSource = src;
            timbreActive = true;
        }
        else if (src.isNotEmpty()) // typed intervals (Spectrum Builder)
        {
            loadTimbreText (src, err);
        }
    }
}

void FractalAdditiveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = captureState().createXml())
        copyXmlToBinary (*xml, destData);
}

void FractalAdditiveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        restoreState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// File presets (.gsting XML). User presets live in Documents/Galactic Sting/Presets;
// factory presets shipped in a "Presets" folder next to the binary are copied in on
// first run so they "come with the download".
juce::File FractalAdditiveAudioProcessor::getPresetFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Galactic Sting").getChildFile ("Presets");
    if (! dir.exists())
    {
        dir.createDirectory();
        // Seed with any factory presets shipped alongside the executable/plugin.
        auto factory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                           .getParentDirectory().getChildFile ("Presets");
        if (factory.isDirectory())
            for (const auto& f : factory.findChildFiles (juce::File::findFiles, false, "*.gsting"))
                f.copyFileTo (dir.getChildFile (f.getFileName()));
    }
    return dir;
}

juce::StringArray FractalAdditiveAudioProcessor::getPresetNames()
{
    juce::StringArray names;
    for (const auto& f : getPresetFolder().findChildFiles (juce::File::findFiles, false, "*.gsting"))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    return names;
}

juce::String FractalAdditiveAudioProcessor::savePreset (const juce::String& name)
{
    // Returns the name actually stored (filename characters are sanitised, so it
    // can differ from what was typed), or empty on failure - the editor shows it.
    const auto safe = juce::File::createLegalFileName (name).trim();
    if (safe.isEmpty())
        return {};
    auto file = getPresetFolder().getChildFile (safe + ".gsting");
    if (auto xml = captureState().createXml())
        if (xml->writeTo (file))
            return safe;
    return {};
}

bool FractalAdditiveAudioProcessor::loadPreset (const juce::String& name)
{
    auto file = getPresetFolder().getChildFile (name + ".gsting");
    if (! file.existsAsFile())
        return false;
    if (auto xml = juce::XmlDocument::parse (file))
    {
        // .gsting files are user-editable: only apply a tree of OUR type, or a
        // foreign XML would reset every parameter and wipe the tuning/timbre
        // while reporting success.
        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.hasType (apvts.state.getType()))
            return false;
        restoreState (tree);
        return true;
    }
    return false;
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FractalAdditiveAudioProcessor();
}
