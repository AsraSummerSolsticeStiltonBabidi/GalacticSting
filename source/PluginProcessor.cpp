#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Dissonance.h"
#include "BinaryData.h"

#include <string>
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
    // Stop and reap the background workers before members are destroyed.
    chordCancel = true;
    nadCancel   = true;
    if (chordThread != nullptr && chordThread->joinable())
        chordThread->join();
    if (nadThread != nullptr && nadThread->joinable())
        nadThread->join();
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
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "primeMode", 1 }, "Prime Mode",
        StringArray { "Limit", "Whitelist" }, 0));
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

    //-- Spectral morphing (glide partials to their new positions on a spectrum change)
    params.push_back (std::make_unique<AudioParameterBool>(
        ParameterID { "morph", 1 }, "Morphing", false));
    params.push_back (std::make_unique<AudioParameterFloat>(
        ParameterID { "morphTime", 1 }, "Morph Time", timeRange (5.0f), 0.300f));

    //-- Tuning ----------------------------------------------------------------
    // Index 0 (plain 12ED2) is no longer OFFERED - MTS-ESP already falls back to
    // 12-TET on its own when no master is connected, so it is the honest default
    // and 12ED2 became a redundant duplicate of that fallback. The entry stays in
    // the list so projects saved with it still restore to exactly what they had.
    params.push_back (std::make_unique<AudioParameterChoice>(
        ParameterID { "tuningMode", 1 }, "Tuning",
        StringArray { "12ED2", "MTS-ESP", "Scala (.scl)", "MPE (bend input)" }, 1));
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
    s.selection     = apvts.getRawParameterValue ("harmonics")->load();
    s.curveSlope    = apvts.getRawParameterValue ("curveSlope")->load();

    // Prime limit is a CONTINUOUS position on the Off - 23 - 19 ... - 2 fader: the
    // whole value below is the limit in force, the one above is the next stop, and
    // the fraction between them fades out the harmonics that stop would remove.
    const float plPos = juce::jlimit (0.0f, 9.0f, apvts.getRawParameterValue ("primeLimit")->load());
    const int plIdx   = juce::jlimit (0, 9, (int) std::floor (plPos));
    s.primeLimitPos  = plPos;
    s.primeLimit     = primeLimitValues[plIdx];
    s.primeLimitNext = primeLimitValues[juce::jmin (9, plIdx + 1)];

    s.primeUseWhitelist = apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
    unsigned mask = 0;
    for (int i = 0; i < spectrum::primeListSize; ++i)
        if (apvts.getRawParameterValue ("primeWL" + juce::String (i))->load() > 0.5f)
            mask |= (1u << i);
    s.primeWhitelistMask = mask;

    s.fractalLevel   = apvts.getRawParameterValue ("fractalLevel")->load();
    s.spectrumRangeOct = (int) apvts.getRawParameterValue ("spectrumRange")->load();
    s.ampFloorDb     = -120.0f;
    s.maxPartials    = spectrum::kMaxPartials;

    // TEMPER mode: the harmonic engine keeps running and its partials snap to the
    // grid of the ACTIVE TUNING (whatever is loaded in the Tuning box - .scl, MTS,
    // MPE...), read live every poll so tuning changes retune the spectrum in real
    // time. Independent of the Custom Engine's own loaded scale.
    const bool temper = apvts.getRawParameterValue ("timbreTemper")->load() > 0.5f;
    if (temper)
    {
        s.source         = spectrum::Source::Harmonic;
        s.temperToTimbre = true;
        const int D = juce::jmax (1, tuning.degreesPerPeriod()); // no cap: every degree is a grid point
        s.timbreDegrees.clear();
        s.timbreDegrees.reserve ((size_t) D);
        for (int d = 0; d < D; ++d)
            s.timbreDegrees.push_back ((float) getDegreeRatio (d));
        s.timbrePeriod = (float) tuning.periodRatio(); // the tuning's OWN period, not a key lookup
        if (! (s.timbrePeriod > 1.0001f)) { s.timbrePeriod = 2.0f; }
    }
    else if (isTimbreActive())
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
                    clearSegmentSource(); // a bank .scl replaces any segment scale
                }
            }
        }
    }

    // ONE-SHOT MTS-ESP adoption, exactly like the jingle's single trigger: if a
    // master is up when this Fibló loads, it opens following it. It is consumed
    // here whether or not a master was found, so switching to another mode later
    // can never be undone by a second check - there is no second check.
    if (mtsCheckPending)
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
    if (++undoTxnTick >= 60) // 60 Hz timer -> ~1 s, same grouping as before
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
    setTimbreOnParam (true);
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
    setTimbreOnParam (true);
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

    // Same analysis spectrum the generator uses: recompiled from the current
    // settings (message thread, deterministic, no audio-thread interaction).
    const auto tmpl = spectrum::compileSpectrum (currentSettings());
    if (tmpl.partials.empty())
        return out;

    // Anchor the partials at the ACTUAL sounding tonic of the active tuning, so the
    // absolute frequencies (which Sethares roughness depends on) are correct.
    double f0 = tuning.frequencyForNote (60);
    if (! (f0 > 0.0) || ! std::isfinite (f0))
        f0 = TuningManager::kRefFreq * tuning.referenceScale();

    std::vector<dissonance::Partial> spec;
    spec.reserve (tmpl.partials.size());
    for (const auto& p : tmpl.partials)
        spec.push_back ({ (double) p.ratio * f0, (double) p.amp });
    if (spec.size() > 64) // analysis cap only; the SOUND is never capped
    {
        std::sort (spec.begin(), spec.end(),
                   [] (const dissonance::Partial& a, const dissonance::Partial& b) { return a.amp > b.amp; });
        spec.resize (64);
    }

    const int D = juce::jmax (1, getScaleDegrees());
    out.reserve ((size_t) D + 1);
    for (int s = 0; s <= D; ++s) // 0 = tonic (1/1), D = period
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

    // Analyse the spectrum the synth is REALLY producing right now (same prep as the
    // dyad Transmute: absolute frequencies anchored at middle C x the A-reference,
    // strongest-64 analysis cap - analysis only, never the sound).
    const auto tmpl = spectrum::compileSpectrum (currentSettings());
    if (tmpl.partials.empty())
    {
        errorOut = "The current settings produce no partials - nothing to analyse.";
        return false;
    }
    const double f0 = TuningManager::kRefFreq * tuning.referenceScale();
    std::vector<dissonance::Partial> spec;
    spec.reserve (tmpl.partials.size());
    for (const auto& p : tmpl.partials)
        spec.push_back ({ (double) p.ratio * f0, (double) p.amp });
    if (spec.size() > 64)
    {
        std::sort (spec.begin(), spec.end(),
                   [] (const dissonance::Partial& a, const dissonance::Partial& b) { return a.amp > b.amp; });
        spec.resize (64);
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

bool FractalAdditiveAudioProcessor::loadChordAsScale (int index, juce::String& errorOut)
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
    syncBankSelector (bankAutoAdd (name, scl));
    return true;
}

//==============================================================================
// Scale editing on the keyboard (any tuning -> .scl form, then edit + reload).
juce::String FractalAdditiveAudioProcessor::currentTuningAsScl (const juce::String& name) const
{
    const int D = juce::jmax (1, tuning.degreesPerPeriod());
    juce::String scl;
    scl << "! Edited in Fibló Galàctic\n" << name << "\n " << D << "\n!\n";
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
    const auto keepName   = tuning.getScalaName();
    const auto keepSource = tuning.getScalaSource();
    const int  keepMode   = getTuningMode();
    restoreState (tree);
    // Put the tuning back (restoreState may have carried the patch's own).
    if (keepSource.isNotEmpty())
    {
        juce::String err;
        tuning.loadScalaString (keepSource, keepName, err);
    }
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) keepMode));
    return true;
}

bool FractalAdditiveAudioProcessor::addNoteToTuning (double ratioFromTonic, juce::String& errorOut,
                                                     const juce::String& exactText)
{
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
    scl << "! Edited in Fibló Galàctic (note added)\n" << name << "\n " << (int) degs.size() + 1 << "\n!\n";
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
    scl << "! Edited in Fibló Galàctic (degree " << degree << " changed)\n" << name << "\n " << D << "\n!\n";
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
    scl << "! Edited in Fibló Galàctic (degree " << degree << " deleted)\n" << name << "\n " << D - 1 << "\n!\n";
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
    const auto tmpl = spectrum::compileSpectrum (currentSettings());
    // The period the search reasons with: the tuning's own unless the user typed a
    // different one in the box (it decides what counts as an equal-step scale and
    // which intervals are just the same pitch class again).
    const double periodC = 1200.0 * std::log2 (std::max (1.0e-9,
                              periodOverride > 1.0 ? periodOverride : tuning.periodRatio()));
    const double minGap = juce::jlimit (0.0, 600.0, minGapCents);
    // Same absolute anchor as the Degrees list (computeActiveScaleDissonance):
    // Sethares roughness depends on register, so both lists must agree.
    double f0 = tuning.frequencyForNote (60);
    if (! (f0 > 0.0) || ! std::isfinite (f0))
        f0 = TuningManager::kRefFreq * tuning.referenceScale();
    std::vector<dissonance::Partial> spec;
    spec.reserve (tmpl.partials.size());
    for (const auto& p : tmpl.partials) spec.push_back ({ (double) p.ratio * f0, (double) p.amp });
    if (spec.size() > 64)
    {
        std::sort (spec.begin(), spec.end(),
                   [] (const dissonance::Partial& a, const dissonance::Partial& b) { return a.amp > b.amp; });
        spec.resize (64);
    }

    nadBusy = true;
    nadThread = std::make_unique<std::thread> ([this, notes, D, periodC, rootOnly, maxMode, minGap,
                                                ratios = std::move (ratios), spec = std::move (spec)]()
    {
        std::vector<FractalAdditiveAudioProcessor::NadRow> out;

        // Equal-step scale? Then only 0-rooted combinations are enumerated (see
        // below), which also shrinks the search space from C(D,n) to C(D-1,n-1).
        std::vector<double> cents ((size_t) D);
        for (int d = 0; d < D; ++d) cents[(size_t) d] = 1200.0 * std::log2 (std::max (1.0e-9, ratios[(size_t) d]));
        bool regular = D >= 2;
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
                    if (gap < floorC || std::abs (gap - periodC) < 0.5) return true;
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
            nadGeneration.fetch_add (1);
        }
        nadBusy = false;
    });
}

bool FractalAdditiveAudioProcessor::saveCurrentTuningScl (const juce::File& f, juce::String& errorOut)
{
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
    juce::ignoreUnused (samplesPerBlock);
    synth.setCurrentPlaybackSampleRate (sampleRate);
    previewCollector.reset (sampleRate);
    noteGate.prepare (sampleRate);
    modEngine.prepare (sampleRate);
    modNoteOns = 0; // in step with ModEngine::prepare - the first note-on must retrigger
    modHeld = 0;
    for (auto& row : modKeyHeld) row.fill (false);
    lastSr = sampleRate;
    lastGain = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("masterGain")->load());

    // Build once synchronously so a template exists before the first note.
    lastPushed = currentSettings();
    spectrumEngine.buildNow (lastPushed);

    applyTuningMode ((int) apvts.getRawParameterValue ("tuningMode")->load());
    rhythm.prepare (sampleRate);

    // Poll the spectrum parameters and rebuild off the audio thread. 60 Hz (not 30):
    // a modulator driving a parameter that REBUILDS the spectrum (Base N, Harmonics,
    // Slope, Fractal, Prime, Range, Reps) is only sampled here, so a fast envelope
    // used to be caught on some notes and missed on others.
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
        noteGate.prepare (lastSr > 0.0 ? lastSr : getSampleRate());
        modHeld = 0; modNoteOns = 0;
        for (auto& row : modKeyHeld) row.fill (false);
    }

    // Merge on-screen / computer-keyboard notes into the incoming MIDI stream.
    keyboardState.processNextMidiBuffer (midiMessages, 0, nSamp, true);
    // Chord-preview MIDI (editor play buttons): kept SEPARATE - it feeds only the
    // synth, never the rhythm engine (a preview must not trigger pulse trains or
    // leak to the DAW's MIDI out).
    previewMidi.clear();
    previewCollector.removeNextBlockOfMessages (previewMidi, nSamp);

    // Note-start gate (all default off): quantize note starts to the host's beat
    // grid, mono behaviour, and/or quantize to the running pattern's reset.
    {
        const bool qTempo = apvts.getRawParameterValue ("rhythmQTempo")->load() > 0.5f;
        const bool mono   = apvts.getRawParameterValue ("rhythmMono")->load()   > 0.5f;
        const bool qPulse = apvts.getRawParameterValue ("rhythmQPulse")->load() > 0.5f;
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
        rhythm.flushOpenGates();
        rhythm.flushEchoes();
        noteGate.prepare (lastSr > 0.0 ? lastSr : getSampleRate());
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
        juce::MidiBuffer held;
        for (const auto meta : midiMessages)
        {
            auto m = meta.getMessage();
            if (m.isNoteOff())                       { m.setChannel (16); held.addEvent (m, meta.samplePosition); }
            else if (drone && (m.isNoteOn() || m.isPitchWheel() || m.isAftertouch() || m.isChannelPressure()))
                                                     { m.setChannel (16); held.addEvent (m, meta.samplePosition); }
        }
        rhythm.process (midiMessages, nSamp);              // midiMessages := pulse MIDI

        juce::MidiBuffer synthMidi;
        synthMidi.addEvents (midiMessages, 0, nSamp, 0);   // synth plays the pulses...
        synthMidi.addEvents (held, 0, nSamp, 0);           // ...and the drone channel (ch 16)
        synthMidi.addEvents (previewMidi, 0, nSamp, 0);    // chord preview always audible AS a chord
        synth.renderNextBlock (buffer, synthMidi, 0, nSamp);

        if (! midiOut) midiMessages.clear();               // internal-only: don't leak pulses to the DAW
    }
    else if (! previewMidi.isEmpty())
    {
        // Preview active: the synth hears played notes + the preview; the rhythm
        // engine still sees ONLY the played notes.
        juce::MidiBuffer synthMidi;
        synthMidi.addEvents (midiMessages, 0, nSamp, 0);
        synthMidi.addEvents (previewMidi, 0, nSamp, 0);
        synth.renderNextBlock (buffer, synthMidi, 0, nSamp);
        rhythm.process (midiMessages, nSamp);
    }
    else
    {
        synth.renderNextBlock (buffer, midiMessages, 0, nSamp); // held notes sound normally
        // Timbre -> rhythm MIDI out: generated in this very block with exact sample
        // stamps, from the same f0/tuning maths as the voices -> sample-locked to the
        // audio. Consumes the input events; only the pulse MIDI leaves the plugin.
        rhythm.process (midiMessages, nSamp);
    }

    const auto gain = juce::Decibels::decibelsToGain (
        apvts.getRawParameterValue ("masterGain")->load());
    buffer.applyGainRamp (0, nSamp, lastGain, gain);
    lastGain = gain;
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
    if (tuning.getScalaSource().isNotEmpty())
    {
        juce::ValueTree scl ("SCALA");
        scl.setProperty ("name",   tuning.getScalaName(),   nullptr);
        scl.setProperty ("source", tuning.getScalaSource(), nullptr);
        state.appendChild (scl, nullptr);
    }

    state.removeChild (state.getChildWithName ("TIMBRE"), nullptr);
    if (timbreSource.isNotEmpty()) // kept even while switched OFF (On/Off toggle)
    {
        juce::ValueTree tim ("TIMBRE");
        tim.setProperty ("name",   timbreName,   nullptr);
        tim.setProperty ("source", timbreSource, nullptr);
        tim.setProperty ("on",     isTimbreActive(), nullptr);
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
    return state;
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

    // Unmap BEFORE replaceState: replaceState's internal flush must not capture
    // base+mod for the OLD routings into the fresh tree.
    for (int m = 0; m < ModEngine::kMods; ++m) modEngine.unmapAll (m);
    apvts.replaceState (tree);

    // A state saved BEFORE the Harmonics fader existed needs two migrations (the
    // absence of a "harmonics" child is what marks it as old).
    if (! tree.getChildWithProperty ("id", "harmonics").isValid())
    {
        // 1. The "selection" choice (0=All 1=Even 2=Odd 3=Prime) -> the continuous
        //    Harmonics axis (0=Even 1=All 2=Odd 3=Prime).
        if (auto legacy = tree.getChildWithProperty ("id", "selection"); legacy.isValid())
        {
            static const float map[4] = { (float) spectrum::All, (float) spectrum::Even,
                                          (float) spectrum::Odd, (float) spectrum::Prime };
            const int idx = juce::jlimit (0, 3, (int) (double) legacy.getProperty ("value", 0.0));
            if (auto* p = apvts.getParameter ("harmonics"))
                p->setValueNotifyingHost (p->convertTo0to1 (map[idx]));
        }
        // 2. Prime Limit was listed ascending (Off,2,3,5..23); it now runs the other
        //    way (Off,23,19..2) so raising the fader always filters MORE.
        if (auto legacy = tree.getChildWithProperty ("id", "primeLimit"); legacy.isValid())
        {
            static const int oldOrder[10] = { 0, 2, 3, 5, 7, 11, 13, 17, 19, 23 };
            const int oldIdx = juce::jlimit (0, 9, (int) (double) legacy.getProperty ("value", 0.0));
            int newIdx = 0;
            for (int i = 0; i < 10; ++i) if (primeLimitValues[i] == oldOrder[oldIdx]) { newIdx = i; break; }
            if (auto* p = apvts.getParameter ("primeLimit"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) newIdx));
        }
    }

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
            setTimbreOnParam ((bool) tim.getProperty ("on", true));
        }
        else if (src.isNotEmpty()) // typed intervals (Spectrum Builder)
        {
            loadTimbreText (src, err);
            setTimbreOnParam ((bool) tim.getProperty ("on", true) && ! timbreDegrees.empty());
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
        {
            snapshotLoadedPreset (safe);
            return safe;
        }
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

        // The preset's OWN tuning travels IN the file (so it is never lost) but is
        // never applied by loading the preset: only the SOUND changes, never the
        // scale under whatever is already loaded or being played on. It is offered
        // separately - see hasPendingPresetTuning() / loadPendingPresetTuning() -
        // the Tuning panel shows a button for it only when the two actually differ.
        if (auto scl = tree.getChildWithName ("SCALA"); scl.isValid())
        {
            presetPendingSclName   = scl.getProperty ("name").toString();
            presetPendingSclSource = scl.getProperty ("source").toString();
        }
        else { presetPendingSclName.clear(); presetPendingSclSource.clear(); }

        const auto keepName   = tuning.getScalaName();
        const auto keepSource = tuning.getScalaSource();
        const int  keepMode   = getTuningMode();

        restoreState (tree);

        // Put the CURRENTLY ACTIVE tuning back exactly as it was.
        if (keepSource.isNotEmpty())
        {
            juce::String err;
            tuning.loadScalaString (keepSource, keepName, err);
        }
        else
            tuning.clearScala();
        if (auto* p = apvts.getParameter ("tuningMode"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) keepMode));
        applyTuningMode (keepMode);
        snapshotLoadedPreset (name); // AFTER the tuning revert - that is the real final state
        return true;
    }
    return false;
}

// The Tuning panel's "Load preset's tuning" button.
bool FractalAdditiveAudioProcessor::loadPendingPresetTuning (juce::String& errorOut)
{
    if (presetPendingSclSource.isEmpty())
    {
        errorOut = "This preset carries no tuning of its own.";
        return false;
    }
    if (! tuning.loadScalaString (presetPendingSclSource, presetPendingSclName, errorOut))
        return false;
    if (auto* p = apvts.getParameter ("tuningMode"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
    applyTuningMode ((int) TuningManager::Mode::Scala);
    clearSegmentSource();
    syncBankSelector (bankAutoAdd (presetPendingSclName, presetPendingSclSource));
    return true;
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FractalAdditiveAudioProcessor();
}
