#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Dissonance.h"
#include "BinaryData.h"

#include <string>
#include <limits>
#include <unordered_map>

namespace
{
    // Prime-limit choice index -> largest prime allowed (0 = off).
    // Fader order: Off first, then the WIDEST limit down to the narrowest, so pushing
    // the fader up always means "filter more".
    constexpr int primeLimitValues[] = { 0, 23, 19, 17, 13, 11, 7, 5, 3, 2 };
}

//==============================================================================
FractalAdditiveAudioProcessor::FractalAdditiveAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "PARAMS", createParameterLayout())
{
    // Legacy mode lets the MPE engine also respond to ordinary MIDI, so the
    // computer keyboard and plain Ableton MIDI clips produce sound. Each MIDI
    // channel is independent with its own pitch bend, which is exactly how
    // Ableton Live 12 delivers its tuning systems / Microtuner: per-note MPE
    // pitch bend with an assumed range of +-48 semitones (the MPE default).
    // The range is a parameter ("bendRange"); 48 is required for Live tuning.
    synth.enableLegacyMode (48);
    synth.setVoiceStealingEnabled (true);

    // Watch the RANDOM SPECTRUM trigger for rising edges (see RandomTrigListener).
    // Baselined to the parameter's current level first, so merely attaching cannot
    // manufacture an edge and randomise a spectrum nobody asked for.
    if (auto* rp = apvts.getParameter ("specRandom"))
    {
        randomTrigLevel.store (rp->getValue() >= 0.5f, std::memory_order_release);
        randomTrigListener.owner = this;
        rp->addListener (&randomTrigListener);
    }

    for (int i = 0; i < 32; ++i)
        synth.addVoice (new AdditiveVoice (apvts, spectrumEngine, tuning, modEngine));

    // Global app settings (recent .scl files), shared across all instances.
    juce::PropertiesFile::Options o;
    o.applicationName     = "GalacticSting";
    o.filenameSuffix      = "settings";
    o.folderName          = "GalacticSting";
    o.osxLibrarySubFolder = "Application Support";
    appProps = std::make_unique<juce::PropertiesFile> (o);
    recentSclPaths.addLines (appProps->getValue ("recentScl"));
    recentSclPaths.removeEmptyStrings();
    // The keyboard's note-name preference, read back BEFORE any editor is built so the
    // first editor opens already in the state the user left it in - "que se abra ya con
    // ese setting al re-abrirse". Defaults to false, so a fresh install shows numbers.
    keyboardNoteNames = appProps->getBoolValue ("keyboardNoteNames", false);

    // Decode the jingle once, here, so clicking the title never has to touch a
    // decoder on the audio thread. It does NOT start playing - see triggerJingle().
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (
            new juce::MemoryInputStream (BinaryData::jingle_wav, (size_t) BinaryData::jingle_wavSize, false),
            true));
        if (reader != nullptr && reader->lengthInSamples > 0)
        {
            jingleNativeSr = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
            jingleNative.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&jingleNative, 0, (int) reader->lengthInSamples, 0, true, true);
        }
    }
}

void FractalAdditiveAudioProcessor::addRecentScl (const juce::File& f)
{
    const auto path = f.getFullPathName();
    recentSclPaths.removeString (path);
    recentSclPaths.insert (0, path);
    while (recentSclPaths.size() > 64)   // the Recent window lists them all, newest first
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
    // Detach before anything else: the listener holds a raw back-pointer into this
    // object, and a host automation write arriving mid-destruction would follow it.
    if (auto* rp = apvts.getParameter ("specRandom"))
        rp->removeListener (&randomTrigListener);
    randomTrigListener.owner = nullptr;
    // Stop and reap the background workers before members are destroyed.
    chordCancel = true;
    nadCancel   = true;
    adaptCancel = true;
    if (chordThread != nullptr && chordThread->joinable())
        chordThread->join();
    if (nadThread != nullptr && nadThread->joinable())
        nadThread->join();
    if (adaptThread != nullptr && adaptThread->joinable())
        adaptThread->join();
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
    // Harmonics: a continuous fader Even - All - Odd - Prime (default All). Whole
    // values are the classic sets; in between, the harmonics that belong to only
    // one of the two neighbouring sets fade in/out (see spectrum::selectionWeight).
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "harmonics", 1 }, "Harmonics",
        NormalisableRange<float> (0.0f, 3.0f, 0.01f), 1.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "curveSlope", 1 }, "Partial Amp Decay Slope",
        NormalisableRange<float> (0.0f, 4.0f, 0.001f), 1.0f));
    // Continuous prime limit: whole values are the classic limits (Off, 23, 19 ... 2)
    // and the fader snaps to them, but it can rest in between - there the harmonics
    // the NEXT limit would remove fade down instead of disappearing.
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "primeLimit", 1 }, "Prime Limit",
        NormalisableRange<float> (0.0f, 9.0f, 0.01f), 0.0f));
    // Default WHITELIST, not Limit ("por defecto que este en modo whitelist"). It is the
    // mode that SHOWS what it is doing - nine ticks you can read at a glance - where
    // Limit is a single number whose meaning ("largest prime factor allowed") has to be
    // known in advance. It costs nothing to default to: primeOn ships OFF, so neither
    // mode filters anything until the user turns the panel on.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "primeMode", 1 }, "Prime Mode",
        StringArray { "Limit", "Whitelist" }, 1));
    {
        const char* pn[] = { "2","3","5","7","11","13","17","19","23" };
        for (int i = 0; i < spectrum::primeListSize; ++i)
            params.push_back (std::make_unique<AudioParameterBool>(
                ParameterID { "primeWL" + String (i), 1 }, "Prime " + String (pn[i]), true));
    }
    // Continuous fractality: 1..4 with decimals; fractional levels add the next
    // level's NEW partials progressively from LOW to HIGH. Same normalised mapping
    // as the old 1..4 integer param, so saved projects load to the same spot.
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "fractalLevel", 1 }, "Fractal Level",
        NormalisableRange<float> (1.0f, 4.0f, 0.01f), 1.0f));
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "spectrumRange", 1 }, "Spectrum Range (oct)", 1, 10, 10));
    // PHASE, CONTINUOUS. Was a two-way Choice {Aligned, Scattered}; it is now a fader,
    // 0 = every partial at its peak (one pulse), 1 = as scattered as PhaseSets.h can
    // prove is better than the shipping hash.
    //
    // THE ID IS KEPT ON PURPOSE. "phaseMode" is what the mod matrix routes to, what the
    // playground corners capture, and what every automation lane a user has already
    // drawn addresses. Renaming it would silently drop all three - the modulator would
    // point at nothing, the corner would restore a default, and the lane would go dead -
    // and none of those failures announce themselves.
    //
    // AND THE OLD STATE STILL LOADS WITHOUT A MIGRATION STEP, which is worth writing
    // down because it is arithmetic and not luck. APVTS stores the DENORMALISED value
    // (ParameterAdapter::flushToTree writes unnormalisedValue), and AudioParameterChoice
    // denormalises to the choice INDEX over NormalisableRange(0, numChoices - 1). With
    // exactly two choices that range is [0, 1], so an old chunk holds 0.0 for Aligned and
    // 1.0 for Scattered - already this parameter's own values, and already the host's own
    // normalised 0.0/1.0 for any automation lane. FftVerify's phase-state round trip feeds
    // a blob written by the OLD parameter object through setStateInformation and asserts
    // both ends, so if anyone ever widens this range the check fails instead of a user's
    // saved project quietly opening on a different sound.
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "phaseMode", 1 }, "Phase",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    // THERE IS NO "phaseGlide" PARAMETER ANY MORE, AND THAT IS THE POINT.
    //
    // How a MID-NOTE Phase move reaches voices that are already sounding is not a
    // separate choice the user has to keep in sync with anything: it is ALREADY
    // answered by the SPECTRUM panel's transition, because re-phasing IS a spectrum
    // change and those two controls exist to say how a spectrum change is made smooth.
    //   Morph on   -> PITCHBEND: each partial takes a momentary frequency offset until
    //                 it has walked to its new phase. That is literally what Morph does
    //                 to a spectrum change, so Phase borrows it rather than restating it.
    //                 It TRACKS A DRAG continuously (the offset is proportional to how
    //                 fast the fader is moving), and the price is that the offset is an
    //                 offset in Hz, so it is a real interval on a low partial - measured
    //                 and reported by RhythmVerify run 53 rather than assumed away.
    //   Dry/Wet on -> CROSSFADE: the old phases fade out while the new ones fade in, with
    //                 NOTHING DETUNED anywhere - which is the reason to pick it. Again the
    //                 same machine, driven by the same rule. The incoming bank's phases
    //                 may not be re-placed while it is audible, so a drag lands in steps
    //                 of one transition each instead of tracking.
    //   Both on    -> MORPH WINS, exactly as it does for the spectrum itself (see the
    //                 Dry/Wet block below): a voice has ONE crossfade between two banks,
    //                 so there is only one answer available and it is Morph's.
    //   Both off   -> no live re-phasing at all; the new phases take effect at the next
    //                 note-on - AND SO DOES THE LEVEL. This line used to end "the level
    //                 still follows the fader immediately, because the normalisation law
    //                 is blended live", and that was the bug the user found by playing
    //                 it: "el volumen aumenta pero no se produce el efecto o no del
    //                 todo". The normalisation exists to compensate the crest factor the
    //                 phases produce, so moving it while the phases stand still is not a
    //                 compensation at all, it is arbitrary loudness. A held note's level
    //                 now moves when, and only when, its phases do.
    // and the DURATION is that same transition's Time fader (morphTime / dryWetTime).
    //
    // WHY DELETING IT IS SAFE FOR PROJECTS SAVED BY YESTERDAY'S BUILD: APVTS reads the
    // state by asking each parameter it OWNS for its child node, so a child naming a
    // parameter that no longer exists is never looked at and is dropped on the next
    // save. FftVerify's phase-state round trip feeds a chunk that still carries a
    // <PARAM id="phaseGlide"> child through setStateInformation and asserts phaseMode
    // survives it, so this stays a checked claim rather than a remembered one.

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
        ParameterID { "ampDecay", 1 }, "Amp Decay", timeRange (20.0f), 0.120f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "ampSustain", 1 }, "Amp Sustain",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.80f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "ampRelease", 1 }, "Amp Release", timeRange (30.0f), 0.300f));
    // Segment shapes for the three voice envelopes (0 = straight, + convex, - concave)
    // - the same family the modulators and the envelope editor use.
    {
        auto curve = [] { return NormalisableRange<float> (-1.0f, 1.0f, 0.001f); };
        for (const char* pfx : { "amp", "filter", "pitch" })
        {
            const String base (pfx);
            const String nice = base.substring (0, 1).toUpperCase() + base.substring (1);
            params.push_back (std::make_unique<AudioParameterFloat>(
                ParameterID { base + "CurveA", 1 }, nice + " Attack Curve", curve(), 0.0f));
            params.push_back (std::make_unique<AudioParameterFloat>(
                ParameterID { base + "CurveD", 1 }, nice + " Decay Curve", curve(), 0.55f));
            params.push_back (std::make_unique<AudioParameterFloat>(
                ParameterID { base + "CurveR", 1 }, nice + " Release Curve", curve(), 0.55f));
        }
    }

    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "masterGain", 1 }, "Master Gain",
        // +6 dB at the top used to be the ceiling - even maxed out, the synth read
        // noticeably quieter than reference plugins. Raised to +12 dB: the default
        // (0 dB) is untouched, this only gives more room to push into on purpose.
        NormalisableRange<float> (-60.0f, 12.0f, 0.01f), 0.0f));

    //-- MODULATORS: 3 custom envelopes + 3 LFOs, each mappable onto ANY number of
    // parameters (right-click a fader -> "Map to..."). They ride ON TOP of the
    // parameter's own value over +-Range and are stepped per audio block.
    for (int n = 1; n <= 3; ++n)
    {
        const auto pfx = "modE" + String (n);
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "A", 1 }, "Env " + String (n) + " Attack", timeRange (5.0f), 0.100f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "D", 1 }, "Env " + String (n) + " Decay", timeRange (20.0f), 0.300f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "S", 1 }, "Env " + String (n) + " Sustain",
            NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.70f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "R", 1 }, "Env " + String (n) + " Release", timeRange (30.0f), 0.400f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "Range", 1 }, "Env " + String (n) + " Range",
            NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.5f));
        // Segment SHAPE: 0 = straight line, negative = concave, positive = convex.
        // The decay defaults convex, which is how a natural decay actually falls
        // (fast at first, long tail) - a straight ramp reads as an abrupt drop.
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "CurveA", 1 }, "Env " + String (n) + " Attack Curve",
            NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "CurveD", 1 }, "Env " + String (n) + " Decay Curve",
            NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.55f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "CurveR", 1 }, "Env " + String (n) + " Release Curve",
            NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.55f));
        params.push_back (std::make_unique<AudioParameterBool>(
            ParameterID { pfx + "On", 1 }, "Env " + String (n) + " On", true));
    }
    for (int n = 1; n <= 3; ++n)
    {
        const auto pfx = "modL" + String (n);
        params.push_back (std::make_unique<AudioParameterChoice>(
            ParameterID { pfx + "Wave", 1 }, "LFO " + String (n) + " Wave",
            StringArray { "Sine", "Triangle", "Saw up", "Saw down", "Square", "Custom", "Random" }, 0));
        // Rate: BPM-divided / fundamental-divided (default - follows the lowest held
        // note) / free Hz. Div halves the rate per step (deep enough to bring an
        // audio-rate fundamental down to slow sweeps).
        params.push_back (std::make_unique<AudioParameterChoice>(
            ParameterID { pfx + "RateMode", 1 }, "LFO " + String (n) + " Rate Mode",
            StringArray { "BPM / 2^n", "Fundamental / 2^n", "Free Hz" }, 1));
        params.push_back (std::make_unique<AudioParameterInt>(
            ParameterID { pfx + "Div", 1 }, "LFO " + String (n) + " Divide", 0, 24, 8));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "Hz", 1 }, "LFO " + String (n) + " Hz",
            NormalisableRange<float> (0.01f, 30.0f, 0.001f, 0.4f), 1.0f));
        // Phase: restart at every note-on (default) or locked to the host transport.
        params.push_back (std::make_unique<AudioParameterChoice>(
            ParameterID { pfx + "PhaseMode", 1 }, "LFO " + String (n) + " Phase Mode",
            StringArray { "Note restart", "Project transport" }, 0));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "Phase", 1 }, "LFO " + String (n) + " Phase",
            NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { pfx + "Range", 1 }, "LFO " + String (n) + " Range",
            NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.5f));
        params.push_back (std::make_unique<AudioParameterBool>(
            ParameterID { pfx + "On", 1 }, "LFO " + String (n) + " On", true));
    }

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
        ParameterID { "filterDecay", 1 }, "Filter Decay", timeRange (20.0f), 0.200f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterSustain", 1 }, "Filter Sustain",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 1.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "filterRelease", 1 }, "Filter Release", timeRange (30.0f), 0.300f));

    //-- Pitch envelope (bends the played pitch, in semitones) ------------------
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchEnvAmount", 1 }, "Pitch Env Amount (st)",
        NormalisableRange<float> (-48.0f, 48.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchAttack", 1 }, "Pitch Attack", timeRange (5.0f), 0.005f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchDecay", 1 }, "Pitch Decay", timeRange (20.0f), 0.150f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchSustain", 1 }, "Pitch Sustain",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));   // min by default
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pitchRelease", 1 }, "Pitch Release", timeRange (30.0f), 0.001f)); // min by default

    //-- Manual per-partial amplitudes. While this is on the user is drawing the
    //   spectrum by hand, so SLOPE stops having any effect: it is frozen at whatever it
    //   was when manual was switched on, which means nothing jumps at the moment of
    //   switching and the fader simply stops doing anything until manual is off again.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "partialManual", 1 }, "Manual Partial Amps", false));

    //-- Spectral morphing (glide partials to their new positions on a spectrum change)
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "morph", 1 }, "Morphing", false));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "morphTime", 1 }, "Morph Time", timeRange (5.0f), 0.300f));

    //-- DRY/WET TRANSITION: the OTHER way to make a spectrum change smooth, and a
    //   deliberately independent one. Morph glides partial FREQUENCIES from where they are
    //   to where they are going (a per-partial pitch bend); this CROSSFADES - the previous
    //   spectrum fades out while the new one fades in, simultaneously, over this time, with
    //   no frequency movement at all. It is the alternative the user asked for ("es una
    //   alternativa al pitch bending de morph"), not a refinement of it.
    //
    //   THE TWO CANNOT BOTH ACT. A voice has exactly one crossfade machine - the
    //   phase[]/alignPhase[] bank pair with a single weight (see AdditiveVoice.h) - and
    //   both features are that machine driven by different rules. MORPH WINS: with Morph on,
    //   this is inert and the panel greys it out and says why, rather than leaving the user
    //   to believe two smoothings are running. Same range and same skew as Morph Time, so
    //   the fader feels identical.
    //
    //   IT DEFAULTS TO ON, and Morph does not: "haz que la default sea drywet". The panel
    //   asks ONE question now - TRANSITION: Off / Morph / Dry/Wet - and this pair of
    //   booleans is what that question is stored in (see the combo in PluginEditor.cpp),
    //   so "the default option is Dry/Wet" is exactly this flag starting true. A brand new
    //   instance therefore CROSSFADES its spectrum changes instead of snapping them.
    //
    //   OLD PROJECTS ARE NOT DRAGGED WITH IT. APVTS leaves a parameter whose <PARAM> child
    //   is missing from the loaded chunk at whatever it currently holds - which on a fresh
    //   instance is this default - so a project saved before this feature existed would
    //   silently open with a 0.3 s crossfade on every spectrum change it never had. The
    //   absent child is exactly what marks it as older than the feature, and restoreState
    //   turns the flag back off when it sees that. Measured there, not assumed.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "dryWet", 1 }, "Dry/Wet Transition", true));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "dryWetTime", 1 }, "Dry/Wet Time", timeRange (5.0f), 0.300f));

    //-- Tuning ----------------------------------------------------------------
    // Index 0 (plain 12ED2) is no longer OFFERED - MTS-ESP already falls back to
    // 12-TET on its own when no master is connected, so it is the honest default
    // and 12ED2 became a redundant duplicate of that fallback. The entry stays in
    // the list so projects saved with it still restore to exactly what they had.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "tuningMode", 1 }, "Tuning",
        StringArray { "12ED2", "MTS-ESP", "Scala (.scl)", "MPE (bend input)" }, 2));
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

    //-- MONOPHONIC GLIDE (Pitch tab) ------------------------------------------
    // "al activarlo el sinte no puede producir acordes pero al pulsar una nota y luego
    //  otra se hace pitchbend de una a la otra en un tiempo especificado en ms".
    // ON drops the synth - and every playground MIX sub-engine - to ONE voice, so a new
    // note-on TAKES OVER the sounding one instead of adding to it, and the voice bends
    // its pitch from the previous note's tuned frequency to the new one's. Last-note
    // priority, no envelope retrigger while a key is held, and a release returns to the
    // most recent key still down (see MonoGlide.h and AdditiveVoice::noteStarted).
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "monoGlide", 1 }, "Monophonic Glide", false));
    // The portamento's DURATION, log-mapped over 1 ms .. 5 s. 1 ms rather than 0 because
    // the mapping is logarithmic and because "the shortest glide there is" is a more
    // useful bottom than a dead zone: at 1 ms the takeover is already indistinguishable
    // from a jump, so no value is wasted. The centre sits at 100 ms, which is where the
    // musically useful range lives - the fader spends half its travel between 1 and 100.
    {
        NormalisableRange<float> r (1.0f, 5000.0f, 0.1f);
        r.setSkewForCentre (100.0f);
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { "glideTime", 1 }, "Glide Time (ms)", r, 120.0f));
    }

    //-- SHEPARD MODE (Tuning panel) -------------------------------------------
    // A PLAYED mode, not a generator: the key you press becomes the root of an
    // endlessly rising tone built from THIS synth's spectrum (see ShepardEngine.h
    // and AdditiveVoice). Default OFF, so nothing changes until it is asked for.
    // All four are automatable, and all four are read per VOICE - so a playground
    // corner whose preset is a Shepard patch really sounds like one.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "shepOn", 1 }, "Shepard Mode", false));
    // ONE rate control, two dresses: FREE = the ms fader below; SYNC = a musical
    // duration against the host BPM (the beats list). SYNC IS THE DEFAULT: a rise
    // that drifts against the track is the thing people fix first, and with no
    // transport BPM the free value takes over anyway and the editor says so (see
    // shepardSyncHasNoBpm).
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "shepMode", 1 }, "Shepard Rate Mode",
        StringArray { "Free (ms)", "Sync Project Tempo" }, 1));
    // Time to rise ONE interval, log-mapped over 100 ms .. 60 s.
    {
        NormalisableRange<float> r (100.0f, 60000.0f, 1.0f);
        r.setSkewForCentre (2449.5f); // sqrt(100*60000): log-shaped travel
        params.push_back (std::make_unique<AudioParameterFloat>(
            ParameterID { "shepTime", 1 }, "Shepard Rise Time (ms)", r, 8000.0f));
    }
    // The beats list mirrors shepard::kDivBeats ENTRY FOR ENTRY - the index is the
    // contract between this choice and beatsForDiv(), and a list edited on one side
    // only would silently re-time every saved project.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "shepDiv", 1 }, "Shepard Rise Beats",
        StringArray { "1/16", "1/8", "1/4", "1/3", "1/2", "2/3", "1", "2", "3", "4", "8", "16" }, 9));
    // WHAT ONE RISE STEP IS. An OCTAVE by default in every tuning - that is what a
    // listener hears as "the same note again", and silently rising by 3/1 because a
    // Bohlen-Pierce scale happens to be loaded would be a tuning decision nobody
    // asked for. The tuning's own period is offered instead only when it differs
    // from 2/1, and the editor hides the control entirely when it does not.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "shepRise", 1 }, "Shepard Rise Interval",
        StringArray { "Octave pitch bend", "Period pitch bend" }, 0));

    //-- Spectrum -> rhythm MIDI out --------------------------------------------
    // Each selected partial pulses at f0 * ratio / x^n; all of these are automatable.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmOn", 1 }, "Rhythm MIDI On", false));
    // "Play in Fibló": the rhythm MIDI is interpreted by THIS synth (the held note
    // itself stays silent, only the rhythmic pulses sound). Independent of MIDI-out.
    // ON by default: routing a partial then plays the rhythm right here; disable it to
    // use MIDI-out only. It only takes effect once at least one partial is routed, so
    // the synth still plays normally out of the box.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmSelfPlay", 1 }, "Rhythm Play In Synth", true));
    // "Note under pulses": when self-play is on, ALSO sound the held note normally
    // underneath the pulses (off = only the rhythm sounds).
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmDrone", 1 }, "Rhythm Note Under Pulses", false));
    // Internal FEEDBACK loop (self-play): every pulse spawns a decaying burst of
    // repeats ("pulses of the pulse"), each quieter by the slope. Bounded (velocity
    // floor + loop cap) so it always converges - never a runaway.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmFeedback", 1 }, "Rhythm Feedback", false));
    // The volume slope: fraction of velocity kept per loop. 0.5 = halve each repeat
    // (short tail); higher = longer, louder tail.
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "rhythmVelSlope", 1 }, "Rhythm Velocity Slope",
        NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "rhythmFeedbackDecay", 1 }, "Rhythm Feedback Decay",
        juce::NormalisableRange<float> (0.05f, 0.95f, 0.01f), 0.5f));
    // Global octave shift of ALL pulses at once (+-4 octaves).
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "rhythmOctAll", 1 }, "Rhythm Pulses Octave", -4, 4, 0));
    // Global steps offset on every step-pitched pulse (modulatable; JI/ratio slots
    // are unaffected because they have no step to shift).
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "rhythmStepsAll", 1 }, "Rhythm Pulses Steps",
        NormalisableRange<float> (-24.0f, 24.0f, 1.0f), 0.0f));
    // Note-gate modes (all default OFF, independent):
    //  - Quantize to project tempo: a note may only START on a beat of the host BPM.
    //  - Mono: a new note stops the one currently sounding.
    //  - Quantize to pulse: a new note waits for the moment the running polyrhythm
    //    pattern resets (all subdivisions coincide on the 1) and joins there.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmQTempo", 1 }, "Quantize To Project Tempo", false));
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmMono", 1 }, "Mono Mode", false));
    // ON by default: it only acts while a pattern is actually running, and joiners
    // within ~120 ms enter immediately, phase-aligned - so it never eats clicks.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmQPulse", 1 }, "Quantize To Pulse", true));
    // Default: divide 8 times by 2 (x^n = 2^8 = 256). Base 1 is not offered - it
    // turned the pulses into an audio-rate MIDI flood.
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "rhythmDivX", 1 }, "Rhythm Divide Base (x)", 2, 12, 2));
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "rhythmDivN", 1 }, "Rhythm Divide Times (n)", 1, 16, 8));
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "rhythmMode", 1 }, "Rhythm Mode",
        StringArray { "Lowest note", "All notes" }, 1)); // default: every held note drives its trains
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmMpe", 1 }, "Rhythm MPE Out", false));
    // DAW-tempo sync toggle: a persisted parameter (not editor-only state) so it
    // survives the editor being closed/reopened and project reloads - it was
    // resetting itself when Ableton re-created the plugin editor on record/play.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmTempoSync", 1 }, "Rhythm Tempo Sync", false));
    // The x2 / /2 octave scaling of the synced tempo. A PARAMETER (not editor-only
    // state) because the tempo is now driven by the processor, so it must work with
    // the window closed - and it survives a project reload as a bonus.
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "rhythmTempoOct", 1 }, "Rhythm Tempo Octave", -6, 6, 0));
    // The rhythm panel's JI switch, and whether IT was the one that turned MPE out on.
    // Both were editor-only, so the effect (slots on their partial's just ratio)
    // persisted while the button did not: a reopened window read "JI: OFF" over JI
    // pulses, newly routed partials silently got scale steps instead of ratios, and
    // the "put MPE back as I found it" branch was disarmed.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmJi", 1 }, "Rhythm JI", false));
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "rhythmJiForcedMpe", 1 }, "Rhythm JI Forced MPE", false));

    // Temper the HARMONIC ENGINE's partials to the ACTIVE TUNING's grid (whatever
    // is loaded in the Tuning box), followed live: with this ON both engines work
    // together (the engine generates, the tuning tempers) and the Custom Engine's
    // own scale is bypassed. The id stays "timbreTemper" so old projects load.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "timbreTemper", 1 }, "Temper To Tuning", false));
    // Custom Engine on/off. A parameter (not plain state) so it is automatable and
    // Ctrl+Z undoes switching it, like every other control.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "timbreOn", 1 }, "Custom Engine On", true));
    // RANDOM SPECTRUM: a TRIGGER, not a state. Its RISING edge (0 -> 1) generates a
    // fresh random custom spectrum; the level itself means nothing. A momentary button
    // with no parameter behind it could not be automated at all, and a parameter whose
    // VALUE was the spectrum would be a lie (there is no continuum from one random
    // spectrum to the next) - so the host drives it the way it drives any other trigger:
    // draw a 0->1 step wherever a new spectrum should land, and each step fires once.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "specRandom", 1 }, "Random Spectrum (trigger)", false));

    //-- Scala-as-timbre --------------------------------------------------------
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "timbreRepeats", 1 }, "Timbre Period Repeats", 1, 8, 3));

    //-- PRESET PLAYGROUND: the blend point, as two real parameters --------------
    // The point in the rectangle IS these two numbers, which is what makes the whole
    // feature automatable: dragging it writes them through a proper gesture so the host
    // records the move, and playing that automation back moves the point (and everything
    // it drives) with the window open OR shut - the window only ever draws them.
    // (0,0) is the top-left corner, so a playground at rest sounds exactly like the
    // preset the user dropped on corner A rather than some blend nobody chose.
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pgX", 1 }, "Playground X",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "pgY", 1 }, "Playground Y",
        NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
    // The playground's MODE is a parameter too, so a DAW can switch morph <-> mix
    // under automation (and the choice survives save/reload like everything else).
    // Excluded from the morph itself - see PresetMorph.h - or dropping a preset saved
    // in mix mode onto a corner would flip the playground under the user's feet.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "pgMode", 1 }, "Playground Mode",
        StringArray { "Morph", "Mix" }, 0));
    // MIX runs up to FIVE engines at once (four corners + the main patch tracking
    // underneath), and the measured worst case (32 voices each, dense spectra) is ~3x
    // a 48 kHz / 512 block - so the per-corner polyphony is a VISIBLE control, never a
    // silent cap. 8 is the honest default: normal playing never steals under it, and
    // the measured realistic cost fits the block budget; anyone with the CPU can push
    // it to the main engine's own 32.
    params.push_back (std::make_unique<AudioParameterInt>(
        ParameterID { "pgVoices", 1 }, "Mix Voices per Corner", 1, 32, 8));

    // PRIME CONTROL, ON AND OFF. "PON UN BOTON DE ON/OFF... POR DEFECTO WHITELIST Y
    // APAGADO."
    //
    // Until now the only way to have no prime filtering was to know that the LIMIT
    // fader's leftmost stop is called "Off" - which does nothing at all for the OTHER
    // mode, because a whitelist with every prime ticked is still a filter: harmonic 29
    // has a prime factor nobody can tick, and it was being dropped by a control the user
    // never touched. So the panel gets its own switch, and OFF means the spectrum sees
    // no prime filter of any kind (see prepareSettings, where it forces the neutral
    // state rather than relying on any particular fader position).
    //
    // Appended at the END of the list on purpose: a host's automation lanes are indexed,
    // and inserting this next to its neighbours would renumber every parameter after it.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "primeOn", 1 }, "Prime Control On", false));

    // WHICH WAY IT GOES. "Haz que el shepard pueda ser ascending o descending pulsando un
    // boton con flecha que al pulsarlo pasa de hacia arriba a hacia abajo."
    //
    // A Shepard tone that FALLS for ever is the same illusion run backwards and it is the
    // half of the effect this synth could not make: the classic demonstration is a pair,
    // and a descending one is what a riser is answered with. It is one sign on the rise
    // rate - the grid, the window and the band are all symmetric, and the copies leave
    // through the zero at whichever edge they reach - so this costs the engine a negation
    // and nothing else.
    //
    // Appended at the END of the parameter list, like every addition here: a host's
    // automation lanes are indexed, and inserting it beside its four Shepard neighbours
    // would renumber every parameter after it in projects people have already saved.
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "shepDown", 1 }, "Shepard Descending", false));

    return { params.begin(), params.end() };
}

//==============================================================================
spectrum::SpectrumSettings FractalAdditiveAudioProcessor::currentSettings() const
{
    // Compile at the LOWEST slope the modulation can REACH, not at the knob's value.
    //
    // Each voice rescales the published amplitudes from the template's slope to its own,
    // which is exact - but only for partials the template actually contains, and the
    // amplitude floor culls by an amplitude that DOES depend on slope (at -120 dB it
    // removes everything above ratio 10^(6/s): nothing at slope 1, but only up to ratio
    // ~32 at slope 4). A template compiled steep therefore physically lacks the partials
    // a shallower voice would need, and the fidelity rule is that nothing goes missing.
    // With nothing mapped the reachable minimum IS the knob value, so this costs exactly
    // nothing until a modulator is pointed at slope.
    float slope;
    if (apvts.getRawParameterValue ("partialManual")->load() > 0.5f)
    {
        // MANUAL mode: the amplitudes belong to the user's per-partial edits, so slope
        // is frozen where it stood when manual was switched on. Freezing rather than
        // zeroing is what keeps the sound identical at the moment of switching - the
        // curve slope was drawing simply becomes the starting point of the drawing.
        slope = manualSlopeFrozen;
    }
    else
    {
        auto* p = apvts.getParameter ("curveSlope");
        float lo = 0.0f, hi = 0.0f;
        modEngine.modulationSpan ("curveSlope", lo, hi);
        slope = p != nullptr
              ? p->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, p->getValue() + lo))
              : apvts.getRawParameterValue ("curveSlope")->load();
    }

    return settingsFromValues ([this] (const char* id)
                               { return apvts.getRawParameterValue (id)->load(); },
                               timbreDegrees, timbrePeriod, slope);
}

// See the contract in PluginProcessor.h: the ONE place a set of parameter values plus a
// custom scale becomes SpectrumSettings, shared by the live patch and by every preset
// sitting on a playground corner.
spectrum::SpectrumSettings FractalAdditiveAudioProcessor::settingsFromValues (
    const std::function<float (const char*)>& get,
    const std::vector<float>& degrees, float period, float slope) const
{
    spectrum::SpectrumSettings s;
    s.baseN      = (int) get ("baseN");
    s.baseMode   = (spectrum::BaseMode) (int) get ("baseMode");
    s.selection  = get ("harmonics");
    s.curveSlope = slope;

    // Prime limit is a CONTINUOUS position on the Off - 23 - 19 ... - 2 fader: the
    // whole value below is the limit in force, the one above is the next stop, and
    // the fraction between them fades out the harmonics that stop would remove.
    const float plPos = juce::jlimit (0.0f, 9.0f, get ("primeLimit"));
    const int plIdx   = juce::jlimit (0, 9, (int) std::floor (plPos));
    s.primeLimitPos  = plPos;
    s.primeLimit     = primeLimitValues[plIdx];
    s.primeLimitNext = primeLimitValues[juce::jmin (9, plIdx + 1)];

    s.primeUseWhitelist = get ("primeMode") > 0.5f;
    unsigned mask = 0;
    for (int i = 0; i < spectrum::primeListSize; ++i)
        if (get (("primeWL" + juce::String (i)).toRawUTF8()) > 0.5f)
            mask |= (1u << i);
    s.primeWhitelistMask = mask;

    // OFF MEANS OFF, in both modes and whatever the fader and the ticks happen to say.
    // Written as the NEUTRAL SETTINGS rather than as a branch inside the generator, so
    // exactly one place decides what "no prime filter" is: limit 0 (which primeLimitOk()
    // passes unconditionally), no blend towards a next stop, and the whitelist path not
    // taken. Nothing downstream has to know the switch exists.
    if (get ("primeOn") <= 0.5f)
    {
        s.primeUseWhitelist  = false;
        s.primeLimit         = 0;
        s.primeLimitNext     = 0;
        s.primeLimitPos      = 0.0f;
        s.primeWhitelistMask = 0xFFFFFFFFu;
    }

    s.fractalLevel   = get ("fractalLevel");
    s.spectrumRangeOct = (int) get ("spectrumRange");
    s.ampFloorDb     = -120.0f;
    s.maxPartials    = spectrum::kMaxPartials;

    // TEMPER mode: the harmonic engine keeps running and its partials snap to the
    // grid of the ACTIVE TUNING (whatever is loaded in the Tuning box - .scl, MTS,
    // MPE...), read live every poll so tuning changes retune the spectrum in real
    // time. Independent of the Custom Engine's own loaded scale.
    // The branch is engineInForce()'s rule, applied to whichever patch is being asked
    // about: the UI names the engine from that helper, so a spectrum built here by other
    // rules would be labelled wrongly on screen (that is how temper mode came to be
    // invisible). Written out rather than calling engineInForce() because a playground
    // corner's engine is decided by ITS values, not by the live ones.
    const bool temper   = get ("timbreTemper") > 0.5f;
    const bool customOn = ! degrees.empty() && get ("timbreOn") > 0.5f;
    // Temper's grid is "degree x period^k", so it EXISTS only when the tuning states a
    // period that ascends. hasUsablePeriod() is asked instead of periodRatio() because
    // that one answers 2.0 for a tuning with no period at all (it feeds displays, which
    // always need a number) - and snapping to that 2.0 tempers the spectrum to an octave
    // the user's file never contained. A descending .scl and a degenerate period-1 scale
    // are both legal and both hit it. So temper REFUSES here: the harmonic engine runs
    // UNTEMPERED, no partial moves, and the toggle on screen says why (see the editor's
    // temperToggle). Falling through to the Custom Engine instead would be a second
    // silent substitution, so it is deliberately not done.
    const bool canTemper = tuning.hasUsablePeriod();
    if (temper && canTemper)
    {
        s.source         = spectrum::Source::Harmonic;
        s.temperToTimbre = true;
        const int D = juce::jmax (1, tuning.degreesPerPeriod()); // no cap: every degree is a grid point
        s.timbreDegrees.clear();
        s.timbreDegrees.reserve ((size_t) D);
        for (int d = 0; d < D; ++d)
            s.timbreDegrees.push_back ((float) getDegreeRatio (d));
        s.timbrePeriod = (float) tuning.statedPeriodRatio(); // the tuning's OWN period, never a fallback
    }
    else if (temper)
    {
        s.source         = spectrum::Source::Harmonic;
        s.temperToTimbre = false; // refused, and visibly so - never quietly tempered to 2/1
    }
    else if (customOn)
    {
        s.source              = spectrum::Source::ScalaTimbre;
        s.timbreDegrees       = degrees;
        s.timbrePeriod        = period;
        s.timbrePeriodRepeats = (int) get ("timbreRepeats");
    }
    else
    {
        s.source = spectrum::Source::Harmonic;
    }
    return s;
}

void FractalAdditiveAudioProcessor::timerCallback()
{
    // Catch the moment manual mode is switched on and FREEZE slope there, before
    // currentSettings() below reads it. Everything the user had is preserved: the sound
    // does not move by a single dB at the switch, and from then on the slope fader is
    // inert until manual goes off again.
    {
        const bool man = apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
        if (man != lastManualMode)
        {
            if (man) manualSlopeFrozen = apvts.getRawParameterValue ("curveSlope")->load();
            lastManualMode = man;
            // ...and hand the engine the switch's new state. It only learns whether the
            // edits are in force through setOverrides, so with nothing pushing here the
            // parameter could go ON with a whole hand-drawn spectrum stored and NOTHING
            // would happen - the switch looked broken. Also the reverse: OFF has to
            // release the edits so the compiled amplitudes come back.
            setPartialGains (partialGains, man);
        }
    }

    // THE PRESET PLAYGROUND. Driven from here, not from the window, for the same reason
    // every other automatable thing in this plugin is: pgX/pgY are parameters, and
    // automation has to move the point (and everything it blends) with the editor shut.
    // It runs BEFORE the ordinary rebuild test below, and leaves lastPushed describing
    // the morphed parameters, so the two never publish over each other.
    drivePlayground();

    const auto s = currentSettings();
    if (s != lastPushed)
    {
        // While the playground is engaged IN MORPH MODE the SPECTRUM comes from the
        // blend of the corners' compiled partial lists, which is the only thing that
        // means anything when two corners run different engines. Compiling the morphed
        // parameters here as well would publish that over the blend a moment later.
        // MIX mode is different: the live spectrum still belongs to the live patch
        // (the corners sound through their own engines), so an edit made while the
        // mix plays must keep rebuilding it - it is what comes back on disengage.
        if (! pgActive || pgMixDriving)
            spectrumEngine.requestRebuild (s);
        lastPushed = s;
    }

    // RANDOM SPECTRUM: the rising edge the listener caught (a click, or host
    // automation stepping 0 -> 1) is spent HERE, on the message thread, because
    // building the spectrum parses text and republishes the template. In the processor
    // rather than the editor so an automated trigger fires with the window shut - a
    // trigger that only worked while someone was looking would not be automatable.
    pollRandomSpectrumTrigger();

    // MONOPHONIC GLIDE: the voice pool follows the switch. Here rather than in the
    // editor because it is an automatable parameter and a project can arrive with it
    // already on - the pool has to be one voice before the first note is played, not
    // once somebody opens the window.
    {
        const bool mg = apvts.getRawParameterValue ("monoGlide")->load() > 0.5f;
        if (mg != lastMonoGlide)
        {
            lastMonoGlide = mg;
            monoShrinkTicks = 0;
            applyMonoVoiceCount (mg);
        }
        else if (synth.getNumVoices() > (mg ? 1 : kPolyVoices))
        {
            // A shrink thrown under a held chord does not finish in one go: the notes it
            // has to drop are faded first and the pool follows only once they are quiet
            // (see applyMonoVoiceCount). This is the later tick that finishes it, and it
            // costs one integer compare on every other tick there has ever been.
            applyMonoVoiceCount (mg);
        }
    }

    // Spectrum <-> tuning REALTIME links (throttled + coalesced; refuses to close a
    // feedback loop). Here, not in the editor, so they keep working with the window shut.
    driveRealtimeLink();
    // ...and the lookahead budget, which the link above can have just switched off by
    // refusing itself (a feedback-loop refusal ends the link, so the latency must go
    // with it instead of being paid for a follower that is no longer running).
    applyLookahead();

    // Segment scale tracks the timbre live: whenever the spectrum template changes
    // (Base N, fractal, a completing morph...) OR after a project reload, rebuild it
    // from the current partials so the scale + "from n/1" mapping stay correct.
    if (segmentModeActive && ! segSel.empty())
        if (const auto* lt = spectrumEngine.getLive(); lt != nullptr && lt->generation != segRebuiltGen)
            rebuildSegmentScale (false); // auto-refresh: never steals the tuning mode

    // Tuning-bank slot switched (UI or host automation): load that slot's .scl.
    // Runs on the message thread (timerCallback), so the parse/load is safe here.
    {
        const int slot = getTuningBankSlotParam();
        const bool inRange = slot >= 1 && slot <= kBankSlots;
        const juce::String src  = inRange ? bankSource[(size_t) (slot - 1)] : juce::String();
        const juce::String name = inRange ? bankName  [(size_t) (slot - 1)] : juce::String();

        // A CHANGE OF SLOT, OR A FAILED SLOT WHOSE TEXT HAS MOVED - and nothing else.
        //
        // `lastBankSlot = slot` used to be the first statement in this block, so the slot
        // was marked applied before the load had run. A slot whose stored text no longer
        // parses then switched the SELECTOR and not the TUNING: the combo read "31-EDO",
        // the instrument went on playing whatever it had, `err` was dropped unread on the
        // next line, and nothing anywhere said a word. For a microtonal instrument that is
        // the quiet failure that matters - everything still sounds, and it sounds wrong.
        //
        // The sticky index then blocked every retry, which is the same bug from the other
        // side. Picking that slot again is not a change, so it did nothing; and neither did
        // the obvious repair of dropping a good file into it, because setTuningBankFile ends
        // with setTuningBankSlotParam(slot0 + 1), a no-op when that slot is already
        // selected. The replacement text sat in the bank, correct and unloaded, with the
        // broken tuning still under the keys.
        //
        // SO THE TEXT IS ONLY CONSULTED FOR A SLOT WHOSE LOAD FAILED, and that narrowness
        // is measured, not cautious. Testing the text unconditionally re-loads a slot
        // whenever its stored source changes under the same index - which is exactly what
        // the plugin itself does when a tuning is edited and re-stored - and that stomps the
        // mode back to Scala on top of whatever is active. syncBankSelector documents that
        // very hazard two hundred lines up and sets lastBankSlot FIRST to dodge it; a wider
        // test here walks straight past that guard. Measured on the full suite: 8 checks in
        // run 14 and three other runs went red, all of them a tuning coming back after a
        // clearScala or a mode change.
        if (slot != lastBankSlot || (lastBankFailed && src != lastBankSource))
        {
            lastBankSlot   = slot;
            lastBankSource = src;
            lastBankFailed = false;
            if (src.isNotEmpty())
            {
                juce::String err;
                // loadTuningString, not loadScalaString: a bank slot may hold .tun text
                // (a non-repeating .tun has no .scl form), and the .scl parser would
                // reject it and leave the previous tuning sounding under the new name.
                if (tuning.loadTuningString (src, name, err))
                {
                    if (auto* pm = apvts.getParameter ("tuningMode"))
                        pm->setValueNotifyingHost (pm->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
                    clearSegmentSource(); // a bank .scl replaces any segment scale
                }
                else
                {
                    // A SLOT THAT DOES NOT LOAD SAYS SO, and says the parser's own reason.
                    // Parked rather than shown from here: this poll runs whether or not a
                    // window exists (host automation moves the slot parameter with the
                    // editor shut), so the editor collects it on its next tick through
                    // takeBankSlotError - the same one-shot route a project that loses its
                    // scale already takes.
                    lastBankFailed = true;
                    bankSlotError  = name.isNotEmpty() ? name + " \xe2\x80\x94 " + err : err;
                }
            }
        }
    }

    // ONE-SHOT MTS-ESP adoption, exactly like the jingle's single trigger: if a
    // master is up when this Fibló loads, it opens following it. It is consumed
    // here whether or not a master was found, so switching to another mode later
    // can never be undone by a second check - there is no second check.
    //
    // It fires ONLY for a genuinely NEW instance: setStateInformation disarms it, so a
    // restored project or preset keeps whatever the user saved.
    //
    // The wait matters. This timer is started by prepareToPlay, and a host is free to
    // call prepareToPlay BEFORE it hands over the saved state - so deciding on the very
    // first tick could adopt MTS-ESP a moment before the project arrives to say this
    // instance already has a tuning of its own. Half a second is far beyond any host's
    // hand-over and is only ever waited once, at load.
    if (mtsCheckPending && ++mtsCheckTicks >= 30)
    {
        mtsCheckPending = false;
        if (tuning.hasMtsMaster())
            if (auto* pm = apvts.getParameter ("tuningMode"))
                pm->setValueNotifyingHost (pm->convertTo0to1 ((float) (int) TuningManager::Mode::MtsEsp));
    }

    const int tmode = (int) apvts.getRawParameterValue ("tuningMode")->load();
    applyTuningMode (tmode);

    // The REFERENCE belongs to MTS-ESP while - and only while - that is the mode.
    // A master already bakes its own reference into the frequencies it hands out,
    // so applying the user's refA on top would double it. In MTS mode the box
    // therefore SHOWS what the master is really doing and the multiplier is left
    // at 1; the hand-set value is parked and handed straight back on the way out,
    // so MTS can never permanently overwrite a reference the user typed.
    const bool mtsOwnsRef = tmode == (int) TuningManager::Mode::MtsEsp && tuning.hasMtsMaster();
    if (mtsOwnsRef)
    {
        if (! mtsHadRef) { manualRefA = apvts.getRawParameterValue ("refA")->load(); mtsHadRef = true; }
        tuning.setReferenceA (440.0); // no multiplier: the master's own tuning is absolute
        const double mtsA = tuning.mtsFrequencyForKey (69);
        if (mtsA > 0.0 && std::abs (mtsA - (double) apvts.getRawParameterValue ("refA")->load()) > 0.001)
            if (auto* p = apvts.getParameter ("refA"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) mtsA)); // display only
    }
    else
    {
        if (mtsHadRef) // just left MTS (or the master went away): give the user theirs back
        {
            mtsHadRef = false;
            if (auto* p = apvts.getParameter ("refA"))
                p->setValueNotifyingHost (p->convertTo0to1 (manualRefA));
        }
        tuning.setReferenceA ((double) apvts.getRawParameterValue ("refA")->load());
    }
    tuning.setOctaveShift ((int) apvts.getRawParameterValue ("octaveShift")->load());

    // DAW-tempo sync (CC20). This lives HERE, not in the editor: driven from the
    // editor's timer it only worked while the plugin window was open and had been
    // clicked, so the project tempo silently stopped following as soon as you looked
    // away. The processor's timer runs regardless of the editor.
    if (apvts.getRawParameterValue ("rhythmTempoSync")->load() > 0.5f)
    {
        double bpm = 0.0;
        for (int i = 0; i < RhythmEngine::kSlots; ++i)
            if (rhythm.slots[i].enabled.load())
            {
                const double r = (double) rhythm.getRateHz (i);
                if (r > 0.0)
                {
                    bpm = r * 60.0 * std::exp2 ((double) apvts.getRawParameterValue ("rhythmTempoOct")->load());
                    break;
                }
            }
        const int b = bpm > 0.0 ? (int) std::lround (bpm) : -1;
        if (b > 0 && b != lastTempoSent)
        {
            lastTempoSent = b;
            rhythm.sendController (20, juce::jlimit (0, 127, b - 20));
        }
    }
    else lastTempoSent = -1;

    // Undo granularity: cut parameter edits into ~1-second transactions, so a slider
    // drag (or an automation ramp) undoes as one step instead of hundreds of
    // micro-moves. This lived in the editor, which meant that with the window shut
    // NOTHING ever opened a transaction: every automated change piled into one
    // unbounded ActionSet and the first Ctrl+Z after opening the window reverted the
    // lot back to the values the plugin was instantiated with.
    //
    // ...but NOT in the middle of a gesture. A pencil drag is one user action that can
    // last several seconds, and a cut inside it made "I dragged a partial" two Ctrl+Zs -
    // the second of which leaves manual mode armed with no edits (see beginUndoGesture).
    if (undoGestureDepth > 0)
        undoTxnTick = 0;
    else if (++undoTxnTick >= 60) // 60 Hz timer -> ~1 s, same grouping as before
    {
        undoTxnTick = 0;
        undoManager.beginNewTransaction();
    }

    // Apply the bend-range choice only when it changes: setLegacyModePitchbendRange
    // releases all held notes, so it must never be called redundantly.
    static constexpr int bendValues[] = { 2, 12, 48 };
    const int bendIdx = juce::jlimit (0, 2, (int) apvts.getRawParameterValue ("bendRange")->load());
    if (bendValues[bendIdx] != lastBendApplied)
    {
        lastBendApplied = bendValues[bendIdx];
        synth.setLegacyModePitchbendRange (lastBendApplied);
        // The MIX sub-engines bend by the same range - one MIDI-to-pitch mapping for
        // the whole instrument (in MPE zone mode the zone already fixes it at +-48).
        for (auto& e : mixEng)
            if (e != nullptr && ! lastZoneMpe)
                e->synth.setLegacyModePitchbendRange (lastBendApplied);
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
    clearSegmentSource(); // this .scl is not the spectrum segment - drop its "from n/1" mapping
    syncBankSelector (bankAutoAdd (tuning.getScalaName(), tuning.getScalaSource()));
    return true;
}

//==============================================================================
// Every tuning loaded into this Fibló joins the bank (first empty slot; exact
// duplicates skipped; a full bank is left for the user to curate with the X).
// Returns the 0-based slot the scale ended up in (an existing duplicate counts), or
// -1 if it could not be stored. The caller points the bank selector at it: without
// that the combo kept displaying whatever was selected before, and re-picking the
// entry you are actually on is a no-op in JUCE (a ComboBox fires nothing when the
// already-selected id is chosen again), so the list needed TWO selections to recover.
int FractalAdditiveAudioProcessor::bankAutoAdd (const juce::String& name, const juce::String& source)
{
    if (source.isEmpty()) return -1;
    for (int i = 0; i < kBankSlots; ++i)
        if (bankSource[(size_t) i] == source) return i;
    for (int i = 0; i < kBankSlots; ++i)
        if (bankSource[(size_t) i].isEmpty())
        {
            bankSource[(size_t) i] = source;
            bankName[(size_t) i]   = name.isNotEmpty() ? name : juce::String ("scale");
            return i;
        }
    return -1; // bank full - the user curates it with the X
}

// Point the bank selector at `slot0` without the timer then re-loading that slot's
// .scl on top of what is already active (which would stomp the tuning mode back to
// Scala and drop any segment mapping). lastBankSlot is set FIRST for that reason.
void FractalAdditiveAudioProcessor::syncBankSelector (int slot0)
{
    if (! juce::isPositiveAndBelow (slot0, kBankSlots)) return;
    lastBankSlot = slot0 + 1;
    // The other two halves of "already applied" go with it. The tuning that is active right
    // now IS this slot's, so recording its text and clearing the failed flag is simply the
    // truth - and it keeps the retry test above from ever looking at this slot's text again.
    lastBankSource = bankSource[(size_t) slot0];
    lastBankFailed = false;
    if (auto* p = apvts.getParameter ("tuningBankSlot"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (slot0 + 1)));
}

void FractalAdditiveAudioProcessor::bankClearSlot (int slot0)
{
    if (! juce::isPositiveAndBelow (slot0, kBankSlots)) return;
    bankSource[(size_t) slot0].clear();
    bankName[(size_t) slot0].clear();
    if (getTuningBankSlotParam() == slot0 + 1)
        setTuningBankSlotParam (0); // the removed slot was selected -> back to "none"
}

void FractalAdditiveAudioProcessor::clearScala()
{
    tuning.clearScala();
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Equal));
    tuning.setMode (TuningManager::Mode::Equal);
    clearSegmentSource();
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
            // (members, per-note bend, MASTER bend). The master range used to be the
            // conventional 2 semitones, which quietly broke retuning whenever the host
            // put notes on channel 1 - the zone's master channel - instead of on a
            // member channel: the very same bend bytes then meant 1/24 of the pitch,
            // so a 31-EDO retuning collapsed to within a couple of cents of 12-EDO,
            // and steps sharing a nearest 12-EDO key became the same note (some of
            // them stealing each other's voice, so they did not sound at all).
            // Both ranges are +-48 here, so the retuning survives whichever channel
            // the host chooses. That is also exactly what the UI promises: the bend
            // selector is hidden in this mode because "the MPE zone fixes it at +-48".
            zone.setLowerZone (15, 48, 48);
            synth.setZoneLayout (zone);
        }
        else
        {
            synth.enableLegacyMode (lastBendApplied);
        }
        // The MIX sub-engines follow the SAME zone plumbing: the tuning - and how MIDI
        // maps to pitch - is global, exactly what the playground promises never to
        // split per corner.
        for (auto& e : mixEng)
            if (e != nullptr) applyZoneTo (e->synth);
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

    auto text = f.loadFileAsString();
    // The bank stores tuning SOURCE text and re-parses it on every slot switch and on
    // every session reload, so whatever goes in has to be provably loadable - a slot
    // holding text that fails to parse silently reverts to the previous tuning.
    //
    // A .tun that converts to a scale is stored as that .scl (the degrees and period are
    // what the rest of the plugin reasons about). A non-repeating one is stored VERBATIM:
    // it has no .scl form at all, and dropping it here is how it used to disappear from
    // the bank without a word.
    if (f.hasFileExtension ("tun") || TuningManager::looksLikeTun (text))
    {
        juce::String scl, tunErr;
        if (TuningManager::tunToScala (text, f.getFileNameWithoutExtension(), scl, tunErr))
            text = scl;
        else
        {
            std::array<double, 128> freqs {};
            juce::String tabErr;
            if (! TuningManager::readTunTable (text, freqs, tabErr))
                return;                       // not a readable .tun either: nothing to store
            bankSource[(size_t) slot0] = text; // the table itself is the source
            bankName[(size_t) slot0]   = f.getFileNameWithoutExtension();
            addRecentScl (f);
            setTuningBankSlotParam (slot0 + 1);
            return;
        }
    }

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

    auto text = f.loadFileAsString();
    // PROVENANCE: a scale file loaded AS the spectrum is itself a tuning, so "the
    // tuning this spectrum came from" is exactly this file. The ORIGINAL text is kept
    // (captured here, before any .tun -> .scl conversion below rewrites `text`):
    // loading it back as the tuning must give what loading the FILE as a tuning would
    // have given, not Fibló's converted paraphrase of it.
    const auto provText = text;
    // Same conversion as the tuning loader: a .tun's degrees are a perfectly good
    // spectrum, and timbreSource is re-parsed as .scl on state restore, so what gets
    // stored has to be the converted .scl and not the .tun.
    //
    // A NON-repeating .tun is refused here on purpose, even though the tuning side now
    // loads it as an absolute table: a spectrum IS a set of degrees repeating at a period
    // (the Reps control stacks it), so there is nothing for the Custom Engine to build
    // from 128 absolute keys. Saying so beats picking one octave's worth and calling it
    // the timbre.
    if (f.hasFileExtension ("tun") || TuningManager::looksLikeTun (text))
    {
        // ASK WHETHER IT IS A READABLE .tun BEFORE BLAMING ITS STRUCTURE. tunToScala fails
        // for two entirely different reasons and its `errorOut` is what tells them apart:
        // the file is unreadable (parseTun's own message - a missing [Exact Tuning] block,
        // a note line that is not a number, fewer than 128 keys), or it reads perfectly and
        // simply does not repeat. The line below used to overwrite `errorOut` for BOTH, so
        // a malformed .tun was told the one thing that was not wrong with it - and the
        // remedy that message offers, "Load it as a TUNING instead", then fails too, because
        // loadTunString starts with the same parseTun. The user is handed a wrong diagnosis
        // and a next step that cannot work.
        //
        // readTunTable IS parseTun (TuningManager exposes it under that name for exactly
        // this kind of caller - proving a text will still load without loading it), so this
        // probe and the conversion below agree by construction rather than by resemblance.
        // It costs one extra parse of a file the user just picked, on the message thread.
        std::array<double, 128> probe {};
        if (! TuningManager::readTunTable (text, probe, errorOut))
            return false;   // parseTun's own reason, kept verbatim - it is the true one

        juce::String scl;
        if (! TuningManager::tunToScala (text, f.getFileNameWithoutExtension(), scl, errorOut))
        {
            // It reads; it just does not repeat. NOW the structural message is the truth,
            // and the remedy it offers really does work - loadTunString holds a
            // non-repeating .tun as an absolute 128-key table.
            errorOut = "This .tun does not repeat at any interval, so it states no degrees "
                       "for the Custom Engine to use. Load it as a TUNING instead - it will "
                       "sound exactly as written.";
            return false;
        }
        text = scl;
    }

    std::vector<double> perDeg;
    double period = 2.0;
    if (! TuningManager::parseScalaScale (text, perDeg, period, errorOut))
        return false;

    timbreDegrees.assign (perDeg.begin(), perDeg.end()); // double -> float
    timbrePeriod = (float) period;
    timbreName   = f.getFileNameWithoutExtension();
    timbreSource = text;
    specProvName   = timbreName;
    specProvSource = provText;
    specProvPeriod.clear(); // a repeating file needs no table period (tables are refused above)
    setTimbreOnParam (true);
    addRecentScl (f);
    return true;
}

// The typed-intervals parser on its own, writing NOTHING. Split out of loadTimbreText
// because a preset sitting on a playground corner has to be compiled from ITS intervals
// without disturbing the live Custom Engine (assigning a corner must not change the
// sound), and a second copy of this grammar would eventually accept a different language
// than the box the user types into.
bool FractalAdditiveAudioProcessor::parseTimbreIntervals (const juce::String& text,
                                                          std::vector<float>& degOut,
                                                          float& periodOut,
                                                          juce::String& errorOut)
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

        // THE FUNDAMENTAL IS A TOKEN YOU CAN TYPE, and it used to be thrown away.
        //
        // "1" (and "1/1", and "0c") was silently dropped by a `> 1.0000001` test, so the
        // one thing a user naturally types first was rejected with "type at least one
        // interval above 1/1" - and the whole Custom Engine looked broken to anyone who
        // did not already know that the box wants intervals ABOVE the fundamental and
        // that its highest entry becomes the PERIOD rather than a partial.
        //
        // "QUE POR DEFECTO SOLO HAYA 1 ESCRITO EN LA CASILLA, Y ENTONCES SUENE UN SINE,
        // Y QUE AL ANADIR O QUITAR MAS COSAS TENGA EFECTO... QUE SE PUEDA QUITAR EL 1
        // PERO QUE DE NORMAL SIEMPRE ESTE AHI POR DEFECTO."
        //
        // So the fundamental is accepted, deduplicated with the implicit one below, and a
        // list that holds nothing else is a SINE rather than an error - one partial at
        // ratio 1, period 2. Everything above it behaves exactly as before: the highest
        // entry is still the period and the rest are still the partials, so every
        // spectrum ever typed or saved reads back identically.
        if (std::isfinite (r) && r > 0.9999999)
            ratios.push_back (r);
    }

    std::sort (ratios.begin(), ratios.end());
    ratios.erase (std::unique (ratios.begin(), ratios.end(),
                               [] (double a, double b) { return std::abs (a - b) < 1.0e-6; }),
                  ratios.end());
    if (ratios.empty())
    {
        errorOut = "Type at least one interval - \"1\" on its own is a sine, and "
                   "\"1 3/2 2\" is a fundamental, a fifth and an octave period.";
        return false;
    }

    // JUST THE FUNDAMENTAL IS A SINE. The period cannot come from the list here - the
    // list IS the fundamental and a period of 1/1 is degenerate, every "period" landing
    // on the same pitch - so it falls back to the octave, which is what an unspecified
    // period means everywhere else in this plugin.
    if (ratios.size() == 1 && ratios.front() < 1.0000001)
    {
        degOut    = { 1.0f };
        periodOut = 2.0f;
        return true;
    }

    // Highest interval = the period; the rest (plus the implicit 1/1) are the partials.
    const double period = ratios.back();
    std::vector<float> deg;
    deg.reserve (ratios.size());
    deg.push_back (1.0f);
    for (size_t i = 0; i + 1 < ratios.size(); ++i)
        if (ratios[i] > 1.0000001)      // the fundamental is already in, typed or not
            deg.push_back ((float) ratios[i]);

    degOut    = deg;
    periodOut = (float) period;
    return true;
}

bool FractalAdditiveAudioProcessor::loadTimbreText (const juce::String& text, juce::String& errorOut)
{
    std::vector<float> deg;
    float period = 2.0f;
    if (! parseTimbreIntervals (text, deg, period, errorOut))
        return false;

    timbreDegrees = deg;
    timbrePeriod  = period;
    timbreName    = "typed";
    timbreSource  = text;
    // Typed text (and the random generator, which comes through here) has no tuning
    // origin, so any previous spectrum's provenance dies with that spectrum. The two
    // paths that DO have one (applySpectrumAdapt, restoreState) re-set it right after
    // this call - which is what keeps a stale "Load spectrum's tuning" button from
    // offering the ORIGIN OF A SPECTRUM THAT IS NO LONGER LOADED.
    specProvName.clear(); specProvSource.clear(); specProvPeriod.clear();
    setTimbreOnParam (true);
    return true;
}

//==============================================================================
// RANDOM SPECTRUM - the distribution, stated in full because a random generator with
// an unexplained distribution is unusable: nobody can tell a bug from a draw.
//
//  * PARTIAL COUNT: uniform in [kRandMinPartials, kRandMaxPartials] = 5..12 partials
//    per period. Below ~5 there is not enough of a spectrum to have a timbre at all;
//    above ~12 the partials inside one period sit closer than a critical band over
//    most of the range and the result stops being a chord of partials and starts being
//    a band of noise. Reps (1..8) stacks the period, so what the ear gets is this many
//    times Reps - a rich sound from a countable set.
//
//  * POSITION: uniform in LOG frequency (i.e. uniform in CENTS), which is the only
//    uniform that sounds uniform: uniform in Hz would pile every draw into the top
//    octave and leave the bottom of the spectrum empty every single time.
//
//  * REACH: the span comes from the two controls that already decide how far the
//    spectrum reaches - the OCTAVE RANGE cut (spectrumRange, in octaves above the
//    played pitch) divided by how many times Reps stacks the period - clamped to
//    1..3 octaves. So a random spectrum lands inside the reach the user has set
//    instead of overshooting it (partials above the cut are dropped by the range
//    control, and a generator that aimed above it would be asking for that).
//
//  * MINIMUM SPACING: kRandMinGapCents (35c) between neighbours, built into the draw
//    (a uniform draw over the span MINUS the gaps, with the gaps added back - which is
//    still exactly uniform, just conditioned on the spacing). Two reasons, and the
//    second is the important one: partials a few cents apart are one beating partial,
//    not two; and loadTimbreText DEDUPES ratios within 1e-6, so an unconstrained draw
//    could silently hand back fewer partials than it generated. This synth never drops
//    a partial quietly, so the generator does not produce ones that would be dropped.
//
//  * THE TOP INTERVAL IS THE EXACT PERIOD (2^span). loadTimbreText reads the highest
//    interval as the repeat point, so leaving it random would make Reps stack at a
//    random, usually irrational period - the difference between an instrument and a
//    smear. A clean octave-multiple period is what makes repeated draws sound like
//    different instruments rather than different accidents.
juce::String FractalAdditiveAudioProcessor::makeRandomSpectrumText()
{
    constexpr int    kRandMinPartials = 5;
    constexpr int    kRandMaxPartials = 12;
    constexpr double kRandMinGapCents = 35.0;

    const int reps = juce::jlimit (1, 8, (int) apvts.getRawParameterValue ("timbreRepeats")->load());
    const int cut  = juce::jlimit (1, 10, (int) apvts.getRawParameterValue ("spectrumRange")->load());
    const int spanOct   = juce::jlimit (1, 3, cut / reps);
    const double spanC  = 1200.0 * (double) spanOct;

    juce::String text;
    // Redraw on the astronomically unlikely event that a draw repeats the previous one
    // exactly: "every press is a different spectrum" is a promise, not a probability.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        // n partials INSIDE the period; the period itself is written as the last token.
        int n = randomSpectrumRng.nextInt ({ kRandMinPartials, kRandMaxPartials + 1 });
        // Never ask for more partials than the span can hold at the minimum spacing:
        // the alternative is a draw that quietly violates its own spacing rule.
        n = juce::jmin (n, (int) std::floor (spanC / kRandMinGapCents) - 1);
        n = juce::jmax (1, n);

        // Uniform-with-a-minimum-gap: draw n points uniformly over the span shrunk by
        // the (n+1) gaps that must exist (n interior gaps + the one below the period),
        // sort them, then push the i-th up by (i+1) gaps. Exactly uniform, exactly
        // spaced - no rejection loop that could spin.
        const double free = juce::jmax (0.0, spanC - (double) (n + 1) * kRandMinGapCents);
        std::vector<double> cents;
        cents.reserve ((size_t) n);
        for (int i = 0; i < n; ++i) cents.push_back (randomSpectrumRng.nextDouble() * free);
        std::sort (cents.begin(), cents.end());
        for (int i = 0; i < n; ++i) cents[(size_t) i] += (double) (i + 1) * kRandMinGapCents;

        juce::String t;
        for (int i = 0; i < n; ++i)
            t << juce::String (cents[(size_t) i], 2) << "c ";
        t << juce::String (spanC, 2) << "c";   // the period, exact

        if (t != lastRandomSpectrumText) { text = t; break; }
        text = t; // last attempt wins rather than returning nothing
    }
    lastRandomSpectrumText = text;
    return text;
}

bool FractalAdditiveAudioProcessor::pollRandomSpectrumTrigger()
{
    if (! randomTrigPending.exchange (false, std::memory_order_acq_rel)) return false;
    juce::String err;
    // Undoable, like every other control: one Ctrl+Z brings the previous spectrum back.
    // A random button that overwrote a hand-typed scale with no way back would make the
    // feature something you cannot risk pressing.
    if (! loadTimbreTextUndoable (makeRandomSpectrumText(), err)) return false;
    ++randomSpectrumCount;
    return true;
}

void FractalAdditiveAudioProcessor::clearTimbre()
{
    // Only the SCALE is dropped: the on/off parameter is left alone (it is part of
    // the saved state, and isTimbreActive() already reports off with no scale
    // loaded). Clearing it here overwrote the value a state restore had just set.
    timbreDegrees.clear();
    timbreName.clear();
    timbreSource.clear();
    // No spectrum, no origin: provenance describes the LOADED spectrum, and keeping it
    // past a clear would offer a tuning "this spectrum" never came from (there is none).
    specProvName.clear(); specProvSource.clear(); specProvPeriod.clear();
}

//==============================================================================
double FractalAdditiveAudioProcessor::analysisAnchorHz() const
{
    // The tonic that is REALLY sounding on key 60 under the active tuning - octave
    // shift, reference-A and a .tun/@ABL table anchor included. The scale generator
    // and the chord search used kRefFreq * referenceScale() instead, which loses the
    // octave shift and any table anchor: with octave shift -2 they scored a register
    // four times higher than the one being played, while the degree list right next
    // to them scored the real one - two incomparable columns of numbers.
    const double f0 = tuning.frequencyForNote (60);
    if (f0 > 0.0 && std::isfinite (f0))
        return f0;
    return TuningManager::kRefFreq * tuning.referenceScale();
}

std::vector<dissonance::Partial> FractalAdditiveAudioProcessor::analysisSpectrumAt (double f0Hz) const
{
    // The PUBLISHED template first, never a routine recompile: this runs on the
    // message thread and a compile at fractalLevel > 1 costs hundreds of
    // milliseconds - the editor's timer re-running it starved the 60 Hz
    // timerCallback (the only place a modulated Base is sampled into a rebuild
    // request) and froze the spectrum at whatever Base value was caught last.
    // Being at most one publish behind is invisible to a 64-partial analysis, and
    // it keeps every readout scoring the SAME spectrum. The live template carries
    // the user's per-partial gains already resolved (SpectrumEngine::fillGains).
    if (const auto* live = spectrumEngine.getLive(); live != nullptr && ! live->partials.empty())
        return dissonance::analysisSpectrum (live->partials, live->gain, f0Hz);

    // Cold start (before the first publish): compile once, and resolve the manual
    // edits against that list exactly as the engine would - a muted or boosted
    // partial must weigh the same here as it will the moment the engine publishes.
    const auto tmpl = spectrum::compileSpectrum (currentSettings());
    std::vector<float> gain;
    if (apvts.getRawParameterValue ("partialManual")->load() > 0.5f && ! partialGains.empty())
        spectrum::resolveGains (tmpl.partials, partialGains, gain);
    return dissonance::analysisSpectrum (tmpl.partials, gain, f0Hz);
}

//==============================================================================
bool FractalAdditiveAudioProcessor::generateDissonanceScale (int count, bool useMaxima,
                                                             double periodRatio,
                                                             const juce::String& periodSclLine,
                                                             double minGapCents,
                                                             juce::String& errorOut)
{
    count = juce::jlimit (1, 24, count);
    if (! (periodRatio > 1.02) || ! std::isfinite (periodRatio))
    {
        errorOut = "Invalid scale period.";
        return false;
    }

    // Analyse the spectrum the synth is REALLY producing (published template at its
    // sounding amplitudes, per-partial edits included) at the register it is really
    // played in - the same spectrum and the same anchor as the degree list that
    // colours the result, because the two sets of heights sit side by side.
    const auto spec = analysisSpectrumAt (analysisAnchorHz());
    if (spec.empty())
    {
        errorOut = "The current settings produce no partials - nothing to analyse.";
        return false;
    }

    const auto curve = dissonance::dissonanceCurve (spec, periodRatio, 1.0);
    auto found = dissonance::pickExtremes (spec, curve, count, useMaxima, periodRatio);
    // Minimum-interval rule (default 33c): drop extremes closer than minGap to their
    // neighbour (keeping the stronger one), to the tonic, or to the period.
    if (minGapCents > 0.0 && ! found.empty())
    {
        const double periodCents = 1200.0 * std::log2 (periodRatio);
        std::vector<dissonance::ExtremeInfo> kept;
        for (const auto& e : found) // found is sorted ascending by ratio
        {
            const double c = 1200.0 * std::log2 (e.ratio);
            if (c < minGapCents || periodCents - c < minGapCents) continue;
            if (! kept.empty() && c - 1200.0 * std::log2 (kept.back().ratio) < minGapCents)
            {
                const bool better = useMaxima ? e.value > kept.back().value
                                              : e.value < kept.back().value;
                if (better) kept.back() = e;
            }
            else kept.push_back (e);
        }
        found = std::move (kept);
    }
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
    syncBankSelector (bankAutoAdd (name, scl));

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

std::vector<FractalAdditiveAudioProcessor::DissDegreeInfo>
FractalAdditiveAudioProcessor::computeActiveScaleDissonance() const
{
    std::vector<DissDegreeInfo> out;

    // The shared analysis spectrum (published template, sounding amplitudes, 64-cap)
    // at the ACTUAL sounding tonic of the active tuning. Reading the PUBLISHED
    // template rather than recompiling is what analysisSpectrumAt exists for: this
    // is re-run from the editor's timer, and a synchronous compile here used to
    // starve the 60 Hz timerCallback that samples a modulated Base (the spectrum
    // froze at whatever Base value was caught last, for as long as fractal > 1).
    const auto spec = analysisSpectrumAt (analysisAnchorHz());
    if (spec.empty())
        return out;

    const int D = juce::jmax (1, getScaleDegrees());
    // THE LAST ROW IS THE PERIOD, AND ONLY IF THERE IS ONE.
    // getDegreeRatio(D) is degree 0 one period up, i.e. degree-0's ratio times
    // periodRatio() - and periodRatio() answers 2.0 when the tuning states nothing.
    // A descending .scl and a degenerate 1/1 scale therefore both ended this list with
    // a row reading "2/1", scored as if the user's file had an octave in it. It does
    // not, so the row is not written: the list says what the scale says and stops. The
    // editor prints the reason under it (see nadHadNoPeriod / the degrees read-out), so
    // a shorter list is stated rather than silent.
    const int last = tuningStatesPeriod() ? D : D - 1;
    out.reserve ((size_t) last + 1);
    for (int s = 0; s <= last; ++s) // 0 = tonic (1/1), D = period
    {
        const double r = getDegreeRatio (s);
        out.push_back ({ s, r, dissonance::dissonanceAt (spec, r) });
    }
    return out;
}

//==============================================================================
// "From spectrum's segment" scale building.
namespace
{
    // "2" / "3/2" / "500c" -> linear period ratio (>1). Falls back to 2.
    double parsePeriodSpec (juce::String t)
    {
        t = t.trim();
        double v = 2.0;
        if (t.endsWithIgnoreCase ("c"))
            v = std::exp2 (t.dropLastCharacters (1).getDoubleValue() / 1200.0);
        else if (t.containsChar ('/'))
        {
            const double p = t.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
            const double q = t.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
            v = (p > 0.0 && q > 0.0) ? p / q : 2.0;
        }
        else if (t.getDoubleValue() > 0.0)
            v = t.getDoubleValue();
        return (v > 1.0001 && std::isfinite (v)) ? v : 2.0;
    }
}

void FractalAdditiveAudioProcessor::segmentToggle (int partialIndex, bool rangeToAnchor)
{
    if (partialIndex < 0) return;
    if (rangeToAnchor && segAnchor >= 0)
    {
        const int lo = juce::jmin (segAnchor, partialIndex);
        const int hi = juce::jmax (segAnchor, partialIndex);
        for (int i = lo; i <= hi; ++i) segSel.insert (i); // shift-click ADDS the span
    }
    else if (segSel.count (partialIndex))
        segSel.erase (partialIndex);       // plain click on a selected one -> deselect
    else
        segSel.insert (partialIndex);      // plain click -> select
    segAnchor = partialIndex;
    rebuildSegmentScale();
}

void FractalAdditiveAudioProcessor::segmentClear() { segSel.clear(); segAnchor = -1; }
bool FractalAdditiveAudioProcessor::segmentSelected (int i) const { return segSel.count (i) > 0; }
int  FractalAdditiveAudioProcessor::segmentTonicIndex() const { return segSel.empty() ? -1 : *segSel.begin(); }
int  FractalAdditiveAudioProcessor::segmentCount() const { return (int) segSel.size(); }
void FractalAdditiveAudioProcessor::setSegmentPeriodText (const juce::String& s) { segPeriodText = s; rebuildSegmentScale(); }
void FractalAdditiveAudioProcessor::setSegmentUseTopPeriod (bool b) { segUseTopPeriod = b; rebuildSegmentScale(); }

void FractalAdditiveAudioProcessor::rebuildSegmentScale (bool userInitiated)
{
    const auto* t = spectrumEngine.getLive();
    if (t == nullptr || segSel.empty()) return;
    segRebuiltGen = t->generation; // mark this generation handled, even if we bail below (no 30Hz thrash)

    const int tonic = *segSel.begin();
    if (tonic >= (int) t->partials.size()) return;
    const double r0 = (double) t->partials[(size_t) tonic].ratio;
    if (! (r0 > 0.0)) return;

    // Period: the highest selected partial (relative to the tonic), or the typed spec.
    double period = parsePeriodSpec (segPeriodText);
    if (segUseTopPeriod)
    {
        const int top = *segSel.rbegin();
        if (top < (int) t->partials.size())
        {
            const double pr = (double) t->partials[(size_t) top].ratio / r0;
            if (pr > 1.0001 && std::isfinite (pr)) period = pr;
        }
    }

    // Fold each selected partial's ratio-to-tonic into [1, period); keep, per
    // resulting degree, the SOURCE partial's raw ratio (n/1 before folding) for the
    // keyboard's "from n/1" hover + the .scl comments. Drop the tonic (1/1 is
    // implicit) and any duplicate within ~0.5 cent.
    struct Deg { double folded, source; };
    std::vector<Deg> degs;
    for (int idx : segSel)
    {
        if (idx >= (int) t->partials.size()) continue;
        const double src = (double) t->partials[(size_t) idx].ratio; // raw n/1
        double r = src / r0;
        if (! (r > 0.0) || ! std::isfinite (r)) continue;
        while (r >= period - 1.0e-9) r /= period;
        while (r < 1.0 - 1.0e-9)     r *= period;
        if (r <= 1.0 + 1.0e-6) continue; // the tonic itself
        bool dup = false;
        for (const auto& d : degs) if (std::abs (1200.0 * std::log2 (r / d.folded)) < 0.5) { dup = true; break; }
        if (! dup) degs.push_back ({ r, src });
    }
    std::sort (degs.begin(), degs.end(), [] (const Deg& a, const Deg& b) { return a.folded < b.folded; });

    // Source partial per keyboard degree: degree 0 = tonic, i = interior, last = period.
    segSourceByDegree.clear();
    segSourceByDegree.push_back (r0); // tonic
    for (const auto& d : degs) segSourceByDegree.push_back (d.source);

    auto ratioName = [] (double r) { return juce::String::fromUTF8 (dissonance::justIntervalName (r).c_str()); };

    juce::String scl;
    scl << "! Fibl\xc3\xb3 - scale from spectrum segment\n"
        << "! Each degree comes from a spectrum partial (n/1 = the raw harmonic, before octave folding).\n"
        << "! tonic 1/1  <- partial " << ratioName (r0) << "\n"
        << "Spectrum segment\n " << juce::String ((int) degs.size() + 1) << "\n!\n";
    for (const auto& d : degs)
        scl << "! from partial " << ratioName (d.source) << "\n"
            << " " << juce::String (1200.0 * std::log2 (d.folded), 5) << "\n";
    scl << "! period\n " << juce::String (1200.0 * std::log2 (period), 5) << "\n";

    // The 30 Hz auto-refresh must NOT re-impose Scala mode: doing so made a segment
    // scale impossible to leave (choosing 12ED2, or undoing, was undone again on the
    // next spectrum change). Only an actual user action switches the mode.
    if (! userInitiated && getTuningMode() != (int) TuningManager::Mode::Scala)
        return;

    juce::String err;
    if (tuning.loadScalaString (scl, "segment", err))
    {
        if (userInitiated || getTuningMode() == (int) TuningManager::Mode::Scala)
        {
            if (auto* p = apvts.getParameter ("tuningMode"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
            tuning.setMode (TuningManager::Mode::Scala);
        }
        lastDissScl = scl; lastDissName = "spectrum-segment";
        segRebuiltGen = t->generation;
        ++dissGeneration; // nudge the UI (keyboard/dissonance list) to refresh
    }
    else
        segSourceByDegree.clear();
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
// N-note chord search (Sethares union measure) - background worker.
bool FractalAdditiveAudioProcessor::startChordSearch (int notes, double periodRatio,
                                                      const juce::String& periodSclLine,
                                                      int count, bool maxima, double minGapCents,
                                                      bool rootOnly, juce::String& errorOut)
{
    notes = juce::jlimit (2, 8, notes);
    count = juce::jlimit (1, 24, count);
    if (! (periodRatio > 1.02) || ! std::isfinite (periodRatio))
    {
        errorOut = "Invalid period.";
        return false;
    }

    // Reap any previous worker (message thread; the worker only reads its own copy).
    chordCancel = true;
    if (chordThread != nullptr && chordThread->joinable())
        chordThread->join();
    chordCancel = false;

    // Analyse the spectrum the synth is REALLY producing (same prep as the scale
    // generator and the degree list: published template, SOUNDING amplitudes with
    // the user's per-partial edits, strongest-64 analysis cap, anchored at the
    // active tuning's real key-60 tonic). The realtime follower re-runs this on
    // every spectrum change, so reading the published template also spares it a
    // full recompile per trigger.
    std::vector<dissonance::Partial> spec = analysisSpectrumAt (analysisAnchorHz());
    if (spec.empty())
    {
        errorOut = "The current settings produce no partials - nothing to analyse.";
        return false;
    }

    chordPeriodSclLine = periodSclLine;
    chordPeriodRatio   = periodRatio;
    chordProgress      = 0.0f;
    chordRunning       = true;
    chordRequested     = count;   // so a search the minimum-interval rule prunes to
    chordFound         = 0;       // nothing can SAY so instead of just going blank
    chordMinGap        = minGapCents;

    chordThread = std::make_unique<std::thread> (
        [this, spec = std::move (spec), periodRatio, notes, count, maxima, minGapCents, rootOnly,
         periodLine = periodSclLine]()
        {
            auto res = dissonance::searchChords (spec, periodRatio, notes, count, maxima, minGapCents, rootOnly,
                [this] (float p) { chordProgress.store (p); return ! chordCancel.load(); });
            {
                const juce::ScopedLock sl (chordLock);
                chordResults.clear();
                for (auto& r : res)
                    chordResults.push_back ({ std::move (r.alphas), r.value });
                chordResultsPeriodLine = periodLine; // results + their period publish TOGETHER
                chordFound = (int) chordResults.size();
            }
            chordProgress = 1.0f;
            chordGeneration.fetch_add (1);
            chordRunning = false;
        });
    return true;
}

void FractalAdditiveAudioProcessor::cancelChordSearch()
{
    chordCancel = true; // the worker exits at its next progress poll; reaped on next start/destroy
}

bool FractalAdditiveAudioProcessor::loadChordAsScale (int index, juce::String& errorOut, bool addToBank)
{
    ChordHit hit;
    juce::String periodLine;
    {
        const juce::ScopedLock sl (chordLock);
        if (! juce::isPositiveAndBelow (index, (int) chordResults.size()))
        {
            errorOut = "No such chord.";
            return false;
        }
        hit        = chordResults[(size_t) index];
        periodLine = chordResultsPeriodLine; // the period THESE results were searched in
    }

    // Degrees = the chord's notes above the root, in cents (source-honest: these are
    // searched/refined values, not typed ratios), then the EXACT typed period.
    juce::String scl;
    scl << "! Generated by Fibló Galàctic - Sethares chord search (rank " << (index + 1) << ")\n"
        << "Chord of " << (int) hit.alphas.size() << " notes by sensory consonance\n"
        << " " << (int) hit.alphas.size() << "\n!\n";
    for (size_t k = 1; k < hit.alphas.size(); ++k)
        scl << " " << juce::String (1200.0 * std::log2 (hit.alphas[k]), 5) << "\n";
    scl << " " << periodLine << "\n";

    const auto name = "chord-" + juce::String ((int) hit.alphas.size()) + "n-rank" + juce::String (index + 1);
    if (! tuning.loadScalaString (scl, name, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    tuning.setMode (TuningManager::Mode::Scala);
    clearSegmentSource(); // a chord scale replaces any segment mapping
    lastDissScl  = scl;
    lastDissName = name;
    if (addToBank)
        syncBankSelector (bankAutoAdd (name, scl));
    ++dissGeneration;   // the keyboard/degree list must refresh even when the bank does not
    return true;
}

//==============================================================================
// Scale editing on the keyboard (any tuning -> .scl form, then edit + reload).
juce::String FractalAdditiveAudioProcessor::currentTuningAsScl (const juce::String& name) const
{
    const int D = juce::jmax (1, tuning.degreesPerPeriod());
    juce::String scl;
    // The scale's own anchor, when it has one - see sclAnchorLine. Without it a .scl
    // rendered from an anchored tuning re-pins to 12-TET middle C on the reload, and the
    // whole instrument moves on an edit that changed nothing.
    scl << "! Edited in Fibló Galàctic\n" << name << "\n " << D << "\n" << sclAnchorLine() << "!\n";
    for (int d = 1; d <= D; ++d)
    {
        long long p = 1, q = 1;
        if (getDegreeIsRational (d, p, q))
            scl << " " << juce::String (p) << "/" << juce::String (q) << "\n";
        else
            scl << " " << juce::String (1200.0 * std::log2 (juce::jmax (1.0e-9, getDegreeRatio (d))), 5) << "\n";
    }
    scl << embeddedPresetComment();
    return scl;
}

// The whole plugin state, folded into "!" comment lines at the end of a .scl. Any
// other Scala host ignores comments, but WE can read the sound the scale was made
// with back out of it ("Load its original spectrum").
juce::String FractalAdditiveAudioProcessor::embeddedPresetComment() const
{
    auto state = const_cast<FractalAdditiveAudioProcessor*> (this)->captureState();
    // The scale itself is already in the file - no need to carry it twice.
    state.removeChild (state.getChildWithName ("SCALA"), nullptr);
    auto xml = state.createXml();
    if (xml == nullptr) return {};
    const auto b64 = juce::Base64::toBase64 (xml->toString (juce::XmlElement::TextFormat().singleLine()));

    juce::String out;
    out << "!\n! " << kPresetTag << " v1 - the Fibló Galàctic patch this scale was made with.\n"
        << "! (Plain Scala comments: every other program ignores them.)\n";
    for (int i = 0; i < b64.length(); i += 120)
        out << "! " << kPresetTag << ":" << b64.substring (i, i + 120) << "\n";
    return out;
}

// Pull that patch back out of a .scl (empty if the file carries none).
juce::ValueTree FractalAdditiveAudioProcessor::presetFromScl (const juce::String& sclText)
{
    juce::String b64;
    for (const auto& line : juce::StringArray::fromLines (sclText))
    {
        const auto t = line.trim();
        if (t.startsWith ("! " + juce::String (kPresetTag) + ":"))
            b64 += t.fromFirstOccurrenceOf (":", false, false).trim();
    }
    if (b64.isEmpty()) return {};
    juce::MemoryOutputStream mo;
    if (! juce::Base64::convertFromBase64 (mo, b64)) return {};
    if (auto xml = juce::parseXML (mo.toString()))
        return juce::ValueTree::fromXml (*xml);
    return {};
}

// Restore ONLY the sound from an embedded patch: the tuning the user just loaded
// stays exactly as it is.
bool FractalAdditiveAudioProcessor::loadSpectrumFromScl (const juce::String& sclText)
{
    auto tree = presetFromScl (sclText);
    if (! tree.isValid() || ! tree.hasType (apvts.state.getType())) return false;
    const auto keep     = tuning.captureTuning();
    const int  keepMode = getTuningMode();
    restoreState (tree);
    // Put the tuning back (restoreState may have carried the patch's own). The whole
    // snapshot, not just name+source: a table's user-chosen period is part of the tuning
    // that was active, and reloading the source alone reset it to Fibló's estimate.
    if (keep.source.isNotEmpty())
    {
        juce::String err;
        tuning.restoreTuning (keep, err);
    }
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) keepMode));
    return true;
}

// The three scale editors below (add / edit / delete a degree) all work by writing a
// NEW .scl out of the current degrees and loading it. An absolute 128-key table has no
// degrees to write, so doing that would replace the user's file with a scale invented
// from one period's worth of keys - the whole tuning, silently changed by an edit that
// looks local. They refuse instead, and say what the tuning actually is.
static bool refuseTableEdit (bool isTable, juce::String& errorOut)
{
    if (! isTable) return false;
    errorOut = "This tuning is a 128-key table (a .tun that does not repeat), not a scale, "
               "so its degrees cannot be edited - an edit would have to throw the table "
               "away. Save it as .tun and edit the keys there.";
    return true;
}

bool FractalAdditiveAudioProcessor::addNoteToTuning (double ratioFromTonic, juce::String& errorOut,
                                                     const juce::String& exactText)
{
    if (refuseTableEdit (tuningTableIsInForce(), errorOut)) return false;
    const int    D      = juce::jmax (1, tuning.degreesPerPeriod());
    const double period = getDegreeRatio (D);
    if (! (period > 1.0001) || ! (ratioFromTonic > 0.0) || ! std::isfinite (ratioFromTonic))
    {
        errorOut = "Invalid pitch.";
        return false;
    }
    // Fold into [1, period): the note joins EVERY repetition of the period.
    double r = ratioFromTonic;
    while (r >= period) r /= period;
    while (r < 1.0)     r *= period;

    // Existing degrees (rationals preserved as text) + the new one, sorted.
    struct Deg { double ratio; juce::String text; };
    std::vector<Deg> degs;
    for (int d = 1; d < D; ++d)
    {
        long long p = 1, q = 1;
        const double dr = getDegreeRatio (d);
        degs.push_back ({ dr, getDegreeIsRational (d, p, q)
                                  ? juce::String (p) + "/" + juce::String (q)
                                  : juce::String (1200.0 * std::log2 (dr), 5) });
    }
    const double newCents = 1200.0 * std::log2 (r);
    for (const auto& dg : degs) // duplicate (within half a cent) -> nothing to add
        if (std::abs (1200.0 * std::log2 (dg.ratio) - newCents) < 0.5)
        {
            errorOut = "That pitch is already a degree of the scale.";
            return false;
        }
    // The typed spec is kept verbatim when it IS the folded pitch. When it is not
    // ("3/1" under an octave period), a RATIONAL spec is folded as a fraction so it
    // still lands as an exact ratio (3/1 -> 3/2), never as rounded cents.
    juce::String newText = juce::String (newCents, 5);
    if (exactText.isNotEmpty())
    {
        double tr_ = 0.0; TuningManager::DegreeSrc s{};
        if (TuningManager::parsePitchSpec (exactText, tr_, s))
        {
            if (std::abs (1200.0 * std::log2 (juce::jmax (1.0e-9, tr_)) - newCents) < 1.0e-6)
                newText = exactText.trim();
            else if (long long ppL = 1, pqL = 1;
                     s.rational && s.q > 0 && getDegreeIsRational (D, ppL, pqL) && ppL > pqL && pqL > 0)
            {
                long long np = s.p, nq = s.q;
                bool ok = true;
                const auto mul = [&ok] (long long& a, long long b)
                {
                    if (b == 0 || std::llabs (a) > (long long) 3'000'000'000LL / std::llabs (b)) ok = false;
                    else a *= b;
                };
                // Fold by whole periods, as fractions - overflow ends the attempt.
                for (int guard = 0; ok && guard < 64 && (double) np * (double) pqL >= (double) nq * (double) ppL; ++guard)
                    { mul (nq, ppL); mul (np, pqL); }
                for (int guard = 0; ok && guard < 64 && (double) np < (double) nq; ++guard)
                    { mul (np, ppL); mul (nq, pqL); }
                if (ok)
                {
                    long long a = np < 0 ? -np : np, b = nq < 0 ? -nq : nq;
                    while (b != 0) { const long long t = a % b; a = b; b = t; }
                    if (a > 1) { np /= a; nq /= a; }
                    if (std::abs (1200.0 * std::log2 ((double) np / (double) nq) - newCents) < 1.0e-6)
                        newText = juce::String (np) + "/" + juce::String (nq);
                }
            }
        }
    }
    if (r > 1.0 + 1.0e-9)
        degs.push_back ({ r, newText });
    std::sort (degs.begin(), degs.end(), [] (const Deg& a, const Deg& b) { return a.ratio < b.ratio; });

    long long pp = 1, pq = 1;
    const juce::String periodText = getDegreeIsRational (D, pp, pq)
                                        ? juce::String (pp) + "/" + juce::String (pq)
                                        : juce::String (1200.0 * std::log2 (period), 5);
    juce::String scl;
    const auto name = (tuning.getScalaName().isNotEmpty() ? tuning.getScalaName() : juce::String ("scale")) + "+";
    scl << "! Edited in Fibló Galàctic (note added)\n" << name << "\n " << (int) degs.size() + 1 << "\n" << sclAnchorLine() << "!\n";
    for (const auto& dg : degs) scl << " " << dg.text << "\n";
    scl << " " << periodText << "\n";

    if (! tuning.loadScalaString (scl, name, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    tuning.setMode (TuningManager::Mode::Scala);
    clearSegmentSource();
    lastDissScl = scl; lastDissName = name;
    syncBankSelector (bankAutoAdd (name, scl));
    return true;
}

bool FractalAdditiveAudioProcessor::editTuningDegree (int degree, const juce::String& valueText,
                                                      juce::String& errorOut)
{
    if (refuseTableEdit (tuningTableIsInForce(), errorOut)) return false;
    const int D = juce::jmax (1, tuning.degreesPerPeriod());
    if (degree < 1 || degree > D)
    {
        errorOut = "The tonic is fixed at 1/1 - edit any other degree.";
        return false;
    }
    double newRatio = 0.0;
    TuningManager::DegreeSrc src;
    if (! TuningManager::parsePitchSpec (valueText, newRatio, src) || ! (newRatio > 0.0))
    {
        errorOut = "Invalid value. Use cents with a decimal point (\"702.0\") or a ratio (\"3/2\").";
        return false;
    }

    juce::String scl;
    const auto name = (tuning.getScalaName().isNotEmpty() ? tuning.getScalaName() : juce::String ("scale")) + "*";
    scl << "! Edited in Fibló Galàctic (degree " << degree << " changed)\n" << name << "\n " << D << "\n" << sclAnchorLine() << "!\n";
    for (int d = 1; d <= D; ++d)
    {
        if (d == degree)
            scl << " " << valueText.trim() << "\n"; // the user's exact spec (keeps its source form)
        else
        {
            long long p = 1, q = 1;
            scl << " " << (getDegreeIsRational (d, p, q)
                               ? juce::String (p) + "/" + juce::String (q)
                               : juce::String (1200.0 * std::log2 (juce::jmax (1.0e-9, getDegreeRatio (d))), 5))
                << "\n";
        }
    }
    if (! tuning.loadScalaString (scl, name, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    tuning.setMode (TuningManager::Mode::Scala);
    clearSegmentSource();
    lastDissScl = scl; lastDissName = name;
    syncBankSelector (bankAutoAdd (name, scl));
    return true;
}

// Right-click -> Delete: the degree leaves the scale, so the key is gone from
// EVERY repetition of the period (a scale degree is a pitch class, not one key).
bool FractalAdditiveAudioProcessor::deleteTuningDegree (int degree, juce::String& errorOut)
{
    if (refuseTableEdit (tuningTableIsInForce(), errorOut)) return false;
    const int D = juce::jmax (1, tuning.degreesPerPeriod());
    if (degree < 1 || degree >= D)
    {
        errorOut = "The tonic and the period cannot be deleted - delete any degree between them.";
        return false;
    }
    if (D < 2)
    {
        errorOut = "A scale needs at least one degree per period.";
        return false;
    }

    juce::String scl;
    const auto name = (tuning.getScalaName().isNotEmpty() ? tuning.getScalaName() : juce::String ("scale")) + "-";
    scl << "! Edited in Fibló Galàctic (degree " << degree << " deleted)\n" << name << "\n " << D - 1 << "\n" << sclAnchorLine() << "!\n";
    for (int d = 1; d <= D; ++d)   // d == D is the period line, always kept
    {
        if (d == degree) continue;
        long long p = 1, q = 1;
        scl << " " << (getDegreeIsRational (d, p, q)
                           ? juce::String (p) + "/" + juce::String (q)
                           : juce::String (1200.0 * std::log2 (juce::jmax (1.0e-9, getDegreeRatio (d))), 5))
            << "\n";
    }
    if (! tuning.loadScalaString (scl, name, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    tuning.setMode (TuningManager::Mode::Scala);
    clearSegmentSource();
    lastDissScl = scl; lastDissName = name;
    syncBankSelector (bankAutoAdd (name, scl));
    return true;
}

//==============================================================================
// n-ad ranking: combinations of n scale degrees ranked by TOTAL beating (union
// dissmeasure, exact pair decomposition - every pair of degrees counted).
void FractalAdditiveAudioProcessor::startNadCompute (int notes, bool rootOnly, bool maxMode,
                                                     double periodOverride, double minGapCents)
{
    notes = juce::jlimit (2, 12, notes);
    nadCancel = true;
    if (nadThread != nullptr && nadThread->joinable()) nadThread->join();
    nadCancel = false;

    // Snapshot on the message thread: the degrees' absolute frequencies + spectrum.
    const int totalD = tuning.degreesPerPeriod();
    const int D = juce::jlimit (1, 128, totalD);
    nadTotalD = totalD;
    nadUsedD  = D;
    std::vector<double> ratios ((size_t) D);
    for (int d = 0; d < D; ++d) ratios[(size_t) d] = getDegreeRatio (d);
    // The period the search reasons with: the tuning's own unless the user typed a
    // different one in the box (it decides what counts as an equal-step scale and
    // which intervals are just the same pitch class again).
    //
    // 0.0 MEANS "THIS TUNING STATES NONE", and both readers below check for it.
    // statedPeriodRatio(), not periodRatio(): the fallback would have handed a
    // descending .scl (or a degenerate 1/1) a 2/1 the file never contained, and the
    // list would then have called octave-spaced degrees "the same pitch class again"
    // and dropped them, and called the scale rootless and shown "any". Both are
    // statements about a repetition that does not exist.
    const double statedPeriod = periodOverride > 1.0 ? periodOverride : tuning.statedPeriodRatio();
    const double periodC = statedPeriod > 1.0001 ? 1200.0 * std::log2 (statedPeriod) : 0.0;
    nadNoPeriod = ! (periodC > 0.0);   // surfaced in the list, never silent
    const double minGap = juce::jlimit (0.0, 600.0, minGapCents);
    // The SAME analysis spectrum and absolute anchor as the Degrees list
    // (computeActiveScaleDissonance): Sethares roughness depends on register and on
    // amplitude, and the two lists sit side by side, so they must score the same
    // spectrum from the same register or their numbers are incomparable. The vector
    // is an owned copy, because the search continues on a worker thread and the
    // engine's live slot can be recycled under it.
    std::vector<dissonance::Partial> spec = analysisSpectrumAt (analysisAnchorHz());

    nadBusy = true;
    nadThread = std::make_unique<std::thread> ([this, notes, D, periodC, rootOnly, maxMode, minGap,
                                                ratios = std::move (ratios), spec = std::move (spec)]()
    {
        std::vector<FractalAdditiveAudioProcessor::NadRow> out;

        // Equal-step scale? Then only 0-rooted combinations are enumerated (see
        // below), which also shrinks the search space from C(D,n) to C(D-1,n-1).
        std::vector<double> cents ((size_t) D);
        for (int d = 0; d < D; ++d) cents[(size_t) d] = 1200.0 * std::log2 (std::max (1.0e-9, ratios[(size_t) d]));
        // periodC == 0 is "no period stated": a shape cannot be rootless in a scale
        // that never comes back to itself, so the full enumeration is the honest one.
        bool regular = D >= 2 && periodC > 0.0;
        {
            const double step = periodC / (double) D;
            for (int d = 0; d < D && regular; ++d)
                if (std::abs (cents[(size_t) d] - step * (double) d) > 0.5) regular = false;
        }

        long long combos = 1;
        {
            const int nSel = regular ? notes - 1 : notes;   // regular: degree 0 is fixed
            const int pool = regular ? D - 1     : D;
            for (int i = 0; i < nSel; ++i) combos = combos * (long long) (pool - i) / (long long) (i + 1);
        }
        nadOverBudget = ! (combos > 0 && combos <= 4'000'000LL); // surfaced in the list, never silent
        if (D >= notes && combos > 0 && combos <= 4'000'000LL && ! spec.empty())
        {
            // Exact pair tables over the degree set (cancel-polled per row AND per
            // column: a cancel must not stall the message thread's join()).
            std::vector<double> dself ((size_t) D);
            std::vector<double> dcross ((size_t) D * (size_t) D, 0.0);
            for (int i = 0; i < D && ! nadCancel.load(); ++i)
            {
                dself[(size_t) i] = dissonance::selfDissonance (spec, ratios[(size_t) i]);
                for (int j = i + 1; j < D; ++j)
                {
                    if ((j & 15) == 0 && nadCancel.load()) break;
                    const double v = dissonance::crossDissonance (spec, ratios[(size_t) i], ratios[(size_t) j]);
                    dcross[(size_t) i * (size_t) D + (size_t) j] = v;
                    dcross[(size_t) j * (size_t) D + (size_t) i] = v;
                }
            }

            // In REGULAR tunings (EDOs, any equal-step scale) every chord shape exists
            // at every degree, so listing all transpositions would repeat the same
            // shape D times: there the enumeration is ROOTED AT 0 (see `regular`
            // above) - exactly the "only from 0" the list promises. Irregular scales
            // keep the full enumeration (their transpositions are genuinely different
            // chords) with a pattern dedup for shapes that DO repeat (symmetric
            // scales), where the first copy seen is the lowest-rooted one.

            // Pattern key for the irregular path: the consecutive interval sizes,
            // quantised to 0.5 c and packed into one integer (no allocation).
            const auto patternKey = [&cents, notes] (const int* cc)
            {
                unsigned long long k = 1469598103934665603ull; // FNV-1a over the intervals
                for (int i = 1; i < notes; ++i)
                {
                    const long long q = (long long) std::llround ((cents[(size_t) cc[i]] - cents[(size_t) cc[i - 1]]) * 2.0);
                    k = (k ^ (unsigned long long) q) * 1099511628211ull;
                }
                return k;
            };
            std::unordered_map<unsigned long long, int> seen; // shapes already listed

            // Bounded best-K enumeration (ascending beating). A combination that is
            // just the unison repeated, or that pairs 0 with the period (the same
            // pitch class an octave up), says nothing - those are skipped.
            // ...and neither does a combination whose closest two notes are nearer
            // than the smallest interval the user allows.
            const double floorC = std::max (0.5, minGap);
            const auto degenerate = [&cents, notes, periodC, floorC] (const int* cc)
            {
                for (int i = 1; i < notes; ++i)
                {
                    const double gap = std::abs (cents[(size_t) cc[i]] - cents[(size_t) cc[i - 1]]);
                    if (gap < floorC) return true;
                    // "the same pitch class an octave up" is only a thing to skip when
                    // the tuning HAS that octave. With periodC == 0 (nothing stated) the
                    // old form compared every gap against 0 cents and, worse, would have
                    // compared it against a fabricated 1200 - dropping perfectly ordinary
                    // pairs from the list for repeating a period the scale does not have.
                    if (periodC > 0.0 && std::abs (gap - periodC) < 0.5) return true;
                }
                return false;
            };
            const int keep = 40;
            std::vector<FractalAdditiveAudioProcessor::NadRow> best;
            int c[12];
            for (int i = 0; i < notes; ++i) c[i] = i;   // regular: c[0] stays 0
            while (! nadCancel.load())
            {
                // SCOPE: rank by every pair's beating, or only by each note against 0.
                double v = 0.0;
                for (int i = 0; i < notes; ++i)
                {
                    v += dself[(size_t) c[i]];
                    if (rootOnly) { if (i > 0) v += dcross[(size_t) c[0] * (size_t) D + (size_t) c[i]]; }
                    else for (int j = i + 1; j < notes; ++j)
                        v += dcross[(size_t) c[i] * (size_t) D + (size_t) c[j]];
                }
                // Min = the quietest beating first, Max = the roughest first.
                const auto better = [maxMode] (double a, double b) { return maxMode ? a > b : a < b; };
                if (! degenerate (c) && ((int) best.size() < keep || better (v, best.back().value)))
                {
                    // In the irregular path the FIRST time a shape is seen is its
                    // lowest-rooted copy (lexicographic order), so keeping that one
                    // keeps the 0-rooted copy whenever it exists.
                    const bool fresh = regular || seen.find (patternKey (c)) == seen.end();
                    if (fresh)
                    {
                        if (! regular) seen.emplace (patternKey (c), 1);
                        FractalAdditiveAudioProcessor::NadRow row;
                        row.value = v;
                        row.degs.assign (c, c + notes);
                        const auto pos = std::lower_bound (best.begin(), best.end(), row,
                            [maxMode] (const NadRow& a, const NadRow& b)
                            { return maxMode ? a.value > b.value : a.value < b.value; });
                        best.insert (pos, std::move (row));
                        if ((int) best.size() > keep) best.pop_back();
                    }
                }
                // Next combination. In the regular path index 0 is pinned to degree 0.
                int i = notes - 1;
                while (i >= (regular ? 1 : 0) && c[i] == D - (notes - i)) --i;
                if (i < (regular ? 1 : 0)) break;
                ++c[i];
                for (int j = i + 1; j < notes; ++j) c[j] = c[j - 1] + 1;
            }
            out = std::move (best);
        }
        // A cancelled run must NOT publish its partial ranking (rows would be
        // missing because enumeration stopped, not because they lost).
        if (! nadCancel.load())
        {
            {
                const juce::ScopedLock sl (nadLock);
                nadResults = std::move (out);
            }
            // The degree COUNT these rows index into, stamped before the generation bump
            // so a reader that sees the new generation also sees the matching D. Their
            // numbers mean nothing next to any other scale.
            nadResultD.store (D);
            nadGeneration.fetch_add (1);
        }
        nadBusy = false;
    });
}

//==============================================================================
// THE INVERSE SEARCH: adapt the SPECTRUM to a set of notes (see Dissonance.h).
//
// The analysis spectrum is anchored in absolute Hz exactly like the chord search, for
// the same reason: Sethares' critical band is a frequency width, not a ratio, so the
// same chord an octave lower wants a different timbre.

// The active tuning's degrees, as ratios above the tonic, at the tonic's real pitch.
std::vector<double> FractalAdditiveAudioProcessor::tuningAsTargetAlphas (double& f0Out) const
{
    std::vector<double> a;
    const int D = juce::jmax (1, tuning.degreesPerPeriod());
    a.reserve ((size_t) D);
    for (int d = 0; d < D; ++d)
    {
        const double r = getDegreeRatio (d);
        if (r > 0.0 && std::isfinite (r)) a.push_back (r);
    }
    // Same absolute anchor as the Degrees list and the n-ad search, so all three agree.
    f0Out = analysisAnchorHz();
    return a;
}

// The chord under the fingers, as ratios above its lowest note. The frequencies come
// from the rhythm engine's held-note bookkeeping (see RhythmEngine::getHeldChord), so
// they are the pitches that are REALLY sounding - the tuning's, per-note bend included.
std::vector<double> FractalAdditiveAudioProcessor::heldChordAsTargetAlphas (double& f0Out) const
{
    double hz[RhythmEngine::kMaxNotes] = {};
    const int n = rhythm.getHeldChord (hz, RhythmEngine::kMaxNotes);
    f0Out = 0.0;
    if (n <= 0 || ! (hz[0] > 0.0))
        return {};
    f0Out = hz[0];
    std::vector<double> a;
    a.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
        if (hz[i] > 0.0) a.push_back (hz[i] / f0Out);
    return a;
}

// The lookahead's early view of the same chord. The pitch formula is the one
// RhythmEngine::noteHz uses - the tuning's frequency for the key times its channel's
// bend - and the bend-range table is the same three values the rhythm engine and the
// MPE zone share, read from the same parameter, so "the notes about to sound" and "the
// notes sounding" can only ever differ by the budget, never by their tuning.
std::vector<double> FractalAdditiveAudioProcessor::heldChordEarlyAsTargetAlphas (double& f0Out) const
{
    f0Out = 0.0;
    int keys[LookaheadPeek::kMaxHeld], chans[LookaheadPeek::kMaxHeld];
    const int n = laPeek.getHeld (keys, chans, LookaheadPeek::kMaxHeld);
    if (n <= 0) return {};

    static constexpr double bendValues[] = { 2.0, 12.0, 48.0 };
    const double range = bendValues[juce::jlimit (0, 2,
        (int) apvts.getRawParameterValue ("bendRange")->load())];

    std::vector<double> hz;
    hz.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double semis = (double) laPeek.wheelFor (chans[i]) / 8192.0 * range;
        const double f = tuning.frequencyForNote (keys[i]) * std::exp2 (semis / 12.0);
        if (f > 0.0 && std::isfinite (f)) hz.push_back (f);
    }
    if (hz.empty()) return {};
    std::sort (hz.begin(), hz.end());
    f0Out = hz.front();
    for (auto& v : hz) v /= f0Out;
    return hz;
}

double FractalAdditiveAudioProcessor::measureCurrentAgainstTarget (const std::vector<double>& alphas,
                                                                  double f0Hz, bool rootOnly,
                                                                  const dissonance::Registers& regs) const
{
    if (alphas.empty() || ! (f0Hz > 0.0)) return 0.0;
    // The shared analysis spectrum: published template, SOUNDING amplitudes (this is
    // the "really sounding" number, so the user's per-partial edits must weigh in),
    // 64-cap. Anchored at the caller's f0 - the same anchor the adaptation itself
    // used - so "sounding now" and the adaptation's before/after are one scale.
    const auto spec = analysisSpectrumAt (f0Hz);
    if (spec.empty()) return 0.0;
    // ...and aggregated over the SAME registers, the same way (see the header): the MEAN,
    // which is what the search minimises and therefore the only scale "after" sits on.
    const auto rs = dissonance::scoreOverRegisters (spec, f0Hz, regs.f0, alphas, rootOnly,
                                                    dissonance::Goal::Min, 0.0, 0.0);
    return rs.mean;
}

// THE ANCHOR the register range hangs off (see the header). Held beats last-played beats
// key 60 - and the "held" part comes from the rhythm engine's own note bookkeeping, the
// same one heldChordAsTargetAlphas reads, because a second note-tracker is a second answer
// to "which note are we on" and they drift.
int FractalAdditiveAudioProcessor::registerAnchorKey() const noexcept
{
    // With lookahead active the anchor comes from the early view for the same reason the
    // target does: the register band the search optimises for has to belong to the note
    // it is preparing, not to the one still sounding a budget behind it.
    const int k = isLookaheadActive() ? laPeek.getAnchorNote() : rhythm.getAnchorNote();
    return k >= 0 ? juce::jlimit (0, 127, k) : 60;
}

// PERIODS -> THE TWO FREQUENCIES, in ONE place (see the header for why it is not keys).
// The anchor's pitch comes from the KEY LADDER, not from a sounding f0: f0 carries per-note
// pitch bend, and a band whose ends followed a bend wheel would resample the objective -
// and re-price the run - on every wheel message.
void FractalAdditiveAudioProcessor::registerBandHz (double periods,
                                                    double& loHz, double& hiHz) const noexcept
{
    const int anchor = registerAnchorKey();
    double a = getNoteFrequency (anchor);
    if (! (a > 0.0) || ! std::isfinite (a)) a = 261.6255653005986;   // no tuning yet: middle C
    loHz = hiHz = a;
    const double p = juce::jlimit (0.0, dissonance::kAdaptRegisterMaxPeriods,
                                   std::isfinite (periods) ? periods : 0.0);
    if (p <= 0.0) return;                       // a zero width IS the played register alone
    const double ratio = std::pow (tuning.periodRatio(), p);
    if (! (ratio > 1.0) || ! std::isfinite (ratio)) return;
    // CLAMPED TO THE BOARD at both ends, never wrapped and never re-anchored: the user said
    // "N periods either side of the note I am playing", and sliding the anchor to make N fit
    // would optimise for registers they never asked about. The panel prints the periods that
    // really fit at each end whenever this bites, so a shortened band is stated and not
    // silent - and it is now asymmetric near either edge, which is why the read-out has a
    // two-sided form at all.
    const double bottom = getNoteFrequency (0), top = getNoteFrequency (127);
    loHz = a / ratio;
    hiHz = a * ratio;
    if (bottom > 0.0 && std::isfinite (bottom)) loHz = std::max (loHz, std::min (a, bottom));
    if (top    > 0.0 && std::isfinite (top))    hiHz = std::min (hiHz, std::max (a, top));
}

// WHICH PERIOD the band is being built on, and how much of a fact it is (see the header).
// Message thread: the table estimate is not an atomic.
FractalAdditiveAudioProcessor::RegisterPeriodInfo
FractalAdditiveAudioProcessor::registerPeriodInfo() const
{
    RegisterPeriodInfo i;
    i.ratio = tuning.periodRatio();
    i.keys  = juce::jmax (1, getScaleDegrees());
    // A SCALE THAT STATES NO PERIOD IS NOT A SCALE THAT STATES ONE.
    // This used to return here for everything that is not an absolute table, i.e. it
    // called the answer a FACT for every .scl - including a descending one and a
    // degenerate 1/1, whose stated period is 0.0 and whose `ratio` above is therefore
    // the 2/1 display fallback. The band's unit was then an octave the user's file
    // never contained, printed with no qualifier at all, while the .tun that has the
    // very same problem got one. Now the qualifier follows the fact, not the format.
    // IN FORCE, not merely loaded, in all three tests below. A .tun table left over from
    // an earlier load with the box back on 12-TET made this print the TABLE's worst
    // deviation beside a 2/1 that 12-TET states exactly - a qualifier borrowed from a
    // tuning that is not sounding, on a band whose unit is a plain octave.
    if (! tuning.hasUsablePeriod())
    {
        i.source = RegisterPeriodSource::NoneStated;
        if (tuningTableIsInForce()) i.devCents = tuning.tablePeriodDevCents();
        return i;
    }
    if (! tuningTableIsInForce())
        return i;                      // a scale (or MTS master) states its own period
    i.devCents = tuning.tablePeriodDevCents();
    // `found` is false when the file has nothing ascending to measure at all. periodRatio()
    // then answers the 2/1-over-12-keys fallback, which is neither the file's nor the
    // user's - the one case where the band is standing on a number nobody supplied.
    i.source = (tuning.getTablePeriodEstimate().found || tuning.tablePeriodIsUserSet())
                 ? RegisterPeriodSource::TableEstimate
                 : RegisterPeriodSource::NoneStated;
    return i;
}

// THE REGISTER RANGE, resolved in ONE place. The panel prints what this returns and the
// search runs on what this returns, so the two can never disagree about which registers a
// spectrum was optimised for - and that answer genuinely differs (a spectrum optimised at
// C1 is ~3.7x rougher at C6 than one optimised there; see SpectrumTests).
//
// ANCHOR +- N PERIODS, all resolved here rather than in the editor: the realtime follower
// runs with the window shut, and a range the panel had baked into two frequencies would
// have stayed pinned to whatever register happened to be played when the window closed -
// which is the exact complaint (the same spectrum whatever octave you play in) this form
// exists to answer.
//
// BOTH DIRECTIONS. The band used to start AT the anchor and run up, so the spectrum was
// never asked what it does below the note under the fingers - and Sethares' critical band
// is a width in Hz, so that half of the keyboard is a different question, not a mirror of
// this one (SpectrumTests (8) measures it: one period below the played note costs 1.46x
// under the upward band and 1.27x under the symmetric one).
dissonance::Registers FractalAdditiveAudioProcessor::adaptRegistersFor (const SpectrumAdaptSettings& s,
                                                                        double f0Hz, bool realtime) const
{
    dissonance::Registers regs;
    const double per = s.regPeriods >= 0.0 ? s.regPeriods : dissonance::kAdaptRegisterDefaultPeriods;
    double lo = 0.0, hi = 0.0;
    registerBandHz (per, lo, hi);
    if (! (lo > 0.0) || ! std::isfinite (lo)) lo = f0Hz;
    if (! (hi > 0.0) || ! std::isfinite (hi)) hi = lo;
    if (hi < lo) std::swap (lo, hi);
    const int cap = realtime ? dissonance::kAdaptRegisterCapRt : dissonance::kAdaptRegisterCap;
    regs.f0 = dissonance::registerPoints (lo, hi, dissonance::registerCountFor (lo, hi, cap));
    return regs;
}

// The price of one run, resolved from the SAME settings the run itself will use - the
// partial count after its own clamp, and the register count adaptRegistersFor really
// returns for this path. Reading either of those from anywhere else would let the number
// on the button and the work the thread does drift apart.
double FractalAdditiveAudioProcessor::estimateAdaptMs (const SpectrumAdaptSettings& s,
                                                       int targetNotes, double f0Hz, bool realtime) const
{
    if (targetNotes <= 0) return 0.0;
    const int N    = juce::jlimit (2, 24, s.partials);
    const auto regs = adaptRegistersFor (s, f0Hz, realtime);
    return dissonance::adaptEstimateMs (N, targetNotes, s.rootOnly,
                                        (int) juce::jmax<size_t> (1, regs.f0.size()), s.shapeAmps);
}

bool FractalAdditiveAudioProcessor::startSpectrumAdapt (const SpectrumAdaptSettings& s,
                                                        const std::vector<double>& targetAlphas,
                                                        double f0Hz, juce::String& errorOut,
                                                        bool realtime, bool targetIsTuning)
{
    const int N = juce::jlimit (2, 24, s.partials);
    if (targetAlphas.empty() || ! (f0Hz > 0.0) || ! std::isfinite (f0Hz))
    {
        errorOut = "No target notes to adapt to.";
        return false;
    }
    const double period = s.periodRatio;
    if (! (period > 1.02) || ! std::isfinite (period))
    {
        errorOut = "Invalid period.";
        return false;
    }
    // The no-merge guarantee has to be POSSIBLE before the search starts, and it needs N
    // gaps, not N-1: partial 0 sits at 1/1 and the PERIOD itself is occupied - by partial
    // 0's own copy one period up, which the Custom Engine stacks there. A partial allowed
    // to reach the period is therefore a duplicate, and it was worse than that in practice:
    // the .scl text this writes ends with the period line, loadTimbreText dedupes equal
    // intervals, and a partial that landed exactly on 1200 c was ABSORBED into it - eight
    // partials asked for, seven compiled, no complaint anywhere. `topRatio` below keeps the
    // whole gap clear of the period, so that cannot happen.
    const double totalC = 1200.0 * std::log2 (period);
    const double gap    = juce::jlimit (0.0, 600.0, s.minGapCents);
    if (std::max (1.0, gap) * (double) N > totalC)
    {
        errorOut = "Those " + juce::String (N) + " partials cannot all keep "
                 + juce::String (gap, 1) + " cents of air inside a "
                 + juce::String (totalC, 1) + "-cent period (the period itself counts: the "
                   "spectrum repeats there). Use fewer partials, a smaller smallest "
                   "interval, or a wider period.";
        return false;
    }
    const double topRatio = std::exp2 ((totalC - std::max (1.0, gap)) / 1200.0);

    // COST IS A PRICE, NOT A VETO. This used to refuse outright: "all vs all over 31 notes
    // x 16 partials is too heavy to search - switch the scope to 0 vs all". It is a ~45 s
    // computation. Adapt is a one-shot, user-initiated calculation that already runs on a
    // worker, reports progress and cancels on a second press, so 45 s is a decision the
    // player is entitled to make - and refusing it removed the feature (a 31-note scale
    // simply could not be adapted to in the scope that hears every note against every
    // other). The estimate now goes on the button; only the genuinely absurd is stopped.
    //
    // The bound is kAdaptAbsurdPairs - ten minutes priced at the NOMINAL per-pair cost,
    // fixed in pair terms - and it exists because there IS a shape beyond argument:
    // 24 partials against a 128-key absolute table in "all vs all" is ~44 minutes,
    // minutes per single sweep, and it would be indistinguishable from a hang. The
    // refusal states the number rather than a verdict. Compared in PAIRS, not against
    // the calibrated ms estimate, because the estimate follows the machine and the
    // one-shot calibration probe can catch a load spike: measured once, that combination
    // refused the 31x16 all-vs-all search this very function was fixed to offer (see
    // kAdaptAbsurdPairs). The estimate still prices the button; only the refusal is
    // machine-independent.
    const auto regs = adaptRegistersFor (s, f0Hz, realtime);
    const int  nRegs = (int) std::max<size_t> (1, regs.f0.size());
    const double estMs = dissonance::adaptEstimateMs (N, (int) targetAlphas.size(), s.rootOnly,
                                                      nRegs, s.shapeAmps);
    if (dissonance::adaptTotalPairs (N, (int) targetAlphas.size(), s.rootOnly,
                                     nRegs, s.shapeAmps, 6) > dissonance::kAdaptAbsurdPairs)
    {
        errorOut = "This search is " + juce::String ((int) targetAlphas.size()) + " notes x "
                 + juce::String (N) + " partials"
                 + (s.rootOnly ? juce::String (", 0 vs all") : juce::String (", all vs all"))
                 + ", " + juce::String ((int) regs.f0.size()) + " registers - past the hard cap "
                   "(about " + juce::String ((int) (dissonance::kAdaptAbsurdMs / 60000.0))
                 + " minutes of computation; on this machine an estimated "
                 + juce::String (estMs / 60000.0, 1) + "). Fewer partials, fewer registers, or "
                   "the \"0 vs all\" scope will bring it inside.";
        return false;
    }

    adaptCancel = true;
    if (adaptThread != nullptr && adaptThread->joinable())
        adaptThread->join();
    adaptCancel = false;

    // SEED: the plain harmonic series 1, 2, 3... N folded into the period, at the user's
    // own slope, spaced evenly if folding would collide. Seeding from a harmonic timbre is
    // what makes the reported "before" mean something familiar - and it is the spectrum
    // Fibló makes with everything off.
    const double slope = juce::jlimit (0.1, 6.0, (double) apvts.getRawParameterValue ("curveSlope")->load());
    std::vector<dissonance::Partial> seed;
    seed.reserve ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        // Evenly spread inside the period: the harmonic series itself would put partial 2
        // at the octave, which is the period's own edge in the default 2/1 - and the
        // spread start already satisfies the minimum gap the check above proved possible.
        const double c = totalC * (double) i / (double) N;
        const double r = std::exp2 (c / 1200.0);
        seed.push_back ({ r * f0Hz, std::pow (r, -slope) });
    }
    jassert (seed.back().freq <= topRatio * f0Hz * 1.000001); // the spread must start legal

    adaptPeriodRatio = period;
    adaptProgress    = 0.0f;
    adaptRunning     = true;

    // PROVENANCE, captured NOW (message thread) and carried INSIDE the result: the user
    // can load another tuning while the worker runs, and the finished spectrum still
    // belongs to the tuning it was aimed at, not to whatever is active when it lands.
    // Only a tuning-targeted run gets one, and only when the active tuning can actually
    // be brought back from text (Scala mode with a source; MTS-ESP and Equal mode have
    // no reloadable source, and recording a stale .scl that is NOT what the alphas came
    // from would make the button load the wrong tuning while claiming this one).
    TuningManager::Snapshot prov;
    if (targetIsTuning && getTuningMode() == (int) TuningManager::Mode::Scala)
        prov = tuning.captureTuning();

    // `regs` was resolved above, BEFORE the thread starts and before the cost bound was
    // measured, from the same helper the panel reads - so the run, the read-out and the
    // price on the button are the same registers by construction.
    adaptThread = std::make_unique<std::thread> (
        [this, seed = std::move (seed), alphas = targetAlphas, f0Hz, s, N, gap, topRatio, regs, prov]()
        {
            const auto goal = s.goal == 1 ? dissonance::Goal::Max
                            : s.goal == 2 ? dissonance::Goal::Range
                                          : dissonance::Goal::Min;
            auto res = dissonance::adaptSpectrum (seed, f0Hz, alphas, s.rootOnly, goal,
                                                  s.rangeLo, s.rangeHi, gap, topRatio,
                                                  s.shapeAmps, (double) spectrum::kGainMaxDb, 6, regs,
                                                  [this] (float p) { adaptProgress.store (p); return ! adaptCancel.load(); });
            if (! adaptCancel.load() && ! res.spectrum.empty())
            {
                AdaptInfo info;
                info.ratios.reserve (res.spectrum.size());
                info.amps.reserve (res.spectrum.size());
                for (const auto& p : res.spectrum)
                {
                    info.ratios.push_back (p.freq / f0Hz);
                    info.amps.push_back ((double) p.amp);
                }
                info.before      = res.seedScore;
                info.after       = res.score;
                info.targetNotes = (int) alphas.size();
                info.valid       = (int) info.ratios.size() == N;
                info.regLoHz     = regs.f0.empty() ? f0Hz : regs.f0.front();
                info.regHiHz     = regs.f0.empty() ? f0Hz : regs.f0.back();
                info.regPoints   = (int) std::max<size_t> (1, regs.f0.size());
                info.mean        = res.mean;
                info.worst       = res.worst;
                info.seedMean    = res.seedMean;
                info.seedWorst   = res.seedWorst;
                info.perRegister = res.perRegister;
                info.registerF0  = res.registerF0;
                info.worstHz     = juce::isPositiveAndBelow (res.worstIndex, (int) res.registerF0.size())
                                       ? res.registerF0[(size_t) res.worstIndex] : f0Hz;
                info.provName    = prov.name;
                info.provSource  = prov.source;
                info.provPeriod  = prov.periodSpec;
                {
                    const juce::ScopedLock sl (adaptLock);
                    adaptResult = std::move (info);
                }
                adaptGeneration.fetch_add (1);
            }
            adaptProgress = 1.0f;
            adaptRunning  = false;
        });
    return true;
}

// Write the adapted spectrum into the Custom Engine.
//
// TWO channels, because the engine has two: the partial RATIOS go in as the Custom
// Engine's scale (typed intervals, the same path the Spectrum Builder uses), and the
// per-partial AMPLITUDES go in as manual gain edits (PartialGains.h), which is the ONLY
// place an amplitude can be set without the compiler's floor/cap/normalisation deleting
// partials in response to it. The ratios alone would leave the amplitudes on the slope
// curve - half the adaptation thrown away - so both are used.
bool FractalAdditiveAudioProcessor::applySpectrumAdapt (bool undoable, juce::String& errorOut)
{
    AdaptInfo info;
    {
        const juce::ScopedLock sl (adaptLock);
        info = adaptResult;
    }
    if (info.ratios.size() < 2)
    {
        errorOut = "Nothing adapted yet.";
        return false;
    }
    // The searcher marks a result invalid if it did not come back with the partial count it
    // was given. It cannot, by construction - but writing a spectrum with fewer partials
    // than the user asked for is exactly the failure this whole feature must never have, so
    // it refuses out loud rather than trusting the construction.
    if (! info.valid)
    {
        errorOut = "The adaptation came back with " + juce::String ((int) info.ratios.size())
                 + " partials instead of the number asked for. Nothing was written.";
        return false;
    }
    // Temper mode snaps the HARMONIC engine to the tuning and bypasses the Custom-Engine
    // scale entirely (engineInForce), so this would write a whole adapted spectrum that
    // never reaches a single voice - and it would overwrite whatever scale the Custom
    // Engine was holding to do it. Refuse and say which switch is in the way.
    if (apvts.getRawParameterValue ("timbreTemper")->load() > 0.5f)
    {
        errorOut = "Temper mode is on, so the harmonic engine's partials are snapped to the "
                   "tuning and a Custom-Engine spectrum is bypassed entirely - the adapted "
                   "spectrum would be written and never heard. Switch temper off first.";
        return false;
    }

    const auto before = captureSpecSnapshot();

    // MANUAL AMPLITUDE MODE GOES ON FIRST, and exactly the way timerCallback would do it
    // (freeze the slope, remember the mode). The per-partial edits below only apply while
    // it is on, but currentSettings() reads a DIFFERENT slope in each mode - so building
    // `ts` below in one mode and letting the next poll notice the other produced a second,
    // pointless template a few milliseconds behind the first: a second amplitude fade for
    // a spectrum that had not changed.
    if (apvts.getRawParameterValue ("partialManual")->load() <= 0.5f)
    {
        manualSlopeFrozen = apvts.getRawParameterValue ("curveSlope")->load();
        lastManualMode    = true;
        if (auto* pm = apvts.getParameter ("partialManual"))
            pm->setValueNotifyingHost (1.0f);
    }

    // The typed text: every partial above 1/1 in cents, then the PERIOD as the highest
    // value (loadTimbreText's rule: the top interval is the repeat point). 1/1 is implicit.
    // centsText, not String(x, 5): this text lands in the Custom Engine's box for the
    // user to read and edit, and a fixed 5-decimal print filled it with "100.00000c
    // 1200.00000c" noise. The 'c' suffix alone marks cents to the parser, so "100c"
    // round-trips identically - only genuinely inexact values keep their decimals.
    juce::String txt;
    for (size_t i = 1; i < info.ratios.size(); ++i)
        txt << dissonance::centsText (1200.0 * std::log2 (juce::jmax (1.0e-9, info.ratios[i]))).c_str() << "c ";
    txt << dissonance::centsText (1200.0 * std::log2 (juce::jmax (1.0001, adaptPeriodRatio))).c_str() << "c";

    if (! loadTimbreText (txt, errorOut))
        return false;
    // AFTER loadTimbreText (which clears it as "typed text"): this spectrum's origin is
    // the tuning the search was aimed at, carried inside the result itself - empty when
    // the target was a held chord, in which case the clear above is the truth.
    specProvName   = info.provName;
    specProvSource = info.provSource;
    specProvPeriod = info.provPeriod;

    // Amplitudes, resolved against what the compiler ACTUALLY produced. The gain is a dB
    // OFFSET from the compiled amplitude, so it can only be computed once the compiled
    // amplitudes are known - guessing them from slope would have silently mis-set every
    // one of them the moment Reps or fractal was not 1.
    //
    // The settings are built HERE rather than taken from currentSettings(): loadTimbreText
    // above turns the Custom Engine on through a PARAMETER, and this runs in the same call,
    // so currentSettings() could still be reading the old value and hand back the harmonic
    // engine's partials - against which not one of these gains would bind.
    auto ts = currentSettings();
    ts.source              = spectrum::Source::ScalaTimbre;
    ts.temperToTimbre      = false;
    ts.timbreDegrees       = timbreDegrees;
    ts.timbrePeriod        = timbrePeriod;
    ts.timbrePeriodRepeats = (int) apvts.getRawParameterValue ("timbreRepeats")->load();
    auto tmpl = spectrum::compileSpectrum (ts);   // published below - not thrown away
    std::vector<spectrum::PartialGain> gains;
    if (! tmpl.partials.empty())
    {
        // Normalise the adaptation's amplitudes to its own loudest partial, then match the
        // compiled template's loudest: the search's absolute level is arbitrary (Sethares
        // dissonance scales with amplitude) and pushing it onto the sound would change the
        // patch's volume, not its timbre.
        double aMax = 0.0, cMax = 0.0;
        for (const double a : info.amps) aMax = juce::jmax (aMax, a);
        for (const auto& p : tmpl.partials) cMax = juce::jmax (cMax, (double) p.amp);
        if (aMax > 0.0 && cMax > 0.0)
            for (size_t i = 0; i < info.ratios.size(); ++i)
            {
                // Find the compiled partial this adapted one became (nearest in cents).
                int best = -1; double bestC = 1.0e9;
                for (size_t q = 0; q < tmpl.partials.size(); ++q)
                {
                    const double d = std::abs (1200.0 * std::log2 (juce::jmax (1.0e-9,
                                        (double) tmpl.partials[q].ratio / info.ratios[i])));
                    if (d < bestC) { bestC = d; best = (int) q; }
                }
                if (best < 0 || bestC > 1.0) continue;   // not this period's copy: leave it alone
                const double want = info.amps[i] / aMax * cMax;
                const double have = (double) tmpl.partials[(size_t) best].amp;
                if (! (want > 0.0) || ! (have > 0.0)) continue;
                const float dB = (float) juce::jlimit (-(double) spectrum::kGainMaxDb,
                                                       (double) spectrum::kGainMaxDb,
                                                       20.0 * std::log10 (want / have));
                spectrum::setPartialGain (gains, spectrum::gainKey (info.ratios[i]), dB);
            }
    }
    // ONE EVENT, NOT THREE. An adaptation changes the partial RATIOS and their AMPLITUDES
    // together, and until now they reached the voices apart:
    //   1. setPartialGains republished the LIVE (still OLD) template with gain keys
    //      computed for the new partials. Keys are log-frequencies (PartialGains.h), so
    //      against the old list almost none of them bind - which also STRIPS the previous
    //      adaptation's shaping off the partials that are still sounding. That republish
    //      deliberately leaves the generation alone, so no morph starts and no 50 ms
    //      template fade arms: the voices got the whole amplitude change over the bare
    //      7 ms de-click slew. Measured (run 42): a one-hop short-term RMS step 4.6x the
    //      note's own ripple - +57% then -24% inside 20 ms. That is the reported "golpe".
    //   2. ~17-35 ms later the real template arrived, via the next timer poll and the
    //      build worker - pure latency, since the template was compiled RIGHT HERE.
    // So: hand the edits over WITHOUT republishing, then publish the template we already
    // built. publish() resolves the stored edits against it (fillGains), stamps a new
    // generation - which is what arms the fade and the morph - and swaps once.
    setPartialGains (gains, true, /*republish*/ false);
    spectrumEngine.publishNow (std::move (tmpl));
    // ...and the poll must not build the identical thing again a tick later: `ts` IS what
    // currentSettings() will return now that the Custom Engine holds these degrees.
    lastPushed = ts;

    if (undoable)
    {
        undoManager.beginNewTransaction();
        undoManager.perform (new SpecSnapshotAction (*this, before, captureSpecSnapshot()));
    }
    return true;
}

//==============================================================================
// REALTIME links (see the enum's comment in the header).
FractalAdditiveAudioProcessor::SpecSnapshot FractalAdditiveAudioProcessor::captureSpecSnapshot() const
{
    SpecSnapshot s;
    s.timbreText = timbreSource;
    s.gains      = partialGains;
    s.manual     = apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
    s.timbreOn   = apvts.getRawParameterValue ("timbreOn")->load() > 0.5f;
    s.taken      = true;
    s.provName   = specProvName;   // the origin is part of the spectrum being snapshot
    s.provSource = specProvSource;
    s.provPeriod = specProvPeriod;
    return s;
}

void FractalAdditiveAudioProcessor::applySpecSnapshot (const SpecSnapshot& s)
{
    juce::String err;
    if (s.timbreText.trim().isEmpty()) clearTimbre();
    else                              loadTimbreText (s.timbreText, err);
    // AFTER the load (loadTimbreText clears provenance): the snapshot's origin belongs
    // to the snapshot's spectrum, so undo/redo moves the two together - see SpecSnapshot.
    specProvName   = s.provName;
    specProvSource = s.provSource;
    specProvPeriod = s.provPeriod;
    setPartialGains (s.gains, s.manual);
    if (auto* p = apvts.getParameter ("partialManual"))
        p->setValueNotifyingHost (s.manual ? 1.0f : 0.0f);
    // AFTER the timbre text: loadTimbreText switches the Custom Engine ON by itself, and
    // a snapshot taken with it off has to come back off.
    if (auto* p = apvts.getParameter ("timbreOn"))
        p->setValueNotifyingHost (s.timbreOn ? 1.0f : 0.0f);
}

void FractalAdditiveAudioProcessor::takeRealtimeBaseline (RtLink mode)
{
    // BOTH trackers are cleared first. commitRealtimeBaseline returns early when the link
    // never applied anything, leaving its tracker set - so engaging the tuning link, leaving
    // it unused, then engaging a SPECTRUM link would have found rtTuneBaseTaken still true
    // and pushed a tuning undo step for a spectrum change.
    rtTuneBaseTaken = false;
    rtSpecBase = {};
    if (mode == RtLink::TuningFromSpectrum)
    {
        rtTuneBase      = tuning.captureTuning();
        rtTuneBaseMode  = getTuningMode();
        rtTuneBaseTaken = true;
        // ...and into the bank ONCE, so the tuning the follower is about to replace is a
        // single click away even without touching the undo history. bankAutoAdd skips
        // exact duplicates, so re-engaging does not add a second copy.
        if (rtTuneBase.source.isNotEmpty())
            bankAutoAdd (tuning.getScalaName().isNotEmpty() ? tuning.getScalaName()
                                                            : juce::String ("before realtime"),
                         rtTuneBase.source);
    }
    else
    {
        rtSpecBase = captureSpecSnapshot();
    }
}

void FractalAdditiveAudioProcessor::commitRealtimeBaseline()
{
    if (rtApplies <= 0) return;   // the link never changed anything: nothing to undo
    if (rtTuneBaseTaken && rtTuneBase.source.isNotEmpty())
    {
        undoManager.beginNewTransaction();
        undoManager.perform (new TuningSnapshotAction (*this, rtTuneBase, rtTuneBaseMode,
                                                       tuning.captureTuning(), getTuningMode()));
    }
    else if (rtSpecBase.taken)
    {
        undoManager.beginNewTransaction();
        undoManager.perform (new SpecSnapshotAction (*this, rtSpecBase, captureSpecSnapshot()));
    }
    rtTuneBaseTaken = false;
    rtSpecBase = {};
}

void FractalAdditiveAudioProcessor::setRealtimeLink (RtLink mode, const RtSettings& s)
{
    rtSet = s;
    if (mode == rtLink) return;

    if (rtLink != RtLink::Off)
        commitRealtimeBaseline();      // leaving: one undo step back to what it displaced

    rtLink    = mode;
    rtRefusal.clear();
    rtPending = mode != RtLink::Off;   // engaging adapts once immediately
    rtApplies = 0;
    rtStarts  = 0;
    rtLastStart = rtTick - 10000;      // the first launch is not throttled
    // Adopt the CURRENT result generations, so engaging never republishes a stale search
    // result as if the link had just produced it.
    rtSeenSpecGen  = spectrumEngine.getGeneration();
    rtSeenChordGen = chordGeneration.load();
    rtSeenAdaptGen = adaptGeneration.load();
    rtHeldTarget.clear();
    rtHeldF0  = 0.0;
    rtTuneSig = -1.0;
    // A landing measured by a link that is no longer running is not this link's answer.
    // Cleared rather than kept, so the button reads "-" until this link has produced a
    // real one instead of showing the previous mode's timing under the new mode's name.
    rtMovedAtMs = -1.0;
    rtLandingMs = -1.0;
    if (mode != RtLink::Off)
        takeRealtimeBaseline (mode);
    // The budget is only armed while a spectrum-side follower is running, so the link
    // changing is exactly when it has to be re-decided (and the host re-told).
    applyLookahead();
}

// Signature of a set of ratios: changes iff the set does (and cheap enough for 60 Hz).
static double ratioSetSig (const std::vector<double>& r)
{
    double s = (double) r.size() * 1.7;
    for (size_t i = 0; i < r.size(); ++i) s += r[i] * (double) (i + 3);
    return s;
}

int FractalAdditiveAudioProcessor::mixVoiceCountForTests (int corner) const noexcept
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners)) return -1;
    auto* e = mixEng[(size_t) corner].get();
    return e != nullptr ? e->synth.getNumVoices() : -1;
}

//==============================================================================
// MONOPHONIC GLIDE: the voice pool. Message thread only.
//
// ONE voice is the whole mechanism. With no free voice left, MPESynthesiser starts a
// new note by calling noteStarted() on the voice that is already sounding, which is
// precisely the takeover AdditiveVoice bends through; and a pool of one is also, by
// construction, the promise that the synth "cannot produce chords". Nothing is
// filtered out of the MIDI to achieve it - every note-on and note-off still arrives
// (see MonoGlide.h), so the modulator gate and the rhythm engine still see the real
// keyboard.
//
// reduceNumVoices() steals sounding voices properly (fast-kill, no click), and
// MPESynthesiser's own voice lock serialises this against the audio thread - the same
// contract MixCornerEngine::setVoices already relies on.
void FractalAdditiveAudioProcessor::applyMonoVoiceCount (bool mono)
{
    const int want = mono ? 1 : kPolyVoices;
    while (synth.getNumVoices() < want)
        synth.addVoice (new AdditiveVoice (apvts, spectrumEngine, tuning, modEngine));
    if (synth.getNumVoices() <= want) { monoShrinkTicks = 0; return; }

    // SHRINKING WITH KEYS DOWN. This is a panel switch and an automatable parameter, so
    // it WILL be thrown mid-chord, and reduceNumVoices() on its own got two things wrong
    // there - both of them audible.
    //
    //   * It DELETES sounding voices. A deleted voice stops mid-waveform: there is no
    //     fade, because the object that would render one is gone.
    //   * The wrong note survived. JUCE's findVoiceToSteal protects the lowest and the
    //     topmost sounding notes and gives the LOWEST the final say, so the note left
    //     ringing was the bottom of the chord - against this mode's own written contract,
    //     which is last-note priority ("the newest key always wins; a low-note rule would
    //     be a different instrument", MonoGlide.h). Measured on a 60/72/65 chord with 65
    //     pressed last: the survivor was 60.
    //
    // One two-step shrink answers both. FADE FIRST: every sounding voice except the one
    // the contract says to keep is ASKED to fade itself out over its own 5 ms fast kill,
    // the same fade a voice steal already uses. The request is an atomic flag the voice
    // consumes at the top of its own render (AdditiveVoice::requestFadeOut) rather than a
    // call made from here, because beginFastKill() from the message thread would mutate
    // ADSR state and currentlyPlayingNote underneath a block the audio thread is rendering
    // and MPESynthesiser's voicesLock is not reachable from outside.
    //
    // DELETE SECOND, on a later 60 Hz tick, once those voices have gone quiet. That is
    // also what makes the SURVIVOR right without any index surgery from this thread:
    // findFreeVoice() hands reduceNumVoices the INACTIVE voices first, so the one still
    // sounding is the one left standing, and every read of voice state that decides it
    // happens inside JUCE, under its own lock.
    int active = 0;
    juce::MPESynthesiserVoice* held = nullptr;   // newest voice whose KEY is still down
    juce::MPESynthesiserVoice* any  = nullptr;   // newest sounding voice, held or not
    for (int v = 0; v < synth.getNumVoices(); ++v)
    {
        auto* mv = synth.getVoice (v);
        if (mv == nullptr || ! mv->isActive()) continue;
        ++active;
        if (any == nullptr || mv->noteOnTime > any->noteOnTime) any = mv;
        // A key that has been LET GO is not what "the last note" means: a released note is
        // in its tail, and keeping the newest key still under the player's finger is the
        // same rule MonoGlide's own held-key stack follows.
        const auto ks = mv->getCurrentlyPlayingNote().keyState;
        if (ks == juce::MPENote::keyDown || ks == juce::MPENote::keyDownAndSustained)
            if (held == nullptr || mv->noteOnTime > held->noteOnTime) held = mv;
    }
    auto* keep = held != nullptr ? held : any;

    if (active > want && monoShrinkTicks < kMonoShrinkTicks)
    {
        ++monoShrinkTicks;
        // Re-asked every tick rather than once: a key pressed DURING the shrink takes the
        // newest slot, and last-note priority has to follow it there too.
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive() && av != keep)
                    av->requestFadeOut();
        return;   // the delete waits for the fades; re-entered from the 60 Hz tick
    }

    // Either the fades have landed, or nothing is rendering them - a stopped transport, a
    // bypassed plugin - and the mode's ONE-VOICE promise must not wait for ever on a voice
    // that will never be asked for another sample. The cap is half a second of ticks;
    // anything that could be heard faded a hundred times over before then.
    monoShrinkTicks = 0;
    synth.reduceNumVoices (want);
}

//==============================================================================
// ADAPTIVE LOOKAHEAD: arm or disarm the budget. Message thread only, because
// setLatencySamples() must not run on the audio thread.
//
// THE JUCE MECHANICS THIS RELIES ON, named so a future reader does not have to guess
// why a toggle is enough:
//   AudioProcessor::setLatencySamples(n) stores n and calls updateHostDisplay() with
//   ChangeDetails().withLatencyChanged(true). The VST3 wrapper turns that into the
//   Vst::kLatencyChanged flag and hands it to its ComponentRestarter (an AsyncUpdater),
//   which calls IComponentHandler::restartComponent on the MESSAGE thread; the host then
//   re-queries getLatencySamples() and re-runs its delay compensation. setLatencySamples
//   is a no-op when the value has not changed, so calling this every 60 Hz tick costs
//   nothing and cannot spam the host.
//
// THE BUDGET IS ROUNDED UP TO WHOLE BLOCKS. A host's compensation is exact in samples
// either way, but a whole number of blocks means the delayed copy of an event lands at
// the same offset WITHIN a block as the original, so nothing downstream that reasons in
// block-relative positions (the note gate's beat grid, the rhythm engine's pattern
// anchor) has to deal with an event that changed its place in the buffer.
//
// It is only armed while a SPECTRUM-side realtime link is running. Latency with no
// follower to feed would be a cost with nothing bought - and the tuning-side link
// (TuningFromSpectrum) is triggered by the spectrum, not by notes, so a head start on
// the keyboard would give it nothing either.
void FractalAdditiveAudioProcessor::applyLookahead()
{
    const bool want = rtSet.lookahead
                   && (rtLink == RtLink::SpectrumFromChord || rtLink == RtLink::SpectrumFromTuning);
    const int  sr   = (int) juce::jmax (1.0, lastSr > 0.0 ? lastSr : getSampleRate());
    const int  blk  = juce::jmax (16, lastBlockSize);

    int budget = 0;
    if (want)
    {
        const int raw = (int) std::ceil (kLookaheadMs * 0.001 * (double) sr);
        budget = ((raw + blk - 1) / blk) * blk;      // whole blocks, always >= the ms asked for
    }

    if (want == laLastOn && sr == laLastSr && blk == laLastBlk && budget == laSamples.load())
        return;

    laLastOn = want; laLastSr = sr; laLastBlk = blk;
    laSamples.store (budget, std::memory_order_relaxed);
    // Events already in flight were stamped for the OLD budget: leaving them would put
    // them at the wrong distance from everything around them. The audio thread does the
    // flush (and the all-notes-off that goes with it) so no MIDI state is touched from
    // two threads at once.
    laFlush.store (true, std::memory_order_release);
    setLatencySamples (budget);
}

void FractalAdditiveAudioProcessor::driveRealtimeLink()
{
    ++rtTick;
    if (rtLink == RtLink::Off)
        return;

    // FEEDBACK GUARD. In temper mode the TUNING builds the spectrum (the harmonic engine's
    // partials are snapped to the tuning's grid), so:
    //   - "tuning follows the spectrum" closes a loop: a new tuning retunes the spectrum,
    //     which fires a new search, which lands on another tuning... a runaway that never
    //     settles, retuning the instrument under the player's hands forever;
    //   - "spectrum follows the tuning" is pointless there anyway, because temper mode
    //     bypasses the Custom-Engine scale this writes into entirely.
    // Refuse and SAY so, rather than oscillate or pretend to work.
    if (apvts.getRawParameterValue ("timbreTemper")->load() > 0.5f)
    {
        rtRefusal = "Temper mode already builds the spectrum FROM the tuning. Running this "
                    "at the same time would feed each into the other with no end to it, so "
                    "realtime is off. Switch temper off to use it.";
        commitRealtimeBaseline();
        rtLink = RtLink::Off;
        return;
    }
    // The same loop the other way round: "from spectrum's segment" makes the SPECTRUM
    // build the tuning, so an adaptation driven by that tuning would chase its own tail.
    if (segmentModeActive && rtLink != RtLink::TuningFromSpectrum)
    {
        rtRefusal = "\"From spectrum's segment\" already builds the tuning FROM the spectrum. "
                    "Adapting the spectrum to that tuning would feed each into the other, so "
                    "realtime is off.";
        commitRealtimeBaseline();
        rtLink = RtLink::Off;
        return;
    }

    if (rtLink == RtLink::TuningFromSpectrum)
    {
        // THE TRIGGER IS THE SPECTRUM. The template generation is bumped by every rebuild
        // - a knob, a modulator, a preset - and by nothing else, so it is exactly "the
        // spectrum changed" and nothing more.
        if (const auto gen = spectrumEngine.getGeneration(); gen != rtSeenSpecGen)
        {
            rtSeenSpecGen = gen;
            rtPending     = true;      // COALESCED: many changes while busy = one search
        }
        if (rtPending && ! chordRunning.load() && rtTick - rtLastStart >= kRtTuneTicks)
        {
            rtPending   = false;
            rtLastStart = rtTick;
            juce::String err;
            // Four results, not sixteen: the follower only ever adopts the top one, and
            // the refinement pass costs per result kept.
            if (startChordSearch (rtSet.notes, rtSet.periodRatio, rtSet.periodLine, 4,
                                  rtSet.maxima, rtSet.minGapCents, rtSet.rootOnly, err))
            {
                ++rtStarts;
            }
            else
            {
                // A launch that cannot even start means the link can never do its job with
                // these settings. Switching it off (and saying why) beats a lit REALTIME
                // button that silently does nothing for ever.
                rtRefusal = err;
                commitRealtimeBaseline();
                rtLink = RtLink::Off;
            }
        }
        if (const int g = chordGeneration.load(); g != rtSeenChordGen)
        {
            rtSeenChordGen = g;
            juce::String err;
            if (loadChordAsScale (0, err, /*addToBank*/ false))
                ++rtApplies;
        }
        return;
    }

    // ...the two SPECTRUM-side links. Same throttle + coalesce, aimed at a different target.
    double f0 = 0.0;
    std::vector<double> alphas;
    const bool fromChord = rtLink == RtLink::SpectrumFromChord;
    // Read BEFORE anything is published: whether a finished result still belongs to the
    // hands on the keys is the first thing that has to be decided about it.
    // WITH LOOKAHEAD ON the follower reads the notes that have ARRIVED, not the ones
    // that are sounding: they are the same notes one budget apart, and that budget is
    // exactly the head start the search is being given (see LookaheadDelay.h).
    if (fromChord) alphas = isLookaheadActive() ? heldChordEarlyAsTargetAlphas (f0)
                                                : heldChordAsTargetAlphas (f0);

    // Has the chord actually MOVED away from the set the last search was AIMED at?
    // Compared note by note, in cents, including the root's absolute pitch: Sethares'
    // roughness is a frequency width, so the same chord an octave up is a different target
    // and must re-adapt, while a bend jitter below a cent must not.
    bool moved = false;
    if (fromChord && ! alphas.empty())
    {
        moved = alphas.size() != rtHeldTarget.size()
             || ! (rtHeldF0 > 0.0)
             || std::abs (1200.0 * std::log2 (f0 / rtHeldF0)) > kRtHeldMoveCents;
        for (size_t i = 0; i < alphas.size() && ! moved; ++i)
            if (std::abs (1200.0 * std::log2 (alphas[i] / rtHeldTarget[i])) > kRtHeldMoveCents)
                moved = true;
    }

    // START THE STOPWATCH the lookahead budget is racing (see getLastRealtimeLandingMs).
    // Here, not at the launch below: the interval that matters to the user is from the
    // chord MOVING to the spectrum landing, and the throttle's wait before the search is
    // even allowed to start is the largest part of it.
    if (moved && rtMovedAtMs < 0.0)
        rtMovedAtMs = juce::Time::getMillisecondCounterHiRes();

    // A finished adaptation is published FIRST, before anything below can decide there is
    // nothing to do. A chord let go a moment before its search finished was still a chord
    // the user played, and the result belongs to it: held back here, it would have been
    // applied on some later tick against a completely different chord.
    //
    // ...but a result aimed at a chord the player has already MOVED OFF is a different
    // thing entirely, and applying it is heard as a lurch: a full respectrum onto a
    // spectrum nobody asked for, replaced by the right one a few tens of milliseconds
    // later. Two spectrum changes where the music called for one. So it is dropped -
    // and dropping it costs nothing, because `moved` has already set rtPending and the
    // search for the chord that IS being held starts on the next allowed tick. "Let go"
    // (alphas empty) is deliberately NOT stale: that case is the one above.
    if (const int g = adaptGeneration.load(); g != rtSeenAdaptGen)
    {
        rtSeenAdaptGen = g;
        if (! (fromChord && ! alphas.empty() && moved))
        {
            juce::String aerr;
            if (applySpectrumAdapt (/*undoable*/ false, aerr))
            {
                ++rtApplies;
                // STOP THE STOPWATCH. This is the number the lookahead button prints and
                // the only honest answer to "did the budget fit?" - measured on the user's
                // machine with the user's settings, rather than estimated from a constant
                // that is known wrong here (see kLookaheadMs).
                if (rtMovedAtMs > 0.0)
                    rtLandingMs = juce::Time::getMillisecondCounterHiRes() - rtMovedAtMs;
                rtMovedAtMs = -1.0;
            }
        }
    }

    if (fromChord)
    {
        // THE TRIGGER IS WHAT IS BEING PLAYED. The frequencies come from the rhythm
        // engine's held-note bookkeeping, which is published from every processBlock
        // whether or not the rhythm engine is switched on, so they are the pitches really
        // sounding - the tuning's, per-note bend included. (Read at the top of this
        // function, because the publish above has to know whether they still match.)
        if (alphas.empty())
        {
            // NOTHING held: leave the spectrum exactly as it is, and DROP any pending
            // adaptation. Without this, engaging the link with nothing down left rtPending
            // true and the first chord to arrive was adapted to with whatever this tick
            // happened to read - and, worse, a chord released mid-search came back as a
            // re-adaptation to a chord that was no longer being played.
            rtPending = false;
            return;
        }
        // ONE NOTE IS A LEGAL TARGET. The bar here used to be two notes, so a monophonic
        // line - a lead, a bass, one finger on the keyboard - lit the REALTIME button and
        // then never adapted once, for ever: the guard returned before startSpectrumAdapt
        // was ever reached. Nothing in the model needed that. With a single note alphas is
        // {1.0} and dissonance::targetScore collapses, under BOTH scopes, to exactly
        // dissonance::selfDissonance (spec, 1.0) - the spectrum's own INTERNAL roughness,
        // the beating of the partials against each other with no chord involved. That is
        // what the user asked to minimise and it is far from degenerate: sliding one
        // partial of an 8-partial harmonic seed across 300 cents at C4 swings the measure
        // from 6.50 to 7.25 (see SpectrumTests, "a SINGLE held note").
        //
        // The register comes along for free, and it has to: heldChordAsTargetAlphas hands
        // back f0 = the note's REAL sounding frequency, and Sethares' critical band is a
        // width in Hz, not in cents - the same partial set is a different roughness an
        // octave down, and the adaptation answers with different partial positions there.
        // (`moved` - "is this still the chord the last search was aimed at?" - is computed
        // at the top of this function; it decides BOTH whether to search again and whether
        // an already-finished result is still worth applying.)
        if (moved) rtPending = true;
    }
    else
    {
        alphas = tuningAsTargetAlphas (f0);
        if (alphas.size() < 2)
            return;
        const double sig = ratioSetSig (alphas) + (double) getTuningMode() * 3.0;
        if (std::abs (sig - rtTuneSig) > 1.0e-9) { rtTuneSig = sig; rtPending = true; }
    }

    if (rtPending && ! adaptRunning.load() && rtTick - rtLastStart >= kRtAdaptTicks)
    {
        rtPending   = false;
        rtLastStart = rtTick;
        juce::String err;
        // `realtime` = true: the follower takes the SMALLER register cap (3 instead of 7).
        // The cost is exactly linear in the number of registers and this runs on a ~250 ms
        // throttle, so seven of them would simply stop it keeping up with the fingers. The
        // panel says so out loud (see the "registers" read-out) rather than degrading quietly.
        if (startSpectrumAdapt (rtSet.adapt, alphas, f0, err, /*realtime*/ true,
                                /*targetIsTuning*/ ! fromChord))
        {
            ++rtStarts;
            // Remember what this search is aimed at, not what merely passed by: the
            // throttle can hold a launch for several ticks and the chord may have changed
            // again in between, so the reference has to be the target actually used.
            rtHeldTarget = alphas;
            rtHeldF0     = f0;
        }
        else
        {
            rtRefusal = err;          // same rule as above: a link that cannot start, stops
            commitRealtimeBaseline();
            rtLink = RtLink::Off;
            return;
        }
    }
}

bool FractalAdditiveAudioProcessor::saveCurrentTuningScl (const juce::File& f, juce::String& errorOut)
{
    // A .scl can only say "N degrees repeating at a period" (a per-key map needs a .kbm
    // alongside it), so an absolute table cannot go out this way without being flattened
    // into a scale it is not. .tun holds it exactly - that is why .tun is the default.
    //
    // IN FORCE, not merely loaded (tuningTableIsInForce, not usesAbsoluteTable): with a
    // .tun table loaded earlier and the box back on 12-TET, this refused to export 12-TET
    // as a .scl on the grounds that the tuning is a table - which by then it was not.
    if (tuningTableIsInForce())
    {
        errorOut = "This tuning is a 128-key table (a .tun that does not repeat), and a .scl "
                   "can only hold degrees repeating at a period. Save it as .tun instead - "
                   "that keeps every key exactly.";
        return false;
    }
    const auto scl = currentTuningAsScl (f.getFileNameWithoutExtension());
    if (! f.replaceWithText (scl))
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
    // Build the phase engine's fader-slope table HERE, on a thread that is allowed to be
    // slow. A live re-phase reads it on the audio thread, and the FIRST read is the one
    // that fills 16384 entries behind an initialisation guard - 16384 std::sin calls and
    // a lock, in a render callback, exactly once, which is a dropout nobody could ever
    // reproduce. Idempotent, so calling it on every prepare costs nothing.
    spectrum::phaseset::warmScatterDeltas();
    // ...and the same argument for the three MIDI buffers the self-play and preview paths
    // fill. juce::MidiBuffer allocates on its first addEvent, so reserving here means the
    // FIRST block after a prepare does not allocate either - not just the ones after it.
    // A generous 4096 bytes each: a pulse train at the fastest rate over a large buffer is
    // a few hundred events, and this is message-thread memory bought once.
    previewMidi.ensureSize (4096);
    rtHeld     .ensureSize (4096);
    rtSynthMidi.ensureSize (4096);

    synth.setCurrentPlaybackSampleRate (sampleRate);
    previewCollector.reset (sampleRate);
    noteGate.prepare (sampleRate);
    monoGate.prepare();
    laDelay.prepare();
    laPeek.reset();
    modEngine.prepare (sampleRate);
    modNoteOns = 0; // in step with ModEngine::prepare - the first note-on must retrigger
    modHeld = 0;
    for (auto& row : modKeyHeld) row.fill (false);
    lastSr = sampleRate;

    // MIX mode's scratch buses are sized HERE, on the prepare call, because the audio
    // thread must never allocate: a block larger than this falls back to the plain
    // single-engine render (defensive - hosts honour their own samplesPerBlock).
    lastBlockSize = juce::jmax (16, samplesPerBlock);
    mixScratch.setSize (2, juce::jmax (512, lastBlockSize));
    // THE UNIT IS BYTES, AND THAT IS THE WHOLE DEFECT. MidiBuffer::ensureSize takes BYTES,
    // not events, and JUCE stores each event as its data plus a 4-byte sample position and a
    // 2-byte length (juce_MidiBuffer.cpp: getEventTotalSize) - 9 bytes for a note-on. This
    // buffer used to reserve 512 of them, which is 56 events, and it is filled on the MIX
    // engage edge with the ENTIRE incoming block PLUS one note-on for every key held at that
    // instant (see renderSynthMix). Past 56, MidiBuffer reallocates: on the audio thread, in
    // a render callback, which is the one thing that must never happen here. It was the last
    // buffer in the plugin still at the old number - NoteGate and MonoGlide reserve 4096, the
    // lookahead ring 8192.
    //
    // SIZED TO THE FILL'S OWN BOUND rather than to a comfortable guess. That fill is a
    // 16-channel x 128-key loop, so it can emit at most 2048 note-ons = 18,432 bytes; the
    // rest of this reservation is ~14 kB, about 1,500 more events, for the copy of the
    // incoming block that goes in beside them. 32 kB, once per prepareToPlay, next to four
    // sub-engines each holding their own audio buffers.
    //
    // WHAT IT STILL DOES NOT COVER, said out loud rather than papered over: the incoming
    // block is not bounded by anything this plugin owns, so a host block carrying more than
    // ~1,500 events on top of a full hold would grow the buffer anyway. When that happens
    // MidiBuffer GROWS - it never drops an event - so the cost is one allocation and never a
    // lost note. RhythmVerify run 67 measures the bound that IS ours: the 2048-event fill,
    // against this exact constant.
    mixInject.ensureSize ((size_t) kMixInjectBytes);
    for (auto& e : mixEng)
        if (e != nullptr)
        {
            e->synth.setCurrentPlaybackSampleRate (sampleRate);
            e->mods.prepare (sampleRate);
            e->buf.setSize (2, juce::jmax (512, lastBlockSize));
        }
    lastGain = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("masterGain")->load());

    // Build once synchronously so a template exists before the first note.
    lastPushed = currentSettings();
    // The plan analyses which partials can sound at this rate, so it has to know it
    // BEFORE the compile below publishes a template to measure.
    spectrumEngine.setRenderRate (sampleRate);
    spectrumEngine.buildNow (lastPushed);
    // ...and ask the worker to plan it, off the message thread (see requestPlan).
    spectrumEngine.requestPlan();

    applyTuningMode ((int) apvts.getRawParameterValue ("tuningMode")->load());
    // The voice pool and the latency budget both depend on things prepareToPlay is the
    // first to know (a restored monoGlide value; the rate and block the budget rounds to).
    lastMonoGlide = apvts.getRawParameterValue ("monoGlide")->load() > 0.5f;
    applyMonoVoiceCount (lastMonoGlide);
    applyLookahead();
    rhythm.prepare (sampleRate);

    // Poll the spectrum parameters and rebuild off the audio thread. 60 Hz (not 30):
    // a modulator driving a parameter that REBUILDS the spectrum (Base N, Harmonics,
    // Slope, Fractal, Prime, Range, Reps) is only sampled here, so a fast envelope
    // used to be caught on some notes and missed on others.
    // 12ED2 is not a mode, it is a SCALE - so Scala is the default mode and it opens
    // holding 12ED2. A fresh instance therefore sounds exactly as it always did, but the
    // twelve-tone scale is now sitting in the editor where it can be seen and replaced,
    // instead of hiding behind a mode entry that could not be chosen.
    if (! tuning.hasScala())
    {
        juce::String twelve ("! 12ED2.scl\n!\n12 equal divisions of the octave\n 12\n!\n");
        for (int i = 1; i <= 12; ++i) twelve << " " << juce::String (i * 100.0, 6) << "\n";
        juce::String err;
        tuning.loadScalaString (twelve, "12ED2", err);
    }

    startTimerHz (60);

    // Resample the jingle to THIS session's rate once (guarded so a later
    // buffer-size-only prepareToPlay doesn't redo it) - never touches jingleActive,
    // so it cannot cause or prevent playback, only prepare the samples for it.
    if (! jingleResampled && jingleNative.getNumSamples() > 0)
    {
        jingleResampled = true;
        const double ratio = jingleNativeSr / (sampleRate > 0.0 ? sampleRate : jingleNativeSr);
        const int    outN  = juce::jmax (1, (int) std::ceil ((double) jingleNative.getNumSamples() / ratio));
        jingleOut.setSize (jingleNative.getNumChannels(), outN);
        for (int ch = 0; ch < jingleNative.getNumChannels(); ++ch)
        {
            const float* src = jingleNative.getReadPointer (ch);
            float*       dst = jingleOut.getWritePointer (ch);
            const int    srcN = jingleNative.getNumSamples();
            for (int i = 0; i < outN; ++i)
            {
                const double pos  = (double) i * ratio;
                const int    i0   = juce::jlimit (0, srcN - 1, (int) pos);
                const int    i1   = juce::jmin (srcN - 1, i0 + 1);
                const float  frac = (float) (pos - (double) i0);
                dst[i] = src[i0] + (src[i1] - src[i0]) * frac;
            }
        }
        jingleNative.setSize (0, 0); // the native copy has done its job
    }
}

bool FractalAdditiveAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::stereo();
}

// Mixes (adds, never overwrites) the next chunk of the resampled jingle into this
// block. Only ever runs while a title click has asked for it; when it reaches the
// end it stops and stays stopped until the next explicit request.
void FractalAdditiveAudioProcessor::playJingleInto (juce::AudioBuffer<float>& buffer)
{
    if (jingleOut.getNumSamples() == 0) { jingleActive = false; return; } // not resampled yet or empty
    const int remaining = jingleOut.getNumSamples() - jingleReadPos;
    if (remaining <= 0) { jingleActive = false; return; }
    const int n = juce::jmin (remaining, buffer.getNumSamples());
    const int outCh = buffer.getNumChannels();
    const int jinCh  = jingleOut.getNumChannels();
    // Levelled to sit WITH the synth's own output rather than over it.
    constexpr float kJingleGain = 0.12f; // ~ -18 dB
    for (int ch = 0; ch < outCh; ++ch)
        buffer.addFrom (ch, 0, jingleOut, juce::jmin (ch, jinCh - 1), jingleReadPos, n, kJingleGain);
    jingleReadPos += n;
    if (jingleReadPos >= jingleOut.getNumSamples()) jingleActive = false;
}

void FractalAdditiveAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                  juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();

    const int  nSamp    = buffer.getNumSamples();

    // The title was clicked: (re)start the jingle from the top. Consumed here so
    // the read position is only ever touched on the audio thread.
    if (jingleRestart.exchange (false, std::memory_order_acq_rel))
    {
        jingleReadPos = 0;
        jingleActive  = jingleOut.getNumSamples() > 0;
    }
    if (jingleActive) playJingleInto (buffer);

    // A HOST'S PANIC IS A HARD SILENCE REQUEST, and it was not being treated as one.
    //
    // All Notes Off (CC123) and All Sound Off (CC120) are the two commands a host is
    // entitled to assume work - they are what a transport stop, a panic button and a
    // plugin-window close send. The synth acted on them only as far as MPEInstrument
    // does, which leaves the rhythm engine's pulse trains running, the drone voice on its
    // reserved channel sounding, and the MIX corner engines untouched. Measured with
    // self-play and the drone on: All Notes Off took the output from 1.6e-1 to 5.3e-2 and
    // All Sound Off changed it by nothing at all. A stuck note the user has no way to
    // stop is the worst thing a synth can do.
    //
    // The kill below is exactly the right answer and already exists - it drains the main
    // synth AND every corner engine, resets the note gates and the mono gate, and flushes
    // the lookahead - so a panic simply asks for it. Scanned here, immediately before the
    // consumer, so the silence lands in the SAME block the message arrived in rather than
    // the next one. It is a read of the incoming buffer and one atomic store: nothing is
    // allocated and nothing is locked.
    for (const auto meta : midiMessages)
    {
        const auto m = meta.getMessage();
        if (m.isAllNotesOff() || m.isAllSoundOff())
        { pendingHardSilence.store (true, std::memory_order_release); break; }
    }

    // A full state swap (preset load, project reload, "load its original spectrum")
    // was seen leaving a note stuck sounding forever: it rewrites every DSP
    // parameter - envelope times, curves, the additive spectrum itself - under an
    // active voice's feet, and could hand it a contradictory mix of old and new
    // values that never reaches silence. restoreState() only flags the request
    // (it can run on the message thread); the actual kill happens HERE, on the
    // audio thread, so no voice/NoteGate state is ever touched from two threads.
    if (pendingHardSilence.exchange (false, std::memory_order_acq_rel))
    {
        keyboardState.allNotesOff (0);  // any physically-held key gets its note-off THIS block
        synth.turnOffAllVoices (false);        // backstop: instant silence regardless of the cause
        // The MIX sub-engines too: a project swap must not leave a corner voice
        // sounding a preset the new project does not even contain. Idle voices finish
        // instantly; sounding ones fast-kill and are drained by renderSynthMix's own
        // fade-out path (the mix is still rendered while its crossfade is non-zero).
        for (auto& pe : mixEngLive)
            if (auto* ce = pe.load (std::memory_order_acquire))
                ce->synth.turnOffAllVoices (false);
        noteGate.prepare (lastSr > 0.0 ? lastSr : getSampleRate());
        monoGate.reset();
        laDelay.flush(); laPeek.reset();
        modHeld = 0; modNoteOns = 0;
        for (auto& row : modKeyHeld) row.fill (false);
    }

    // AudioProcessor::reset() raised this (see the header for why the work is not done
    // there). Everything above already ran - reset() sets pendingHardSilence too, and it
    // is what silences the voices, empties the lookahead ring and clears the mono glide's
    // held-key stack. What is left is the one thing a PRESET LOAD deliberately does not
    // want: the running pattern's own position. A host reset means "forget what you were
    // doing", so the phases, the gates, the held chord and the engine clock all go back
    // to zero and the next note starts a pattern instead of joining one that is still
    // mid-cycle from before the bypass.
    // resetState() is prepare() minus its single ensureSize, so nothing here allocates.
    if (pendingHostReset.exchange (false, std::memory_order_acq_rel))
        rhythm.resetState();

    // Merge on-screen / computer-keyboard notes into the incoming MIDI stream.
    keyboardState.processNextMidiBuffer (midiMessages, 0, nSamp, true);

    //== ADAPTIVE LOOKAHEAD (see LookaheadDelay.h) =============================
    // The order here IS the feature: the follower's early view is taken from the stream
    // at its ORIGINAL time, and only then is the stream re-stamped one budget later for
    // everything that sounds. Both halves read the same buffer one statement apart.
    {
        const int budget = laSamples.load (std::memory_order_relaxed);

        // A TRANSPORT DISCONTINUITY - a loop jumping back, a locator jump, playback
        // starting or stopping - invalidates everything still in flight: those events
        // belong to bars the transport has left, and replaying them at the loop point
        // is a ghost note the user never wrote. Detected as a ppq position that did not
        // advance by roughly this block's own length.
        bool jumped = false;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                const bool playing = pos->getIsPlaying();
                double ppq = -1.0e30;
                if (pos->getPpqPosition().hasValue()) ppq = *pos->getPpqPosition();
                if (playing != laWasPlaying) jumped = true;
                else if (playing && ppq > -1.0e29 && laLastPpq > -1.0e29 && lastSr > 0.0)
                {
                    double bpm = 120.0;
                    if (pos->getBpm().hasValue() && *pos->getBpm() > 1.0) bpm = *pos->getBpm();
                    const double expect = (double) nSamp * bpm / (60.0 * lastSr);
                    // Half a block of tolerance either way: hosts round their ppq, and a
                    // tempo ramp inside one buffer moves it slightly off the prediction.
                    if (std::abs (ppq - laLastPpq - expect) > expect * 0.5 + 1.0e-6) jumped = true;
                }
                laWasPlaying = playing;
                laLastPpq    = playing ? ppq : -1.0e30;
            }

        // CONSUME THE PENDING-FLUSH FLAG UNCONDITIONALLY, AND BEFORE THE TEST BELOW.
        // It used to be the right-hand side of `jumped || laFlush.exchange (...)`, which
        // SHORT-CIRCUITS: on a block that had jumped, the exchange never ran, so the
        // pending flush survived into the NEXT block and destroyed a ring that by then
        // held a real note. Measured with lookahead armed, identical processors, identical
        // MIDI, identical block counts, the only difference being whether a playhead was
        // attached: no playhead -> first audible sample 12032 (one budget, as designed);
        // with one -> the note NEVER sounded. It fires on every offline bounce and every
        // transport start with lookahead armed, because applyLookahead leaves the flag set
        // on the first poll after prepareToPlay (laLastSr starts at 0).
        const bool pendingFlush = laFlush.exchange (false, std::memory_order_acq_rel);

        // A JUMP ONLY FLUSHES WHAT THERE IS TO FLUSH. rtSet.lookahead defaults to false,
        // so budget == 0 with an empty ring is not "a configuration" - it is what every
        // player gets who never opens the realtime panel, and there is then nothing in
        // flight for a transport discontinuity to invalidate. Without this guard, simply
        // pressing PLAY cut off every key the player was holding (measured: voices 1 -> 0,
        // rms 0.02508 -> 0.00000), and the jump edge is symmetric so STOP and a loop wrap
        // cut them too. That kill arrived in d44263e; its parent had no transport-jump
        // handling at all.
        // With lookahead ARMED the kill deliberately REMAINS, because there the two cases
        // are genuinely different: the ring really does hold events the host stamped for
        // bars the transport has left, replaying them at the loop point is a ghost note
        // nobody wrote, and a buffered note-on dropped with its note-off would hold for
        // ever. run 52 asserts both halves side by side so neither can be "simplified"
        // into the other.
        if ((jumped && (budget > 0 || ! laDelay.isEmpty())) || pendingFlush)
        {
            // Dropping buffered note-ons is the point; a buffered note-OFF is a
            // different animal and is DELIVERED with this block instead. Its note-on
            // left the ring one budget earlier: it has already sounded and has already
            // been counted by the note gate, the mono glide, the modulator tally and the
            // rhythm engine's held chord. Dropping it left all four believing the key
            // was still down - and the rhythm engine then generated pulse trains from a
            // key nobody was holding and emitted that MIDI to the DAW for the rest of
            // the session (measured: 24 ghost pulses in the 600 blocks after one wrap).
            // The turnOffAllVoices below silences the audio; only the delivered note-off
            // can correct the bookkeeping.
            laDelay.flushKeepingNoteOffs();
            laPeek.reset();
            monoGate.reset();
            synth.turnOffAllVoices (false);
            for (auto& pe : mixEngLive)
                if (auto* ce = pe.load (std::memory_order_acquire))
                    ce->synth.turnOffAllVoices (false);
        }

        if (budget > 0) laPeek.observe (midiMessages);   // ...the notes that HAVE arrived
        laDelay.process (midiMessages, nSamp, budget);   // ...and the stream that will sound
    }
    // Chord-preview MIDI (editor play buttons): kept SEPARATE - it feeds only the
    // synth, never the rhythm engine (a preview must not trigger pulse trains or
    // leak to the DAW's MIDI out).
    previewMidi.clear();
    previewCollector.removeNextBlockOfMessages (previewMidi, nSamp);

    // Note-start gate (all default off): quantize note starts to the host's beat
    // grid, mono behaviour, and/or quantize to the running pattern's reset.
    {
        const bool qTempo = apvts.getRawParameterValue ("rhythmQTempo")->load() > 0.5f;
        const bool qPulse = apvts.getRawParameterValue ("rhythmQPulse")->load() > 0.5f;
        // THE GATE'S OWN "Mono" STANDS DOWN WHILE MONOPHONIC GLIDE IS ON, and this is not
        // a nicety - the two are different answers to the same question and the gate's one
        // wins by arriving first. Its Mono emits a note-OFF for the sounding key at the
        // SAME sample as the new note-on and BEFORE it, so the single voice is already in
        // its release tail when the new note lands: AdditiveVoice::noteStarted then sees
        // `releasing` and correctly refuses to call it a legato takeover, and the note
        // restrikes at its own pitch. The result was a glide switch that silently did
        // nothing whenever this older toggle happened to be on.
        // Monophonic glide is already mono (the pool is ONE voice), so nothing is lost by
        // standing the gate down - the notes it would have cut are cut by the takeover
        // instead, with a bend where it had a restrike. The editor disables the gate's
        // toggle and says so rather than leaving it lit over a mode it is not driving.
        const bool monoGlideOn = apvts.getRawParameterValue ("monoGlide")->load() > 0.5f;
        const bool mono   = ! monoGlideOn
                         && apvts.getRawParameterValue ("rhythmMono")->load() > 0.5f;
        double beatOff = -1.0, beatPer = -1.0;
        if (qTempo)
            if (auto* ph = getPlayHead())
                if (auto pos = ph->getPosition())
                    if (pos->getIsPlaying() && pos->getBpm().hasValue() && pos->getPpqPosition().hasValue())
                    {
                        const double bpm = *pos->getBpm();
                        if (bpm > 1.0 && lastSr > 0.0)
                        {
                            beatPer = lastSr * 60.0 / bpm;
                            const double frac = *pos->getPpqPosition() - std::floor (*pos->getPpqPosition());
                            beatOff = frac < 1.0e-9 ? 0.0 : (1.0 - frac) * beatPer;
                        }
                    }
        noteGate.process (midiMessages, nSamp, qTempo, mono, qPulse,
                          beatOff, beatPer, qPulse ? rhythm.samplesToNextReset() : -1.0,
                          qPulse ? rhythm.samplesSinceAnchor()  : -1.0);
    }

    // MONOPHONIC GLIDE's note-stream half, AFTER the gate (a note held back by the beat
    // grid has not been played yet, so it cannot take anything over) and before anything
    // that sounds. It adds the legato return to a still-held key and nothing else; the
    // mono-ness itself comes from the one-voice pool (see applyMonoVoiceCount).
    monoGate.process (midiMessages, nSamp,
                      apvts.getRawParameterValue ("monoGlide")->load() > 0.5f);

    // MODULATORS (3 envelopes + 3 LFOs): stepped once per block, writing straight
    // into the target parameters' raw atomics (base value + modulation) - smooth,
    // host-silent, RT-safe. Held state is tallied from THIS block's events (post-
    // gate stream is consistent) so releases are sample-accurate to the block and
    // a note that starts and ends inside one block still releases. Chord-preview
    // notes count too: the preview must sound with the same envelopes as playing.
    {
        // Held state is a SET of (channel, key), not a counter: mono kills and
        // preview stops can emit a note-off for a key that is already silent, and a
        // plain counter would then under-run and release the envelopes while real
        // keys are still down.
        const auto tally = [this] (const juce::MidiBuffer& buf)
        {
            for (const auto meta : buf)
            {
                const auto& m = meta.getMessage();
                const int ch = juce::jlimit (1, 16, m.getChannel()) - 1;
                if (m.isNoteOn())
                {
                    ++modNoteOns;
                    auto& f = modKeyHeld[(size_t) ch][(size_t) m.getNoteNumber()];
                    if (! f) { f = true; ++modHeld; }
                }
                else if (m.isNoteOff())
                {
                    auto& f = modKeyHeld[(size_t) ch][(size_t) m.getNoteNumber()];
                    if (f) { f = false; modHeld = std::max (0, modHeld - 1); }
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    for (auto& row : modKeyHeld) row.fill (false);
                    modHeld = 0;
                }
            }
        };
        tally (midiMessages);
        tally (previewMidi);
        double bpm = -1.0, ppq = -1.0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (pos->getBpm().hasValue()) bpm = *pos->getBpm();
                if (pos->getIsPlaying() && pos->getPpqPosition().hasValue()) ppq = *pos->getPpqPosition();
            }
        // LFO fundamental: the driver pitch the rhythm engine tracks (it follows the
        // held note INCLUDING its per-note pitch bend, which matters in MPE). Only
        // when that is silent - e.g. during a chord preview, which is deliberately
        // kept out of the rhythm engine - fall back to the lowest key we know is down
        // so the preview modulates exactly like playing those keys would.
        double f0 = (double) rhythm.getDriverF0();
        if (! (f0 > 0.0) && modHeld > 0)
        {
            int lowest = 128;
            for (int ch = 0; ch < 16; ++ch)
                for (int k = 0; k < lowest; ++k)
                    if (modKeyHeld[(size_t) ch][(size_t) k]) { lowest = k; break; }
            if (lowest < 128) f0 = getNoteFrequency (lowest);
        }
        modEngine.processBlock (nSamp, modNoteOns, modHeld > 0, f0, bpm, ppq);
        // Each voice runs its OWN modulators (see AdditiveVoice), so it needs the
        // transport to lock a per-voice LFO to the host timeline. Its LFO fundamental is
        // its own played pitch, which it already knows.
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                av->setTransportForMod (bpm, ppq);
        // Kept for the MIX sub-engines, whose modulators run inside renderSynthMix
        // with exactly these numbers - a corner's transport-locked LFO must read the
        // same clock the main engine's does.
        curBpm = bpm; curPpq = ppq; curF0 = f0;
    }
    const bool midiOut  = apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f;
    const bool selfWant = apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f;
    const bool drone    = apvts.getRawParameterValue ("rhythmDrone")->load()    > 0.5f;
    // Self-play only ENGAGES once a partial is routed - otherwise the synth would go
    // silent out of the box (held notes consumed, no pulses to replace them with).
    const bool selfPlay = selfWant && rhythm.hasEnabledSlot();

    // THE stuck-note bug: self-play rewrites note-OFFs onto channel 16, but a note
    // that started before self-play engaged is sounding on the channel it was
    // played on. MPE legacy mode matches note-offs by (channel, note), so after the
    // switch its note-off never reaches the voice and it sustains forever. Routing a
    // partial auto-enables self-play, so this flipped mid-hold constantly.
    // Any change of the routing - either direction - therefore ends every sounding
    // note and closes every open pulse gate, so nothing can be stranded on the far
    // side of the switch.
    if (! selfPlayKnown)
    {
        selfPlayKnown = true;      // first block: just adopt the state, nothing to strand yet
        wasSelfPlay   = selfPlay;
    }
    else if (selfPlay != wasSelfPlay)
    {
        wasSelfPlay = selfPlay;
        keyboardState.allNotesOff (0);
        synth.turnOffAllVoices (false);
        // THE MIX SUB-ENGINES TOO, and this is not symmetry for its own sake - it is the
        // same stuck note, one engine over. With MIX engaged a held key is sounding in the
        // corner engines as well (renderSynthMix feeds them the same stream, and the engage
        // edge deliberately re-strikes every held key into them), and from the next block on
        // self-play rewrites every note-OFF onto channel 16. That rewritten off matches
        // nothing a corner voice is holding, so the corner DRONES FOR EVER: the key is up,
        // the main engine is silent, and the sound continues with no key to release and no
        // panic that can reach it - the host's all-notes-off goes to keyboardState and the
        // main synth, which are already quiet. Routing a partial auto-enables self-play, so
        // the flip that strands them is one click in the rhythm panel while a note is held.
        //
        // MEASURED, and it took a corrected measurement to see it: with the pulses routed to
        // the SAME note and channel as the held key, the pulse's own gate note-off happened
        // to release the stranded voice and the whole thing looked fine (that collision is
        // the hazard the drone's reserved channel 16 exists for, see below). Route them
        // anywhere else and the corner holds a steady 0.0255 RMS for as long as anyone cares
        // to render, a full second after the key came up. run 61 does exactly that.
        //
        // The two siblings of this kill - the hard-silence backstop at the top of
        // processBlock and the lookahead flush above - have carried this loop from the day
        // they were written; this one was missed.
        for (auto& pe : mixEngLive)
            if (auto* ce = pe.load (std::memory_order_acquire))
                ce->synth.turnOffAllVoices (false);
        rhythm.flushOpenGates();
        rhythm.flushEchoes();
        noteGate.prepare (lastSr > 0.0 ? lastSr : getSampleRate());
        monoGate.reset();
        modHeld = 0;
        for (auto& row : modKeyHeld) row.fill (false);
    }

    if (selfPlay)
    {
        // "Play in Fibló": the rhythm MIDI feeds THIS synth. The played note is only a
        // TRIGGER - it drives the trains but does not sound (unless "note under pulses"
        // is on). NO feedback loop: rhythm.process reads the ORIGINAL played MIDI, never
        // its own pulse output, so pulses never spawn pulses.
        // Drone ("note under pulses") lives on RESERVED channel 16: a pulse landing on
        // the same channel+note would otherwise kill the drone voice with its gate
        // note-off. Note-offs are forwarded ALWAYS (so switching the drone off with a
        // note sounding can never strand its voice); note-ons/bends only while on.
        rtHeld.clear();   // a member, pre-reserved: see its declaration
        auto& held = rtHeld;
        for (const auto meta : midiMessages)
        {
            auto m = meta.getMessage();
            // EVERYTHING THAT CAN STOP A NOTE IS FORWARDED, ALWAYS. Note-offs already
            // were - "so switching the drone off with a note sounding can never strand
            // its voice" - but that reasoning covers the pedal and the panic messages
            // exactly as well, and they were being dropped on the floor. A drone voice
            // lives on reserved channel 16 and is the only thing that hears this buffer,
            // so with self-play engaged the sustain pedal did nothing and the host's own
            // All Notes Off could not stop a drone: the note hung until the plugin was
            // removed. A stuck note the user has no way to stop is the worst thing a
            // synth can do, and a panic message is the one command a host is entitled to
            // assume works.
            //
            // The pedals go through unconditionally too, and not only while the drone is
            // on: a pedal DOWN captured before the drone was switched off has to be
            // releasable afterwards, which is the same argument the note-off line makes.
            const bool stopper = m.isNoteOff()
                              || m.isAllNotesOff() || m.isAllSoundOff() || m.isResetAllControllers()
                              || m.isSustainPedalOn()   || m.isSustainPedalOff()
                              || m.isSostenutoPedalOn() || m.isSostenutoPedalOff();
            if (stopper)                             { m.setChannel (16); held.addEvent (m, meta.samplePosition); }
            else if (drone && (m.isNoteOn() || m.isPitchWheel() || m.isAftertouch() || m.isChannelPressure()))
                                                     { m.setChannel (16); held.addEvent (m, meta.samplePosition); }
        }
        rhythm.process (midiMessages, nSamp);              // midiMessages := pulse MIDI

        rtSynthMidi.clear();
        auto& synthMidi = rtSynthMidi;
        synthMidi.addEvents (midiMessages, 0, nSamp, 0);   // synth plays the pulses...
        synthMidi.addEvents (held, 0, nSamp, 0);           // ...and the drone channel (ch 16)
        synthMidi.addEvents (previewMidi, 0, nSamp, 0);    // chord preview always audible AS a chord
        renderSynthMix (buffer, synthMidi, nSamp);

        if (! midiOut) midiMessages.clear();               // internal-only: don't leak pulses to the DAW
    }
    else if (! previewMidi.isEmpty())
    {
        // Preview active: the synth hears played notes + the preview; the rhythm
        // engine still sees ONLY the played notes.
        rtSynthMidi.clear();
        auto& synthMidi = rtSynthMidi;
        synthMidi.addEvents (midiMessages, 0, nSamp, 0);
        synthMidi.addEvents (previewMidi, 0, nSamp, 0);
        renderSynthMix (buffer, synthMidi, nSamp);
        rhythm.process (midiMessages, nSamp);
    }
    else
    {
        renderSynthMix (buffer, midiMessages, nSamp);      // held notes sound normally
        // Timbre -> rhythm MIDI out: generated in this very block with exact sample
        // stamps, from the same f0/tuning maths as the voices -> sample-locked to the
        // audio. Consumes the input events; only the pulse MIDI leaves the plugin.
        rhythm.process (midiMessages, nSamp);
    }

    // SHEPARD MODE is rendered INSIDE the voices now (see AdditiveVoice) - there is
    // no separate engine to mix in any more, and nothing here touches the buffer.
    // What still belongs here is the one fact only the transport knows: Sync was
    // chosen but the host offers no BPM, so the FREE ms value is what actually drives
    // the rise. The voices reach the same conclusion from the same curBpm; this flag
    // is purely so the editor can SAY it instead of letting the beats box claim a
    // sync that is not happening.
    shepSyncNoBpm.store ((int) apvts.getRawParameterValue ("shepMode")->load() == 1
                             && curBpm <= 1.0,
                         std::memory_order_relaxed);

    const auto gain = juce::Decibels::decibelsToGain (
        apvts.getRawParameterValue ("masterGain")->load());
    buffer.applyGainRamp (0, nSamp, lastGain, gain);
    lastGain = gain;
}

//==============================================================================
// THE MIX RENDER (the audio-thread half of playground MIX mode).
//
// With the mix disengaged and fully faded this is a tail call into the ordinary
// single-engine render - bit-for-bit the old behaviour, zero overhead. Otherwise:
//
//   out = main * sqrt(1 - x)  +  sum_i corner_i * g_i * sqrt(x)
//
// where x is the engage crossfade (50 ms one-pole) and g_i the per-corner
// equal-power gains (7 ms one-pole towards sqrt(w_i)). EVERY gain is applied as a
// per-sample linear ramp from the value the previous block ended on
// (applyGainRamp), so neither a drag, an automation jump across the pad, nor the
// engage/disengage switch can ever step the waveform - the mixer equivalent of the
// voices' 7 ms de-click slew.
//
// The MAIN engine keeps rendering the whole time, fed the ordinary MIDI; only its
// OUTPUT is faded. Its voices, envelopes and note bookkeeping therefore stay
// exactly what they would be with mix off - a note released while it was inaudible
// still received its note-off - so disengaging is a pure fade with nothing to
// rebuild and no voice ever orphaned. The corners are the opposite: they are COLD
// on engage, so the keys held at that moment are re-struck into them (the tally
// knows which keys, not their velocities - a fixed healthy velocity is used and the
// corner's own envelope shapes the rest), and once a disengage fade completes their
// voices are fast-killed and given a few muted blocks to actually finish -
// stopping cold would leave fastKill armed on voices nothing ever renders again,
// the exact trap beginFastKill documents.
void FractalAdditiveAudioProcessor::renderSynthMix (juce::AudioBuffer<float>& out,
                                                    const juce::MidiBuffer& midi, int nSamp)
{
    // THE HOST'S MPE CONFIGURATION MESSAGE UNDOES OUR ZONE, so the zone is checked here
    // and put back before anything is rendered.
    //
    // "lo pongo en mpe, y le envio midi retuneado con mpe y no reacciona, cuando otros
    // sintes compatibles con mpe si lo hacen." Measured, in two halves:
    //
    //   * an MCM whose member count is ZERO - which is how a host says "MPE off", and
    //     which several send before configuring - deactivates the lower zone, after which
    //     MPEInstrument::noteOn early-returns for every channel and the synth is
    //     PERMANENTLY DEAF. applyTuningMode cannot save it: its zone write is behind
    //     `if (mpe != lastZoneMpe)` and that edge has already been spent.
    //   * ANY MCM re-writes the zone through JUCE's own handler, which calls setLowerZone
    //     with a DEFAULTED master range of 2. That silently undoes the +-48 master range
    //     the comment in applyTuningMode explains at length - a retuning arriving on
    //     channel 1 then collapses to 1/24 of itself. Measured: a note asking for 320.222
    //     Hz came out at 263.851, which is 335 cents flat.
    //
    // The repair follows the host where following costs nothing and refuses where it
    // costs the feature: the MEMBER COUNT and the PER-NOTE range are whatever the host
    // last asked for, because those are its business; the zone is never allowed to be
    // inactive while this mode is on, and the MASTER range is held at 48, because both of
    // those are what the UI promises when it hides the bend selector in this mode.
    //
    // Only when it has actually drifted: MPEInstrument::setZoneLayout releases every
    // sounding note, so re-asserting a zone that is already right would cut notes off on
    // every block.
    if (lastZoneMpe)
    {
        auto layout = synth.getZoneLayout();
        const auto lower = layout.getLowerZone();
        const bool dead  = ! lower.isActive();
        if (dead || lower.masterPitchbendRange != 48)
        {
            layout.setLowerZone (dead ? 15 : lower.numMemberChannels,
                                 dead ? 48 : lower.perNotePitchbendRange,
                                 48);
            synth.setZoneLayout (layout);
            for (auto& e : mixEng)
                if (e != nullptr) e->synth.setZoneLayout (layout);
        }
    }

    const bool   on = mixEngaged.load (std::memory_order_acquire);
    const double sr = lastSr > 0.0 ? lastSr : 44100.0;

    const float xTgt = on ? 1.0f : 0.0f;
    const float aX   = 1.0f - (float) std::exp (-(double) nSamp / (0.050 * sr));
    float xNow = mixXfade + (xTgt - mixXfade) * aX;
    if (std::abs (xNow - xTgt) < 1.0e-4f) xNow = xTgt;

    const bool anyMixWork = on || mixXfade > 0.0f || mixTailBlocks > 0;
    mixXfade = xNow;
    if (! anyMixWork)
    {
        mixWasOn       = on;
        mixMainApplied = 1.0f;
        for (auto& a : mixApplied) a = 0.0f;   // the next engage ramps in from silence
        synth.renderNextBlock (out, midi, 0, nSamp);
        return;
    }

    // ENGAGE edge: re-strike the held keys into the cold corner engines (see above).
    const juce::MidiBuffer* cornerFeed = &midi;
    if (on && ! mixWasOn)
    {
        mixInject.clear();
        mixInject.addEvents (midi, 0, nSamp, 0);
        for (int ch = 0; ch < 16; ++ch)
            for (int k = 0; k < 128; ++k)
                if (modKeyHeld[(size_t) ch][(size_t) k])
                    mixInject.addEvent (juce::MidiMessage::noteOn (ch + 1, k, (juce::uint8) 100), 0);
        cornerFeed = &mixInject;
    }
    mixWasOn = on;

    // The main engine's bus. sqrt(1-x) against the corners' sqrt(x): equal-power, so
    // the hand-over neither sags nor bumps.
    const float mainNow = std::sqrt (juce::jmax (0.0f, 1.0f - xNow));
    if (mixScratch.getNumSamples() >= nSamp && mixScratch.getNumChannels() > 0)
    {
        mixScratch.clear();
        synth.renderNextBlock (mixScratch, midi, 0, nSamp);
        mixScratch.applyGainRamp (0, nSamp, mixMainApplied, mainNow);
        const int srcCh = mixScratch.getNumChannels();
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.addFrom (ch, 0, mixScratch, juce::jmin (ch, srcCh - 1), 0, nSamp);
    }
    else
    {
        // Defensive: a block larger than prepareToPlay promised. The main engine must
        // sound whatever happens; the mix sits this block out.
        synth.renderNextBlock (out, midi, 0, nSamp);
    }
    mixMainApplied = mainNow;

    // The corners.
    const float aG   = 1.0f - (float) std::exp (-(double) nSamp / (0.007 * sr));
    const float xAmp = std::sqrt (juce::jmax (0.0f, xNow));
    bool anyCorner = false;
    for (int i = 0; i < kPlaygroundCorners; ++i)
    {
        auto* e = mixEngLive[(size_t) i].load (std::memory_order_acquire);
        if (e == nullptr) continue;
        anyCorner = true;

        // This corner's OWN modulators (reading its bank), and the transport push its
        // voices' per-voice LFOs need - the same clock the main engine's voices got.
        e->mods.processBlock (nSamp, modNoteOns, modHeld > 0, curF0, curBpm, curPpq);
        for (int v = 0; v < e->synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (e->synth.getVoice (v)))
                av->setTransportForMod (curBpm, curPpq);

        mixGainCur[i] += (mixGainTarget[i].load (std::memory_order_relaxed) - mixGainCur[i]) * aG;
        const float now = mixGainCur[i] * xAmp;
        if (e->buf.getNumSamples() < nSamp) { mixApplied[i] = now; continue; }  // defensive, as above
        e->buf.clear();
        e->synth.renderNextBlock (e->buf, *cornerFeed, 0, nSamp);
        e->buf.applyGainRamp (0, nSamp, mixApplied[i], now);
        mixApplied[i] = now;
        const int srcCh = e->buf.getNumChannels();
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.addFrom (ch, 0, e->buf, juce::jmin (ch, srcCh - 1), 0, nSamp);
    }

    // Disengage fade COMPLETED: kill the corner voices and drain them (see above).
    if (! on && xNow <= 0.0f && anyCorner)
    {
        if (! mixKilled)
        {
            mixKilled = true;
            // Enough muted blocks for the 5 ms fast-kill to reach finishNote at ANY
            // block size (10 ms of samples, plus slack) - a fixed block count was too
            // short at small blocks, which is precisely the stuck-fastKill trap.
            mixTailBlocks = (int) std::ceil (0.010 * sr / (double) juce::jmax (1, nSamp)) + 2;
            for (auto& pe : mixEngLive)
                if (auto* ce = pe.load (std::memory_order_acquire))
                    ce->synth.turnOffAllVoices (false);
        }
        else if (mixTailBlocks > 0)
        {
            --mixTailBlocks;
        }
    }
    else if (on)
    {
        mixKilled     = false;
        mixTailBlocks = 0;
    }
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

    // The ModEngine writes base+mod into the raw atomics the flush/copy above
    // snapshots, so a saved value would otherwise drift towards its momentary
    // modulated position. Re-write the user's BASE for EVERY parameter (the
    // parameter object always holds the base - modulation never goes through
    // setValue), which also cleans up targets that were unmapped while displaced.
    for (auto* ap : getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (ap))
        {
            auto child = state.getChildWithProperty ("id", p->paramID);
            if (child.isValid())
                child.setProperty ("value", p->convertFrom0to1 (p->getValue()), nullptr);
        }

    state.removeChild (state.getChildWithName ("SCALA"), nullptr);
    if (const auto snap = tuning.captureTuning(); snap.source.isNotEmpty())
    {
        juce::ValueTree scl ("SCALA");
        scl.setProperty ("name",   snap.name,   nullptr);
        scl.setProperty ("source", snap.source, nullptr);
        // The source is the FILE (a .scl, or the .tun text of an absolute 128-key table),
        // so the table itself reopens byte-exact. What the file does NOT carry is the
        // period the user chose for a table - it is Fibló's metadata, not the .tun's - so
        // it rides here, or a reopened project silently fell back to the estimate.
        if (snap.periodSpec.isNotEmpty())
            scl.setProperty ("tablePeriod", snap.periodSpec, nullptr);
        state.appendChild (scl, nullptr);
    }

    // THE HAND-SET REFERENCE, PARKED - and the promise timerCallback makes about it.
    //
    // While MTS-ESP is the mode and a master is up, the master OWNS refA: the parameter is
    // overwritten with what the master is really doing (display only, so the box does not
    // lie) and the number the user typed lives in manualRefA, handed straight back the
    // instant MTS stops being the mode. The comment there states it outright - "MTS can
    // never permanently overwrite a reference the user typed" - and that held for the
    // session and not one moment longer: refA is a PARAMETER and rode out in the tree
    // above, while manualRefA and the flag saying it is parked were plain members that
    // rode nowhere. Type 432, switch to MTS-ESP, save, reopen: the chunk carries the
    // master's 440 in refA, manualRefA comes back at its 440 default, and no action can
    // recover the 432 because it was never written down.
    //
    // The FLAG travels too, and it is not optional. Without it a project saved in MTS mode
    // reopens with mtsHadRef false, so leaving MTS looks like it was never entered and the
    // parked value is never handed back - the number would be saved and still unreachable.
    state.removeChild (state.getChildWithName ("REFPARK"), nullptr);
    {
        juce::ValueTree rp ("REFPARK");
        rp.setProperty ("manualA", manualRefA, nullptr);
        rp.setProperty ("mtsOwns", mtsHadRef,  nullptr);
        state.appendChild (rp, nullptr);
    }

    state.removeChild (state.getChildWithName ("TIMBRE"), nullptr);
    if (timbreSource.isNotEmpty()) // kept even while switched OFF (On/Off toggle)
    {
        juce::ValueTree tim ("TIMBRE");
        tim.setProperty ("name",   timbreName,   nullptr);
        tim.setProperty ("source", timbreSource, nullptr);
        tim.setProperty ("on",     isTimbreActive(), nullptr);
        // The spectrum's tuning provenance rides on the TIMBRE child because it IS a
        // property of this spectrum: saved with it, gone with it. Full source text
        // (like bankSource), so the button still delivers when the original bank
        // slot/file no longer exists at reload time.
        if (specProvSource.isNotEmpty())
        {
            tim.setProperty ("provName",   specProvName,   nullptr);
            tim.setProperty ("provSource", specProvSource, nullptr);
            if (specProvPeriod.isNotEmpty())
                tim.setProperty ("provPeriod", specProvPeriod, nullptr);
        }
        state.appendChild (tim, nullptr);
    }

    // Per-partial amplitude edits. Stored as one compact "key:dB" string rather than a
    // child per edit: there can be thousands of them and a ValueTree node each would
    // bloat every project file. Keys are ratios in hundredths of a cent (PartialGains.h),
    // so an edit reopens on the partial it was made on even if the spectrum settings
    // changed in between.
    state.removeChild (state.getChildWithName ("PGAINS"), nullptr);
    if (! partialGains.empty())
    {
        juce::String packed;
        packed.preallocateBytes ((size_t) partialGains.size() * 16);
        for (const auto& g : partialGains)
            packed << g.key << ':' << juce::String (g.dB, 3) << ' ';
        juce::ValueTree pg ("PGAINS");
        pg.setProperty ("v", packed.trim(), nullptr);
        state.appendChild (pg, nullptr);
    }

    // THE FROZEN SLOPE, which belongs to the drawing above and was travelling nowhere.
    //
    // While manual per-partial amplitudes are on, currentSettings() compiles the template at
    // manualSlopeFrozen instead of at the fader - the amplitudes are the user's drawing, and
    // the slope the drawing started from is what makes switching manual on inaudible. It is
    // refreshed on exactly ONE event, the moment manual goes on (timerCallback, and the two
    // paths that arm manual themselves). Nothing refreshes it on a state load, so a patch
    // that arrives with manual ALREADY on compiled at whatever the PREVIOUS patch happened
    // to leave here - this instance's 1.0 default on a fresh instrument, or the last thing
    // anyone drew in this session.
    //
    // That is not cosmetic. The amplitude floor culls by an amplitude that depends on slope:
    // at -120 dB it removes everything above ratio 10^(6/s), which is nothing at slope 1 and
    // everything above ratio ~32 at slope 4 (see currentSettings). Load a slope-4 patch and
    // the template physically lacks the partials the drawing was made on, so the user's own
    // edits up there stop applying - their work dropped, silently, by a number that came
    // from a different patch. That is this project's cardinal sin, so the number rides with
    // the patch.
    state.removeChild (state.getChildWithName ("MANUALAMP"), nullptr);
    {
        juce::ValueTree ma ("MANUALAMP");
        ma.setProperty ("slope", manualSlopeFrozen, nullptr);
        state.appendChild (ma, nullptr);
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
                slot.setProperty ("vel",      s.vel.load(),         nullptr);
                slot.setProperty ("on",       s.enabled.load(),     nullptr);
                ry.appendChild (slot, nullptr);
            }
        }
        if (ry.getNumChildren() > 0)
            state.appendChild (ry, nullptr);
    }

    // Modulator routings + drawn LFO waves.
    state.removeChild (state.getChildWithName ("MODS"), nullptr); // never stack a stale copy
    {
        juce::ValueTree ms ("MODS");
        for (int m = 0; m < ModEngine::kMods; ++m)
        {
            juce::String csv;
            for (int t = 0; t < modEngine.numTargets (m); ++t)
                csv += (csv.isEmpty() ? "" : ",") + modEngine.targetId (m, t);
            if (csv.isNotEmpty()) ms.setProperty ("t" + juce::String (m), csv, nullptr);
        }
        for (int l = 0; l < ModEngine::kLfos; ++l)
        {
            if (lfoWavePoints[(size_t) l].isNotEmpty())
                ms.setProperty ("pts" + juce::String (l), lfoWavePoints[(size_t) l], nullptr);
            if (const float* w = modEngine.getCustomWave (l))
            {
                juce::String tw;
                for (int i = 0; i < ModEngine::kWaveN; ++i)
                    tw += (i ? "," : "") + juce::String (w[i], 4);
                ms.setProperty ("w" + juce::String (l), tw, nullptr);
            }
        }
        if (ms.getNumProperties() > 0) state.appendChild (ms, nullptr);
    }

    // "From spectrum's segment": the mode, the picked partials, the period spec, and
    // the generated .scl text (so the picker + Save survive a reopen; the tuning
    // itself already persists via the SCALA child).
    state.removeChild (state.getChildWithName ("SEGMENT"), nullptr); // never stack a stale copy
    {
        juce::ValueTree sg ("SEGMENT");
        sg.setProperty ("modeActive", segmentModeActive, nullptr);
        sg.setProperty ("anchor",     segAnchor,         nullptr);
        sg.setProperty ("periodText", segPeriodText,     nullptr);
        sg.setProperty ("useTop",     segUseTopPeriod,   nullptr);
        juce::String sel;
        for (int i : segSel) sel += (sel.isEmpty() ? "" : ",") + juce::String (i);
        sg.setProperty ("sel", sel, nullptr);
        sg.setProperty ("scl",  lastDissScl,  nullptr);
        sg.setProperty ("name", lastDissName, nullptr);
        state.appendChild (sg, nullptr);
    }

    // THE ADAPTIVE / REALTIME SPECTRUM IS PART OF THE PATCH, not something outside it.
    // A preset whose spectrum follows the chord under your fingers is a different
    // instrument from the same preset with the follower off, so saving it saves that -
    // and the settings it follows WITH (how many partials, the period, the goal, the
    // register width), or reloading would arm a follower aimed somewhere else.
    state.removeChild (state.getChildWithName ("REALTIME"), nullptr);
    state.appendChild (realtimeSettingsToTree(), nullptr);

    // THE PLAYGROUND: which preset is on which corner. Only the NAMES - the corners are
    // presets in their own right and re-reading the files on load is what keeps a corner
    // in step with the preset it points at (and stops a project file carrying four whole
    // copies of other presets around). pgX/pgY are ordinary parameters and are already in
    // the tree above.
    state.removeChild (state.getChildWithName ("PLAYGROUND"), nullptr);
    if (getPlaygroundAssignedCount() > 0)
    {
        juce::ValueTree pg ("PLAYGROUND");
        for (int i = 0; i < kPlaygroundCorners; ++i)
            if (pgCorner[(size_t) i].assigned())
                pg.setProperty ("c" + juce::String (i), pgCorner[(size_t) i].name, nullptr);
        state.appendChild (pg, nullptr);
    }
    return state;
}

juce::ValueTree FractalAdditiveAudioProcessor::realtimeSettingsToTree() const
{
    juce::ValueTree rt ("REALTIME");
    rt.setProperty ("link",    (int) effectiveRealtimeLink(), nullptr);
    rt.setProperty ("notes",   rtSet.notes,       nullptr);
    rt.setProperty ("period",  rtSet.periodRatio, nullptr);
    rt.setProperty ("pline",   rtSet.periodLine,  nullptr);
    rt.setProperty ("maxima",  rtSet.maxima,      nullptr);
    rt.setProperty ("gap",     rtSet.minGapCents, nullptr);
    rt.setProperty ("root",    rtSet.rootOnly,    nullptr);
    rt.setProperty ("aPart",   rtSet.adapt.partials,    nullptr);
    rt.setProperty ("aPeriod", rtSet.adapt.periodRatio, nullptr);
    rt.setProperty ("aGap",    rtSet.adapt.minGapCents, nullptr);
    rt.setProperty ("aRoot",   rtSet.adapt.rootOnly,    nullptr);
    rt.setProperty ("aGoal",   rtSet.adapt.goal,        nullptr);
    rt.setProperty ("aLo",     rtSet.adapt.rangeLo,     nullptr);
    rt.setProperty ("aHi",     rtSet.adapt.rangeHi,     nullptr);
    rt.setProperty ("aAmps",   rtSet.adapt.shapeAmps,   nullptr);
    rt.setProperty ("aPer",    rtSet.adapt.regPeriods,  nullptr);
    rt.setProperty ("look",    rtSet.lookahead,         nullptr);
    return rt;
}

FractalAdditiveAudioProcessor::RtSettings
FractalAdditiveAudioProcessor::realtimeSettingsFromTree (const juce::ValueTree& rt) const
{
    RtSettings s = rtSet;   // anything the file predates keeps what is in force now
    if (! rt.isValid()) return s;
    s.notes       = (int)    rt.getProperty ("notes",  s.notes);
    s.periodRatio = (double) rt.getProperty ("period", s.periodRatio);
    s.periodLine  =          rt.getProperty ("pline",  s.periodLine).toString();
    s.maxima      = (bool)   rt.getProperty ("maxima", s.maxima);
    s.minGapCents = (double) rt.getProperty ("gap",    s.minGapCents);
    s.rootOnly    = (bool)   rt.getProperty ("root",   s.rootOnly);
    s.adapt.partials    = (int)    rt.getProperty ("aPart",   s.adapt.partials);
    s.adapt.periodRatio = (double) rt.getProperty ("aPeriod", s.adapt.periodRatio);
    s.adapt.minGapCents = (double) rt.getProperty ("aGap",    s.adapt.minGapCents);
    s.adapt.rootOnly    = (bool)   rt.getProperty ("aRoot",   s.adapt.rootOnly);
    s.adapt.goal        = (int)    rt.getProperty ("aGoal",   s.adapt.goal);
    s.adapt.rangeLo     = (double) rt.getProperty ("aLo",     s.adapt.rangeLo);
    s.adapt.rangeHi     = (double) rt.getProperty ("aHi",     s.adapt.rangeHi);
    s.adapt.shapeAmps   = (bool)   rt.getProperty ("aAmps",   s.adapt.shapeAmps);
    // THE WIDTH, and the one property in this tree that changed UNIT. Projects saved
    // before the band became symmetric carry "aSteps": N degrees of the loaded tuning,
    // running UPWARD from the anchor and nothing below it. Ignoring it would silently
    // widen every old project to the default; carrying the number across as if it were
    // periods would widen it by the degree count. It is converted so the TOTAL WIDTH is
    // preserved and re-centred on the played note - N steps up becomes N/(2D) periods each
    // way, which is exactly what the new default is to the old one (2 periods up -> +-1).
    if (rt.hasProperty ("aPer"))
        s.adapt.regPeriods = (double) rt.getProperty ("aPer", s.adapt.regPeriods);
    else if (rt.hasProperty ("aSteps"))
    {
        const int steps = (int) rt.getProperty ("aSteps", -1);
        s.adapt.regPeriods = steps < 0 ? -1.0
                                       : (double) steps / (2.0 * (double) juce::jmax (1, getScaleDegrees()));
    }
    s.lookahead         = (bool)   rt.getProperty ("look",    s.lookahead);
    return s;
}

void FractalAdditiveAudioProcessor::restoreState (const juce::ValueTree& tree)
{
    if (! tree.isValid() || ! tree.hasType (apvts.state.getType()))
        return;

    // Every parameter is about to be rewritten under any note that is currently
    // sounding - silence it cleanly rather than risk it never stopping (see the
    // top of processBlock for the actual kill, which must happen on the audio thread).
    pendingHardSilence.store (true, std::memory_order_release);

    // A restored project carries an EXPLICIT tuning mode the user chose and saved.
    // The one-shot MTS adoption must not second-guess it.
    mtsCheckPending = false;

    // THE PRESET'S TUNING ON OFFER BELONGS TO THE PRESET THAT IS LOADED, and after this
    // line that is no longer the one it came from. Nothing ever cleared these three
    // fields, so "Load preset's tuning" went on offering the LAST preset's scale across a
    // project load, a host reopening the session, and every state change after it - a
    // button naming a scale from a preset this instrument is no longer playing, which on
    // a press would replace the reopened project's own tuning with it.
    //
    // The ORDER is the whole trick, and the obvious version of this fix is backwards:
    // loadPreset used to fill these fields and THEN call this function, so clearing here
    // would have disabled the offer outright, for every preset, for ever. loadPreset now
    // reads the preset's SCALA child AFTER restoreState instead - see the note there.
    presetPendingSclName.clear();
    presetPendingSclSource.clear();
    presetPendingTablePeriod.clear();

    // WHICH <PARAM> CHILDREN THE CHUNK ITSELF CARRIES, asked HERE and not one line later.
    // apvts.replaceState does `state = newState`, and a juce::ValueTree assignment shares
    // the same reference-counted object rather than copying it - so from the next line on,
    // `tree` IS apvts.state. Its reconnect pass (updateParameterConnectionsToChildTrees)
    // then APPENDS a child for every parameter the chunk was missing, which means asking
    // `tree` afterwards whether a child is absent always answers "no, it is there". The
    // absence of a child is the only evidence that a chunk predates a parameter, and it
    // survives exactly until replaceState runs.
    const bool chunkHasNoDryWet = ! tree.getChildWithProperty ("id", "dryWet").isValid();

    // THE SAME TRAP, ONE PARAMETER YOUNGER: the PRIME CONTROL switch (ce29433). Read on
    // this line and not below for exactly the reason written above - the absence of the
    // child is the whole evidence, and replaceState is what manufactures the child.
    const bool chunkHasNoPrimeOn = ! tree.getChildWithProperty ("id", "primeOn").isValid();

    // THE SAME TRAP, ONE PARAMETER OLDER - and the reason this block had to move up here.
    // A state saved before the Harmonics fader existed (98a66ed) is recognised ONLY by the
    // absence of a "harmonics" child, and that evidence dies on the replaceState line below
    // for exactly the reason written above. The migration that reads it used to sit AFTER
    // replaceState, where the question always answered "no, the child is there", so it had
    // never run once since the day it was written - and the projects it exists for have
    // been opening on the WRONG PRIME LIMIT ever since. See the migration itself for what
    // that costs and for the one window it deliberately does not cover.
    //
    // The two legacy NUMBERS are snapshotted here as well, not re-read afterwards. Reading
    // them later happens to work today (the reconnect pass leaves an existing child's value
    // alone and flushes the identical number straight back), but that is an accident of the
    // old and new ranges coinciding; taken here, the migration is reading THE CHUNK, which
    // is the only thing it was ever about.
    const bool chunkPredatesHarmonics = ! tree.getChildWithProperty ("id", "harmonics").isValid();
    bool legacyHasSelection = false, legacyHasPrimeLimit = false;
    int  legacySelectionIdx = 0,     legacyPrimeLimitIdx = 0;
    if (chunkPredatesHarmonics)
    {
        const auto sel = tree.getChildWithProperty ("id", "selection");
        const auto pl  = tree.getChildWithProperty ("id", "primeLimit");
        legacyHasSelection  = sel.isValid();
        legacyHasPrimeLimit = pl .isValid();
        legacySelectionIdx  = juce::jlimit (0, 3, (int) (double) sel.getProperty ("value", 0.0));
        legacyPrimeLimitIdx = juce::jlimit (0, 9, (int) (double) pl .getProperty ("value", 0.0));
    }

    // Unmap BEFORE replaceState: replaceState's internal flush must not capture
    // base+mod for the OLD routings into the fresh tree.
    for (int m = 0; m < ModEngine::kMods; ++m) modEngine.unmapAll (m);
    apvts.replaceState (tree);

    // A state saved BEFORE the Harmonics fader existed needs two migrations. The evidence
    // (chunkPredatesHarmonics and the two legacy indices) was read above replaceState,
    // because by this line the chunk and apvts.state are the same object and every child
    // the chunk was missing has been appended to it.
    //
    // WHAT THESE PROJECTS OPEN AS WITHOUT THIS BLOCK, which is why it is worth reviving
    // rather than deleting:
    //   primeLimit  The old chunk DOES carry a child for it, so replaceState applies its
    //               number verbatim - and that number is an index into an order that has
    //               since been REVERSED (09c47e9). Saved "23", the mildest limit, was
    //               index 9 and comes back as index 9 in today's order, which is limit 2:
    //               everything but the powers of two silently gone. Saved "2", the
    //               harshest, comes back as 23, i.e. no filtering at all. Only "Off"
    //               survives, because 0 is 0 in both orders. A patch reopening with most
    //               of its spectrum removed and nothing on screen to say so is the exact
    //               failure this synth must not have.
    //   harmonics   No child at all, so the reconnect pass appends a valueless <PARAM> for
    //               it, that append fires valueTreeChildAdded -> setNewState, and with no
    //               "value" property to read setNewState writes the DEFAULT. The fader
    //               lands on All whatever it held a moment earlier, so the saved
    //               Even/Odd/Prime choice is thrown away - on a fresh instance and equally
    //               on a running plugin fed an old preset.
    // Both migrations were re-checked against the parameters as they exist today, not as
    // they existed when they were written: the old Choice really did list
    // {All,Even,Odd,Prime} and {Off,2,3,5..23} (a7eaa51), APVTS stores a Choice as its
    // denormalised INDEX, and the maps below still land on the current enum and the
    // current 0..9 fader.
    //
    // THE WINDOW THIS DOES NOT COVER, said out loud rather than papered over: the prime
    // reorder shipped in 09c47e9, SIX HOURS AFTER the harmonics fader in 98a66ed. A chunk
    // written in between carries a harmonics child AND the old ascending prime order, so
    // the gate skips it and it still opens on the reversed limit. Nothing in such a chunk
    // distinguishes it from one written today, and guessing would corrupt CURRENT projects
    // to rescue that window - so it is left alone on purpose.
    if (chunkPredatesHarmonics)
    {
        // 1. The "selection" choice (0=All 1=Even 2=Odd 3=Prime) -> the continuous
        //    Harmonics axis (0=Even 1=All 2=Odd 3=Prime).
        if (legacyHasSelection)
        {
            static const float map[4] = { (float) spectrum::All, (float) spectrum::Even,
                                          (float) spectrum::Odd, (float) spectrum::Prime };
            if (auto* p = apvts.getParameter ("harmonics"))
                p->setValueNotifyingHost (p->convertTo0to1 (map[legacySelectionIdx]));
        }
        // 2. Prime Limit was listed ascending (Off,2,3,5..23); it now runs the other
        //    way (Off,23,19..2) so raising the fader always filters MORE.
        if (legacyHasPrimeLimit)
        {
            static const int oldOrder[10] = { 0, 2, 3, 5, 7, 11, 13, 17, 19, 23 };
            int newIdx = 0;
            for (int i = 0; i < 10; ++i)
                if (primeLimitValues[i] == oldOrder[legacyPrimeLimitIdx]) { newIdx = i; break; }
            if (auto* p = apvts.getParameter ("primeLimit"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) newIdx));
        }
    }

    // A state saved BEFORE the Dry/Wet transition existed (it shipped in bf0f768) has no
    // "dryWet" child, and that flag now DEFAULTS TO ON because the panel's one TRANSITION
    // question defaults to Dry/Wet. A parameter whose child is absent comes back at its
    // DEFAULT - the reconnect pass appends a valueless <PARAM>, and the append fires
    // valueTreeChildAdded -> setNewState, which with no "value" to read writes the default
    // (measured, see the Harmonics migration above) - so without this line every project
    // older than that commit would reopen with a 0.3 s crossfade under every spectrum
    // change, on a patch whose author never had the control at all. The missing child IS the evidence: the
    // build that wrote it could only be instant, so instant is what it must come back as.
    // (Read into chunkHasNoDryWet above replaceState - see the note there for why asking
    // `tree` at this point would always answer "the child is present".)
    if (chunkHasNoDryWet)
        if (auto* p = apvts.getParameter ("dryWet"))
            p->setValueNotifyingHost (0.0f);

    // AND THE PRIME CONTROL SWITCH, which is the same migration pointing the other way.
    // "primeOn" shipped in ce29433 and DEFAULTS TO OFF, on purpose and with a measurement
    // behind it: whitelist mode has no neutral position (ticking all nine primes still
    // drops harmonic 29, whose factor nobody can tick), and with the bypass removed the
    // DEFAULT patch built 375 partials instead of 1024. Off is therefore the only honest
    // default for a patch that never had the control.
    //
    // It is the WRONG value for every project written before the switch existed. There,
    // prime filtering was UNCONDITIONAL - whatever the Limit fader and the nine ticks said
    // is exactly what the spectrum was, and it is what the user tuned the sound to. Absent
    // this line those projects reopen with the panel OFF, which PUTS BACK every harmonic
    // the user deliberately removed: a 5-limit patch comes back with its 7ths, 11ths and
    // 13ths audible, on a spectrum nobody re-edited, with no dialog and nothing on screen
    // to say the filter the patch was built around is no longer running. The dryWet line
    // above and the prime-limit reorder above that are the same failure; this one differs
    // only in direction - it ADDS partials rather than removing them, so it is heard as a
    // timbre that has grown teeth rather than as one that went quiet.
    //
    // The missing child IS the evidence, and it is decisive: a build that had no switch
    // could only filter, so filtering is what it must come back as.
    //
    // primeMode gets no line of its own, and that is checked rather than assumed. It has
    // existed since a7eaa51 (the initial commit), so every chunk this migration can ever
    // see carries its own <PARAM> for it and replaceState applies the saved choice
    // verbatim. The 0 -> 1 default flip that rode along in ce29433 can only reach a chunk
    // that predates the PARAMETER, and there is no such chunk.
    if (chunkHasNoPrimeOn)
        if (auto* p = apvts.getParameter ("primeOn"))
            p->setValueNotifyingHost (1.0f);

    if (auto ms = tree.getChildWithName ("MODS"); ms.isValid())
    {
        for (int m = 0; m < ModEngine::kMods; ++m)
        {
            juce::StringArray ids;
            ids.addTokens (ms.getProperty ("t" + juce::String (m)).toString(), ",", "");
            for (const auto& id : ids)
                if (id.isNotEmpty()) modEngine.addTarget (m, id);
        }
        for (int l = 0; l < ModEngine::kLfos; ++l)
        {
            lfoWavePoints[(size_t) l] = ms.getProperty ("pts" + juce::String (l)).toString();
            juce::StringArray vals;
            vals.addTokens (ms.getProperty ("w" + juce::String (l)).toString(), ",", "");
            if (vals.size() == ModEngine::kWaveN)
            {
                float w[ModEngine::kWaveN];
                for (int i = 0; i < ModEngine::kWaveN; ++i) w[i] = vals[i].getFloatValue();
                modEngine.setCustomWave (l, w);
            }
        }
    }
    // Legacy single custom-env target (older sessions) -> envelope 1.
    if (auto ce = tree.getChildWithName ("CUSTENV"); ce.isValid())
        modEngine.addTarget (0, ce.getProperty ("target").toString());

    // Rhythm (spectrum->MIDI) slots.
    for (auto& s : rhythm.slots)
    { s.partial = -1; s.enabled = false; s.steps = 0; s.useRatio = false; s.ratio = 1.0; s.durMs = 60.0f; s.vel = 100; }
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
                rhythm.slots[i].vel      = (int)    slot.getProperty ("vel", 100);
                rhythm.slots[i].enabled  = (bool)   slot.getProperty ("on", false);
            }
        }

    // "From spectrum's segment" state (the tuning itself comes back via SCALA).
    segSel.clear(); segAnchor = -1; segPeriodText = "2"; segUseTopPeriod = false;
    segmentModeActive = false; segSourceByDegree.clear(); segRebuiltGen = 0;
    if (auto sg = tree.getChildWithName ("SEGMENT"); sg.isValid())
    {
        segmentModeActive = (bool) sg.getProperty ("modeActive", false);
        segAnchor         = (int)  sg.getProperty ("anchor", -1);
        segPeriodText     = sg.getProperty ("periodText", "2").toString();
        segUseTopPeriod   = (bool) sg.getProperty ("useTop", false);
        for (const auto& tok : juce::StringArray::fromTokens (sg.getProperty ("sel").toString(), ",", ""))
            if (tok.trim().isNotEmpty()) segSel.insert (tok.getIntValue());
        lastDissScl  = sg.getProperty ("scl").toString();   // so Save works again
        lastDissName = sg.getProperty ("name").toString();
    }

    // Per-partial amplitude edits (see captureState for the encoding). Restored BEFORE
    // the first rebuild request so the spectrum comes back with the user's edits already
    // on it, rather than sounding unedited for a moment and then correcting itself.
    {
        partialGains.clear();
        if (auto pg = tree.getChildWithName ("PGAINS"); pg.isValid())
        {
            const auto packed = pg.getProperty ("v").toString();
            for (const auto& tok : juce::StringArray::fromTokens (packed, " ", ""))
            {
                if (tok.isEmpty()) continue;
                const int c = tok.indexOfChar (':');
                if (c <= 0) continue;
                partialGains.push_back ({ tok.substring (0, c).getLargeIntValue(),
                                          tok.substring (c + 1).getFloatValue() });
            }
            std::sort (partialGains.begin(), partialGains.end(),
                       [] (const spectrum::PartialGain& a, const spectrum::PartialGain& b)
                       { return a.key < b.key; });
        }
        // THE SLOPE THE DRAWING WAS MADE AT comes back with it (see captureState for what
        // was being dropped without this). Restored HERE, beside the edits it belongs to
        // and before the rebuild request below, so the first template this project compiles
        // is already the right one rather than a corrected second attempt.
        //
        // lastManualMode is put in step in the same breath, and that is load-bearing rather
        // than tidy: leaving it stale makes timerCallback see a mode TRANSITION on its very
        // next tick and re-freeze the slope at the fader's current value - which is exactly
        // the number this line is restoring away from, arriving 16 ms later and undoing it.
        // Nothing is lost by pre-empting that branch: its other half, the setPartialGains
        // push that tells the engine whether the edits are in force, is the statement below.
        //
        // A chunk that predates this child falls back to the patch's OWN curveSlope, which
        // replaceState has just restored. That is what the fader reads, it is what the patch
        // was drawn against, and it is the only slope the file contains - the previous
        // patch's is not a candidate under any reading.
        {
            const auto ma = tree.getChildWithName ("MANUALAMP");
            manualSlopeFrozen = (float) (double) ma.getProperty ("slope",
                                    (double) apvts.getRawParameterValue ("curveSlope")->load());
            lastManualMode = apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
        }

        // Through setPartialGains, not the engine directly, so the revision counter is
        // bumped: an open amplitude window would otherwise keep showing the PREVIOUS
        // project's knob positions after a reload (its resync watches that counter).
        setPartialGains (partialGains,
                         apvts.getRawParameterValue ("partialManual")->load() > 0.5f);
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
    // Don't re-load on restore; the state's SCALA child already carries the tuning. The
    // whole "already applied" record is primed, not just the index - a restored instance
    // has attempted nothing and failed at nothing, so the retry test above must not find a
    // stale failure from this instance's previous life pointing at the new project's slot.
    lastBankSlot   = getTuningBankSlotParam();
    lastBankSource = (lastBankSlot >= 1 && lastBankSlot <= kBankSlots)
                   ? bankSource[(size_t) (lastBankSlot - 1)] : juce::String();
    lastBankFailed = false;

    // THE PARKED HAND-SET REFERENCE (see captureState for what it is and what was losing
    // it). Read here, after replaceState so refA below is the RESTORED one, and before the
    // first timerCallback: that poll hands manualRefA back to refA the moment MTS is not
    // the mode, so by then it has to hold the user's own number rather than this instance's
    // 440 default.
    //
    // A chunk that predates this carries neither property, and the fallback is the only
    // reference such a file has: refA itself, which replaceState has just restored. A
    // project saved OUTSIDE MTS therefore parks its own value (correct, and what leaving a
    // mode it was never in would hand back anyway); one saved INSIDE MTS parks the master's,
    // which is exactly what those projects do today - unchanged, not made worse.
    {
        const auto rp = tree.getChildWithName ("REFPARK");
        manualRefA = (float) (double) rp.getProperty ("manualA",
                         (double) apvts.getRawParameterValue ("refA")->load());
        mtsHadRef  = (bool) rp.getProperty ("mtsOwns", false);
    }

    if (auto scl = tree.getChildWithName ("SCALA"); scl.isValid())
    {
        juce::String err;
        // restoreTuning (not loadScalaString): the source may be .tun text holding an
        // absolute 128-key table, and it carries the table's chosen period with it.
        const auto savedName = scl.getProperty ("name").toString();
        if (! tuning.restoreTuning ({ savedName,
                                      scl.getProperty ("source").toString(),
                                      scl.getProperty ("tablePeriod").toString() }, err))
        {
            // A PROJECT THAT LOSES ITS SCALE SAYS SO. `err` was filled in by the parser
            // and thrown away on the next line, so a project saved in 31-EDO whose stored
            // scale text no longer parses reopened playing 12-tone equal temperament -
            // silently, with no dialog and nothing on screen to say the instrument is not
            // in the tuning the project was written in. For a microtonal instrument that
            // is the worst possible quiet failure: everything still sounds, and it sounds
            // wrong.
            //
            // Kept on the processor rather than shown from here, because setStateInformation
            // runs on a load thread with no editor guaranteed to exist. The editor picks it
            // up on its next tick and puts it in the notice overlay it already has for
            // exactly this kind of "something did not go as asked" message.
            tuningRestoreError = savedName.isNotEmpty()
                               ? savedName + " \xe2\x80\x94 " + err
                               : err;
        }
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
            setTimbreOnParam ((bool) tim.getProperty ("on", true));
        }
        else if (src.isNotEmpty()) // typed intervals (Spectrum Builder)
        {
            loadTimbreText (src, err);
            setTimbreOnParam ((bool) tim.getProperty ("on", true) && ! timbreDegrees.empty());
        }
        // Provenance LAST: both load paths above clear it (they cannot tell a saved
        // adaptation from hand-typed text), and only the state knows the difference.
        // Restored only when a spectrum really came back - provenance for a spectrum
        // that failed to parse would offer the origin of nothing.
        if (! timbreDegrees.empty())
        {
            specProvName   = tim.getProperty ("provName").toString();
            specProvSource = tim.getProperty ("provSource").toString();
            specProvPeriod = tim.getProperty ("provPeriod").toString();
        }
    }

    // THE PLAYGROUND, restored BEFORE the realtime link below: engaging it stands the
    // follower down, and doing it the other way round would arm a follower and then
    // immediately suspend it, losing the arming the file actually carried.
    //
    // The corners are re-read from their .gsting files by name. A corner whose preset has
    // been renamed or deleted comes back EMPTY rather than silently substituting
    // something else - a playground quietly morphing towards a preset the user never
    // chose is worse than one that visibly lost a corner.
    //
    // AND THE CORNER-EDIT SESSION GOES WITH THEM, which is not tidiness - it is what stops
    // the patch that was just loaded from being overwritten about sixteen milliseconds
    // later. A corner-edit session holds pgEditLiveState: the WHOLE live patch, frozen at
    // the moment edit mode was entered, to be handed back when it ends (see rule 1 above
    // beginPlaygroundCornerEdit). restoreState never mentioned it, so with the corner-edit
    // window open one click on "Load its original spectrum" - or a project reload, or any
    // other preset load - left the session standing over a patch that no longer existed.
    // The corners reset on the line below then make pgActive false, drivePlayground's very
    // next tick takes the "the playground let go" branch and calls endPlaygroundCornerEdit(),
    // and that writes the PRE-EDIT patch back parameter by parameter. The user loads a
    // preset, hears it, and it is silently replaced by the one they had before, with no
    // click of their own in between.
    //
    // A PLAIN RESET, deliberately NOT a call to endPlaygroundCornerEdit(). That function's
    // entire job is to hand the frozen live patch back - which is precisely the write that
    // must not happen here, because the patch this instrument should be holding is the one
    // in `tree`, already applied by replaceState above. It also unwinds the undo stack and
    // mints a corner-edit step, neither of which belongs to a state load. So the session is
    // simply forgotten: every field back to what a fresh instance has.
    pgEditCorner         = -1;
    pgEditLiveState      = {};
    pgEditLiveX          = 0.0f;
    pgEditLiveY          = 0.0f;
    pgEditLivePresetName = {};
    pgEditLivePresetSnap.clear();
    pgEditCompilePending = false;
    pgEditPrimed         = false;
    pgEditTick           = 0;
    pgEditCornerBefore   = {};
    pgEditStackDepth     = 0;
    pgEditUnwound        = false;
    for (int i = 0; i < kPlaygroundCorners; ++i)
        pgCorner[(size_t) i] = PlaygroundCorner{};
    pgActive = false; pgWinner = -1; pgRtSuspended = false; pgSuspendedRt = RtLink::Off;
    pgLastX = pgLastY = -1.0; pgDirty = true;
    // Adaptive-corner bookkeeping starts over with the corners themselves: a result in
    // flight belongs to the OLD project's corners and must not land into this one.
    pgSeenAdaptGen = pgAdaptGeneration.load();
    pgAdaptInFlight = -1; pgAdaptNext = 0;
    pgAdaptStarts = 0; pgAdaptApplies = 0;
    pgAdaptRefusal.clear();
    if (auto pg = tree.getChildWithName ("PLAYGROUND"); pg.isValid())
        for (int i = 0; i < kPlaygroundCorners; ++i)
        {
            const auto n = pg.getProperty ("c" + juce::String (i)).toString();
            if (n.isNotEmpty()) { juce::String err; assignPlaygroundCorner (i, n, err); }
        }
    refreshPlaygroundActive();

    // The realtime/adaptive link travels with the patch (see captureState). Restored
    // through setRealtimeLink so it takes its own undo baseline exactly as pressing the
    // button would - a follower armed by a preset must be as reversible as one armed by
    // hand. An absent child means the state predates this and the follower stays off.
    {
        const auto rtTree = tree.getChildWithName ("REALTIME");
        const auto s = realtimeSettingsFromTree (rtTree);
        const auto want = (RtLink) (int) (rtTree.isValid() ? (int) rtTree.getProperty ("link", 0) : 0);
        if (pgActive)
        {
            // The playground owns the spectrum: remember the arming, run nothing (see
            // refreshPlaygroundActive for why the two cannot both write the spectrum).
            rtSet = s;
            pgSuspendedRt = want;
            pgRtSuspended = true;
            setRealtimeLink (RtLink::Off, s);
        }
        else
        {
            setRealtimeLink (want, s);
        }
    }

    // RE-BASELINE THE RANDOM TRIGGER against the level the project just restored, and
    // throw away any edge that restoring produced. replaceState() rewrites every
    // parameter, so a project saved with the trigger left at 1 (which is where a press
    // leaves it) arrived as a 0 -> 1 step and REGENERATED the spectrum on load: the user
    // reopened yesterday's project and found a random timbre where their own had been.
    randomTrigLevel.store (apvts.getRawParameterValue ("specRandom")->load() >= 0.5f,
                           std::memory_order_release);
    randomTrigPending.store (false, std::memory_order_release);
}

void FractalAdditiveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // CORNER EDIT, rule 2: while the controls are standing in for a playground corner,
    // captureState() describes THAT CORNER, not this instrument's patch. Saving it here
    // would make a project reopened mid-edit come back as somebody's corner, with the
    // user's own sound gone - so the frozen live patch is what the host is handed. The
    // corner assignments are re-stamped onto it because those CAN change while editing.
    if (pgEditCorner >= 0 && pgEditLiveState.isValid())
    {
        auto live = pgEditLiveState.createCopy();
        live.removeChild (live.getChildWithName ("PLAYGROUND"), nullptr);
        if (getPlaygroundAssignedCount() > 0)
        {
            juce::ValueTree pg ("PLAYGROUND");
            for (int i = 0; i < kPlaygroundCorners; ++i)
                if (pgCorner[(size_t) i].assigned())
                    pg.setProperty ("c" + juce::String (i), pgCorner[(size_t) i].name, nullptr);
            live.appendChild (pg, nullptr);
        }
        if (auto xml = live.createXml())
            copyXmlToBinary (*xml, destData);
        return;
    }
    if (auto xml = captureState().createXml())
        copyXmlToBinary (*xml, destData);
}

void FractalAdditiveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // ANY call here means the host is handing this instance a previous life - a reopened
    // project, a preset, a duplicated or copy-pasted plugin. Disarm the one-shot MTS-ESP
    // adoption BEFORE parsing, so it is disarmed even when the chunk turns out to be
    // empty, foreign or unreadable: restoreState() returns early on those and used to
    // leave the one-shot armed, which made a corrupt-state instance look brand new.
    //
    // This matters a lot. Adopting MTS-ESP on a RESTORED instance would overwrite the
    // tuning mode the user saved, and with it the reference (MTS owns refA while it is
    // the active mode, parking the user's own value) and any loaded Scala scale. Someone
    // reopening yesterday's project would find their tuning silently replaced.
    mtsCheckPending = false;

    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        restoreState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// File presets (.gsting XML). User presets live in Documents/Galactic Sting/Presets;
// factory presets shipped in a "Presets" folder next to the binary are copied in on
// first run so they "come with the download".
// TESTS ONLY (see the header). Never written outside a verifier: production reads it
// once per call and finds it empty.
static juce::File presetFolderOverrideForTests;

void FractalAdditiveAudioProcessor::setPresetFolderForTests (const juce::File& dir)
{
    presetFolderOverrideForTests = dir;
    if (dir != juce::File()) dir.createDirectory();
}

juce::File FractalAdditiveAudioProcessor::getPresetFolder()
{
    if (presetFolderOverrideForTests != juce::File())
        return presetFolderOverrideForTests;

    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Galactic Sting").getChildFile ("Presets");
    if (! dir.exists())
    {
        dir.createDirectory();
        // Seed with any factory presets shipped alongside the executable/plugin.
        auto factory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                           .getParentDirectory().getChildFile ("Presets");
        if (factory.isDirectory())
            for (const auto& f : factory.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kPresetExt))
                f.copyFileTo (dir.getChildFile (f.getFileName()));
    }
    return dir;
}

//==============================================================================
// THE BUNDLED TUNINGS, UNPACKED ONTO REAL DISK.
//
// Same shape as presetFolderOverrideForTests above, and for the identical reason: the
// production value of this is a folder in the user's Documents, and a verifier that
// used it would write six files into it on every run. RhythmVerify redirects it before
// it builds anything.
static juce::File tuningsFolderOverrideForTests;

void FractalAdditiveAudioProcessor::setTuningsFolderForTests (const juce::File& dir)
{
    tuningsFolderOverrideForTests = dir;
    if (dir != juce::File()) dir.createDirectory();
}

juce::File FractalAdditiveAudioProcessor::getTuningsFolder()
{
    auto dir = tuningsFolderOverrideForTests != juce::File()
                 ? tuningsFolderOverrideForTests
                 : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("Galactic Sting").getChildFile ("Tunings");
    dir.createDirectory(); // a no-op that returns ok when it is already there

    // WRITE ONLY WHAT IS NOT THERE. This runs every single time the chooser opens, so
    // it has to be idempotent, and it has to be idempotent in the STRONG sense: a user
    // who edited 31ed2.tun - retuned one degree, changed the reference, deleted a file
    // they never use - must find their own version next time, not ours. So the only
    // write here is a CREATE. "Restore the factory tunings" is a thing a menu item
    // could offer later; it is not something the file chooser gets to do behind them.
    //
    // The resources are found by walking namedResourceList and asking each one for its
    // ORIGINAL filename, rather than by naming the generated symbols. Those names are
    // not guessable and not stable: JUCE builds an identifier out of the filename by
    // DROPPING the characters that cannot appear in one and turning the dot into an
    // underscore, then prefixes "_" when the result starts with a digit. Measured on
    // this build - "harmonic-series-8-16.tun" becomes harmonicseries816_tun (the hyphens
    // vanish, they do not become underscores) and "7ed2.tun" becomes _7ed2_tun. Six
    // hand-copied spellings of that would rot one at a time; this way, adding a .tun to
    // the CMake list is the whole change.
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const auto* resource = BinaryData::namedResourceList[i];
        const auto* original = BinaryData::getNamedResourceOriginalFilename (resource);
        if (original == nullptr)
            continue;

        // "= juce::String (...)", not "juce::String fileName (...)": the parenthesised
        // form is a function DECLARATION to the compiler (most vexing parse) and the
        // next line then fails with "left operand of .endsWithIgnoreCase must have
        // class/struct/union type", which names everything except what is wrong.
        const auto fileName = juce::String (juce::CharPointer_UTF8 (original));
        if (! fileName.endsWithIgnoreCase (".tun"))
            continue; // the fonts and the jingle live in the same target

        const auto out = dir.getChildFile (fileName);
        if (out.exists())
            continue; // theirs now, whatever it says

        int size = 0;
        if (const auto* data = BinaryData::getNamedResource (resource, size))
            if (size > 0)
                out.replaceWithData (data, (size_t) size);
    }
    return dir;
}

juce::File FractalAdditiveAudioProcessor::getTuningChooserFolder() const
{
    if (appProps != nullptr)
    {
        // isAbsolutePath FIRST: constructing a juce::File from a relative or empty
        // string asserts in debug builds, and this string comes off disk, where a
        // half-written settings file can leave anything at all.
        const auto path = appProps->getValue ("tuningFolder");
        if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
        {
            const juce::File last (path);
            // Not "did we store one" but "is it still there": a folder on a USB stick
            // that is not plugged in, or one the user has since deleted, would open the
            // chooser on nothing at all. Falling back to the bundled folder is the one
            // answer that always exists.
            if (last.isDirectory())
                return last;
        }
    }
    return getTuningsFolder();
}

void FractalAdditiveAudioProcessor::setTuningChooserFolder (const juce::File& dir)
{
    if (appProps == nullptr || ! dir.isDirectory())
        return;
    appProps->setValue ("tuningFolder", dir.getFullPathName());
    appProps->saveIfNeeded(); // written NOW: a host that is killed rather than closed
}                            // never runs a destructor to flush it (see keyboardNoteNames)

juce::StringArray FractalAdditiveAudioProcessor::getPresetNames()
{
    juce::StringArray names;
    for (const auto& f : getPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + kPresetExt))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    return names;
}

// Remember the BASE value of every parameter right now, under this preset's name -
// presetHasDrifted() compares against this. Called right after a preset is loaded
// (once the caller's own tuning has been put back) or freshly saved (the new file
// exactly matches the current state, so nothing has drifted from it yet).
void FractalAdditiveAudioProcessor::snapshotLoadedPreset (const juce::String& name)
{
    loadedPresetName = name;
    loadedPresetSnapshot.clear();
    for (auto* ap : getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (ap))
            loadedPresetSnapshot.push_back ({ p->paramID, p->getValue() });
}

bool FractalAdditiveAudioProcessor::presetHasDrifted() const
{
    if (loadedPresetName.isEmpty())
        return false;
    for (const auto& [pid, v] : loadedPresetSnapshot)
        if (auto* p = apvts.getParameter (pid))
            if (std::abs (p->getValue() - v) > 1.0e-4f)
                return true;
    return false;
}

//==============================================================================
// THE FIXED-POINT RULE, and the ONLY place in the program a .gsting is ever written.
//
//   A CORNER IS A FIXED POINT OF THE BLEND. What is captured, saved or re-read for a
//   corner must be the corner's OWN stored state, never the surface the blend is
//   currently driving.
//
// CASE #88, the user's oldest report: "si por ejemplo voy adelante y atras entre dos
// presets uno con menos amp sustain que el otro, el volumen va bajando hasta
// silenciarse... curiosamente con el modo mix pasa lo mismo."
//
// It never reproduced in memory, and could not: ten A->B->A round trips through the
// real drivePlayground drift nothing (run 47). THE LOOP CLOSES THROUGH THE FILE. While
// the pad drives, the live control surface is the blend's OUTPUT; a corner's file is
// the blend's INPUT. Save a preset under an assigned corner's own name while parked
// mid-pad and that corner's file becomes blend(A, B). Re-assign it - or simply reopen
// the project, because captureState stores corner NAMES and setStateInformation re-reads
// every one of them from disk - and the corner IS the previous interpolation's output.
// The next trip blends that with B again, so the corner walks towards B by the blend
// weight every cycle. Measured, run 51: stored ampSustain 0.9000 0.4750 0.2625 0.1563
// ... 0.0501, masterGain 0.00 -9.00 -13.50 ... -17.99 dB, rendered RMS 0.04097 ->
// 0.00029 (-43 dB), monotone, gap ratio 0.4706..0.5075. A geometric series, exactly as
// reported, through the re-assign door AND through a genuine host project reload.
//
// So while the playground is driving, a preset write may not land on the file an
// assigned corner points at. The one way to write a corner's file is the corner's own
// Edit -> Save (savePlaygroundCornerAs), where the controls have been STANDING IN for
// that corner since beginPlaygroundCornerEdit pushed it onto them, so the surface is
// the corner's own state and not a blend of anything.
//
// This lives HERE, in one function every write goes through, rather than as a check at
// savePreset and another at savePlaygroundCornerAs, because there is no such thing as
// remembering to add the third one. And it REFUSES OUT LOUD: the reason travels back to
// the editor, which puts it in front of the user in their own language. A silent no-op
// would be a different bug of the same family.
bool FractalAdditiveAudioProcessor::writePresetFile (const juce::String& safeName,
                                                     const juce::ValueTree& state,
                                                     bool isCornerOwnSave,
                                                     PresetWriteRefusal& why, int& corner)
{
    why    = PresetWriteRefusal::None;
    corner = -1;

    if (safeName.isEmpty())
    { why = PresetWriteRefusal::BadName; return false; }

    if (! isCornerOwnSave && pgActive)
        for (int i = 0; i < kPlaygroundCorners; ++i)
            if (pgCorner[(size_t) i].assigned() && pgCorner[(size_t) i].name == safeName)
            {
                why = PresetWriteRefusal::CornerFixedPoint;
                corner = i;
                return false;
            }

    auto xml = state.createXml();
    if (xml == nullptr || ! xml->writeTo (presetFile (safeName)))
    { why = PresetWriteRefusal::WriteFailed; return false; }
    return true;
}

juce::String FractalAdditiveAudioProcessor::savePreset (const juce::String& name,
                                                        PresetWriteRefusal& why, int& corner)
{
    // Returns the name actually stored (filename characters are sanitised, so it
    // can differ from what was typed), or empty on failure - the editor shows it,
    // and `why` always says which of the reasons it was.
    const auto safe = juce::File::createLegalFileName (name).trim();
    if (! writePresetFile (safe, captureState(), false, why, corner))
        return {};
    snapshotLoadedPreset (safe);
    return safe;
}

juce::String FractalAdditiveAudioProcessor::savePreset (const juce::String& name)
{
    PresetWriteRefusal why = PresetWriteRefusal::None;
    int corner = -1;
    return savePreset (name, why, corner);
}

bool FractalAdditiveAudioProcessor::loadPreset (const juce::String& name)
{
    // A FAILED PRESET LOAD IS STATED. Every `return false` below used to be silent, and
    // the one caller drops the bool: the name stayed in the box, the PREVIOUS preset went
    // on sounding, and the Tuning panel went on offering the previous preset's scale. The
    // player is told they loaded something they did not. Parked rather than shown from
    // here for the same reason tuningRestoreError is - the editor may not exist (the
    // playground assigns corners with the window shut) - and consumed the same way.
    presetLoadError.clear();
    const auto refuse = [this, &name] (const juce::String& why)
    {
        presetLoadError = name.isNotEmpty() ? name + " \xe2\x80\x94 " + why : why;
        return false;
    };

    auto file = presetFile (name);
    if (! file.existsAsFile())
        return refuse ("there is no preset by that name in the preset folder. Nothing was "
                       "loaded - the patch you were playing is untouched.");
    if (auto xml = juce::XmlDocument::parse (file))
    {
        // .gsting files are user-editable: only apply a tree of OUR type, or a
        // foreign XML would reset every parameter and wipe the tuning/timbre
        // while reporting success.
        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.hasType (apvts.state.getType()))
            return refuse ("this file is XML, but it is not a Fibl\xc3\xb3 Gal\xc3\xa0" "ctic preset. "
                           "Nothing was loaded - the patch you were playing is untouched.");

        const auto keep     = tuning.captureTuning();
        const int  keepMode = getTuningMode();
        // THE PARKED HAND-SET REFERENCE IS NOT PART OF A PRESET, for the same reason the
        // active tuning above is not: only the SOUND changes, never what the keys mean.
        //
        // It has to be kept EXPLICITLY now that captureState writes it, and this is the one
        // flow where getting it wrong would lose the very number that fix exists to protect.
        // While MTS-ESP owns the reference, refA holds the master's display value and the
        // user's own is parked in manualRefA. Letting a preset's REFPARK land there would
        // throw the user's number away - and the next poll, seeing the park still in force,
        // would re-park the preset's value in its place, so leaving MTS would hand back a
        // reference the user never typed. Kept unconditionally: with no park in force
        // manualRefA is not read by anything until MTS is next entered, and entering it
        // overwrites the member from refA anyway.
        const float keepManualRefA = manualRefA;
        const bool  keepMtsHadRef  = mtsHadRef;

        restoreState (tree);

        manualRefA = keepManualRefA;
        mtsHadRef  = keepMtsHadRef;

        // The preset's OWN tuning travels IN the file (so it is never lost) but is
        // never applied by loading the preset: only the SOUND changes, never the
        // scale under whatever is already loaded or being played on. It is offered
        // separately - see hasPendingPresetTuning() / loadPendingPresetTuning() -
        // the Tuning panel shows a button for it only when the two actually differ.
        //
        // AFTER restoreState, not before it, and that is not tidiness. restoreState now
        // clears the offer, because an offer belongs to the preset that is loaded and a
        // state load replaces the patch (see the clear there for what it was costing).
        // Read into these fields first, as this used to, and the clear one line later
        // would wipe every offer this function has ever made.
        //
        // `tree` still carries the child at this point: apvts.replaceState makes `tree`
        // and apvts.state the same object and APPENDS the <PARAM> children the chunk was
        // missing, but it removes nothing, so SCALA is exactly where the file put it.
        if (auto scl = tree.getChildWithName ("SCALA"); scl.isValid())
        {
            presetPendingSclName   = scl.getProperty ("name").toString();
            presetPendingSclSource = scl.getProperty ("source").toString();
            // A preset saved on an absolute table carries its period too, so accepting the
            // preset's tuning later reproduces what the preset's author was hearing.
            presetPendingTablePeriod = scl.getProperty ("tablePeriod").toString();
        }

        // Put the CURRENTLY ACTIVE tuning back exactly as it was - source, name AND a
        // table's chosen period, which is as much part of "as it was" as the pitches.
        if (keep.source.isNotEmpty())
        {
            juce::String err;
            tuning.restoreTuning (keep, err);
        }
        else
            tuning.clearScala();
        if (auto* p = apvts.getParameter ("tuningMode"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) keepMode));
        applyTuningMode (keepMode);
        snapshotLoadedPreset (name); // AFTER the tuning revert - that is the real final state
        return true;
    }
    // XmlDocument::parse returned nothing: the file is there and is not XML at all
    // (truncated by a crash mid-save, edited by hand, or simply something else).
    return refuse ("this preset file could not be read - it is not valid XML. Nothing was "
                   "loaded - the patch you were playing is untouched.");
}

//==============================================================================
// THE PRESET PLAYGROUND. The rules it obeys are in dsp/PresetMorph.h (weights, the
// log-vs-linear choice per family, nearest-corner-with-hysteresis) and in
// spectrum::blendTemplates (the partial-list crossfade); this is the wiring.
//==============================================================================
// Every parameter, at its DEFAULT. The base a corner is built on, so the store is total
// from the first instant (see PlaygroundCorner::params): a preset that predates a
// parameter means the parameter's default, in morph, in mix and in corner edit alike.
void FractalAdditiveAudioProcessor::fillCornerDefaults (PlaygroundCorner& c) const
{
    const auto& all = getParameters();
    c.params.assign ((size_t) all.size(), 0.0f);
    for (int i = 0; i < all.size(); ++i)
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
            c.params[(size_t) i] = p->convertFrom0to1 (p->getDefaultValue());
}

bool FractalAdditiveAudioProcessor::assignPlaygroundCorner (int corner, const juce::String& presetName,
                                                            juce::String& errorOut)
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners))
        return false;

    // Re-assigning the corner being EDITED replaces its contents from disk, which is
    // exactly what discards unsaved edits (rule 4) - so edit mode has to end first, or
    // the frozen live patch would be handed back later from a corner that no longer
    // exists and the scratch surface would be left standing as if it were the patch.
    if (corner == pgEditCorner)
        endPlaygroundCornerEdit();

    auto& c = pgCorner[(size_t) corner];
    c = PlaygroundCorner{};                    // clearing is just "assign nothing"

    if (presetName.isNotEmpty())
    {
        auto file = presetFile (presetName);
        if (! file.existsAsFile())
        { errorOut = "That preset no longer exists."; refreshPlaygroundActive(); return false; }

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr)
        { errorOut = "That preset could not be read."; refreshPlaygroundActive(); return false; }

        // Same gate loadPreset uses: .gsting files are user-editable, and a foreign tree
        // would give every corner value a default and make the corner sound like an
        // init patch while claiming to be the preset.
        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.hasType (apvts.state.getType()))
        { errorOut = "That file is not a Fibl\xc3\xb3 preset."; refreshPlaygroundActive(); return false; }

        c.name = presetName;

        // Parameter values, indexed by getParameters() order (see PlaygroundCorner).
        // DEFAULTS FIRST, then whatever the file carries on top: that is what makes the
        // store TOTAL, and "the parameter's default" the one answer for a parameter this
        // preset predates. Never the live patch's value - a corner must sound the same
        // whatever happened to be loaded when it was dropped here.
        const auto& all = getParameters();
        fillCornerDefaults (c);
        for (int i = 0; i < all.size(); ++i)
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
                if (auto child = tree.getChildWithProperty ("id", p->paramID); child.isValid())
                    c.params[(size_t) i] = (float) (double) child.getProperty ("value");

        // The Custom Engine's own scale, and the per-partial amplitude edits: both are
        // state, not parameters, and both are part of what this corner SOUNDS like.
        if (auto tim = tree.getChildWithName ("TIMBRE"); tim.isValid())
        {
            c.timbreName   = tim.getProperty ("name").toString();
            c.timbreSource = tim.getProperty ("source").toString();
            // Its spectrum's tuning provenance travels with it (see PlaygroundCorner):
            // the winner corner must deliver its spectrum WITH the origin it was saved
            // with, or morphing onto an adapted preset would silently drop the button.
            c.provName   = tim.getProperty ("provName").toString();
            c.provSource = tim.getProperty ("provSource").toString();
            c.provPeriod = tim.getProperty ("provPeriod").toString();
        }
        if (auto pg = tree.getChildWithName ("PGAINS"); pg.isValid())
            for (const auto& tok : juce::StringArray::fromTokens (pg.getProperty ("v").toString(), " ", ""))
            {
                const int col = tok.indexOfChar (':');
                if (col > 0)
                    c.gains.push_back ({ tok.substring (0, col).getLargeIntValue(),
                                         tok.substring (col + 1).getFloatValue() });
            }
        std::sort (c.gains.begin(), c.gains.end(),
                   [] (const spectrum::PartialGain& a, const spectrum::PartialGain& b) { return a.key < b.key; });
        {
            double v = 0.0;
            const int mi = paramIndexOf ("partialManual");
            c.manual = c.value (mi, v) && v > 0.5;
        }

        // THE ADAPTIVE / REALTIME SPECTRUM IS PART OF THE PRESET, not something outside
        // it: a corner remembers whether it was saved with the follower armed, and the
        // nearest-corner rule carries that over like any other discrete setting.
        if (auto rt = tree.getChildWithName ("REALTIME"); rt.isValid())
        {
            c.rtLink = (int) rt.getProperty ("link", 0);
            c.rtSet  = realtimeSettingsFromTree (rt);
        }

        // The preset's MODULATOR ROUTING, for MIX mode's per-corner ModEngine (see
        // PlaygroundCorner). Kept as the saved text and parsed only when a sub-engine
        // is refreshed - morph mode never pays for it.
        if (auto ms = tree.getChildWithName ("MODS"); ms.isValid())
        {
            for (int m = 0; m < ModEngine::kMods; ++m)
                c.modTargets[m] = ms.getProperty ("t" + juce::String (m)).toString();
            for (int l = 0; l < ModEngine::kLfos; ++l)
                c.modWaves[l] = ms.getProperty ("w" + juce::String (l)).toString();
        }

        compileCornerTemplate (c);
    }

    refreshPlaygroundActive();
    pgDirty = true;    // a corner moved: the next tick re-applies EVERYTHING, with a
                       // generation bump, because this is a real discontinuity.
    return true;
}

juce::String FractalAdditiveAudioProcessor::getPlaygroundCornerName (int corner) const
{
    return juce::isPositiveAndBelow (corner, kPlaygroundCorners) ? pgCorner[(size_t) corner].name
                                                                 : juce::String();
}

juce::String FractalAdditiveAudioProcessor::getPlaygroundCornerDisplayName (int corner) const
{
    const auto n = getPlaygroundCornerName (corner);
    return (n.isNotEmpty() && isPlaygroundCornerEdited (corner)) ? n + "*" : n;
}

//==============================================================================
// EDITING A CORNER IN PLACE - and THE OWNERSHIP RULES, which are the whole feature.
//
// The complaint this answers is simple: a corner is a preset you are listening to, and
// there was no way to change it without leaving, loading it, editing it, saving it and
// dropping it back. Now the corner's Edit button turns the main editor into that
// corner's editor.
//
// The trap is equally simple, and it is a data-loss one: this instrument already has a
// LIVE PATCH with its own identity (a preset name, a drift mark, an undo history, the
// host's project state), and the main editor's controls are that patch. Pointing them
// at a corner without saying who owns what is how a user loses the sound they were
// working on. So:
//
//  1. THE LIVE PATCH IS FROZEN, NOT REPLACED. Entering edit mode captures the whole
//     live patch (captureState) plus its preset identity and drift reference, and
//     leaving writes them back. The controls in between are a SCRATCH SURFACE.
//  2. THE HOST NEVER SEES THE SCRATCH SURFACE. getStateInformation saves the frozen
//     live patch while edit mode is on - a project saved mid-edit reopens as the patch
//     the user had, never as somebody's corner.
//  3. THE PRESETS COMBO IS NOT THE CORNER. The editor freezes and disables it (and
//     hides the drift mark) for the duration: a corner is not "the loaded preset", and
//     letting the combo load one would drop a whole preset onto the scratch surface.
//  4. THE EDITS LIVE IN THE CORNER, IN MEMORY, FOR THIS SESSION. The .gsting file the
//     corner points at is NEVER written by editing. Unsaved edits stay on the corner
//     when edit mode ends (they are what you were just listening to - discarding them
//     silently would be the worse surprise) and the trailing "*" says so, in the pad,
//     in the corner's box and in the frozen preset field. Re-assigning that corner, or
//     reopening the project - both of which re-read the preset from disk - is what
//     discards them, and both are explicit acts.
//  5. THE TUNING IS NOT PART OF A CORNER. Push and pull both go through
//     presetmorph::morphSpaceFor and skip everything it excludes, so tuningMode, refA,
//     octaveShift, bendRange, the bank slot and the playground's own point cannot be
//     carried into a corner or out of one - the same promise the morph itself makes.
//  6. MORPH MODE PINS THE POINT TO THE CORNER. In morph mode the blend WRITES every
//     continuous parameter sixty times a second, so there is no surface left to type
//     into unless the blend is that corner: the point moves onto the corner on entry
//     (and back off on exit). Then what you see, what you hear and what you are
//     editing are the same thing. In MIX mode nothing is pinned and nothing needs to
//     be - the corner is its own engine, so you hear the edit against the other three
//     at whatever balance the point is sitting at. That is the "in context" case.
//==============================================================================
bool FractalAdditiveAudioProcessor::beginPlaygroundCornerEdit (int corner)
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners)) return false;
    if (! pgCorner[(size_t) corner].assigned() || ! pgActive)     return false;
    if (pgEditCorner == corner) return true;
    // One corner at a time: two scratch surfaces would need two frozen live patches,
    // and the second entry would freeze the FIRST corner as if it were the live one.
    if (pgEditCorner >= 0) endPlaygroundCornerEdit();

    pgEditLiveState       = captureState();
    pgEditLiveX           = apvts.getRawParameterValue ("pgX")->load();
    pgEditLiveY           = apvts.getRawParameterValue ("pgY")->load();
    pgEditLivePresetName  = loadedPresetName;
    pgEditLivePresetSnap  = loadedPresetSnapshot;
    pgEditCorner          = corner;
    pgEditCompilePending  = false;
    pgEditPrimed          = false;   // the next pull is the baseline, not an edit

    // RULE 7: FENCE THE SESSION OFF FROM THE LIVE PATCH'S UNDO HISTORY (see the block
    // above CornerEditBoundaryAction). The marker goes down BEFORE the pin and before
    // pushCornerToLiveSurface, because both of those write parameters and a parameter
    // write is a step on the shared stack that puts corner values on the live patch when
    // it is undone. `pgEditCornerBefore` is what makes the whole session one undoable
    // step on the way out.
    pgEditCornerBefore = pgCorner[(size_t) corner];
    undoManager.beginNewTransaction();
    undoManager.perform (new CornerEditBoundaryAction (*this));
    pgEditStackDepth = undoManager.getUndoDescriptions().size();

    // Rule 6: morph mode pins the point onto the corner being edited.
    if (! isPlaygroundMixMode())
        for (const char* id : { "pgX", "pgY" })
        {
            const bool isX = juce::String (id) == "pgX";
            const float v  = (isX ? (corner & 1) : (corner & 2)) ? 1.0f : 0.0f;
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (v);
        }

    pushCornerToLiveSurface (corner);
    pgDirty = true;   // the next tick republishes with a generation bump
    return true;
}

void FractalAdditiveAudioProcessor::endPlaygroundCornerEdit()
{
    if (pgEditCorner < 0) return;
    const int corner = pgEditCorner;
    pgEditCorner = -1;

    // RULE 7: TAKE THE SESSION BACK OFF THE SHARED UNDO STACK, first, before anything
    // else is written. Every transaction minted while the controls were a scratch
    // surface writes into the LIVE patch when undone or redone: that is what wiped a
    // hand-drawn spectrum on the first Ctrl+Z after leaving and let Ctrl+Y install the
    // corner's own drawing on the user's sound.
    //
    // Undoing them (JUCE cannot drop transactions, and clearUndoHistory() would take the
    // user's own history with it) also walks every parameter back to what the live patch
    // had, which the explicit restore below then confirms value by value.
    //
    // TWO stop conditions, because unwinding one transaction too far eats the user's own
    // last edit: the marker's undo() raises pgEditUnwound, and the depth guard catches
    // the case where the marker has been dropped off the bottom of a full stack. Erring
    // towards leaving residue beats erring towards eating history.
    const auto cornerAfter = pgCorner[(size_t) corner];
    pgEditUnwound = false;
    bool unwound  = false;
    while (! pgEditUnwound
           && undoManager.canUndo()
           && undoManager.getUndoDescriptions().size() >= pgEditStackDepth)
    {
        undoManager.undo();
        unwound = true;
    }
    // The session's edits belong to the CORNER, and the unwind is about the live patch's
    // history only - nothing it undoes may take the corner's own content with it.
    pgCorner[(size_t) corner] = cornerAfter;

    // Hand the LIVE PATCH back. Deliberately NOT restoreState(): that would re-read
    // every corner's .gsting from disk (its PLAYGROUND child re-assigns them), which
    // would throw away the very edits this session was making - including the ones on
    // the corner just left. Only what entering edit mode touched is put back.
    if (pgEditLiveState.isValid())
    {
        const auto& all = getParameters();
        for (int i = 0; i < all.size(); ++i)
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
            {
                const bool isDiscrete = dynamic_cast<juce::AudioParameterChoice*> (all[i]) != nullptr
                                     || dynamic_cast<juce::AudioParameterBool*>   (all[i]) != nullptr;
                if (presetmorph::morphSpaceFor (p->paramID.toStdString(), isDiscrete)
                        == presetmorph::MorphSpace::Excluded)
                    continue;   // never written on the way in, so never written on the way out
                if (auto ch = pgEditLiveState.getChildWithProperty ("id", p->paramID); ch.isValid())
                {
                    const float norm = juce::jlimit (0.0f, 1.0f,
                                        p->convertTo0to1 ((float) (double) ch.getProperty ("value")));
                    if (std::abs (p->getValue() - norm) > 1.0e-6f)
                        p->setValueNotifyingHost (norm);
                }
            }

        // The non-parameter half of the live patch: its Custom Engine scale (with the
        // spectrum's provenance) and its per-partial amplitude edits.
        if (auto tim = pgEditLiveState.getChildWithName ("TIMBRE"); tim.isValid())
        {
            juce::String err;
            const auto src = tim.getProperty ("source").toString();
            const auto nm  = tim.getProperty ("name").toString();
            if (src.isEmpty()) clearTimbre();
            else
            {
                std::vector<double> perDeg; double per = 2.0;
                if (nm != "typed" && TuningManager::parseScalaScale (src, perDeg, per, err))
                {
                    timbreDegrees.assign (perDeg.begin(), perDeg.end());
                    timbrePeriod = (float) per;
                    timbreName   = nm;
                    timbreSource = src;
                }
                else
                    loadTimbreText (src, err);
                specProvName   = tim.getProperty ("provName").toString();
                specProvSource = tim.getProperty ("provSource").toString();
                specProvPeriod = tim.getProperty ("provPeriod").toString();
            }
        }
        else
            clearTimbre();

        {
            std::vector<spectrum::PartialGain> g;
            if (auto pg = pgEditLiveState.getChildWithName ("PGAINS"); pg.isValid())
                for (const auto& tok : juce::StringArray::fromTokens (pg.getProperty ("v").toString(), " ", ""))
                {
                    const int col = tok.indexOfChar (':');
                    if (col > 0)
                        g.push_back ({ tok.substring (0, col).getLargeIntValue(),
                                       tok.substring (col + 1).getFloatValue() });
                }
            bool man = false;
            if (auto ch = pgEditLiveState.getChildWithProperty ("id", "partialManual"); ch.isValid())
                man = (double) ch.getProperty ("value") > 0.5;
            setPartialGains (g, man, false);   // the next blend publish resolves them
        }

        // The point goes back where the user left it (morph mode pinned it - rule 6).
        for (const char* id : { "pgX", "pgY" })
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (juce::String (id) == "pgX" ? pgEditLiveX : pgEditLiveY);

        // ...and so does the live patch's IDENTITY: which preset it came from and what
        // "drifted" is measured against. Without this the editor's dirty mark would be
        // reporting how far the LIVE patch has drifted from the corner it just showed.
        loadedPresetName     = pgEditLivePresetName;
        loadedPresetSnapshot = pgEditLivePresetSnap;
    }

    pgEditLiveState = {};
    pgEditLivePresetSnap.clear();

    // RULE 7, the other half: ONE STEP for the whole session, in place of the dozens it
    // used to leave. It writes only the corner, so no direction of it can reach the live
    // patch - and performing it is the only way to clear the redo stack the unwind just
    // filled, because JUCE drops future transactions on a new action and by no other
    // route. Skipped when the session minted nothing: there is then no redo stack to
    // clear and a step that does nothing is a Ctrl+Z that appears broken.
    if (unwound)
    {
        undoManager.beginNewTransaction ("corner edit");
        undoManager.perform (new CornerEditSessionAction (*this, corner,
                                                          pgEditCornerBefore, cornerAfter));
    }
    pgEditCornerBefore = {};
    pgEditStackDepth   = 0;

    pgDirty = true;   // the morph owns the controls again: re-apply EVERYTHING
}

// Corner -> the live control surface. Everything a corner owns, and only that (rule 5:
// the tuning and the point are excluded and must survive untouched).
//
// EVERY non-excluded parameter is written, without exception. This used to skip the
// parameters the corner's preset predated, which left the BLEND's value for them
// standing on a surface that was supposed to be the corner - and the next
// pullPlaygroundCornerEdit read it straight back into the corner store, so an untouched
// visit walked the corner towards the other one while the "*" said nothing had happened
// (hunt entry 10, run 51(c): stored ampSustain nan -> 0.0500, the blend's value).
// Because the store is total there is now nothing to skip.
void FractalAdditiveAudioProcessor::pushCornerToLiveSurface (int corner)
{
    const auto& c = pgCorner[(size_t) corner];
    const auto& all = getParameters();
    for (int i = 0; i < all.size(); ++i)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]);
        if (p == nullptr) continue;
        const bool isDiscrete = dynamic_cast<juce::AudioParameterChoice*> (all[i]) != nullptr
                             || dynamic_cast<juce::AudioParameterBool*>   (all[i]) != nullptr;
        if (presetmorph::morphSpaceFor (p->paramID.toStdString(), isDiscrete)
                == presetmorph::MorphSpace::Excluded)
            continue;
        double v = 0.0;
        if (! c.value (i, v)) continue;   // not a parameter at all (see PlaygroundCorner::value)
        const float norm = juce::jlimit (0.0f, 1.0f, p->convertTo0to1 ((float) v));
        if (std::abs (p->getValue() - norm) > 1.0e-6f)
            p->setValueNotifyingHost (norm);
    }
    // The Custom Engine scale, the provenance and the per-partial edits - the same
    // non-parameter half the winner-corner rule already knows how to apply.
    applyPlaygroundDiscreteState (corner);
}

// The live control surface -> the corner, once per 60 Hz tick while edit mode is on.
// This is where "the controls edit THAT corner" actually happens.
void FractalAdditiveAudioProcessor::pullPlaygroundCornerEdit()
{
    auto& c = pgCorner[(size_t) pgEditCorner];
    if (! c.assigned()) { endPlaygroundCornerEdit(); return; }

    bool changed = false;
    const auto& all = getParameters();
    if (c.params.size() != (size_t) all.size())
        fillCornerDefaults (c);   // total, always - never a row of zeroes
    for (int i = 0; i < all.size(); ++i)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]);
        if (p == nullptr) continue;
        const bool isDiscrete = dynamic_cast<juce::AudioParameterChoice*> (all[i]) != nullptr
                             || dynamic_cast<juce::AudioParameterBool*>   (all[i]) != nullptr;
        if (presetmorph::morphSpaceFor (p->paramID.toStdString(), isDiscrete)
                == presetmorph::MorphSpace::Excluded)
            continue;
        // The parameter object's own value is the USER'S BASE - modulation never goes
        // through setValue (captureState relies on the same fact), so a corner cannot
        // absorb an LFO's momentary displacement and freeze it as a value.
        const float v = p->convertFrom0to1 (p->getValue());
        if (std::abs (c.params[(size_t) i] - v) > 1.0e-7f)
        {
            c.params[(size_t) i] = v;
            changed = true;
        }
    }

    // ...and the non-parameter half, so editing the Custom Engine's scale or dragging a
    // partial's amplitude lands in the corner too rather than being silently dropped.
    if (c.timbreName != timbreName || c.timbreSource != timbreSource
        || c.provName != specProvName || c.provSource != specProvSource
        || c.provPeriod != specProvPeriod)
    {
        c.timbreName = timbreName; c.timbreSource = timbreSource;
        c.provName = specProvName; c.provSource = specProvSource; c.provPeriod = specProvPeriod;
        changed = true;
    }
    {
        const auto& live = getPartialGains();
        const bool man = apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
        if (man != c.manual || live.size() != c.gains.size()
            || ! std::equal (live.begin(), live.end(), c.gains.begin(),
                             [] (const spectrum::PartialGain& a, const spectrum::PartialGain& b)
                             { return a.key == b.key && std::abs (a.dB - b.dB) < 1.0e-6f; }))
        {
            c.gains  = live;
            c.manual = man;
            changed  = true;
        }
    }

    if (changed && ! pgEditPrimed)
    {
        // The baseline pull (see pgEditPrimed): the corner has taken the surface's view
        // of itself, which is not something the user did.
        ++c.rev;                         // the sub-engine still has to see the values
        changed = false;
    }
    pgEditPrimed = true;
    if (changed)
    {
        c.edited = true;                 // the "*"
        ++c.rev;                         // the mix sub-engine's refill trigger
        pgEditCompilePending = true;
    }
    // THE RECOMPILE, throttled to the glide cadence and never skipped for good: a full
    // compileSpectrum every frame of a knob drag stalls the message thread, and dropping
    // the last one would leave the corner's SPECTRUM permanently one edit behind the
    // controls that produced it (pgParamsPending's lesson, on this side of the fence).
    if (pgEditCompilePending && (++pgEditTick % kPgGlideTicks) == 0)
    {
        compileCornerTemplate (c);
        pgEditCompilePending = false;
        pgDirty = true;                  // republish the blend WITH a generation bump
    }
}

juce::String FractalAdditiveAudioProcessor::savePlaygroundCornerAs (int corner, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners)) return {};
    auto& c = pgCorner[(size_t) corner];
    if (! c.assigned()) return {};
    // ONLY the corner in edit mode can be saved, and that is not a limitation - it is
    // the only corner whose FULL state exists anywhere. A corner's store keeps what a
    // BLEND needs (parameter values, scale, partial edits, routing); a .gsting also
    // carries the rhythm slots, the tuning bank, the segment picker and the preset's own
    // tuning. Writing a corner from its store would produce a preset that quietly lost
    // all of those while claiming to be the same sound.
    if (corner != pgEditCorner) return {};

    const auto safe = juce::File::createLegalFileName (name).trim();

    // The edited corner is standing ON the live control surface - beginPlaygroundCornerEdit
    // pushed every one of its values there and pullPlaygroundCornerEdit has been reading
    // them back ever since - so captureState() describes THAT CORNER, not a blend. This
    // is the one save the fixed-point rule lets through onto a corner's own file, and
    // that is why: here the surface IS the corner's stored state (see writePresetFile).
    PresetWriteRefusal why = PresetWriteRefusal::None;
    int blocked = -1;
    if (! writePresetFile (safe, captureState(), true, why, blocked))
        return {};

    // The corner now IS that file: renamed, and the "*" goes away because the edits it
    // warned about are on disk. snapshotLoadedPreset is deliberately NOT called - the
    // LIVE patch's identity is frozen while a corner is being edited (rule 1), and
    // saving a corner must not quietly claim the user's own patch is now this preset.
    c.name   = safe;
    c.edited = false;
    ++c.rev;
    return safe;
}

float FractalAdditiveAudioProcessor::getPlaygroundCornerParamForTests (int corner,
                                                                       const juce::String& id) const
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners))
        return std::numeric_limits<float>::quiet_NaN();
    // NaN means "this corner has NO stored value for that parameter" - which, before the
    // fixed-point fix, is the state an untouched corner-edit visit turned into "the
    // blend's value, and the * says nothing happened" (hunt entry 10).
    const auto& c = pgCorner[(size_t) corner];
    double v = 0.0;
    if (! c.value (paramIndexOf (id.toRawUTF8()), v))
        return std::numeric_limits<float>::quiet_NaN();
    return (float) v;
}

int FractalAdditiveAudioProcessor::getPlaygroundAssignedCount() const
{
    int n = 0;
    for (const auto& c : pgCorner) if (c.assigned()) ++n;
    return n;
}

bool FractalAdditiveAudioProcessor::isPlaygroundActive() const { return pgActive; }

bool FractalAdditiveAudioProcessor::isPlaygroundSuspendingRealtime() const noexcept
{
    return pgActive && pgRtSuspended && pgSuspendedRt != RtLink::Off;
}

void FractalAdditiveAudioProcessor::getPlaygroundWeights (double wOut[kPlaygroundCorners]) const
{
    bool assigned[kPlaygroundCorners];
    for (int i = 0; i < kPlaygroundCorners; ++i) assigned[i] = pgCorner[(size_t) i].assigned();
    presetmorph::bilinearWeights ((double) apvts.getRawParameterValue ("pgX")->load(),
                                  (double) apvts.getRawParameterValue ("pgY")->load(),
                                  assigned, wOut);
}

int FractalAdditiveAudioProcessor::paramIndexOf (const char* id) const
{
    const auto& all = getParameters();
    for (int i = 0; i < all.size(); ++i)
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
            if (p->paramID == id) return i;
    return -1;
}

// A corner's value for a parameter, by id. The store is total (fillCornerDefaults ran
// before the file was read), so the default a preset that predates the parameter means
// is already sitting in it - this is a lookup, not a fallback, and the last place that
// could have held a second opinion about what "the corner's value" means is gone.
float FractalAdditiveAudioProcessor::cornerValueOr (const PlaygroundCorner& c, const char* id) const
{
    double v = 0.0;
    if (c.value (paramIndexOf (id), v)) return (float) v;
    if (auto* p = apvts.getParameter (id)) return p->convertFrom0to1 (p->getDefaultValue());
    return 0.0f;
}

void FractalAdditiveAudioProcessor::compileCornerTemplate (PlaygroundCorner& c)
{
    // The corner's Custom Engine scale as degree ratios, parsed exactly the way
    // restoreState parses it - a corner has to compile to the same partials the preset
    // would if it were simply loaded.
    std::vector<float> degrees;
    float period = 2.0f;
    if (c.timbreSource.isNotEmpty())
    {
        std::vector<double> perDeg; double per = 2.0; juce::String err;
        if (c.timbreName != "typed" && TuningManager::parseScalaScale (c.timbreSource, perDeg, per, err))
        {
            degrees.assign (perDeg.begin(), perDeg.end());
            period = (float) per;
        }
        else
        {
            parseTimbreIntervals (c.timbreSource, degrees, period, err); // typed intervals
        }
    }

    // A corner has no live modulators and no "manual slope frozen" history, so its
    // compile slope is simply its own curveSlope - which is also what the morph writes
    // to the slope parameter at that corner, so the voices' dS comes out at 0 there.
    double slope = 1.0;
    c.value (paramIndexOf ("curveSlope"), slope);

    const auto s = settingsFromValues (
        [&c, this] (const char* id) { return cornerValueOr (c, id); },
        degrees, period, (float) slope);

    c.tmpl = std::make_shared<const spectrum::SpectrumTemplate> (spectrum::compileSpectrum (s));
}

//==============================================================================
// ADAPTIVE CORNERS: a corner saved with the SpectrumFromChord follower armed keeps
// adapting INSIDE the playground - to the chord under the fingers, against its own
// settings - and its template updates live so the blend moves with the playing.
// The rules are the plain follower's, applied per corner; the publish is the blend.
//==============================================================================
int FractalAdditiveAudioProcessor::getPlaygroundAdaptiveCount() const noexcept
{
    int n = 0;
    for (const auto& c : pgCorner) if (c.rtAdaptive()) ++n;
    return n;
}

bool FractalAdditiveAudioProcessor::startCornerAdapt (int corner, const std::vector<double>& alphas,
                                                      double f0, juce::String& errorOut)
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners)) return false;
    auto& c = pgCorner[(size_t) corner];
    const auto s = c.rtSet.adapt;            // BY VALUE: it travels into the worker thread
    const int  N = juce::jlimit (2, 24, s.partials);
    if (alphas.empty() || ! (f0 > 0.0) || ! std::isfinite (f0))
    {
        errorOut = "No target notes to adapt to.";
        return false;
    }
    // The same refusal the plain follower makes: with the CORNER's temper switch on the
    // adapted spectrum would be compiled from the tuning grid instead and never heard.
    if (cornerValueOr (c, "timbreTemper") > 0.5f)
    {
        errorOut = "This preset was saved with temper mode on, which builds its spectrum "
                   "from the tuning - an adapted spectrum would never be heard.";
        return false;
    }
    const double period = s.periodRatio;
    if (! (period > 1.02) || ! std::isfinite (period))
    {
        errorOut = "Invalid period.";
        return false;
    }
    // The no-merge precondition, exactly as startSpectrumAdapt states it: N gaps must
    // fit inside the period, the period's own copy of partial 0 included.
    const double totalC = 1200.0 * std::log2 (period);
    const double gap    = juce::jlimit (0.0, 600.0, s.minGapCents);
    if (std::max (1.0, gap) * (double) N > totalC)
    {
        errorOut = "Those " + juce::String (N) + " partials cannot all keep "
                 + juce::String (gap, 1) + " cents of air inside a "
                 + juce::String (totalC, 1) + "-cent period.";
        return false;
    }
    const double topRatio = std::exp2 ((totalC - std::max (1.0, gap)) / 1200.0);

    // Priced with the realtime register cap (this IS a follower), and only the absurd is
    // refused - same bound, same reason, same PAIR-count comparison as startSpectrumAdapt
    // (the calibrated estimate follows the machine and its one-shot probe can catch a
    // load spike; the refusal must not).
    const auto regs = adaptRegistersFor (s, f0, /*realtime*/ true);
    const int  nRegs = (int) std::max<size_t> (1, regs.f0.size());
    const double estMs = dissonance::adaptEstimateMs (N, (int) alphas.size(), s.rootOnly,
                                                      nRegs, s.shapeAmps);
    if (dissonance::adaptTotalPairs (N, (int) alphas.size(), s.rootOnly,
                                     nRegs, s.shapeAmps, 6) > dissonance::kAdaptAbsurdPairs)
    {
        errorOut = "This corner's adaptation is estimated at " + juce::String (estMs / 60000.0, 1)
                 + " minutes per search - it cannot follow anything at that price.";
        return false;
    }

    adaptCancel = true;
    if (adaptThread != nullptr && adaptThread->joinable())
        adaptThread->join();
    adaptCancel = false;

    // Seed at the CORNER's slope, not the live patch's: the search belongs to the
    // corner's preset, and the live slope is a morphed value that describes no preset.
    const double slope = juce::jlimit (0.1, 6.0, (double) cornerValueOr (c, "curveSlope"));
    std::vector<dissonance::Partial> seed;
    seed.reserve ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const double cts = totalC * (double) i / (double) N;
        const double r   = std::exp2 (cts / 1200.0);
        seed.push_back ({ r * f0, std::pow (r, -slope) });
    }

    adaptProgress = 0.0f;
    adaptRunning  = true;   // "never two at once", shared with the follower + the button

    adaptThread = std::make_unique<std::thread> (
        [this, seed = std::move (seed), alphas, f0, s, N, gap, topRatio, regs,
         corner, name = c.name, period]()
        {
            const auto goal = s.goal == 1 ? dissonance::Goal::Max
                            : s.goal == 2 ? dissonance::Goal::Range
                                          : dissonance::Goal::Min;
            auto res = dissonance::adaptSpectrum (seed, f0, alphas, s.rootOnly, goal,
                                                  s.rangeLo, s.rangeHi, gap, topRatio,
                                                  s.shapeAmps, (double) spectrum::kGainMaxDb, 6, regs,
                                                  [this] (float p) { adaptProgress.store (p); return ! adaptCancel.load(); });
            if (! adaptCancel.load() && ! res.spectrum.empty())
            {
                CornerAdaptResult r;
                r.corner     = corner;
                r.cornerName = name;    // identity travels WITH the result: the corner may
                r.period     = period;  // be reassigned before this lands (see the consume)
                r.aimAlphas  = alphas;
                r.aimF0      = f0;
                r.ratios.reserve (res.spectrum.size());
                r.amps.reserve (res.spectrum.size());
                for (const auto& p : res.spectrum)
                {
                    r.ratios.push_back (p.freq / f0);
                    r.amps.push_back ((double) p.amp);
                }
                r.valid = (int) r.ratios.size() == N;
                {
                    const juce::ScopedLock sl (adaptLock);
                    pgAdaptResult = std::move (r);
                }
                // Its OWN generation, never adaptGeneration: the Adapt panel's result -
                // and its Apply button - must not be able to pick up a corner's run.
                pgAdaptGeneration.fetch_add (1);
            }
            adaptProgress = 1.0f;
            adaptRunning  = false;
        });
    return true;
}

void FractalAdditiveAudioProcessor::applyCornerAdaptResult (PlaygroundCorner& c,
                                                            const CornerAdaptResult& r)
{
    // The adapted intervals become the corner's typed spectrum - the same text
    // applySpectrumAdapt writes into the live patch, so the winner path (which loads
    // c.timbreSource into the Custom Engine box) shows exactly what is sounding.
    juce::String txt;
    for (size_t i = 1; i < r.ratios.size(); ++i)
        txt << dissonance::centsText (1200.0 * std::log2 (juce::jmax (1.0e-9, r.ratios[i]))).c_str() << "c ";
    txt << dissonance::centsText (1200.0 * std::log2 (juce::jmax (1.0001, r.period))).c_str() << "c";
    c.timbreName   = "typed";
    c.timbreSource = txt;
    // Adapted to the HANDS, not to a tuning: provenance is honestly nothing - the same
    // rule applySpectrumAdapt applies to every chord-targeted run.
    c.provName.clear(); c.provSource.clear(); c.provPeriod.clear();

    // The amplitude shaping is BAKED into the template below, so it is in the BLEND at
    // any weight - diluted but present, which is the whole feature. The manual-gain
    // layer is cleared with the switch: fillGains resolves the winner's gains onto the
    // published blend, so leaving them here would apply the shaping a SECOND time the
    // moment this corner wins.
    c.gains.clear();
    c.manual = false;
    const auto setParam = [this, &c] (const char* id, float v)
    {
        const int i = paramIndexOf (id);
        if (juce::isPositiveAndBelow (i, (int) c.params.size()))
            c.params[(size_t) i] = v;
    };
    setParam ("timbreOn", 1.0f);        // the adapted spectrum replaces the corner's
    setParam ("partialManual", 0.0f);   // engine, exactly as the follower replaces the live one

    // Compile at the CORNER's settings (fractal, reps, range... all its own), with the
    // adapted degrees as the Custom Engine's scale - shared settingsFromValues, so this
    // corner and a live patch holding the same adaptation can never disagree.
    std::vector<float> degrees;
    degrees.reserve (r.ratios.size());
    for (const double v : r.ratios) degrees.push_back ((float) v);
    auto s = settingsFromValues ([this, &c] (const char* id) { return cornerValueOr (c, id); },
                                 degrees, (float) r.period,
                                 juce::jlimit (0.1f, 6.0f, cornerValueOr (c, "curveSlope")));
    // Stated explicitly rather than trusted to the getter (the same defence
    // applySpectrumAdapt makes): this template must be the adapted spectrum, whatever
    // the corner's stored switches were a moment ago.
    s.source              = spectrum::Source::ScalaTimbre;
    s.temperToTimbre      = false;
    s.timbreDegrees       = degrees;
    s.timbrePeriod        = (float) r.period;
    s.timbrePeriodRepeats = (int) cornerValueOr (c, "timbreRepeats");
    auto tmpl = spectrum::compileSpectrum (s);

    // Bake the adapted amplitudes onto the compiled partials: same nearest-cents match
    // and the same +/-24 dB bound as the follower's gain path, so a baked amp can never
    // exceed what that path could have produced.
    if (! tmpl.partials.empty() && ! r.amps.empty())
    {
        double aMax = 0.0, cMax = 0.0;
        for (const double a : r.amps) aMax = juce::jmax (aMax, a);
        for (const auto& p : tmpl.partials) cMax = juce::jmax (cMax, (double) p.amp);
        if (aMax > 0.0 && cMax > 0.0)
            for (size_t i = 0; i < r.ratios.size() && i < r.amps.size(); ++i)
            {
                int best = -1; double bestC = 1.0e9;
                for (size_t q = 0; q < tmpl.partials.size(); ++q)
                {
                    const double d = std::abs (1200.0 * std::log2 (juce::jmax (1.0e-9,
                                        (double) tmpl.partials[q].ratio / r.ratios[i])));
                    if (d < bestC) { bestC = d; best = (int) q; }
                }
                if (best < 0 || bestC > 1.0) continue;   // a stacked copy: leave it alone
                const double want = r.amps[i] / aMax * cMax;
                const double have = (double) tmpl.partials[(size_t) best].amp;
                if (! (want > 0.0) || ! (have > 0.0)) continue;
                const double dB = juce::jlimit (-(double) spectrum::kGainMaxDb,
                                                (double) spectrum::kGainMaxDb,
                                                20.0 * std::log10 (want / have));
                tmpl.partials[(size_t) best].amp = (float) (have * std::pow (10.0, dB / 20.0));
            }
    }
    c.tmpl = std::make_shared<const spectrum::SpectrumTemplate> (std::move (tmpl));
}

void FractalAdditiveAudioProcessor::drivePlaygroundAdaptive()
{
    // The chord under the fingers RIGHT NOW - read once, because it answers three
    // questions (is a finished result stale? has each corner's target moved? what does
    // a new search aim at?) and they must all be answered about the same instant.
    double f0 = 0.0;
    const std::vector<double> held = heldChordAsTargetAlphas (f0);
    const auto movedFrom = [&held, f0] (const std::vector<double>& aim, double aimF0)
    {
        // Per-note, in cents, root's absolute pitch included - the follower's own rule
        // (kRtHeldMoveCents): a sub-cent bend wobble is not a new chord.
        bool moved = held.size() != aim.size() || ! (aimF0 > 0.0)
                  || std::abs (1200.0 * std::log2 (f0 / aimF0)) > kRtHeldMoveCents;
        for (size_t i = 0; i < held.size() && ! moved; ++i)
            if (std::abs (1200.0 * std::log2 (held[i] / aim[i])) > kRtHeldMoveCents)
                moved = true;
        return moved;
    };

    // 1. A FINISHED RESULT lands first (the follower's order: whether it still belongs
    // to the hands on the keys is the first thing decided about it). Stale - the player
    // has MOVED to a different chord - means dropped, never applied late; a RELEASED
    // chord is not stale (that chord was played, and its result belongs to it).
    if (const int g = pgAdaptGeneration.load(); g != pgSeenAdaptGen)
    {
        pgSeenAdaptGen  = g;
        pgAdaptInFlight = -1;
        CornerAdaptResult r;
        {
            const juce::ScopedLock sl (adaptLock);
            r = pgAdaptResult;
        }
        const bool stale = ! held.empty() && movedFrom (r.aimAlphas, r.aimF0);
        auto* c = juce::isPositiveAndBelow (r.corner, kPlaygroundCorners)
                      ? &pgCorner[(size_t) r.corner] : nullptr;
        // The name check is the identity check: the corner may have been reassigned (or
        // cleared) while the search ran, and this result belongs to the preset it was
        // started FOR, not to whichever one sits on that corner now.
        if (r.valid && ! stale && c != nullptr && c->rtAdaptive() && c->name == r.cornerName)
        {
            applyCornerAdaptResult (*c, r);
            ++pgAdaptApplies;
            // When the adapting corner IS the winner, the live patch's spectrum state
            // (Custom Engine text, gains, manual switch) is what the screen describes -
            // it has to follow the adaptation or it would describe a spectrum that is
            // no longer the one sounding. MORPH mode only: MIX never writes the live
            // patch (its corners sound through their own engines - the replaced c.tmpl
            // reaches the ear via refreshMixEngine's republish, generation bump and all).
            if (pgWinner == r.corner && ! isPlaygroundMixMode())
                applyPlaygroundDiscreteState (r.corner);
            // A REAL spectrum change, not a drag: pgDirty makes the blend below publish
            // WITH a generation bump, so the voices glide to it (morph on) or cross the
            // 50 ms template fade (morph off) - "like changing spectrum with morph on".
            // A bump-less publish would instead land every adaptation as a bare 7 ms
            // amplitude step: run 42's documented thump.
            pgDirty = true;
        }
    }
    else if (pgAdaptInFlight >= 0 && ! adaptRunning.load())
    {
        // The search died with no result (cancelled by a hand-pressed Adapt, or torn
        // down): that corner is still owed its adaptation, or it would sit stale on the
        // current chord for ever.
        if (pgCorner[(size_t) pgAdaptInFlight].rtAdaptive())
            pgCorner[(size_t) pgAdaptInFlight].rtPend = true;
        pgAdaptInFlight = -1;
    }

    // 2. HAS THE CHORD MOVED away from what each corner last aimed at? Nothing held
    // clears every pending search - the follower's rule: engaging over silence must not
    // adapt to whatever the next tick happens to read.
    bool anyAdaptive = false;
    for (auto& c : pgCorner)
    {
        if (! c.rtAdaptive()) continue;
        anyAdaptive = true;
        if (held.empty())                            c.rtPend = false;
        else if (movedFrom (c.rtAimAlphas, c.rtAimF0)) c.rtPend = true;
    }
    if (! anyAdaptive || held.empty())
        return;

    // 3. LAUNCH what is owed: one search at a time (adaptRunning - shared with the
    // follower and the Adapt button), the follower's own launch floor between starts,
    // round-robin so two adaptive corners share the worker fairly.
    if (adaptRunning.load() || rtTick - pgAdaptLastStart < kRtAdaptTicks)
        return;
    for (int k = 0; k < kPlaygroundCorners; ++k)
    {
        const int i = (pgAdaptNext + k) % kPlaygroundCorners;
        auto& c = pgCorner[(size_t) i];
        if (! c.rtAdaptive() || ! c.rtPend) continue;
        juce::String err;
        if (startCornerAdapt (i, held, f0, err))
        {
            ++pgAdaptStarts;
            c.rtPend      = false;
            // The target actually USED, not what merely passed by - the launch may have
            // been held for several ticks (same reasoning as the follower's rtHeldTarget).
            c.rtAimAlphas = held;
            c.rtAimF0     = f0;
            pgAdaptLastStart = rtTick;
            pgAdaptInFlight  = i;
            pgAdaptNext      = i + 1;
        }
        else
        {
            // These settings can never run. Stop asking (a refusal per tick is a flood)
            // and SAY so where the window shows it - never a silent stop.
            c.rtDead = true;
            c.rtPend = false;
            pgAdaptRefusal = juce::String::charToString ((juce::juce_wchar) ('A' + i)) + ": " + err;
        }
        break;
    }
}

void FractalAdditiveAudioProcessor::refreshPlaygroundActive()
{
    const bool was = pgActive;
    pgActive = getPlaygroundAssignedCount() >= 2;
    if (pgActive == was)
        return;

    if (pgActive)
    {
        // ENGAGING. The realtime/adaptive follower is stood down for the duration.
        //
        // Not because the two are incompatible in principle, but because they both write
        // the SPECTRUM and only one can own it. SpectrumFromChord re-adapts the partials
        // (and the per-partial amplitudes) up to ten times a second and publishes them
        // directly; it would overwrite the blend within ~100 ms of every move, so the
        // point in the rectangle would visibly do nothing and the feature would read as
        // broken. TuningFromSpectrum is worse: the blend changes the spectrum sixty times
        // a second, so it would retune the instrument continuously - and the tuning is
        // exactly what the playground promises never to touch.
        //
        // The arming is REMEMBERED, not discarded, and handed straight back below.
        pgSuspendedRt = rtLink;
        pgRtSuspended = true;
        if (rtLink != RtLink::Off)
            setRealtimeLink (RtLink::Off, rtSet);
        pgWinner = -1;   // no incumbent: the first apply adopts the highest weight outright
        pgBlendTick = 0; pgLastBump = -1000000; pgGlidePending = false;
        // Adaptive corners start FRESH: adopt the current result generation so nothing
        // from a previous engagement can land into this one, and lift the launch floor
        // so the first search is not throttled (the follower's own engage rule).
        pgSeenAdaptGen   = pgAdaptGeneration.load();
        pgAdaptLastStart = rtTick - 10000;
        pgAdaptInFlight  = -1;
        pgAdaptRefusal.clear();
    }
    else
    {
        // LETTING GO. Put the follower back, then force ONE ordinary rebuild: the
        // spectrum has been coming from the blend, so lastPushed no longer describes
        // anything the compiler produced and the usual "settings changed?" test would
        // leave the last blend sounding under un-morphed controls for ever.
        if (pgRtSuspended && pgSuspendedRt != RtLink::Off)
            setRealtimeLink (pgSuspendedRt, rtSet);
        pgRtSuspended = false;
        pgSuspendedRt = RtLink::Off;
        lastPushed = currentSettings();
        spectrumEngine.requestRebuild (lastPushed);
    }
    pgDirty = true;
}

void FractalAdditiveAudioProcessor::drivePlayground()
{
    refreshPlaygroundActive();
    if (! pgActive)
    {
        // The playground let go (or never engaged): the mix must let go with it, or
        // four sub-engines would keep sounding under a pad that no longer exists.
        // Corner edit goes with it for the same reason - a scratch surface standing in
        // for a corner nobody is blending is just the user's patch, silently replaced.
        endPlaygroundCornerEdit();
        pgMixDriving  = false;
        pgLastMixMode = false;
        mixEngaged.store (false, std::memory_order_release);
        return;
    }

    // CORNER EDIT: the controls belong to a corner this tick, so read them INTO it
    // before anything else looks at either. Done first so the mix refresh and the blend
    // below both see the edit in the same frame it was made.
    if (pgEditCorner >= 0) pullPlaygroundCornerEdit();
    const bool editing = pgEditCorner >= 0;   // the pull can end edit mode (corner gone)

    const bool mixMode = isPlaygroundMixMode();
    if (mixMode != pgLastMixMode)
    {
        pgLastMixMode = mixMode;
        // Returning to MORPH: the live spectrum has not been the blend while MIX owned
        // the sound, so the next morph tick must re-apply EVERYTHING - with the
        // generation bump a real spectrum change deserves (the voices morph/fade it,
        // which is also what makes the mode switch itself click-free on this side).
        if (! mixMode) pgDirty = true;
    }

    // ADAPTIVE CORNERS first: a finished adaptation updates its corner's template.
    // Morph mode publishes it through the blend below (with a generation bump); mix
    // mode republishes it into that corner's own sub-engine (see refreshMixEngine).
    drivePlaygroundAdaptive();

    const double x = (double) apvts.getRawParameterValue ("pgX")->load();
    const double y = (double) apvts.getRawParameterValue ("pgY")->load();

    double w[kPlaygroundCorners];
    bool assigned[kPlaygroundCorners];
    for (int i = 0; i < kPlaygroundCorners; ++i) assigned[i] = pgCorner[(size_t) i].assigned();
    presetmorph::bilinearWeights (x, y, assigned, w);

    if (mixMode)
    {
        // MIX drives the sub-engines and NOTHING else: no parameter writes, no live
        // spectrum publish - the live patch stays exactly as the user left it, which
        // is what makes disengaging restore the single-engine state bit-for-bit.
        drivePlaygroundMix (w);
        pgMixDriving = true;
        return;
    }
    pgMixDriving = false;
    mixEngaged.store (false, std::memory_order_release);

    const int  prevWinner = pgWinner;
    const int  winner = presetmorph::nearestCorner (w, pgWinner, presetmorph::kHysteresis);
    const bool winnerChanged = winner != prevWinner;
    pgWinner = winner;

    // A JUMP, as opposed to a drag: automation cutting from one corner to another, the
    // point being clicked somewhere else, a corner being (re)assigned, or a discrete
    // setting flipping. Those are genuine spectrum CHANGES and the voices' own machinery
    // (the per-voice morph, or the 50 ms template fade when Morph is off) is what should
    // carry them, immediately. A drag is smaller than that, and its generation bumps are
    // THROTTLED to 10 Hz instead (see the glide block below) - bumping all sixty frames
    // a second is the click machine this project spent days removing, bumping none of
    // them is "no pitch bend en los parciales".
    const double moved = (pgLastX < 0.0) ? 1.0 : std::abs (x - pgLastX) + std::abs (y - pgLastY);
    const bool   jump  = pgDirty || winnerChanged || moved > 0.15;

    const bool movedAny = pgDirty || winnerChanged || moved >= 1.0e-6;
    if (movedAny) pgParamsPending = true;
    ++pgBlendTick;
    if (! movedAny && ! pgParamsPending && ! pgGlidePending)
        return;                                  // parked AND settled: nothing left to do

    // Both writes below are SKIPPED while a corner is being edited: they exist to drive
    // the controls FROM the blend, and the controls are the user's hands right now. Left
    // running, every edit would be overwritten within 33 ms and the feature would read as
    // a knob that snaps back.
    if (winnerChanged && ! editing)
        applyPlaygroundDiscreteState (winner);

    // THE CONTROLS follow at ~30 Hz, not 60. Every write goes through the normal
    // parameter path (so the sliders move, the host sees the value, and Ctrl+Z steps back
    // through the drag in the usual ~1 s transactions), and that path records an undo
    // action per changed property: at 60 Hz across ~200 parameters it is pure churn for a
    // movement no eye can resolve at 33 ms anyway. THE SPECTRUM - the part that is
    // actually being listened to - is republished on every tick regardless.
    //
    // pgParamsPending is what keeps the throttle honest. Without it, a drag that STOPS on
    // a skipped tick leaves every control one frame short of the point - for ever, because
    // the next tick sees no movement and returns early. The parked patch then quietly did
    // not match the pad, by a hair that never closed.
    if (! editing && (pgDirty || winnerChanged || (++pgParamTick & 1) == 0))
    {
        applyPlaygroundContinuous (w);
        pgParamsPending = false;
    }

    if (movedAny)
    {
        // THE FREQUENCY GLIDE (see kPgGlideTicks in the header): every ~6th frame of a
        // drag publishes WITH a generation bump - 10 Hz, the adaptive corners' own
        // republish rate - so with Morph on the voices BEND their partials to the
        // moving blend, and with Morph off they cross the 50 ms fade. The frames in
        // between publish bump-less: amplitudes keep tracking at 60 Hz without
        // restarting anyone's morph clock.
        const bool bump = jump || pgBlendTick - pgLastBump >= kPgGlideTicks;
        postPlaygroundBlend (w, bump);
        if (bump) { pgLastBump = pgBlendTick; pgGlidePending = false; }
        else        pgGlidePending = true;
    }
    else if (pgGlidePending)
    {
        // The drag STOPPED between two glide ticks: the last bumped publish aimed the
        // voices at a blend up to 100 ms behind the point. One final bumped publish
        // lands them EXACTLY on the parked position - without it a parked pad would
        // keep sounding a hair short of its own point for ever (the same one-frame-
        // short trap pgParamsPending closes for the controls).
        postPlaygroundBlend (w, true);
        pgLastBump      = pgBlendTick;
        pgGlidePending  = false;
    }

    // The playground owns the spectrum while it is engaged, so the ordinary
    // "settings changed -> requestRebuild" test in timerCallback must not fire behind it
    // (it would compile the morphed PARAMETERS and publish that over the blend, which is
    // precisely the un-blended cross-engine nonsense the blend exists to avoid).
    lastPushed = currentSettings();

    pgLastX = x; pgLastY = y;
    pgDirty = false;
}

//==============================================================================
// PLAYGROUND MIX MODE - the message-thread half (the audio half is renderSynthMix).
// Runs at 60 Hz while the playground is engaged in mix mode: keeps a sub-engine
// alive and current for every assigned corner, and turns the bilinear weights into
// per-corner gain TARGETS (equal-power - see presetmorph::mixGains). The audio
// thread slews towards those targets; nothing here touches the live patch.
//==============================================================================
void FractalAdditiveAudioProcessor::drivePlaygroundMix (const double w[kPlaygroundCorners])
{
    // MONOPHONIC GLIDE IN MIX MODE: the rule applies to EVERY sub-engine, identically,
    // and it is not refused here. It could not sensibly be: there is ONE note stream and
    // the corners all hear it, so a mix that was mono in one engine and polyphonic in
    // another would answer a single held chord with four different articulations. Each
    // corner therefore drops to one voice exactly as the main engine does, and the
    // takeover glides in each of them (the voices read the LIVE mono parameters, never
    // the corner preset's - see AdditiveVoice's constructor for why).
    //
    // "Mix Voices per Corner" is not silently overridden: while mono is on the control
    // is disabled and reads 1, so the panel states what is in force (see the editor).
    const int wantVoices = lastMonoGlide ? 1 : (int) apvts.getRawParameterValue ("pgVoices")->load();
    for (int i = 0; i < kPlaygroundCorners; ++i)
    {
        if (! pgCorner[(size_t) i].assigned()) continue;
        if (mixEng[(size_t) i] == nullptr) buildMixEngine (i);
        mixEng[(size_t) i]->setVoices (*this, wantVoices);  // the VISIBLE polyphony control
        refreshMixEngine (i);
    }

    double g[kPlaygroundCorners];
    presetmorph::mixGains (w, g);
    for (int i = 0; i < kPlaygroundCorners; ++i)
        mixGainTarget[i].store (pgCorner[(size_t) i].assigned()
                                    && mixEngLive[(size_t) i].load (std::memory_order_relaxed) != nullptr
                                ? (float) g[i] : 0.0f,
                                std::memory_order_relaxed);
    // Engaged LAST, after every assigned corner has a live engine: the audio thread
    // must never fade in a mix whose engines it cannot find.
    mixEngaged.store (true, std::memory_order_release);
}

// Build corner `corner`'s sub-engine. Message thread; the audio thread only learns of
// it through the release-store at the end, by which time it is fully prepared.
void FractalAdditiveAudioProcessor::buildMixEngine (int corner)
{
    auto e = std::make_unique<MixCornerEngine> (*this);
    const double sr = lastSr > 0.0 ? lastSr : 48000.0;
    e->synth.setCurrentPlaybackSampleRate (sr);
    e->mods.prepare (sr);
    applyZoneTo (e->synth);   // the LIVE zone/bend: tuning plumbing is global
    e->buf.setSize (2, juce::jmax (512, lastBlockSize));
    mixEng[(size_t) corner] = std::move (e);
    mixEngLive[(size_t) corner].store (mixEng[(size_t) corner].get(), std::memory_order_release);
}

// Bring corner `corner`'s sub-engine in step with the corner: the preset's values into
// the bank, its modulator routing into its ModEngine, its compiled template into its
// SpectrumEngine. Cheap when nothing changed (one pointer + one string compare), so
// calling it every tick is fine; a real change is an assign or an adaptive apply.
void FractalAdditiveAudioProcessor::refreshMixEngine (int corner)
{
    auto* e = mixEng[(size_t) corner].get();
    auto& c = pgCorner[(size_t) corner];
    if (e == nullptr || ! c.assigned()) return;
    if (e->lastTmpl == c.tmpl && e->lastName == c.name && e->lastRev == c.rev) return;
    // A CORNER EDIT that moved only voice parameters leaves the template pointer alone,
    // so the template publish below is gated on the pointer really changing: the bank and
    // the routing refresh at the full tick rate (they are cheap and the user is turning
    // the knob NOW), while publishNow - which bumps the sub-engine's generation and
    // restarts every voice's morph - stays at the ~10 Hz the recompile itself runs at.
    // Publishing every frame is the click machine this project keeps documenting.
    const bool tmplChanged = (e->lastTmpl != c.tmpl);
    e->lastTmpl = c.tmpl;
    e->lastName = c.name;
    e->lastRev  = c.rev;

    // The preset's values (defaults where it predates a parameter - cornerValueOr's
    // rule, applied by the bank so voices and modulators read the same numbers).
    e->bank.fill ([&c] (int i, double& out) { return c.value (i, out); });

    // The preset's modulator routing + drawn LFO waves. An empty routing means the
    // corner's modulators drive nothing - i.e. they are off, exactly as saved.
    for (int m = 0; m < ModEngine::kMods; ++m)
    {
        e->mods.unmapAll (m);
        juce::StringArray ids;
        ids.addTokens (c.modTargets[m], ",", "");
        for (const auto& id : ids)
            if (id.isNotEmpty()) e->mods.addTarget (m, id);
    }
    for (int l = 0; l < ModEngine::kLfos; ++l)
    {
        juce::StringArray vals;
        vals.addTokens (c.modWaves[l], ",", "");
        if (vals.size() == ModEngine::kWaveN)
        {
            float wv[ModEngine::kWaveN];
            for (int i = 0; i < ModEngine::kWaveN; ++i) wv[i] = vals[i].getFloatValue();
            e->mods.setCustomWave (l, wv);
        }
    }

    // The corner's per-partial edits are STORED first and the template published
    // second, so the publish resolves the edits against exactly this partial list
    // (the same store-then-publish order applyPlaygroundDiscreteState documents).
    // publishNow bumps the sub-engine's generation: on an adaptive corner's update
    // the corner's OWN voices morph (its morph on) or cross the 50 ms fade (off) -
    // the same treatment a live spectrum change gets, which is the whole point.
    e->spectrum.setOverrides (c.gains, c.manual, false);
    if (tmplChanged && c.tmpl != nullptr)
        e->spectrum.publishNow (*c.tmpl);
}

// Everything that cannot be averaged comes from ONE corner (see PresetMorph.h). The
// parameters themselves are written by applyPlaygroundContinuous, which skips nothing:
// this is the non-parameter half - the Custom Engine's scale and the per-partial
// amplitude edits, both of which belong to a specific spectrum and cannot be mixed.
void FractalAdditiveAudioProcessor::applyPlaygroundDiscreteState (int corner)
{
    if (! juce::isPositiveAndBelow (corner, kPlaygroundCorners)) return;
    const auto& c = pgCorner[(size_t) corner];
    if (! c.assigned()) return;

    // The Custom Engine's scale. Loaded the same way restoreState loads it, and BEFORE
    // the timbreOn parameter is written (loadTimbreText switches the engine on by
    // itself, so a corner saved with it off has to be able to put it back off).
    if (c.timbreSource.isEmpty())
    {
        clearTimbre();
    }
    else
    {
        std::vector<double> perDeg; double per = 2.0; juce::String err;
        if (c.timbreName != "typed" && TuningManager::parseScalaScale (c.timbreSource, perDeg, per, err))
        {
            timbreDegrees.assign (perDeg.begin(), perDeg.end());
            timbrePeriod = (float) per;
            timbreName   = c.timbreName;
            timbreSource = c.timbreSource;
        }
        else
        {
            loadTimbreText (c.timbreSource, err);
        }
        // AFTER the loads (loadTimbreText clears provenance): the winner's spectrum
        // arrives with the winner's own origin - which is empty when its preset had
        // none, exactly the clear the button needs to disappear honestly.
        specProvName   = c.provName;
        specProvSource = c.provSource;
        specProvPeriod = c.provPeriod;
    }

    // The per-partial amplitude edits, STORED without republishing: the blend that
    // follows in the same tick is the publish, and republishing here first would swap in
    // the previous blend carrying gain keys computed for a different partial list - the
    // same 7 ms amplitude thump the adaptation's apply path documents.
    setPartialGains (c.gains, c.manual, false);
}

void FractalAdditiveAudioProcessor::applyPlaygroundContinuous (const double w[kPlaygroundCorners])
{
    const auto& all = getParameters();
    for (int i = 0; i < all.size(); ++i)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]);
        if (p == nullptr) continue;

        const bool isDiscrete = dynamic_cast<juce::AudioParameterChoice*> (all[i]) != nullptr
                             || dynamic_cast<juce::AudioParameterBool*>   (all[i]) != nullptr;
        const auto space = presetmorph::morphSpaceFor (p->paramID.toStdString(), isDiscrete);
        if (space == presetmorph::MorphSpace::Excluded)
            continue;

        double target = 0.0;
        if (space == presetmorph::MorphSpace::Discrete)
        {
            // Nearest corner - one winner for every discrete setting at once, so the
            // engine, the scale and the manual switch can never come from three
            // different presets (see nearestCorner in PresetMorph.h). The winner ALWAYS
            // has a value (the store is total), so a corner whose preset predates a
            // discrete setting now reproduces that setting's default at its own vertex
            // instead of leaving whatever the pad last wrote - which is what left a
            // corner that never mentioned mono permanently monophonic (hunt entry 8).
            if (pgWinner < 0 || ! pgCorner[(size_t) pgWinner].value (i, target))
                continue;
        }
        else
        {
            // Every corner with weight contributes, always: the store is total, so the
            // weights are never renormalised over "the corners that happen to know this
            // parameter". That renormalisation is what let one corner rule a parameter
            // 100 % across the whole pad and made the vertex path-dependent (hunt entry
            // 11), and it disagreed with MIX mode, where ParamBank::fill has always used
            // the default. One rule now, in both modes.
            double v[kPlaygroundCorners], ww[kPlaygroundCorners];
            int n = 0;
            for (int k = 0; k < kPlaygroundCorners; ++k)
                if (w[k] > 0.0 && pgCorner[(size_t) k].value (i, v[n]))
                    ww[n++] = w[k];
            if (n == 0) continue;      // nothing assigned at all
            target = presetmorph::blendScalar (v, ww, n, space);
        }

        const float norm = juce::jlimit (0.0f, 1.0f, p->convertTo0to1 ((float) target));
        // Only write what actually moved. A parked point, or a parameter every corner
        // agrees on, then costs nothing at all - no host notification, no undo action.
        if (std::abs (p->getValue() - norm) > 1.0e-5f)
            p->setValueNotifyingHost (norm);
    }
}

void FractalAdditiveAudioProcessor::postPlaygroundBlend (const double w[kPlaygroundCorners],
                                                          bool bumpGeneration)
{
    spectrum::SpectrumEngine::BlendJob job;
    bool any = false;
    for (int i = 0; i < kPlaygroundCorners; ++i)
    {
        job.w[i] = w[i];
        if (w[i] > 0.0 && pgCorner[(size_t) i].tmpl != nullptr)
        { job.corner[i] = pgCorner[(size_t) i].tmpl; any = true; }
        else
            job.w[i] = 0.0;
    }
    if (! any) return;

    // Stamp the blend with the slope the voices will read off the slope PARAMETER, so an
    // unmodulated voice computes dS = 0 and renders the blended amplitudes exactly as
    // they are. Stamping the corners' own slopes instead would have every voice re-slope
    // the whole crossfade by ratio^-(paramSlope - blendedSlope) - a tilt across the
    // spectrum that grows and shrinks as the point moves, which is not a blend of
    // anything.
    job.curveSlope     = apvts.getRawParameterValue ("curveSlope")->load();
    job.bumpGeneration = bumpGeneration;
    spectrumEngine.requestBlend (std::move (job));
}

// The Tuning panel's "Load preset's tuning" button.
bool FractalAdditiveAudioProcessor::loadPendingPresetTuning (juce::String& errorOut)
{
    if (presetPendingSclSource.isEmpty())
    {
        errorOut = "This preset carries no tuning of its own.";
        return false;
    }
    if (! tuning.restoreTuning ({ presetPendingSclName, presetPendingSclSource,
                                  presetPendingTablePeriod }, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    applyTuningMode ((int) TuningManager::Mode::Scala);
    clearSegmentSource();
    syncBankSelector (bankAutoAdd (presetPendingSclName, presetPendingSclSource));
    return true;
}

// The Tuning panel's "Load spectrum's tuning" button: bring back the tuning the current
// custom spectrum was derived from (see the provenance block in the header).
bool FractalAdditiveAudioProcessor::loadSpectrumProvenanceTuning (juce::String& errorOut)
{
    if (specProvSource.isEmpty())
    {
        errorOut = "This spectrum does not come from a tuning.";
        return false;
    }
    if (spectrumProvenanceIsActive())
    {
        // The editor disables the button in this state; refusing here too keeps a host
        // script (or a race with the 60 Hz poll) from pushing a do-nothing undo step.
        errorOut = "That tuning is already the active one.";
        return false;
    }
    // THROUGH THE UNDO STACK, unlike loadPendingPresetTuning: the displaced tuning must
    // be one Ctrl+Z away. The snapshots are taken first, the tuning applied directly
    // (exactly like loadPendingPresetTuning), and THEN the action is performed - whose
    // perform() re-applies the same snapshot idempotently (the same pattern
    // commitRealtimeBaseline relies on), so failure can be reported BEFORE anything is
    // pushed: a failed load that still left an undo step would undo to a lie.
    const auto before     = tuning.captureTuning();
    const int  beforeMode = getTuningMode();
    if (! tuning.restoreTuning ({ specProvName, specProvSource, specProvPeriod }, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    applyTuningMode ((int) TuningManager::Mode::Scala);
    clearSegmentSource();
    syncBankSelector (bankAutoAdd (specProvName.isNotEmpty() ? specProvName
                                                             : juce::String ("spectrum's tuning"),
                                   specProvSource));
    undoManager.beginNewTransaction();
    undoManager.perform (new TuningSnapshotAction (*this, before, beforeMode,
                                                   tuning.captureTuning(),
                                                   (int) TuningManager::Mode::Scala));
    return true;
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FractalAdditiveAudioProcessor();
}
