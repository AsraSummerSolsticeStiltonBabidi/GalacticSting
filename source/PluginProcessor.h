#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <set>
#include <thread>
#include <vector>

#include "dsp/AdditiveVoice.h"
#include "dsp/Dissonance.h"
#include "dsp/LookaheadDelay.h"
#include "dsp/ModEngine.h"
#include "dsp/MonoGlide.h"
#include "dsp/NoteGate.h"
#include "dsp/PresetMorph.h"
#include "dsp/RhythmEngine.h"
#include "dsp/ShepardEngine.h"
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
    // HOST RESET: "stop making sound now, and forget what you were doing".
    // A bypass, a panic button and a transport re-arm all arrive here, and until this
    // was written it only flushed the rhythm-MIDI gates - so a note the player was
    // holding went on sounding for ever through a bypass, and a panic silenced nothing
    // at all. It owes both halves: silence, AND a synth that is not left deaf, so the
    // very next note must still play normally.
    //
    // The work itself is NOT done here. reset() can be called from the message thread
    // while the audio thread is inside processBlock, and voice / NoteGate / MonoGlide /
    // lookahead-ring state is only ever allowed to be touched from one of them - the
    // same rule restoreState() already follows through pendingHardSilence. So this only
    // RAISES the request; the next block carries it out, one block being the shortest
    // honest latency for "silence" from a thread that does not own the voices.
    void reset() override
    {
        pendingHardSilence.store (true, std::memory_order_release);
        pendingHostReset .store (true, std::memory_order_release);
    }
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
    // ...and whether that count is an ANSWER or still its initial value - see
    // SpectrumEngine::reportRendered. The meter must not print "0 partials audible" as a
    // diagnosis when the honest statement is "nothing has been played yet".
    bool hasSoundedYet() const noexcept   { return spectrumEngine.hasEverRendered(); }
    int  getTemplatePartialCount() const noexcept { return spectrumEngine.getPartialCount(); }
    int  getRawPartialCount() const noexcept { return spectrumEngine.getRawCount(); }
    bool isSpectrumTruncated() const noexcept { return spectrumEngine.isTruncated(); }
    // The SHEPARD's own version of the same honesty: while the band's bottom edge is
    // travelling up to a density change, the deepest copies want more partials than one
    // kStride slot holds and the rest are not sounding. Kept separate from the engine's
    // flag (that one describes the TEMPLATE and is republished on every compile) and
    // OR-ed into the same meter, so the one cap left in the Shepard is never silent.
    bool isShepardSlotCapped() const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<const AdditiveVoice*> (synth.getVoice (v)))
                if (av->shepardSlotCapped()) return true;
        return false;
    }

    //==========================================================================
    // Microtuning (12-TET / MTS-ESP / Scala). Called from the editor.
    bool loadScalaFile (const juce::File& f, juce::String& errorOut);
    // Load a .scl straight from text (used by the tuning bank, the generators and
    // the verifiers). Switches the tuning mode to Scala on success.
    bool loadScalaText (const juce::String& scl, const juce::String& name, juce::String& errorOut)
    {
        if (! tuning.loadScalaString (scl, name, errorOut)) return false;
        if (auto* p = apvts.getParameter ("tuningMode"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) TuningManager::Mode::Scala));
        tuning.setMode (TuningManager::Mode::Scala);
        return true;
    }
    // Write the CURRENT tuning as an AnaMark .tun (v2). The text itself is built by
    // TuningManager::writeTunText - it belongs beside the scale it describes, and only
    // there can a unit test load a scale, export it and load the export back.
    bool saveCurrentTuningTun (const juce::File& f, juce::String& errorOut)
    {
        juce::String text;
        if (! tuning.writeTunText (text, errorOut)) return false;
        if (! f.replaceWithText (text)) { errorOut = "Could not write " + f.getFullPathName(); return false; }
        return true;
    }


    void clearScala();
    juce::String getTuningStatus() const { return tuning.getStatusText(); }

    // Run one parameter-poll tick by hand. The poll normally rides the message
    // timer, which needs a running message loop - the offline verifiers have none,
    // so without this they would silently test a plugin whose tuning never picked
    // up Reference A / Octave Transpose / the tuning mode at all.
    void pollParametersForTests() { timerCallback(); }
    // Is the one-shot MTS-ESP adoption still armed? It must stay armed on a brand-new
    // instance and be disarmed the moment the host hands over ANY saved state, so that
    // reopening a project never overwrites the tuning the user saved. Tests only - the
    // adoption itself cannot be exercised offline because it needs a live MTS master.
    bool mtsAdoptionArmedForTests() const noexcept { return mtsCheckPending; }

    // Raw tuning/timbre state so the editor can format localised status strings.
    int  getTuningMode()    const noexcept { return (int) apvts.getRawParameterValue ("tuningMode")->load(); }
    bool tuningHasMaster()  const noexcept { return tuning.hasMtsMaster(); }
    bool tuningHasScala()   const noexcept { return tuning.hasScala(); }
    juce::String tuningScaleName() const { return tuning.currentScaleName(); }

    // Absolute 128-key table (a non-repeating .tun): the pitches are the file's own and
    // the PERIOD is separate metadata, so the Tuning panel offers it as an editable
    // field marked as Fibló's estimate. Nothing here can move a pitch.
    bool tuningIsAbsoluteTable() const noexcept { return tuning.usesAbsoluteTable(); }
    // ...and the same question asked about WHAT IS SOUNDING. usesAbsoluteTable() answers
    // for the loaded SOURCE and knows nothing about the mode, which is right for it -
    // TuningManager::restoreTuning asks it while putting a tuning back, BEFORE the mode
    // parameter has been restored, so a mode test inside the accessor would break state
    // restore. The guard therefore belongs with the consumers, and this is it.
    //
    // What it cost to be missing: load a non-repeating .tun once, switch the Tuning box
    // back to 12-TET, and the instrument was in plain 12-tone equal temperament while
    // ".scl export" refused with "this tuning is a 128-key table" and right-click degree
    // editing refused the same way. Both were describing a tuning that had not made a
    // sound since the user changed the mode. Everything that REFUSES, REWRITES or EXPORTS
    // "the tuning" asks this one; the panel's period FIELD still asks the sibling above,
    // because that field describes the loaded file rather than the sounding pitches.
    bool tuningTableIsInForce() const noexcept
    {
        return getTuningMode() == (int) TuningManager::Mode::Scala && tuning.usesAbsoluteTable();
    }
    juce::String tuningTablePeriodSpec() const { return tuning.tablePeriodSpec(); }
    bool   tuningTablePeriodIsUserSet() const noexcept { return tuning.tablePeriodIsUserSet(); }
    double tuningTablePeriodDevCents()  const noexcept { return tuning.tablePeriodDevCents(); }
    bool   tuningTablePeriodWasFound()  const { return tuning.getTablePeriodEstimate().found; }
    bool setTuningTablePeriod (const juce::String& spec, juce::String& errorOut)
    { return tuning.setTablePeriodSpec (spec, errorOut); }
    void resetTuningTablePeriod() { tuning.resetTablePeriodToEstimate(); }
    juce::String getTimbreName()   const { return timbreName; }
    // The exact text a typed Spectrum-Builder spectrum was entered as. It is saved in
    // the state, but the box that edits it had no way to read it back, so a reopened
    // window showed an empty field and the spectrum could only be edited by retyping
    // it from memory.
    juce::String getTimbreSource() const { return timbreSource; }

    // The custom-tuning text is not an APVTS parameter, so nothing ever put it on the
    // undo stack: Ctrl+Z sailed straight past it to whatever fader was touched before,
    // and a typed scale could not be taken back. This wraps the change in an
    // UndoableAction so it behaves like every other control.
    struct TimbreTextAction : public juce::UndoableAction
    {
        TimbreTextAction (FractalAdditiveAudioProcessor& p, juce::String prev, juce::String next)
            : proc (p), after (std::move (next)), before (std::move (prev)),
              // The PREVIOUS spectrum's tuning provenance, captured here (construction
              // happens before loadTimbreText wipes it): undoing a typed spectrum back
              // to an adapted one must restore the "Load spectrum's tuning" button too,
              // or Ctrl+Z would bring the spectrum back with its origin quietly gone.
              beforeProvName (p.specProvName), beforeProvSource (p.specProvSource),
              beforeProvPeriod (p.specProvPeriod) {}
        bool perform() override { return applyOne (after, false); }
        bool undo() override    { return applyOne (before, true); }
        int  getSizeInUnits() override { return (int) (after.length() + before.length()) * 2 + 32; }
        bool applyOne (const juce::String& t, bool restoreProv)
        {
            juce::String e;
            bool ok = true;
            if (t.trim().isEmpty()) proc.clearTimbre();
            else                    ok = proc.loadTimbreText (t, e);
            // loadTimbreText/clearTimbre both clear provenance (typed text has none);
            // going BACK re-attaches the origin the previous spectrum really had.
            if (ok && restoreProv)
            {
                proc.specProvName   = beforeProvName;
                proc.specProvSource = beforeProvSource;
                proc.specProvPeriod = beforeProvPeriod;
            }
            return ok;
        }
        FractalAdditiveAudioProcessor& proc;
        juce::String after, before;
        juce::String beforeProvName, beforeProvSource, beforeProvPeriod;
    };

    // Type a custom tuning THROUGH the undo stack. Returns false (with errorOut set) if
    // the text does not parse, in which case nothing is pushed.
    bool loadTimbreTextUndoable (const juce::String& text, juce::String& errorOut)
    {
        // The ACTION is constructed BEFORE applying anything: its constructor captures
        // the previous text's provenance, and loadTimbreText below clears that (typed
        // text has none) - constructing afterwards recorded an empty origin as the one
        // to go back to, so undoing a typed spectrum back to an adapted one silently
        // lost its "Load spectrum's tuning" button (caught by RhythmVerify b2). The
        // same order protects `before` = timbreSource, which the load overwrites too.
        // RULE 7: TimbreTextAction has PartialGainsAction's shape - it always writes the
        // LIVE spectrum - so a copy of it minted while a corner is being edited is the
        // same data-loss bug with a typed scale instead of a drawn one. The corner picks
        // the text up from pullPlaygroundCornerEdit like every other edit in a session.
        if (pgEditCorner >= 0) return loadTimbreText (text, errorOut);
        std::unique_ptr<TimbreTextAction> act (new TimbreTextAction (*this, timbreSource, text));
        if (! loadTimbreText (text, errorOut)) return false;   // rejected: nothing is pushed
        undoManager.beginNewTransaction();
        undoManager.perform (act.release());
        return true;
    }
    int  getTimbreNoteCount() const noexcept { return (int) timbreDegrees.size(); }

    // Drop the loaded custom spectrum THROUGH the undo stack, so the Remove button is
    // as undoable as everything else. clearTimbre() on its own is invisible to Ctrl+Z:
    // the spectrum is not an APVTS parameter, so a plain clear destroyed a hand-typed
    // scale with no way back. Reuses TimbreTextAction, whose empty text IS a clear.
    void clearTimbreUndoable()
    {
        if (timbreSource.isEmpty() && timbreDegrees.empty()) return;
        if (pgEditCorner >= 0) { clearTimbre(); return; }   // rule 7, as above
        const auto before = timbreSource;
        undoManager.beginNewTransaction();
        undoManager.perform (new TimbreTextAction (*this, before, juce::String()));
    }

    //== RANDOM SPECTRUM (the "specRandom" trigger parameter) ====================
    // Press it from the UI: a real 0 -> 1 parameter step, so the host records it as
    // automation and replaying that automation fires exactly the same way a click does.
    // The 0 is written first because the level may already BE 1 (the previous press
    // left it there): without it a second click would be no edge at all, and the button
    // would work exactly once per session.
    void pressRandomSpectrum()
    {
        if (auto* p = apvts.getParameter ("specRandom"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (0.0f);
            p->setValueNotifyingHost (1.0f);
            p->endChangeGesture();
        }
    }
    // Consume a pending rising edge and build the spectrum. Called by the 60 Hz timer
    // (message thread - loadTimbreText parses and republishes, neither of which may run
    // on the audio thread), and directly by the verifier, which has no message loop.
    // Returns true if a spectrum was actually generated.
    bool pollRandomSpectrumTrigger();
    // How many random spectra this instance has generated. The one thing a test can
    // count to prove "the rising edge fired once, and a held level does not re-fire".
    int  getRandomSpectrumCount() const noexcept { return randomSpectrumCount; }
    // The interval text a random press would produce. Public so the generator's
    // distribution can be measured directly (spacing, count, span) instead of inferred
    // from the partials that survive compilation.
    juce::String makeRandomSpectrumText();

    // Degrees per period of the active tuning (for the colour keyboard).
    // The MTS master's own keyboard-mapping report, straight through (see
    // TuningManager::mtsMapInfo for why it exists and what -1 means).
    TuningManager::MtsMapInfo mtsMapInfo() const { return tuning.mtsMapInfo(); }
    int getScaleDegrees() const noexcept { return tuning.degreesPerPeriod(); }
    // How many EQUAL steps the active tuning cuts the OCTAVE into, or 0 when it does not
    // cut the octave into equal steps at all. The keyboard's note-name option hangs off
    // this - see TuningManager::equalDivisionsOfOctave. Message thread.
    int getEqualDivisionsOfOctave() const { return tuning.equalDivisionsOfOctave(); }

    // True when EVERY degree sits one equal step from the last (any N-EDO, or any
    // scale that happens to be equal-stepped). In such a tuning a chord shape has
    // the SAME intervals wherever you start it, so a result found on degree 0 is
    // valid from every degree - which is what the n-ad list shows with "any".
    // Same 0.5-cent criterion the n-ad search itself uses to root at 0.
    bool tuningIsEqualStep() const
    {
        const int D = tuning.degreesPerPeriod();
        if (D < 2) return false;
        // NO STATED PERIOD, NO EQUAL STEPS. "Equal-stepped" here means "the scale
        // repeats, and every step of the repeat is the same size" - the second half is
        // meaningless without the first. Asking periodRatio() would have measured the
        // steps against a 2/1 nobody wrote, and a descending .scl whose steps happen to
        // divide an octave evenly would have been declared rootless: the n-ad list then
        // prints "any +10 +18", telling the user a shape works from every degree of a
        // scale that does not come back to itself at all.
        if (! tuning.hasUsablePeriod()) return false;
        const double periodC = 1200.0 * std::log2 (std::max (1.0e-9, tuning.statedPeriodRatio()));
        const double step    = periodC / (double) D;
        for (int d = 0; d < D; ++d)
        {
            const double c = 1200.0 * std::log2 (std::max (1.0e-9, getDegreeRatio (d)));
            if (std::abs (c - step * (double) d) > 0.5) return false;
        }
        return true;
    }

    // The FULL playable MIDI range the colour keyboard should offer, adapted to the
    // active tuning so wide-period scales only expose keys in the musical band
    // [loHz, hiHz]. The keyboard itself shows a readable-width WINDOW of this and
    // scrolls (arrows) through the rest, so dense scales (31-EDO...) keep big keys
    // while still reaching the whole audible span.
    void getKeyboardKeyRange (int& lowKey, int& numKeys) const noexcept
    {
        constexpr double loHz = 40.0, hiHz = 4186.0; // ~E1 .. ~C8
        constexpr int tonic = 60, minKeys = 8;
        int lo = tonic, hi = tonic;
        while (lo > 0   && tuning.frequencyForNote (lo - 1) >= loHz) --lo;
        while (hi < 127 && tuning.frequencyForNote (hi + 1) <= hiHz) ++hi;
        if (hi - lo + 1 < minKeys) // ultra-wide period: still show a few keys
        {
            lo = juce::jlimit (0, 127 - minKeys + 1, tonic - minKeys / 2);
            hi = lo + minKeys - 1;
        }
        lowKey  = lo;
        numKeys = hi - lo + 1;
    }

    // The ACTIVE tuning's repeat interval (2/1 in octave tunings, 3/1 under
    // Bohlen-Pierce). The editor asks so the Shepard's rise-interval choice can exist
    // only when it is really a choice - see shepard::periodDiffersFromOctave.
    double getTuningPeriodRatio() const noexcept { return tuning.periodRatio(); }

    // ...and the SAME question asked honestly: the period the active tuning really
    // STATES, or 0.0 when it states none that ascends. Anything that DESCRIBES the
    // tuning to the user has to ask this one, for the same reason the sound path does:
    // getTuningPeriodRatio() answers 2.0 for a descending .scl and for a degenerate
    // 1/1 scale, and a read-out built on that number prints an octave the user's file
    // never contained. Naming it after the question ("does this tuning state a period")
    // rather than after temper, because temper is no longer the only caller.
    double getStatedTuningPeriodRatio() const noexcept { return tuning.statedPeriodRatio(); }
    bool   tuningStatesPeriod() const noexcept         { return tuning.hasUsablePeriod(); }

    // Can "Temper to tuning" do anything at all? Temper snaps partials onto
    // "degree x period^k", which needs a period that ASCENDS - a descending .scl or a
    // degenerate period-1 scale states none. The editor asks so the toggle can be
    // greyed WITH A REASON instead of appearing to work while the spectrum is silently
    // snapped to a 2/1 grid the user's file never contained.
    bool temperIsPossible() const noexcept { return tuningStatesPeriod(); }

    // Frequency ratio of scale degree `d` relative to degree 0 (the tonic on
    // key 60), for the keyboard's just-interval labels. Degree d is mapped to an
    // IN-RANGE key of the same degree class (walking down whole periods) rather
    // than clamping the MIDI note, so dense scales (72-EDO etc.) label correctly.
    // Moved into TuningManager (see TuningManager::degreeRatio) so the .tun exporter and
    // every readout mean the same thing by "degree d" - the exporter used to reach this
    // through the processor, which is why nothing cheap could test what it wrote.
    double getDegreeRatio (int d) const { return tuning.degreeRatio (d); }

    // Genuine JI test BY SOURCE: is scale degree `d` (0..getScaleDegrees(), where the
    // top index is the period) a rational-sourced interval, and if so its exact p/q?
    //  - Scala mode (incl. segment + dissonance scales built as .scl): from the .scl's
    //    per-degree source, so a ratio degree "3/2" is genuine but a cents degree
    //    "701.955" is NOT (it only approximates one).
    //  - Any other mode (equal divisions, MTS, MPE): only the unison and an exact 2/1
    //    octave are rational; every tempered step is shown as cents (+ "≈p/q *").
    // The one exact cents interval, 1200c = 2/1, is the period case handled here.
    //  - Absolute-table mode: the file states cents per KEY and no degrees at all, so
    //    only the unison is rational. It falls through to the generic branch, whose 2/1
    //    test is real work here: a stretched table's "octave" is NOT 2/1 and must not be
    //    labelled as one.
    bool getDegreeIsRational (int d, long long& p, long long& q) const
    {
        const int D = juce::jmax (1, tuning.degreesPerPeriod());
        if (getTuningMode() == (int) TuningManager::Mode::Scala && ! tuningTableIsInForce())
            return tuning.scalaDegreeRational (d, p, q);
        if (d <= 0) { p = 1; q = 1; return true; }        // unison
        if (d >= D)                                        // the period/octave
        {
            const double r = getDegreeRatio (d);
            if (r > 0.0 && std::abs (1200.0 * std::log2 (r) - 1200.0) < 0.05) { p = 2; q = 1; return true; }
        }
        return false;                                      // tempered step
    }

    //==========================================================================
    // Scala-as-timbre: a loaded .scl becomes the partial set instead of the
    // harmonic engine. Called from the editor.
    bool loadTimbreFile (const juce::File& f, juce::String& errorOut);
    // Build the timbre spectrum from typed intervals (cents "701.955c" / "700.0",
    // ratios "3/2", or integers "3"), space/comma/newline separated. The highest
    // interval becomes the period (the Reps repeat point).
    bool loadTimbreText (const juce::String& text, juce::String& errorOut);
    // The same grammar, parsed WITHOUT touching the live Custom Engine - see the .cpp.
    static bool parseTimbreIntervals (const juce::String& text, std::vector<float>& degOut,
                                      float& periodOut, juce::String& errorOut);
    void clearTimbre();
    // The Custom Engine's on/off is a real PARAMETER, so it is automatable and -
    // the reason it moved here - Ctrl+Z undoes it like every other control. It only
    // counts as on while a scale is actually loaded.
    bool isTimbreActive() const noexcept
    { return ! timbreDegrees.empty() && apvts.getRawParameterValue ("timbreOn")->load() > 0.5f; }

    // Recently-used .scl files (persisted across sessions, newest first, max 3).
    juce::StringArray getRecentSclPaths() const { return recentSclPaths; }

    //== USER PREFERENCES, not patch state =====================================
    // "quiero que ... el sinte se acuerde de si el usuario la activo o no y que se abra
    // ya con ese setting al re-abrirse."
    //
    // Note names on the keyboard are a PREFERENCE about how this person reads a
    // keyboard, not a property of the sound. They therefore live in the same global
    // settings file as the recent-.scl list (appProps) and NOT in getStateInformation:
    // patch state travels with the project and with every preset, so a preference stored
    // there would flip back the moment someone loaded a preset saved before they turned
    // it on - which is exactly the "it forgot again" the user asked twice to be rid of.
    // Stored globally, it survives closing the plugin, the host, and the machine, and it
    // is the same in every project and every instance.
    bool getKeyboardNoteNames() const noexcept { return keyboardNoteNames; }
    void setKeyboardNoteNames (bool on)
    {
        keyboardNoteNames = on;
        if (appProps != nullptr)
        {
            appProps->setValue ("keyboardNoteNames", on);
            appProps->saveIfNeeded();   // written NOW: a host that is killed rather than
        }                               // closed never runs a destructor to flush it
    }

    //==========================================================================
    // Timbre -> rhythm MIDI out (see RhythmEngine.h). The editor edits the slots
    // directly (atomics) and reads the live pulse rates for its panel.
    RhythmEngine& getRhythm() noexcept { return rhythm; }

    //==========================================================================
    // SHEPARD MODE (Tuning panel). There is no separate engine any more: the mode is
    // rendered inside the VOICES, from the key that was pressed and from the synth's
    // own spectrum (see AdditiveVoice::planShepard). Only one fact still belongs to
    // the processor - the transport's, below.
    //
    // TESTS ONLY: the Shepard copies of the first SOUNDING main-engine voice - grid
    // index, ROOT frequency (the transposed played pitch each copy's spectrum is
    // built on), window amplitude and partial count. Any pointer may be null. This is
    // how run 46 proves the stack is anchored on the key that was pressed.
    int voiceShepardForTests (long long* ms, double* roots, double* wins, int* counts, int maxN)
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive())
                    return av->shepProbeForTests (ms, roots, wins, counts, maxN);
        return 0;
    }
    // The rise interval the first sounding voice is ACTUALLY using, as a ratio.
    double voiceShepardRiseRatioForTests()
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive())
                    return av->shepRiseRatioForTests();
        return 0.0;
    }
    // TESTS ONLY: the worst-case evidence for the Shepard's three stated invariants
    // (see AdditiveVoice::shepEvidenceForTests). Reset covers EVERY voice, because the
    // note under test may be a stolen one and a per-voice reset would measure a fresh
    // voice while the evidence sat on the one that was actually playing.
    void shepardEvidenceResetForTests()
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                av->shepEvidenceResetForTests();
    }
    // ...and the worst case across every voice, for the same reason.
    void shepardEvidenceForTests (double& claimW, double& claimX, double& dropW, double& loStep,
                                  int& planned, int& dropped, int& minCopies, int& blocks,
                                  int& cutBlocks, int& cutWorst)
    {
        claimW = claimX = dropW = loStep = 0.0; planned = dropped = blocks = 0; minCopies = 0;
        cutBlocks = cutWorst = 0;
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
            {
                double cw, cx, dw, ls; int pl, dr, mc, bl, cb, cwo;
                av->shepEvidenceForTests (cw, cx, dw, ls, pl, dr, mc, bl, cb, cwo);
                if (bl == 0) continue;
                if (cw > claimW) { claimW = cw; claimX = cx; }
                dropW = std::max (dropW, dw); loStep = std::max (loStep, ls);
                planned = std::max (planned, pl); dropped += dr; blocks += bl;
                cutBlocks += cb; cutWorst = std::max (cutWorst, cwo);
                minCopies = (minCopies == 0) ? mc : std::min (minCopies, mc);
            }
    }
    // TRUE while the rate control is in Sync mode but the host offers no BPM, so
    // the FREE (ms) value is what actually drives the rise. The editor shows this
    // state instead of letting the beats box claim a sync that is not happening.
    bool shepardSyncHasNoBpm() const noexcept { return shepSyncNoBpm.load (std::memory_order_relaxed); }
    const spectrum::SpectrumTemplate* getLiveTemplate() const noexcept { return spectrumEngine.getLive(); }
    // The phase plan measured on that template, and the report that says how it did.
    // Exposed so the offline verifiers can hold the engine to its own guarantee - a plan
    // that is never worse than the hash is a claim, and a claim nobody measures is a
    // comment. Not used by the audio path yet: this publishes and checks the plan, the
    // voice consuming it is the next step.
    const spectrum::phaseset::PhasePlan* getLivePhasePlan() const noexcept
    { return spectrumEngine.getLivePlan(); }
    const spectrum::phaseset::PhaseReport* getLivePhaseReport() const noexcept
    { return spectrumEngine.getLiveReport(); }

    //== MONOPHONIC GLIDE probes (verifiers only, single-threaded harnesses) ====
    // The size of the pool, how many of it are sounding, and the pitch the sounding
    // voice ended its last block on. Run 48 checks all three: the pool being ONE is
    // the "no chords" promise, the active count is what a second simultaneous key must
    // not increase, and the pitch is the internal counterpart of the audio's own f0
    // probe (a disagreement between them names which half broke).
    int synthVoiceCountForTests() const noexcept { return synth.getNumVoices(); }
    // One MIX sub-engine's pool size and how many of its voices are sounding. The pair is
    // what proves the shrink FADES rather than deletes: a fade leaves the pool at its old
    // size for a tick or two while the doomed voices go quiet, so `pool` may exceed the
    // requested count while `active` falls. A delete moves both in the same instant, and
    // that is the difference no audio metric could see - a dense spectrum's own envelope
    // swings further in 1.3 ms than six of eight sub-engine voices are worth.
    int mixVoicePoolForTests (int corner) const noexcept
    {
        if (corner < 0 || corner >= kPlaygroundCorners || mixEng[(size_t) corner] == nullptr) return -1;
        return mixEng[(size_t) corner]->synth.getNumVoices();
    }
    int mixVoicesActiveForTests (int corner) const noexcept
    {
        if (corner < 0 || corner >= kPlaygroundCorners || mixEng[(size_t) corner] == nullptr) return -1;
        auto& s = mixEng[(size_t) corner]->synth;
        int n = 0;
        for (int v = 0; v < s.getNumVoices(); ++v)
            if (auto* mv = s.getVoice (v); mv != nullptr && mv->isActive()) ++n;
        return n;
    }
    int activeVoiceCountForTests() const noexcept
    {
        int c = 0;
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (synth.getVoice (v)->isActive()) ++c;
        return c;
    }
    double voiceSoundingHzForTests() const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) return av->soundingHzForTests();
        return 0.0;
    }
    // Is the sounding voice still in Shepard mode, and is a portamento still running on
    // it? Run 48 uses the pair to prove a takeover does not interrupt a rising Shepard:
    // its stack is a played mode latched per NOTE, and a legato takeover is one note.
    bool voiceShepardActiveForTests() const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) return av->shepActiveForTests();
        return false;
    }
    bool voiceGlidingForTests() const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) return av->glidingForTests();
        return false;
    }
    double voiceMorphWeightForTests() const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) return av->morphWeightForTests();
        return 0.0;
    }
    //== PHASE FADER probes (verifiers only) ===================================
    // Where the SOUNDING phases sit on the Phase fader, and what those phases are.
    // The pair is how run 53 separates the two halves of the reported symptom: the
    // level is a function of the first, and the second is the thing that has to have
    // moved before the first is allowed to.
    double voiceSoundingScatterForTests() const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) return av->soundingScatterForTests();
        return -1.0;
    }
    int voicePhasesForTests (double* out, int maxN, int base = 0) const noexcept
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) return av->phasesForTests (out, maxN, base);
        return 0;
    }
    // The worst detune a live re-phase actually applied, in cents of the partial it was
    // applied to. The whole answer to "a single transition time cannot be inaudible
    // across the whole bank" is a measured number, and this is where it comes from.
    double voiceRephaseWorstCentsForTests() const noexcept
    {
        double w = 0.0;
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive()) w = std::max (w, av->rephaseWorstCentsForTests());
        return w;
    }
    // Voices in one playground MIX sub-engine (-1 when that corner has no engine).
    // Declared here, defined after kPlaygroundCorners exists.
    int mixVoiceCountForTests (int corner) const noexcept;
    // Every swap of the live template, gains-only republishes included (see
    // SpectrumEngine::getLiveRevision). Read by run 42 to count what the ear gets.
    juce::uint32 getSpectrumLiveRevision() const noexcept { return spectrumEngine.getLiveRevision(); }

    // Per-partial amplitude edits. The authoritative copy lives HERE, because it has to
    // be saved with the project; the spectrum engine only ever holds a resolved
    // index-aligned copy for the voices. Message thread.
    // `republish == false` stores the edits without swapping the live template - only for
    // a caller that publishes its own template in the same call (see
    // SpectrumEngine::setOverrides and applySpectrumAdapt).
    void setPartialGains (std::vector<spectrum::PartialGain> ov, bool on, bool republish = true)
    {
        partialGains = ov;
        ++partialGainsRev;
        spectrumEngine.setOverrides (std::move (ov), on, republish);
    }
    const std::vector<spectrum::PartialGain>& getPartialGains() const noexcept { return partialGains; }
    // Bumped on every write. The editor window has to notice edits that did NOT come
    // from its own knobs - an undo, a preset, a project reload - and it cannot watch the
    // template generation for that: setOverrides deliberately leaves the generation
    // alone (a bump there would restart every voice's spectral morph mid-drag). Without
    // this counter an undo left the window showing the values it had just undone.
    unsigned getPartialGainsRevision() const noexcept { return partialGainsRev; }

    // The per-partial edits are not APVTS parameters, so nothing put them on the undo
    // stack: Ctrl+Z after drawing a spectrum by hand sailed past it to whatever fader
    // was touched before, and a hand-drawn spectrum could not be taken back. One
    // transaction per drag gesture (the editor commits on mouse-up), not per frame.
    struct PartialGainsAction : public juce::UndoableAction
    {
        PartialGainsAction (FractalAdditiveAudioProcessor& p,
                            std::vector<spectrum::PartialGain> prev,
                            std::vector<spectrum::PartialGain> next)
            : proc (p), after (std::move (next)), before (std::move (prev)) {}
        bool perform() override { return applyOne (after); }
        bool undo() override    { return applyOne (before); }
        int  getSizeInUnits() override
        { return (int) ((after.size() + before.size()) * sizeof (spectrum::PartialGain)) + 32; }
        bool applyOne (const std::vector<spectrum::PartialGain>& g)
        {
            proc.setPartialGains (g, proc.apvts.getRawParameterValue ("partialManual")->load() > 0.5f);
            return true;
        }
        FractalAdditiveAudioProcessor& proc;
        std::vector<spectrum::PartialGain> after, before;
    };

    // Commit one gesture's worth of edits THROUGH the undo stack. `before` is the list
    // as it stood when the gesture started - captured by the caller, because by the time
    // the mouse comes up the live list is already the new one.
    //
    // `startNewTransaction == false` is for the gesture that ALSO armed manual mode
    // (armManualPartialAmpsForEdit below): that call already opened the transaction, and
    // opening a second one here would split one user action - "I dragged a partial" -
    // into two undo steps, the second of which leaves manual switched on with no edits.
    void commitPartialGains (std::vector<spectrum::PartialGain> before,
                             std::vector<spectrum::PartialGain> after,
                             bool startNewTransaction = true)
    {
        if (before == after) return;                 // a click that moved nothing
        // RULE 7: while a corner is being edited the pencil is drawing THAT CORNER, not
        // the live patch - but this action always writes the LIVE gains, so a copy of it
        // left on the shared stack is what wiped a hand-drawn spectrum one Ctrl+Z after
        // leaving edit mode (gains=1 -> gains=0, and redo then installed the corner's
        // drawing). The edit still reaches the corner: pullPlaygroundCornerEdit reads
        // getPartialGains() every tick and the corner's own rev carries it.
        if (pgEditCorner >= 0)
        {
            setPartialGains (std::move (after),
                             apvts.getRawParameterValue ("partialManual")->load() > 0.5f);
            return;
        }
        if (startNewTransaction) undoManager.beginNewTransaction();
        undoManager.perform (new PartialGainsAction (*this, std::move (before), std::move (after)));
    }

    //== UNDO / REDO, THROUGH THE PROCESSOR ======================================
    // The editor's Ctrl+Z used to call undoManager.undo() straight. Inside a corner-edit
    // session that unwound the LIVE PATCH's history into the scratch surface, and the
    // next pull absorbed the result into the corner and stamped it with the "*" - so the
    // honesty mark blamed the user for a change they never made. Measured inside the
    // first second of a session: "fast undo: surface 4000 (starred yes); corner B now
    // reads 4000 (saved 9000)". UndoManager::undo() unwinds the currently-open
    // transaction too, so there is no grace period to hide behind.
    //
    // There is no per-corner history to walk instead, so the honest answer is to refuse.
    // Returns false when nothing was done, so a caller can say so.
    bool performUndo()
    {
        if (pgEditCorner >= 0) return false;   // a corner has no history of its own
        return undoManager.undo();
    }
    bool performRedo()
    {
        if (pgEditCorner >= 0) return false;
        return undoManager.redo();
    }
    // How many undo STEPS are on the stack. The unit a test has to count when the claim
    // is "one gesture is one Ctrl+Z" - the actions inside a step are not the question,
    // how many times the user has to press Ctrl+Z is.
    int undoStackDepthForTests() const { return undoManager.getUndoDescriptions().size(); }

    //== ONE GESTURE IS ONE UNDO STEP, EVEN WHEN IT IS SLOW =======================
    // The 60 Hz timer below cuts parameter edits into ~1-second transactions so a slider
    // drag undoes as one step instead of hundreds of micro-moves. A PENCIL drag is also
    // one user action, and it can easily last longer than a second - so that cut landed
    // INSIDE it and split it in two.
    //
    // The half that hurts is the first pencil drag of a session, the one that arms manual
    // mode: armManualPartialAmpsForEdit() opens the transaction and the flip reaches the
    // undo stack on the APVTS's own 20-500 ms timer, i.e. early in the drag, while
    // commitPartialGains lands at mouse-up. With a cut between them, step 2 is the
    // amplitudes and step 1 is the flip, so ONE Ctrl+Z takes back the edits and leaves
    // manual mode ARMED WITH NO EDITS - exactly the state f40623d exists to forbid, and
    // the state in which SLOPE is frozen for a spectrum nobody drew.
    //
    // So the cut is held off while a gesture is open. It is a COUNT, not a flag, because
    // nothing forbids a second grid opening one, and it is released by the panel's
    // destructor as well as by mouse-up: a mouse-up is not guaranteed if the window dies
    // mid-drag, and a stuck hold would silently restore the unbounded-ActionSet bug the
    // cut was added to prevent.
    void beginUndoGesture() noexcept { ++undoGestureDepth; undoTxnTick = 0; }
    void endUndoGesture()   noexcept
    {
        if (undoGestureDepth > 0) --undoGestureDepth;
        undoTxnTick = 0;   // the next cut is a full second AFTER the gesture, not mid-flight
    }
    int  undoGestureDepthForTests() const noexcept { return undoGestureDepth; }

    // THE FIRST PENCIL EDIT OF A SESSION SWITCHES MANUAL MODE ON, ITSELF.
    //
    // "partialManual" ships OFF, and while it is off SpectrumEngine::fillGains throws the
    // resolved edits away (`t.gain.clear()`), so the voice's amplitude loop is skipped
    // entirely. The gesture was still accepted: the value was stored, the grid dimmed to
    // 55%, the read-out showed the new dB - and the sound moved by EXACTLY 0.00 dB.
    // Measured at 2*f0, a +12 dB pencil edit gave +6.01 dB with manual on and +0.00 dB
    // with it off. That is a control that lies, and it is what the user reported as
    // "cambiar el volumen de 2 no produce ningun cambio perceptible".
    //
    // The alternative - refusing the drag while manual is off - was rejected: the pencil
    // window exists to draw a spectrum by hand, so opening it, dragging a knob and being
    // told "no" is a worse answer than doing the obvious thing. The precedent is already
    // in the plugin: applySpectrumAdapt arms manual the same way, for the same reason.
    // The pill stays, in both places, so it is one click back off.
    //
    // Through the PARAMETER, so the host records it and it is undoable like every other
    // control; and BEFORE the drag's first frame, so:
    //   - the very first pixel of the drag is audible, and
    //   - manualSlopeFrozen catches the PRE-drag slope. Freezing it here rather than
    //     leaving it to timerCallback is the same ordering applySpectrumAdapt uses: the
    //     poll builds its settings AFTER this check, so a flip noticed one tick late
    //     would compile one template at the old slope and a second at the frozen one -
    //     an audible extra amplitude fade for a spectrum that had not changed.
    //
    // Returns true if it actually flipped (and therefore opened the undo transaction).
    bool armManualPartialAmpsForEdit()
    {
        if (apvts.getRawParameterValue ("partialManual")->load() > 0.5f)
            return false;                            // already on: nothing to arm, nothing to undo

        // Opened HERE so the parameter flip cannot land in the PREVIOUS undo step:
        // APVTS writes a parameter into the ValueTree (and so onto the undo stack) on its
        // own 20-500 ms timer, not synchronously, so whichever transaction is open when
        // that timer fires is the one that gets it. Opening ours now and telling
        // commitPartialGains not to open another puts the flip and the edit in the same
        // one whether the flush lands mid-drag or after the mouse comes up.
        //
        // RULE 7: not while a corner is being edited. Opening a transaction there starts
        // a step on the LIVE patch's stack for a flip that belongs to the corner, and the
        // corner's own rev already carries it. (commitPartialGains pushes nothing in a
        // session either, so there is no split gesture to protect against here.)
        if (pgEditCorner < 0)
            undoManager.beginNewTransaction();

        manualSlopeFrozen = apvts.getRawParameterValue ("curveSlope")->load();
        lastManualMode    = true;
        if (auto* pm = apvts.getParameter ("partialManual"))
        {
            pm->beginChangeGesture();
            pm->setValueNotifyingHost (1.0f);
            pm->endChangeGesture();
        }
        // Hand the engine the new state now. It only learns whether the edits are in
        // force through setOverrides, and waiting for timerCallback to notice would make
        // the first frames of the drag silent - the very defect this is fixing.
        setPartialGains (partialGains, true);
        return true;
    }
    // How many of those edits currently sit on a partial that actually exists. The rest
    // are not lost - their partial is simply not in this spectrum right now.
    int getPartialGainsMatched() const noexcept { return spectrumEngine.getGainMatchedCount(); }
    juce::uint32 getTemplateGeneration() const noexcept { return spectrumEngine.getGeneration(); }
    // "Is the spectrum worker still owed anything?" - see SpectrumEngine::isBuildPending.
    bool isSpectrumBuildPending() const { return spectrumEngine.isBuildPending(); }

    //==========================================================================
    // Tuning bank: 128 slots. The ceiling exists because "tuningBankSlot" is an
    // automatable parameter and a parameter needs a bounded range - it is generous, not
    // unlimited, and the difference matters: this used to be described as "effectively
    // unlimited" everywhere while firstEmptyBankSlot() answered 0 with the bank full, so
    // the 129th tuning silently overwrote the user's own first one. A limit is allowed to
    // exist here only because it is now STATED when it bites (see setTuningBankFile and
    // bankAutoAdd, which park the sentence on bankSlotError).
    // 0 = none, 1..N = slot; the switch is picked up on the message thread
    // (timerCallback) and loaded there. The UI only ever shows the loaded slots.
    static constexpr int kBankSlots = 128;
    void setTuningBankFile (int slot0, const juce::File& f);       // load a .scl into a slot + activate it
    juce::String getTuningBankName (int slot0) const { return juce::isPositiveAndBelow (slot0, kBankSlots) ? bankName[(size_t) slot0] : juce::String(); }
    int  getTuningBankSlotParam() const noexcept { return (int) apvts.getRawParameterValue ("tuningBankSlot")->load(); }
    void setTuningBankSlotParam (int slot);                        // 0 = none, 1..N = slot (drives automation + load)
    // The first free slot, or -1 when there is none. NEVER 0-as-a-fallback: the caller
    // passes this straight to setTuningBankFile, and a fallback slot is somebody's scale.
    int  firstEmptyBankSlot() const noexcept;
    // Every loaded tuning joins the bank. Returns the 0-based slot it landed in (or
    // an existing duplicate's), -1 if the bank is full - and a full bank parks a sentence
    // on bankSlotError rather than dropping the tuning out of the bank in silence.
    int  bankAutoAdd (const juce::String& name, const juce::String& source);
    void syncBankSelector (int slot0); // point the selector at a slot without re-loading it
    void bankClearSlot (int slot0);                                          // the bank's X button

    //==========================================================================
    // File presets (.gsting). Called from the editor.
    //
    // ONE SPELLING OF THE EXTENSION, because the literal was written out in a dozen
    // places across this file and the verifiers, and one of those copies drifted:
    // RhythmVerify's run 48 saved through savePreset() (which writes ".gsting") and
    // then deleted "<name>.fibpreset", so its own cleanup deleted nothing and the
    // suite left run48-A.gsting and run48-B.gsting sitting in the USER'S real preset
    // folder, run after run, next to their own patches. presetFile() is the only way
    // to name one, so that cannot happen again by typing.
    static constexpr const char* kPresetExt = ".gsting";
    static juce::File getPresetFolder();
    // TESTS ONLY: point every preset read and write at a scratch directory instead of
    // the user's Documents folder. run 48 saved through savePreset() into the USER'S
    // real folder and then deleted "<name>.fibpreset", so its cleanup removed nothing
    // and run48-A/B.gsting sat beside the player's own patches until somebody noticed.
    // A suite that must WRITE presets redirects the folder rather than promising to
    // tidy up. An empty File restores the real folder.
    static void setPresetFolderForTests (const juce::File& dir);
    static juce::File presetFile (const juce::String& name)
    { return getPresetFolder().getChildFile (name + kPresetExt); }
    juce::StringArray getPresetNames();

    //==========================================================================
    // THE FACTORY TUNINGS, AND THE FOLDER "Load tuning" OPENS IN.
    //
    // "mete archivos .tun DENTRO del VST ... que 'Load tuning' abra esa carpeta por
    // defecto; y si el usuario carga desde otra carpeta, que recuerde ESA."
    //
    // Six .tun files are compiled into the binary (see juce_add_binary_data in
    // CMakeLists.txt): the harmonic series 8..16, 7/12/24/31 equal divisions of the
    // octave, and Bohlen-Pierce. A FILE CHOOSER CANNOT BROWSE BinaryData, so they are
    // written out to a real folder the first time anything asks for it.
    //
    // Documents/Galactic Sting/Tunings - the same idiom and the same parent as
    // getPresetFolder(), because these are the user's files once they exist: they can
    // be edited, renamed, deleted and added to, and nothing here ever writes over one
    // again (see getTuningsFolder). It is NOT the preset folder and never touches it.
    static juce::File getTuningsFolder();
    // TESTS ONLY, and for the same reason setPresetFolderForTests exists: a verifier
    // that called getTuningsFolder() would extract six files into the USER'S Documents
    // on every run. An empty File restores the real folder.
    static void setTuningsFolderForTests (const juce::File& dir);

    // Where the tuning chooser should open: the folder the user last LOADED a tuning
    // from, or the bundled folder above when there is none (a first run, or a
    // remembered folder that has since been moved or unplugged).
    //
    // Stored in appProps beside recentScl / keyboardNoteNames, NOT in the patch state:
    // "the folder I keep my scales in" is a fact about this person's disk, and a patch
    // travels to other machines and into every preset. Saved there it would follow a
    // .gsting file onto a computer where that path is somebody else's folder or none.
    juce::File getTuningChooserFolder() const;
    void setTuningChooserFolder (const juce::File& dir);
    // Why a preset write was refused. The PROCESSOR decides the rule; the EDITOR writes
    // the sentence, because the sentence has to be in the user's language and the
    // processor has no tr(). CornerFixedPoint carries the corner it was refused for.
    enum class PresetWriteRefusal { None, BadName, CornerFixedPoint, WriteFailed };
    // Returns the stored name (sanitised), or empty on failure with `why` set. A CORNER
    // IS A FIXED POINT: while the pad is driving, the controls are the blend's OUTPUT,
    // so a save under an assigned corner's own name is REFUSED, out loud - that write is
    // what closes case #88's drain through the .gsting file. See writePresetFile, which
    // is where the rule lives.
    juce::String savePreset (const juce::String& name, PresetWriteRefusal& why, int& corner);
    juce::String savePreset (const juce::String& name); // ...when the caller has nowhere to show it
    bool loadPreset (const juce::String& name);
    // True once ANY parameter's BASE value (a knob turned, host automation, undo...)
    // no longer matches what was in the loaded preset - modulation from the
    // preset's OWN envelopes/LFOs never counts: it writes only the raw atomic
    // (see ModEngine.h), never the base value this compares against.
    bool presetHasDrifted() const;
    juce::String getLoadedPresetName() const { return loadedPresetName; }

    //==========================================================================
    // THE PRESET PLAYGROUND.
    //
    // Up to four presets sit on the corners of a rectangle and a draggable point
    // inside it blends them. The point IS the two automatable parameters pgX/pgY, so
    // the whole movement records and plays back in the host with the window open or
    // shut - the window is only ever a view onto those two numbers.
    //
    // WHAT MOVES (and the rules live in dsp/PresetMorph.h, where they are unit-tested):
    //   - continuous parameters: weighted average, in log space for times, frequencies,
    //     Q and the base-harmonic index, linear for everything already perceptual;
    //   - discrete settings (engine, phase, prime mode, manual amps, morph, realtime
    //     arming...): ONE nearest corner for all of them, with hysteresis;
    //   - the SPECTRUM: not through the parameters at all. Each corner's spectrum is
    //     compiled once when it is assigned and the compiled partial LISTS are blended
    //     (spectrum::blendTemplates), which is the only thing that means anything when
    //     two corners are running different engines.
    //
    // WHAT NEVER MOVES: the tuning. Mode, loaded scale, reference A, octave shift, bend
    // range, the bank and any absolute table are all excluded - the playground changes
    // the SOUND, not what the keys mean. Same promise loadPreset() already makes.
    static constexpr int kPlaygroundCorners = presetmorph::kCorners;

    // BYTES reserved for the MIX engage-edge injection buffer, sized here (and named, so a
    // verifier can measure the real number rather than a copy of it) and reserved once in
    // prepareToPlay. The arithmetic is written out at that call: the unit is BYTES, not
    // events, and the fill's own bound is the 16 x 128 loop in renderSynthMix.
    static constexpr int kMixInjectBytes = 32768;

    // Put a preset on a corner (empty name clears it). Compiles that preset's spectrum
    // there and then - synchronously, on the message thread, exactly as prepareToPlay's
    // buildNow does - because a corner has to have a template before the very first
    // blend, and the alternative is a first drag that silently does nothing.
    bool assignPlaygroundCorner (int corner, const juce::String& presetName, juce::String& errorOut);
    juce::String getPlaygroundCornerName (int corner) const;
    int  getPlaygroundAssignedCount() const;
    // The morph only DRIVES anything with two or more corners assigned. One preset has
    // nothing to blend with, and letting it apply would mean opening the window and
    // dropping a single preset silently overwrote the patch the user was working on.
    bool isPlaygroundActive() const;
    void getPlaygroundWeights (double wOut[kPlaygroundCorners]) const;
    // Which corner currently owns every discrete setting (-1 = none). The window marks
    // it, because "why did the engine change?" has to have a visible answer.
    int  getPlaygroundNearestCorner() const noexcept { return pgWinner; }
    // The realtime/adaptive link is SUSPENDED while the playground drives the spectrum
    // (see drivePlayground). True = the window should say so, instead of leaving a lit
    // REALTIME button that is quietly doing nothing.
    bool isPlaygroundSuspendingRealtime() const noexcept;

    //==========================================================================
    // THE PLAYGROUND'S SECOND MODE: MIX ("multi-Fibló", integrated).
    //
    // Morph produces ONE blended patch; MIX runs the four corner presets as four real
    // sub-engines SIMULTANEOUSLY - each with its own voices, its own modulators and
    // its own compiled spectrum, all fed the same incoming MIDI - and the point is a
    // four-way volume balance: the bilinear weights become per-corner GAINS under the
    // equal-power law (presetmorph::mixGains - sqrt(w), so total power is constant
    // across the pad and a vertex is exactly that corner alone).
    //
    // SHARED, deliberately: the TUNING (the keys mean one thing - MIX changes the
    // sound, never the pitch of a key), the master output gain (the corners' own saved
    // masterGain values are NOT applied - the master is the master), the preview/MIDI
    // plumbing, and the RHYTHM engine, which stays on the MAIN patch: four rhythm
    // engines firing four pulse trains at once is chaos nobody asked for, and the
    // main patch's rhythm is what the user had dialled in.
    //
    // PER CORNER: the voice/mod parameters (via a ParamBank filled from the corner's
    // stored preset values - see ParamBank.h), the modulator ROUTING the preset was
    // saved with (a corner with no routing simply has its modulators off), and the
    // corner's compiled spectrum, republished into its own engine whenever an
    // adaptive corner adapts (same rules as morph mode - the corner's voices morph or
    // fade to it exactly as the live engine would).
    //
    // WHAT MIX NEVER DOES: touch the live patch. No parameter is written, no spectrum
    // is published over the live one - the main engine keeps rendering the patch the
    // user had (crossfaded out while MIX is engaged), so disengaging restores the
    // ordinary single-engine state EXACTLY, and mid-note: voices in flight simply
    // fade, they are never stolen by a state rewrite.
    //
    // One current, honest limit: a corner preset whose modulators drive SPECTRUM
    // parameters (Base, Fractal, Prime...) does not re-compile that corner's spectrum
    // per block - the corner's spectrum is its compiled template (updated by the
    // adaptive follower when armed). The live engine's 60 Hz recompile loop exists
    // for exactly that and stays with the live patch; per-voice targets (envelopes,
    // filter, pitch, slope) modulate fully in every corner.
    bool isPlaygroundMixMode() const noexcept
    { return apvts.getRawParameterValue ("pgMode")->load() > 0.5f; }
    // Is the mix actually sounding (engaged and past the message->audio handoff)?
    bool isPlaygroundMixEngaged() const noexcept { return mixEngaged.load (std::memory_order_acquire); }

    // ADAPTIVE CORNERS: a corner whose preset was saved with the SpectrumFromChord
    // follower armed KEEPS ADAPTING while the playground is engaged - against its OWN
    // settings, to the chord being held right now - and its corner template updates
    // live, so the blend (and therefore the morphed result) moves with the playing.
    // Half-morphed with a static preset the adaptation is diluted but PRESENT, which
    // is the point: "morphs locos" between several presets, not from one.
    //
    // Only SpectrumFromChord qualifies. TuningFromSpectrum writes the TUNING, which
    // the playground promises never to touch; SpectrumFromTuning aims at a fixed set
    // of notes, so inside the playground it would re-derive the same spectrum for
    // ever (the enum's own comment already documents that trap) - a corner saved with
    // either stays exactly as compiled. The LIVE patch's follower stays suspended as
    // before: the playground owns the live spectrum, and the corner adaptations reach
    // it THROUGH the blend rather than around it.
    //
    // How many corners are adapting right now (assigned + armed + not refused).
    int getPlaygroundAdaptiveCount() const noexcept;
    // Counters for the offline verifier: searches launched for corners, results applied.
    int getPlaygroundAdaptStarts()  const noexcept { return pgAdaptStarts; }
    int getPlaygroundAdaptApplies() const noexcept { return pgAdaptApplies; }
    // Non-empty when a corner's saved settings cannot run at all (bad period, absurd
    // cost...). The corner stops retrying and this SAYS so - never a silent stop.
    juce::String getPlaygroundAdaptRefusal() const { return pgAdaptRefusal; }
    //==========================================================================
    // EDITING A CORNER IN PLACE. Full rules (and the ownership contract this obeys) are
    // in the big comment above beginPlaygroundCornerEdit in PluginProcessor.cpp. The
    // short version, because getting it wrong is a data-loss bug:
    //
    //   * While a corner is being edited the main editor's controls are a SCRATCH
    //     SURFACE showing that corner - they are NOT the live patch. The live patch is
    //     frozen (parameters, timbre, per-partial edits, its preset identity and its
    //     drift mark) and handed back byte-for-byte when edit mode ends.
    //   * The edits live in the CORNER, in memory, for this session only. Nothing is
    //     written to the .gsting the corner points at - ever. The trailing "*" on the
    //     corner's name is the warning, and it survives leaving edit mode.
    //   * Saving from edit mode writes the corner under a NAME OF ITS OWN and renames
    //     the corner to it; the live patch's own preset identity is not touched.
    //
    // Returns false if the corner holds no preset, or the playground is not engaged.
    bool beginPlaygroundCornerEdit (int corner);
    // Hand the live patch back. Safe to call when nothing is being edited.
    void endPlaygroundCornerEdit();
    int  getPlaygroundEditCorner() const noexcept { return pgEditCorner; }
    bool isPlaygroundCornerEdited (int corner) const noexcept
    { return juce::isPositiveAndBelow (corner, kPlaygroundCorners) && pgCorner[(size_t) corner].edited; }
    // The corner's name as the UI must show it: with the "*" when it carries unsaved
    // edits. ONE place, so the pad, the corner box and the frozen preset field cannot
    // disagree about whether a corner has been touched.
    juce::String getPlaygroundCornerDisplayName (int corner) const;
    // Save the corner (edited or not) as a preset file of its own and rename the corner
    // to it. Returns the name actually stored (sanitised) or empty on failure.
    juce::String savePlaygroundCornerAs (int corner, const juce::String& name);
    // TESTS: how many times the edited corner's spectrum has been recompiled, so a check
    // can prove an edit reaches the SOUND and not merely the store.
    int getPlaygroundCornerRevForTests (int corner) const noexcept
    { return juce::isPositiveAndBelow (corner, kPlaygroundCorners) ? pgCorner[(size_t) corner].rev : -1; }

    // TESTS: the corner STORE's own parameter values - the blend's INPUT, in
    // getParameters() order. A corner is a fixed point of the blend, so twenty save /
    // re-assign round trips must leave this vector bit-identical; case #88 is what
    // happens when it does not. Empty for an unassigned corner.
    std::vector<float> getPlaygroundCornerParamsForTests (int corner) const
    {
        return juce::isPositiveAndBelow (corner, kPlaygroundCorners) ? pgCorner[(size_t) corner].params
                                                                     : std::vector<float>{};
    }
    // TESTS: the corner store's value for one parameter id (NaN if unassigned/unknown).
    float getPlaygroundCornerParamForTests (int corner, const juce::String& id) const;

    // The corner's compiled template, so a test can prove it CHANGES when the chord does.
    std::shared_ptr<const spectrum::SpectrumTemplate> getPlaygroundCornerTemplateForTests (int corner) const
    {
        return juce::isPositiveAndBelow (corner, kPlaygroundCorners) ? pgCorner[(size_t) corner].tmpl
                                                                     : nullptr;
    }
    // TESTS ONLY: the gliding ratios of the first ACTIVE main-engine voice (see
    // AdditiveVoice::curRatiosForTests). How run 47 proves a drag BENDS frequencies.
    int voiceCurRatiosForTests (double* out, int maxN)
    {
        for (int v = 0; v < synth.getNumVoices(); ++v)
            if (auto* av = dynamic_cast<AdditiveVoice*> (synth.getVoice (v)))
                if (av->isActive())
                    return av->curRatiosForTests (out, maxN);
        return 0;
    }

    //==========================================================================
    // The TWO inputs every Sethares feature shares. Their numbers are compared with
    // each other (the degree list next to the n-ad list, the generated scale's
    // heights against the degree colouring, "sounding now" against an adaptation),
    // and Sethares' roughness depends on absolute frequency AND on amplitude - so
    // one anchor convention and one spectrum convention, or the columns are
    // incomparable. Message thread.
    //
    // analysisAnchorHz: the frequency key 60 REALLY sounds at under the active
    // tuning (reference-A, octave shift and a .tun/@ABL anchor included).
    //
    // analysisSpectrumAt: the published template at its SOUNDING amplitudes (the
    // user's per-partial gain edits applied), anchored at f0Hz, analysis-capped to
    // the 64 loudest sounding partials. Falls back to a fresh compile (with the
    // same edits resolved) only before the first publish.
    double analysisAnchorHz() const;
    std::vector<dissonance::Partial> analysisSpectrumAt (double f0Hz) const;

    //==========================================================================
    // "Spectrum to Scale" (Sethares): analyses the CURRENT spectrum, builds a
    // .scl from the curve's minima (or maxima in reverse mode) over the chosen
    // period, loads it as the active scale and keeps the text for saving.
    bool generateDissonanceScale (int count, bool useMaxima, double periodRatio,
                                  const juce::String& periodSclLine, double minGapCents,
                                  juce::String& errorOut);
    bool saveGeneratedScl (const juce::File& f, juce::String& errorOut);
    bool hasGeneratedScl() const noexcept { return lastDissScl.isNotEmpty(); }
    juce::String getGeneratedSclName() const { return lastDissName; }

    // Generated-scale degree list for the UI (sorted ascending by curve height).
    struct DissDegreeInfo { int degree; double ratio; double height; };
    const std::vector<DissDegreeInfo>& getDissDegrees() const noexcept { return dissDegrees; }

    //==========================================================================
    // N-note CHORD search (Sethares union measure, TTSS §6.8/App.E) - runs on a
    // BACKGROUND thread (grid + refine is deliberately not realtime); the editor
    // polls progress/generation and copies the results.
    struct ChordHit { std::vector<double> alphas; double value; }; // alphas[0] = 1 (root)
    // "Reset all parameters" (Init) sets every parameter individually, not through
    // restoreState() - it needs the same guarantee: no note may be left sounding.
    void requestHardSilence() { pendingHardSilence.store (true, std::memory_order_release); }
    bool startChordSearch (int notes, double periodRatio, const juce::String& periodSclLine,
                           int count, bool maxima, double minGapCents, bool rootOnly,
                           juce::String& errorOut);
    void cancelChordSearch();
    bool  isChordSearchRunning()  const noexcept { return chordRunning.load(); }
    float getChordSearchProgress() const noexcept { return chordProgress.load(); }
    int   getChordResultsGeneration() const noexcept { return chordGeneration.load(); }
    int   getChordRequested() const noexcept { return chordRequested.load(); }
    int   getChordFound()     const noexcept { return chordFound.load(); }
    double getChordMinGap()   const noexcept { return chordMinGap.load(); }
    std::vector<ChordHit> getChordResults() const
    {
        const juce::ScopedLock sl (chordLock);
        return chordResults;
    }
    // Double-click a result: the chord becomes the active scale (.scl, degrees in
    // cents + the exact typed period), loaded like any generated tuning.
    //
    // `addToBank` is false for the REALTIME follower only. Every hand-made load joins
    // the tuning bank, which is right for a deliberate choice and catastrophic sixty
    // times a second: a modulator sweeping the spectrum would have filled all 128 bank
    // slots with near-identical scales in a couple of seconds and evicted the user's own
    // library from view. The follower's own baseline goes into the bank ONCE, when it is
    // switched on (see setRealtimeLink).
    bool loadChordAsScale (int index, juce::String& errorOut, bool addToBank = true);

    //==========================================================================
    // n-AD ranking of the ACTIVE scale: all combinations of n scale degrees, ranked
    // by their TOTAL beating (union dissmeasure - every pair counted, not just each
    // degree vs the tonic). Background worker; editor polls the generation.
    struct NadRow { std::vector<int> degs; double value = 0.0; };
    // notes = how many notes per combination; rootOnly = rank by the beating against
    // note 0 only (instead of every pair).
    // 2 = dyads, 3 = triads, ...  The searcher takes the same four controls as the
    // free chord search: how many notes, most consonant or most dissonant, which
    // interval counts as the period, and the smallest interval allowed (cents).
    void startNadCompute (int notes, bool rootOnly = false, bool maxMode = false,
                          double periodOverride = 0.0, double minGapCents = 0.0);
    int  getNadGeneration() const noexcept { return nadGeneration.load(); }
    bool isNadBusy() const noexcept { return nadBusy.load(); }
    // Honesty surface: never hide a coverage limit silently.
    bool nadWasOverBudget() const noexcept { return nadOverBudget.load(); }
    // ...and the same rule for the OTHER thing this list can be missing: the tuning it
    // ranked states no ascending period, so the search could not treat one shape as
    // standing for all its transpositions (no "any" chip), could not skip a pair that
    // repeats the period, and nothing here may be read as being "per octave". Published
    // so the list can SAY that rather than quietly behave differently.
    bool nadHadNoPeriod() const noexcept { return nadNoPeriod.load(); }
    int  getNadUsedDegrees()  const noexcept { return nadUsedD.load(); }
    int  getNadTotalDegrees() const noexcept { return nadTotalD.load(); }
    // HOW MANY DEGREES THE PUBLISHED ROWS WERE COMPUTED OVER. Published with the
    // results, not read live off the tuning: a row's numbers are degree INDICES into
    // the scale that was loaded when the worker ran, so a caller that shows them beside
    // a different scale is printing degrees that scale does not have. That is exactly
    // what "en una pentatonica me dice 7 y 11" was - 12-TET's answer left on screen.
    int  getNadResultDegrees() const noexcept { return nadResultD.load(); }
    std::vector<NadRow> getNadResults() const
    {
        const juce::ScopedLock sl (nadLock);
        return nadResults;
    }

    //==========================================================================
    // THE INVERSE SEARCH: adapt the SPECTRUM to a set of notes (see Dissonance.h).
    // The notes are the loaded tuning's degrees, or - in realtime - the chord under
    // the fingers; the partials move. Background worker, same publish-under-lock +
    // generation-counter pattern as the chord search.
    struct SpectrumAdaptSettings
    {
        int    partials    = 8;      // how many partials the adapted spectrum has
        double periodRatio = 2.0;    // partials live in (1, period]; the Custom Engine stacks it
        double minGapCents = 50.0;   // air between partials - the no-merge guarantee
        bool   rootOnly    = false;  // dyads against the root, or the full union
        int    goal        = 0;      // 0 = minimum beating, 1 = maximum, 2 = inside a range
        double rangeLo = 0.0, rangeHi = 0.0;
        bool   shapeAmps   = true;   // also move per-partial AMPLITUDES, not only positions
        // THE REGISTER RANGE the objective is aggregated over (see Dissonance.h). Sethares'
        // critical band is a width in Hz, so a spectrum optimised at ONE pitch is optimal
        // only there - and, worse, an optimiser working in the bass sees tight partial
        // clusters as nearly free and builds them, which is precisely the roughness that
        // dominates two octaves up.
        //
        // ONE NUMBER, and it is a COUNT OF PERIODS EACH WAY: the band runs from
        // `regPeriods` periods BELOW the anchor key (see registerAnchorKey) to the same
        // number of periods ABOVE it. Both halves matter - the interval that beats worst
        // is ~434 cents low and ~61 cents four octaves up, so the register a fifth below
        // the played note is as different a question as the one above it, and an
        // anchor-and-up band (what this was) simply never asked it.
        //
        // WHY PERIODS AND NOT STEPS. A step count was the previous unit and it was exact
        // but unreadable: "62" is two periods in 31-EDO and 62 periods in a one-degree
        // scale, so every value had to be divided by the degree count before it meant
        // anything, and a typed number had to be deliberately protected from being
        // re-derived when the tuning changed. A period IS the tuning's own unit: the same
        // number is the same musical width in every scale, nothing is re-derived, and the
        // only tuning that has no period of its own is the absolute .tun table - where
        // adaptRegistersFor uses the period Fibló measured for the file and the panel says
        // so, rather than pretending.
        //
        // < 0 means "the default width", which is kAdaptRegisterDefaultPeriods (measured,
        // see SpectrumTests (8)) - so a follower running with the window shut still spans
        // the registers around the note being played instead of collapsing to one pitch.
        double regPeriods = -1.0;
    };
    // WHERE THE PERIOD USED BY THE REGISTER BAND COMES FROM. A .scl (or MTS) states its
    // own period, so the band is exact. An absolute 128-key .tun states 128 pitches and
    // NOTHING about repetition: what periodRatio()/degreesPerPeriod() answer there is
    // Fibló's estimate for the file, or - when the file has nothing ascending to measure -
    // the 2/1-over-12-keys fallback, which is a guess and not a measurement. The panel
    // shows which of the three it is standing on, because a band silently built on a made
    // up period is exactly the "silently wrong" this instrument refuses to be.
    // Where the number the register band counts in came from. `NoneStated` was called
    // TableFallback and could only be reached from an absolute .tun - but a .scl that
    // states no ASCENDING period (descending, or a degenerate 1/1) is the same
    // situation exactly: the band is being counted in 2/1s that appear nowhere in the
    // user's file. It is named for the fact now, not for the file format that first
    // showed it.
    enum class RegisterPeriodSource { Scale, TableEstimate, NoneStated };
    struct RegisterPeriodInfo
    {
        RegisterPeriodSource source = RegisterPeriodSource::Scale;
        double ratio    = 2.0;      // the period in force, as a frequency ratio
        int    keys     = 12;       // ...and how many MIDI keys of THIS tuning span it
        double devCents = 0.0;      // worst cents the .tun strays from it (table modes only)
    };
    RegisterPeriodInfo registerPeriodInfo() const;
    // The KEY the register range hangs off: the lowest note being HELD, else the last one
    // played, else 60. In that order and for that reason - the hand-pressed Adapt must have
    // a defensible anchor without the user holding a key down while they reach for the
    // mouse, and "middle C" is only ever the answer before a single note has been played.
    int registerAnchorKey() const noexcept;
    // THE BAND'S TWO ENDS IN Hz, resolved exactly as adaptRegistersFor resolves them (it
    // calls this). Registers are FREQUENCIES, not notes: the objective is sampled at
    // pitches, so the ends are the anchor's frequency multiplied and divided by the
    // tuning's period raised to `periods`. Going through the key ladder instead - anchor
    // +- round(periods x degrees) keys, which is what the step-count form did - rounded
    // every fractional width to the nearest key: 1.5 periods of a five-note scale came out
    // 120 cents wide of what was asked for, silently. Only the CLAMP is in keys, because
    // "the instrument stops here" really is a statement about the keyboard.
    void registerBandHz (double periods, double& loHz, double& hiHz) const noexcept;
    // The registers a run would use, resolved exactly as startSpectrumAdapt resolves them -
    // the panel prints these, so what it claims and what the search does cannot drift.
    // A member, not a static: the range is now anchor-relative and step-counted, so it can
    // only be resolved by something that knows the anchor and the tuning's own frequencies.
    dissonance::Registers adaptRegistersFor (const SpectrumAdaptSettings& s, double f0Hz,
                                             bool realtime) const;
    // WHAT ONE RUN WILL COST, in milliseconds, before it is started (see Dissonance.h).
    // Same helper the panel prints and the same one startSpectrumAdapt's hard bound is
    // measured against, so the price on the button and the only remaining refusal can
    // never be two different numbers.
    double estimateAdaptMs (const SpectrumAdaptSettings& s, int targetNotes,
                            double f0Hz, bool realtime) const;
    // `targetAlphas` are the notes as ratios (alphas[0] is the root, normally 1.0);
    // `f0Hz` is the root's real frequency - Sethares roughness depends on register, so
    // the same chord an octave down wants a different spectrum.
    // `realtime` picks the SMALLER register cap (see Dissonance.h): the follower must keep
    // up with the fingers, and the cost is exactly linear in the number of registers.
    // `targetIsTuning` marks the runs whose alphas came from tuningAsTargetAlphas: only
    // those record the active tuning as the result's provenance (see AdaptInfo). The
    // CALLER states it because the alphas alone cannot say where they came from - a held
    // chord can spell out the scale exactly, and guessing from values would pin a tuning
    // onto a spectrum the user derived from their fingers.
    bool startSpectrumAdapt (const SpectrumAdaptSettings& s, const std::vector<double>& targetAlphas,
                             double f0Hz, juce::String& errorOut, bool realtime = false,
                             bool targetIsTuning = false);
    void cancelSpectrumAdapt() { adaptCancel = true; }
    bool  isSpectrumAdaptRunning() const noexcept { return adaptRunning.load(); }
    float getSpectrumAdaptProgress() const noexcept { return adaptProgress.load(); }
    int   getSpectrumAdaptGeneration() const noexcept { return adaptGeneration.load(); }
    struct AdaptInfo
    {
        std::vector<double> ratios;   // ascending, [0] == 1.0 (the played pitch)
        std::vector<double> amps;     // linear, index-aligned with `ratios`
        double before = 0.0;          // what the SEED spectrum beat against the target
        double after  = 0.0;          // what this one does
        int    targetNotes = 0;
        bool   valid = false;
        // The register range this result was optimised over, and what it achieves across
        // it. `before`/`after` are the aggregate that was minimised - the MEAN over the
        // range - so the panel's one line reports the number the search actually worked on,
        // never middle C's value standing in for six octaves.
        double regLoHz = 0.0, regHiHz = 0.0;
        int    regPoints = 1;
        double mean = 0.0,  worst = 0.0;       // the mean, and the register that scores worst
        double seedMean = 0.0, seedWorst = 0.0;
        double worstHz = 0.0;         // the register that scores worst
        std::vector<double> perRegister, registerF0;
        // PROVENANCE: the tuning this result was adapted TO, captured when the search
        // was STARTED (the user can load another tuning while the worker runs, and the
        // result still belongs to the one it was aimed at). Empty when the target was
        // the held chord, or the active tuning had no reloadable source (MTS/Equal).
        // Travels inside the result - not in a member beside it - so a newer search's
        // target can never be pinned onto an older search's spectrum.
        juce::String provName, provSource, provPeriod;
    };
    AdaptInfo getSpectrumAdaptResult() const
    {
        const juce::ScopedLock sl (adaptLock);
        return adaptResult;
    }
    // The target the adaptation aims at: every degree of the ACTIVE tuning (as ratios),
    // anchored at the tonic's real frequency. Message thread.
    std::vector<double> tuningAsTargetAlphas (double& f0Out) const;
    // The chord being HELD right now, as ratios above its lowest note (empty if nothing
    // is down). f0Out is that lowest note's exact sounding frequency.
    std::vector<double> heldChordAsTargetAlphas (double& f0Out) const;
    // The same chord read one lookahead budget EARLIER - the notes that have arrived but
    // have not sounded yet (see LookaheadPeek). Only ever called while lookahead is
    // active; with it off the rhythm engine's own bookkeeping is the single answer, as
    // heldChordAsTargetAlphas' comment insists.
    std::vector<double> heldChordEarlyAsTargetAlphas (double& f0Out) const;

    // Write the adapted spectrum into the Custom Engine: the partial RATIOS become its
    // scale and the per-partial AMPLITUDES become manual gain edits. `undoable` wraps it
    // in one undo transaction (the hand-pressed button); the realtime follower passes
    // false and relies on the baseline it snapshotted when it engaged.
    bool applySpectrumAdapt (bool undoable, juce::String& errorOut);
    // What the CURRENT spectrum really beats against the current target, measured on the
    // compiled partials - so a fractal level or a Reps count the adaptation did not model
    // shows up as a number the user can see instead of a silent discrepancy.
    // `regs` must be the SAME registers the adaptation ran on: "sounding now" sits next to
    // "after" in one line, and a value measured at one pitch against a mean over six
    // octaves would differ for reasons that have nothing to do with the patch, printing a
    // discrepancy warning that was pure arithmetic.
    double measureCurrentAgainstTarget (const std::vector<double>& alphas, double f0Hz,
                                        bool rootOnly, const dissonance::Registers& regs) const;

    //==========================================================================
    // REALTIME links between the spectrum and the tuning ("Spectrum vs Tuning").
    //   TuningFromSpectrum  - the tuning follows the top beating-search result whenever
    //                         the SPECTRUM changes.
    //   SpectrumFromTuning  - the spectrum is adapted to the loaded tuning. NOT what the
    //                         panel's REALTIME button engages any more: a loaded tuning is
    //                         a fixed set of notes, so a follower aimed at it re-applies
    //                         the SAME adaptation for ever (in 12ED2 it simply kept writing
    //                         12ED2 into the spectrum). One press of Adapt does that job
    //                         better. Kept because the link itself is sound, and because a
    //                         tuning that really is being edited live is a legitimate use.
    //   SpectrumFromChord   - the spectrum is adapted to the chord being held right now.
    //                         This is what REALTIME means in the adapt panel: follow the
    //                         notes actually being played.
    // Driven from timerCallback (60 Hz), so it works with the editor shut and is
    // exercisable offline through pollParametersForTests().
    enum class RtLink { Off = 0, TuningFromSpectrum = 1, SpectrumFromTuning = 2, SpectrumFromChord = 3 };
    struct RtSettings
    {
        // TuningFromSpectrum: the chord search's controls.
        int          notes = 3;
        double       periodRatio = 2.0;
        juce::String periodLine  = "2/1";
        bool         maxima = false;
        double       minGapCents = 50.0;
        bool         rootOnly = false;
        SpectrumAdaptSettings adapt;   // the other two links
        // ADAPTIVE LOOKAHEAD. Deliberately NOT an automatable parameter: it changes the
        // plugin's reported latency, and a host re-running its delay compensation
        // because a knob moved in a clip is not a thing anyone wants automated. It is
        // part of the saved state like the rest of the link (see realtimeSettingsToTree).
        bool lookahead = false;
    };
    void setRealtimeLink (RtLink mode, const RtSettings& s);
    // The panel pushes its settings here on every touch, so this is also where a change
    // to the LOOKAHEAD toggle is noticed. applyLookahead() is a no-op when nothing moved.
    void updateRealtimeSettings (const RtSettings& s) { rtSet = s; applyLookahead(); }
    RtLink getRealtimeLink() const noexcept { return rtLink; }
    // Non-empty when a link REFUSED to run (and switched itself off). Never silent.
    juce::String getRealtimeRefusal() const { return rtRefusal; }
    // Counters for the honesty read-out AND for the throttle tests: how many searches the
    // follower actually launched, and how many results it actually applied.
    int getRealtimeStarts()  const noexcept { return rtStarts; }
    int getRealtimeApplies() const noexcept { return rtApplies; }

    //== ADAPTIVE LOOKAHEAD ====================================================
    // THE BUDGET, and where the number comes from. Every figure below was MEASURED on
    // this machine while this feature was being written, not inherited.
    //
    // The budget has to cover everything between a note ARRIVING and the adapted spectrum
    // being live in the voices. Broken into its four parts, each measured separately:
    //
    //   THE THROTTLE, up to kRtAdaptTicks = 6 ticks = 100 ms. This is the biggest term
    //     and the one a "how long does the search take" reading misses entirely: a chord
    //     change that arrives while the previous one's search is still fresh waits for the
    //     throttle before it even starts. Run 49 measures it directly - its lookahead-OFF
    //     take launched 97.3 ms after the notes arrived, against 12-13 ms when the
    //     throttle was already clear.
    //   ONE 60 Hz POLL to notice the notes at all: 16.7 ms.
    //   THE SEARCH on its worker. Timed with no other load, realtime register cap, over
    //     the whole settings range (RhythmVerify's probe, and Dissonance.h's model):
    //     4 partials 2.6 ms, 8 partials 10.5 ms, 12 partials 33 ms, 16 partials 104 ms,
    //     24 partials 84 ms against a 3-note chord in root-only scope (137 ms at 16
    //     partials in all-vs-all). In situ, with the audio rendering beside it, the
    //     default 8-partial follower measured 17-33 ms.
    //   ONE MORE POLL to apply the finished result (16.7 ms) and the 50 ms template
    //     de-click fade, so the spectrum is not merely published but SETTLED when the
    //     note sounds.
    //
    // 100 + 16.7 + 33 + 16.7 + 50 = 216 ms at the DEFAULT follower settings, and 250 is
    // that rounded up with the slack a loaded machine needs (the same run measured 200 ms
    // for a search that takes 33 ms idle, once, while a compiler was running beside it).
    //
    // IT DOES NOT COVER EVERY SETTING, AND THE PANEL SAYS SO RATHER THAN PRETENDING.
    // 16 or 24 partials against a wide chord outgrow it, and the honest answer is not a
    // latency that silently moves under the user: getLastRealtimeLandingMs() reports how
    // long the LAST spectrum change really took, the lookahead button prints it beside
    // the budget, and it says LATE when it did not fit. A measured read-out beats an
    // estimate - especially here, where the estimator's calibration constant is known
    // wrong on this machine (SpectrumTests fits 14.67 ns/pair against the header's 9.8)
    // and phase 3 re-fits it.
    //
    // The budget stays a CONSTANT rather than a function of the adapt settings so the
    // reported latency only ever moves when the user works the LOOKAHEAD toggle itself.
    // A latency that changed with a partial count would have the host re-run its delay
    // compensation in the middle of a session, and the search cost also depends on how
    // many keys are down - which no reported latency may ever depend on.
    static constexpr double kLookaheadMs = 250.0;
    // Is the user asking for it, and is it actually running? The second is not always the
    // first: the budget is only armed while a spectrum-side realtime link is on, because
    // with no follower to give a head start to, latency would buy nothing at all.
    bool isLookaheadArmed()  const noexcept { return rtSet.lookahead; }
    bool isLookaheadActive() const noexcept { return laSamples.load (std::memory_order_relaxed) > 0; }
    int  getLookaheadSamples() const noexcept { return laSamples.load (std::memory_order_relaxed); }
    void setLookaheadForTests (bool on) { rtSet.lookahead = on; applyLookahead(); }
    // HOW LONG THE LAST SPECTRUM CHANGE REALLY TOOK, in milliseconds from the moment the
    // chord moved to the moment the adapted spectrum was written. < 0 = nothing measured
    // yet. Wall clock, message thread only, and it is the whole reason the budget above is
    // allowed to be a constant: the number the user is shown is the one their own machine
    // produced with their own settings, so a budget that does not fit says so out loud
    // instead of quietly delivering a late spectrum. Read by the editor and by run 49.
    double getLastRealtimeLandingMs() const noexcept { return rtLandingMs; }
    // How many 60 Hz ticks the two links wait between launches, on top of never running two
    // at once. Measured on this machine: a 3-note grid+refine chord search over 2/1 with a
    // rich spectrum (the 64-partial analysis cap) USED to be ~1.4 s and is now ~0.30 s cold
    // and ~0.12 s when the grid tables can be reused ("0 vs all" is 0.03 s) - see the
    // GridTables comment in Dissonance.h. The one-second floor was sized for the old cost
    // and had become the slowest part of the link, so it comes down with it. An 8-partial
    // adaptation is 9 ms against a triad and ~100 ms against 31 scale degrees. Without a
    // throttle a modulator sweeping the spectrum starts a new search on EVERY tick - 200
    // spectrum changes produced 138 searches - and the worker queue never drains.
    static constexpr int kRtTuneTicks  = 20;  // ~0.33 s: the full grid+refine chord search
    // ~100 ms for the adaptation. It was 250 ms, and that was THE lag the player felt:
    // a chord struck from rest is never throttled (the link has been idle for seconds),
    // so the first move landed in ~50 ms and every move made while still playing waited
    // out the floor - measured at 163 ms for a second move 130 ms after the first, i.e.
    // most of it spent doing nothing. The floor was sized when one adaptation cost ~250 ms;
    // an 8-partial triad is now ~30 ms, and "never two at once" (adaptRunning) is what
    // actually keeps the worker from piling up. What is left here is a bound on how often
    // the SPECTRUM may be rewritten, and 10 Hz is a rate the 50 ms template fade absorbs.
    static constexpr int kRtAdaptTicks = 6;
    // The same floor as a DURATION (the timer runs at 60 Hz). It is the budget one realtime
    // adaptation has: a search that takes longer than this cannot land before the next one
    // is due, so the follower's results arrive stale - which the panel warns about instead
    // of silently degrading, and instead of refusing the settings outright.
    static constexpr double kRtAdaptBudgetMs = kRtAdaptTicks * 1000.0 / 60.0;

    //==========================================================================
    // Scale EDITING on the keyboard. Works from ANY loaded tuning (the current
    // tuning is first rendered to .scl form, degree sources preserved).
    // Custom-Engine On/Off WITHOUT losing the loaded scale (plain toggle).
    // Through the parameter (with a gesture) so the change lands in the undo history.
    // Same, without a gesture: for loads/restores that are not a user edit.
    void setTimbreOnParam (bool b)
    {
        if (auto* p = apvts.getParameter ("timbreOn"))
            p->setValueNotifyingHost (b ? 1.0f : 0.0f);
    }
    void setTimbreEnabled (bool b)
    {
        if (timbreDegrees.empty()) return;
        if (auto* p = apvts.getParameter ("timbreOn"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (b ? 1.0f : 0.0f);
            p->endChangeGesture();
        }
    }
    bool timbreLoaded() const noexcept { return ! timbreDegrees.empty(); }

    // Which of the three MUTUALLY EXCLUSIVE spectrum sources is building the partials
    // right now. Temper wins first (it keeps the harmonic engine running and snaps its
    // partials to the ACTIVE TUNING, so a loaded Custom-Engine scale is bypassed
    // entirely), then a Custom-Engine scale, else the plain harmonic series.
    //
    // currentSettings() branches on THIS, and the editor reads it to SAY so on screen.
    // The two used to decide independently - the editor only knew
    // timbreReplacesEngine() - which made temper mode invisible: the Custom Engine box
    // stayed bright and still read "Custom: <name>" while that scale was being ignored.
    enum class EngineInForce { Harmonic, CustomScale, Tempered };
    EngineInForce engineInForce() const noexcept
    {
        // Mirrors currentSettings() exactly, because the UI names the engine from here:
        // with the toggle ON but no usable period to temper TO, nothing is tempered, so
        // saying "Tempered" would label the spectrum after a snap that did not happen.
        if (apvts.getRawParameterValue ("timbreTemper")->load() > 0.5f)
            return temperIsPossible() ? EngineInForce::Tempered : EngineInForce::Harmonic;
        if (isTimbreActive())                                           return EngineInForce::CustomScale;
        return EngineInForce::Harmonic;
    }

    // Does the Custom-Engine scale REPLACE the harmonic engine (temper mode off)?
    // With temper ON both run together, so the engine's controls stay live.
    bool timbreReplacesEngine() const noexcept
    { return engineInForce() == EngineInForce::CustomScale; }

    //==========================================================================
    // MODULATORS (3 custom envelopes + 3 LFOs) -> any parameters. Editor calls.
    ModEngine& getModEngine() noexcept { return modEngine; }
    // Custom LFO wave: the drawn table + its editor breakpoints (persisted).
    void setLfoWave (int lfo, const float* table256, const juce::String& pointsText)
    {
        modEngine.setCustomWave (lfo, table256);
        if (juce::isPositiveAndBelow (lfo, 3)) lfoWavePoints[(size_t) lfo] = pointsText;
    }
    juce::String getLfoWavePoints (int lfo) const
    { return juce::isPositiveAndBelow (lfo, 3) ? lfoWavePoints[(size_t) lfo] : juce::String(); }

    // THE SCALE'S OWN ANCHOR, AS ONE LINE, FOR EVERY RENDERER THAT NEEDS IT.
    //
    // A .scl states INTERVALS and nothing else, so a tuning rendered to .scl form loses any
    // absolute reference it had - and the reload that immediately follows re-pins degree 0
    // to 12-TET middle C. This plugin renders a .scl in FOUR places (export, add a note,
    // edit a degree, delete a degree) and every one of them is reached by an ordinary edit,
    // so a scale anchored anywhere else moved the WHOLE INSTRUMENT the first time the user
    // retyped one key. Measured: an .scl anchored at C = 256 came back at 261.6256 after
    // editing a degree to the value it already had - 37.63 cents, from an edit that changed
    // nothing.
    //
    // "! @ABL REFERENCE_PITCH <octave> <degree> <freq>" is the Ableton .ascl extension this
    // plugin's own parser already honours, so it costs nothing in compatibility: any other
    // Scala host ignores a comment line. Octave 3 degree 0 IS the reference key by that
    // format's definition, which is the key these renderers count their degrees from.
    //
    // Empty when the anchor is already the default, so every .scl this plugin has produced
    // up to now is written byte-identically.
    juce::String sclAnchorLine() const
    {
        const double refHz = tuning.frequencyForNote (TuningManager::kRefKey);
        if (! std::isfinite (refHz) || refHz <= 0.0
            || std::abs (refHz - TuningManager::kRefFreq) <= 1.0e-6)
            return {};
        return "! @ABL REFERENCE_PITCH 3 0 " + juce::String (refHz, 6) + "\n";
    }
    juce::String currentTuningAsScl (const juce::String& name) const; // rationals stay ratios, rest cents
    // What "put the tuning that is playing into the bank" must STORE. For a scale that is
    // the .scl Fibló writes. For an absolute 128-key table there is no .scl to write, so
    // the bank keeps the .tun text itself - currentTuningAsScl() would hand the bank a
    // flattened copy of the user's tuning under the user's own name for it.
    // ...and IN FORCE, not merely loaded: with a table left over from an earlier load and
    // the box back on 12-TET, "put the tuning that is playing into the bank" used to bank
    // the .tun, which is the one tuning that was NOT playing.
    juce::String currentTuningAsBankSource (const juce::String& name) const
    { return tuningTableIsInForce() ? tuning.getScalaSource() : currentTuningAsScl (name); }
    // Scales this synth writes carry the patch they were made with, as Scala
    // comments (invisible to other programs). "Load its original spectrum" reads
    // it back without disturbing the tuning that is loaded.
    static constexpr const char* kPresetTag = "FIBLO-PATCH";
    juce::String embeddedPresetComment() const;
    static juce::ValueTree presetFromScl (const juce::String& sclText);
    bool loadSpectrumFromScl (const juce::String& sclText);
    bool activeSclHasPreset() const { return presetFromScl (tuning.getScalaSource()).isValid(); }
    juce::String activeSclSource() const { return tuning.getScalaSource(); }

    // A .gsting preset carries the tuning it was saved with, but LOADING a preset
    // never applies it - only the sound changes, never the scale under whatever is
    // already loaded/playing. The Tuning panel offers it separately, only when it
    // actually differs from what is active now.
    bool hasPendingPresetTuning() const
    {
        return presetPendingSclSource.isNotEmpty() && presetPendingSclSource != tuning.getScalaSource();
    }
    juce::String getPendingPresetTuningName() const { return presetPendingSclName; }
    bool loadPendingPresetTuning (juce::String& errorOut);

    //==========================================================================
    // SPECTRUM PROVENANCE: the tuning the CURRENT custom spectrum was derived from.
    //
    // Recorded (as name + the FULL source text, like bankSource - a bank slot or a
    // file may be gone later, so the scale text itself is what is stored) by every
    // path that derives a spectrum FROM a tuning:
    //   * Adapt with the TUNING as target (hand-pressed or the SpectrumFromTuning
    //     realtime link) - e.g. the dissonance-minimum spectrum of a tuning;
    //   * a .scl/.tun loaded as the Custom Engine's spectrum (the file IS a tuning,
    //     so "the tuning this spectrum came from" is exactly that file).
    // NOT recorded - the honest rule for each:
    //   * typed intervals and the random generator (no tuning was involved);
    //   * Adapt with the HELD CHORD as target (the notes under the fingers are the
    //     origin, not the scale as a whole - and an MTS/equal-mode tuning could not
    //     be re-loaded from text anyway);
    //   * an adapt run while the active tuning cannot be captured as text
    //     (MTS-ESP / Equal mode: there is no source to bring back later).
    // Cleared whenever the spectrum is replaced by something with no tuning origin
    // (typed text, random, clearTimbre), carried through undo/redo snapshots, the
    // playground's winner corner, save/reload and presets.
    //
    // The Tuning panel shows a "Load spectrum's tuning" button while this exists
    // (disabled when that tuning is already active), which loads it through the
    // normal undoable tuning path - see loadSpectrumProvenanceTuning().
    bool hasSpectrumProvenance() const noexcept
    { return specProvSource.isNotEmpty() && timbreLoaded(); }
    juce::String getSpectrumProvenanceName() const { return specProvName; }
    // Is the provenance tuning the one ACTIVE now? Same text-equality rule as
    // hasPendingPresetTuning, plus the mode: a matching .scl sitting behind
    // MTS/Equal mode is loaded but not in force, so it does not count as active.
    bool spectrumProvenanceIsActive() const
    {
        return specProvSource.isNotEmpty()
            && specProvSource == tuning.getScalaSource()
            && getTuningMode() == (int) TuningManager::Mode::Scala;
    }
    // Load the provenance tuning as the ACTIVE tuning, as one undo transaction
    // (TuningSnapshotAction), so Ctrl+Z brings the displaced tuning straight back.
    bool loadSpectrumProvenanceTuning (juce::String& errorOut);
    // ctrl+click: folded into the period, sorted. exactText, when given, is the
    // spec the user typed, kept verbatim so a ratio stays a ratio in the .scl.
    bool addNoteToTuning  (double ratioFromTonic, juce::String& errorOut,
                           const juce::String& exactText = {});
    bool editTuningDegree (int degree, const juce::String& valueText, juce::String& errorOut); // right-click (Scala mode)
    bool deleteTuningDegree (int degree, juce::String& errorOut); // right-click -> Delete: gone from every period
    bool saveCurrentTuningScl (const juce::File& f, juce::String& errorOut); // Tuning box "Save .scl"

    // Per-degree Sethares dissonance of the ACTIVE tuning (any mode - 12ED2, MTS,
    // Scala, generated), against the CURRENT timbre, for the live scale-analysis
    // list in the Tuning panel. Row for step s in [0, getScaleDegrees()] : degree
    // index s (the tonic is 0), ratio to the tonic, and dissonance height; the last
    // row (s == getScaleDegrees()) is the period. Message thread only (recompiles
    // the spectrum on the calling thread); cheap enough for the editor timer.
    std::vector<DissDegreeInfo> computeActiveScaleDissonance() const;
    double getDissGreenHeight() const noexcept { return dissGreenH; }
    double getDissRedHeight()   const noexcept { return dissRedH; }
    int    getDissRequested()   const noexcept { return dissRequested; }
    int    getDissFound()       const noexcept { return dissFound; }
    int    getDissGeneration()  const noexcept { return dissGeneration; }

    //==========================================================================
    // "From spectrum's segment": use SELECTED partials of the current spectrum as a
    // scale. The LOWEST selected partial is the tonic (1/1, on C3); each selected
    // partial's ratio to it is folded into one PERIOD (the highest selected partial,
    // or a typed ratio/cents), duplicate pitches within the period removed, then
    // loaded as the active .scl - rebuilt live as the selection changes. Editor calls.
    void segmentToggle (int partialIndex, bool rangeToAnchor); // plain click / shift-click
    void segmentClear();
    bool segmentSelected (int partialIndex) const;
    int  segmentTonicIndex() const;   // lowest selected partial index, -1 if none
    int  segmentCount() const;
    void setSegmentPeriodText (const juce::String& s); // "2" / "3/2" / "500c"
    juce::String getSegmentPeriodText() const { return segPeriodText; }
    void setSegmentUseTopPeriod (bool b);
    bool getSegmentUseTopPeriod() const noexcept { return segUseTopPeriod; }
    // Build + load the .scl from the current partial selection. `userInitiated` marks
    // the calls that come from an actual user action: only those may switch the
    // tuning MODE to Scala. The 30 Hz refresh must never override the mode the user
    // chose (that is what made a segment scale impossible to leave).
    void rebuildSegmentScale (bool userInitiated = true);
    // Which Spectrum-to-Tuning mode is active (persisted so it survives reopen).
    void setSegmentModeActive (bool b) noexcept { segmentModeActive = b; if (! b) segSourceByDegree.clear(); }
    bool isSegmentModeActive() const noexcept { return segmentModeActive; }
    // For the keyboard's hover strip: absolute frequency of a MIDI note under the
    // active tuning, and (segment scales only) the source partial ratio n/1 of a
    // scale degree - the harmonic each key represents before octave folding.
    double getNoteFrequency (int midiNote) const { return tuning.frequencyForNote (midiNote); }
    bool   activeScaleIsSegment() const noexcept { return ! segSourceByDegree.empty(); }
    double getSegmentDegreeSource (int degree) const noexcept
    {
        return juce::isPositiveAndBelow (degree, (int) segSourceByDegree.size()) ? segSourceByDegree[(size_t) degree] : 0.0;
    }

    //==========================================================================
    // Shared with the editor:
    juce::MidiKeyboardState keyboardState;
    juce::UndoManager undoManager; // parameter undo/redo (MUST outlive + precede apvts)
    juce::AudioProcessorValueTreeState apvts;
    // Chord-preview injection: the editor queues timestamped MIDI (note+per-channel
    // bend) here; processBlock feeds it to the SYNTH ONLY (never the rhythm engine,
    // never the MIDI out). MidiMessageCollector is thread-safe by design.
    juce::MidiMessageCollector previewCollector;
    juce::MidiBuffer           previewMidi; // reused per block (audio thread)
    // ...AND SO ARE THESE TWO, for the same reason previewMidi is a member and not a
    // local. juce::MidiBuffer holds a juce::Array that ALLOCATES on its first addEvent,
    // so the "held" and "synthMidi" buffers the self-play and preview paths used to build
    // as locals were three heap allocations per block on the audio thread, every block,
    // whenever either path was active - which is the hardest rule this project has, and
    // the one a dropout comes from. Members cleared per block instead: MidiBuffer::clear
    // keeps the capacity, so after the first few blocks nothing allocates at all.
    //
    // One synthMidi for both paths: they are mutually exclusive branches of the same if.
    juce::MidiBuffer           rtHeld, rtSynthMidi;

public:
    // The jingle is ON DEMAND only: clicking the title asks for it. It never plays
    // by itself - not on load, not on a preset/project reload - so it can never
    // sound at a moment the user did not choose. A second request while it is
    // already sounding restarts it from the top rather than overlapping.
    void triggerJingle() noexcept { jingleRestart.store (true, std::memory_order_release); }
    bool isJinglePlaying() const noexcept { return jingleActive; }

private:
    juce::AudioBuffer<float> jingleNative;   // decoded once at construction, file's own rate
    double jingleNativeSr = 44100.0;
    juce::AudioBuffer<float> jingleOut;      // resampled to the session rate, built once
    bool   jingleResampled = false;
    int    jingleReadPos   = 0;
    bool   jingleActive    = false;          // true only while a requested play is running
    std::atomic<bool> jingleRestart { false }; // set by the editor, consumed on the audio thread
    void   playJingleInto (juce::AudioBuffer<float>& buffer);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    spectrum::SpectrumSettings currentSettings() const;
    // The ONE place a set of parameter values plus a custom scale becomes
    // SpectrumSettings. currentSettings() feeds it the live parameters; a playground
    // corner feeds it the preset's own. Two copies of these rules would eventually
    // disagree, and the symptom would be a corner that sounds different on the pad
    // than it does when you simply load it.
    //
    // `get` returns a parameter's REAL (denormalised) value by id. `slope` is passed in
    // because the live patch's slope is not simply the knob (manual mode freezes it, and
    // a modulator mapped to it lowers what has to be compiled) while a corner's is.
    // Temper mode reads the LIVE tuning in both cases - deliberately: the tuning is
    // excluded from the morph, so a tempered corner tempers to whatever is loaded now.
    spectrum::SpectrumSettings settingsFromValues (const std::function<float (const char*)>& get,
                                                  const std::vector<float>& degrees,
                                                  float period, float slope) const;
    void timerCallback() override;
    void addRecentScl (const juce::File&);
    // tuningMode param -> TuningManager mode + synth zone layout. Index 3 = "MPE
    // (bend input)": no internal tuning (equal base) and a REAL MPE lower zone so
    // per-note bends from an MPE controller/host are honoured; other modes run the
    // legacy single-channel layout. Message thread; switching releases held notes.
    void applyTuningMode (int paramIndex);
    bool lastZoneMpe = false;

    // THE ONLY PLACE A .gsting IS WRITTEN, and where the fixed-point rule that closes
    // case #88 is enforced - one gate rather than one check per call site, because
    // there is no such thing as remembering to add the third one. See the .cpp.
    bool writePresetFile (const juce::String& safeName, const juce::ValueTree& state,
                          bool isCornerOwnSave, PresetWriteRefusal& why, int& corner);
    juce::ValueTree captureState();
    void restoreState (const juce::ValueTree&);

    // Declared before `synth`: the AdditiveVoices owned by `synth` hold references
    // to spectrumEngine and tuning, so the voices must be destroyed before them.
    spectrum::SpectrumEngine    spectrumEngine;
    TuningManager               tuning;
    juce::MPESynthesiser        synth;
    spectrum::SpectrumSettings  lastPushed;
    RhythmEngine                rhythm { apvts, spectrumEngine, tuning };
    NoteGate                    noteGate; // quantize-to-tempo / mono / quantize-to-pulse
    MonoGlide                   monoGate; // MONOPHONIC GLIDE's note-stream half (MonoGlide.h)
    // Voices the pool holds when mono glide is OFF. Mono forces it to 1 - which is what
    // makes chords impossible AND what makes every new note a steal, i.e. a takeover the
    // voice can bend through. Tracked here so switching back restores the real number.
    static constexpr int kPolyVoices = 32;
    bool lastMonoGlide = false;           // message thread: last value the voice pool follows
    // The mono shrink is spread over 60 Hz ticks: the notes it has to drop are faded
    // first and the pool follows once they are quiet (see applyMonoVoiceCount). The cap
    // is the backstop for the case where nothing is rendering those fades at all - half a
    // second, after which the ONE-VOICE promise is kept regardless.
    int monoShrinkTicks = 0;
    static constexpr int kMonoShrinkTicks = 30;
    void applyMonoVoiceCount (bool mono); // message thread; see the definition
    //== ADAPTIVE LOOKAHEAD (see LookaheadDelay.h) =============================
    LookaheadDelay laDelay;   // the MIDI is re-stamped `laSamples` later
    LookaheadPeek  laPeek;    // ...and the follower reads it at its ORIGINAL time
    // How many samples of latency the plugin is CURRENTLY reporting and delaying by.
    // Written on the message thread (setLatencySamples must not run on the audio one),
    // read on the audio thread through the atomic.
    std::atomic<int> laSamples { 0 };
    bool  laLastOn   = false;  // last value applyLookahead() acted on
    int   laLastSr   = 0;      // ...and the block/rate the budget was rounded for
    int   laLastBlk  = 0;
    std::atomic<bool> laFlush { false };   // message -> audio: drop what is in flight
    void applyLookahead();     // message thread; see the definition
    // Transport-jump detection, audio thread only. -1e30 = nothing seen yet.
    double laLastPpq = -1.0e30;
    bool   laWasPlaying = false;
    std::atomic<bool>           shepSyncNoBpm { false }; // see shepardSyncHasNoBpm()
    // Set by restoreState() (any thread); consumed at the top of the next processBlock
    // (audio thread) - see there for why. A preset/project load must never leave a
    // note stuck sounding.
    std::atomic<bool>           pendingHardSilence { false };
    // Set by reset() (any thread); consumed in the same place, one block later. It is a
    // SECOND flag rather than a wider pendingHardSilence because the two requests are not
    // the same request: a state swap must not forget where the running pattern was in its
    // cycle, a host reset must. Keeping them apart is what lets reset() also re-zero the
    // rhythm engine without changing what a preset load does.
    std::atomic<bool>           pendingHostReset { false };

    // Scala-as-timbre state (message thread; read into SpectrumSettings each poll).
    std::vector<float> timbreDegrees;
    float              timbrePeriod = 2.0f;
    juce::String       timbreName, timbreSource;
    // SPECTRUM PROVENANCE (see the public block above): the tuning the current custom
    // spectrum was derived from - display name, the full reloadable source text, and a
    // table's period spec (the same triple TuningManager::Snapshot restores from).
    // Kept as text like bankSource, never as a slot/file reference: the slot can be
    // cleared and the file deleted, and the button must still be able to deliver.
    juce::String       specProvName, specProvSource, specProvPeriod;

    //== RANDOM SPECTRUM trigger ("specRandom") =================================
    // Edge detection lives in a parameter LISTENER, not in the 60 Hz poll, because a
    // press writes 0 then 1 back to back: a poll that only ever sees the LEVEL would
    // read 1, then 1 again next tick, and every click after the first would do nothing.
    // The listener is called synchronously by setValueNotifyingHost on whatever thread
    // wrote (the audio thread, for host automation), so it does the minimum possible -
    // raise an atomic flag - and the message-thread timer does the actual work.
    struct RandomTrigListener : juce::AudioProcessorParameter::Listener
    {
        FractalAdditiveAudioProcessor* owner = nullptr;
        void parameterValueChanged (int, float newValue) override
        {
            if (owner == nullptr) return;
            const bool now = newValue >= 0.5f;
            const bool was = owner->randomTrigLevel.exchange (now, std::memory_order_acq_rel);
            if (now && ! was) owner->randomTrigPending.store (true, std::memory_order_release);
        }
        void parameterGestureChanged (int, bool) override {}
    };
    RandomTrigListener randomTrigListener;
    std::atomic<bool>  randomTrigLevel   { false }; // last level the listener saw
    std::atomic<bool>  randomTrigPending { false }; // a rising edge is waiting for the timer
    int                randomSpectrumCount = 0;     // how many have been generated (tests count it)
    // Seeded once from the clock, then advanced by every draw: two presses in the same
    // millisecond still get different spectra, which a per-press time seed would not give.
    juce::Random       randomSpectrumRng { (juce::int64) juce::Time::getHighResolutionTicks() };
    juce::String       lastRandomSpectrumText; // redraw if a draw repeats the previous one

    // Persistent recent-.scl list + UI preferences (global app settings, shared across
    // instances). See setKeyboardNoteNames for why a preference is not patch state.
    std::unique_ptr<juce::PropertiesFile> appProps;
    juce::StringArray                     recentSclPaths;
    bool                                  keyboardNoteNames = false; // OFF by default

    // Tuning bank: per-instance .scl slots (name + raw source), persisted in state.
    std::array<juce::String, kBankSlots> bankName, bankSource;
    int lastBankSlot = 0; // last applied "tuningBankSlot" value (message thread)
    // ...the SOURCE TEXT of the last load ATTEMPTED for that slot, and whether it failed.
    // A slot whose load fails keeps its index, so the index alone could never retry it -
    // not even for the obvious repair of dropping a good file into it. These two let a
    // FAILED slot be re-attempted when its text changes, and only then. See the bank block
    // in timerCallback. Message thread.
    juce::String lastBankSource;
    bool         lastBankFailed = false;

    // Last dissonance-generated .scl (text + display name), for saving to disk,
    // plus the degree/height data shown in the Spectrum-to-Scale panel.
    juce::String lastDissScl, lastDissName;
    juce::String presetPendingSclSource, presetPendingSclName; // see hasPendingPresetTuning()
    // THE SCALE A PROJECT COULD NOT GET BACK, for the editor to show once.
    //
    // setStateInformation runs on a load thread and there may be no editor at all, so the
    // failure is parked here rather than raised there. Consumed by takeTuningRestoreError,
    // which clears it: this is a one-shot message about one load, not a state.
    juce::String tuningRestoreError;
    // WHAT THE BANK COULD NOT DO, for the editor to show once, on the same one-shot
    // pattern and for the same reason: the load runs on the processor's timer (host
    // automation can move that parameter with no window open at all), so the parser's
    // message is parked here rather than raised from a place that may have no editor.
    // Consumed by takeBankSlotError, which clears it - one message about one event.
    // Two events use it: a slot whose stored text will not load, and a bank that is FULL
    // (setTuningBankFile / bankAutoAdd) - the second used to overwrite slot 0 in silence.
    juce::String bankSlotError;
    // THE PRESET THAT DID NOT LOAD, on the same one-shot pattern and for the same reason
    // as the two above. loadPreset returns false from three places - the file is not in
    // the folder, the XML will not parse, the tree is not one of ours - and its only
    // caller drops that bool on the floor ("if (name.isNotEmpty() && loadPreset (name))").
    // So a preset that failed left the NAME in the box, the PREVIOUS preset's sound in the
    // instrument, and the previous preset's tuning on offer in the Tuning panel, with
    // nothing anywhere saying the load had not happened. Three lies for the price of one
    // dropped return value.
    juce::String presetLoadError;
public:
    juce::String takeTuningRestoreError()
    { auto e = tuningRestoreError; tuningRestoreError.clear(); return e; }
    juce::String takeBankSlotError()
    { auto e = bankSlotError; bankSlotError.clear(); return e; }
    // Consumed exactly like the two above - the editor's timer already takes those two
    // and puts them on the notice overlay; this one belongs beside them.
    juce::String takePresetLoadError()
    { auto e = presetLoadError; presetLoadError.clear(); return e; }
private:
    juce::String presetPendingTablePeriod;                     // its table period, if it had one
    // Self-play routing as of the last block: a CHANGE strands notes (see processBlock).
    // `known` starts false so the very first block only records the state - flushing
    // there would kill a note that arrived in that same block.
    bool wasSelfPlay = false, selfPlayKnown = false;
    int  lastTempoSent = -1; // last CC20 BPM sent by the tempo sync (see timerCallback)
    int  undoTxnTick   = 0;  // groups edits into ~1 s undo transactions (see timerCallback)
    // How many gestures are open right now. While this is above zero the ~1 s cut is
    // held off, so a drag that outlasts a second is still ONE Ctrl+Z (see
    // beginUndoGesture for the failure that made this necessary).
    int  undoGestureDepth = 0;

    // MTS-ESP adoption is a ONE-SHOT check at load (see timerCallback), never repeated.
    // Manual per-partial amplitudes: slope is frozen at the value it had when manual was
    // switched on, so switching is silent and the fader is genuinely inert while it lasts.
    std::vector<spectrum::PartialGain> partialGains;
    unsigned partialGainsRev = 0;   // see getPartialGainsRevision()

    float manualSlopeFrozen = 1.0f;
    bool  lastManualMode    = false;

    bool  mtsCheckPending = true;
    // Timer ticks waited before the one-shot above is allowed to decide, so a host that
    // calls prepareToPlay before setStateInformation cannot make a restored instance look
    // brand new. Only ever counted once, at load.
    int   mtsCheckTicks   = 0;
    bool  mtsHadRef       = false; // MTS is currently driving the reference
    float manualRefA      = 440.0f; // the hand-set reference, parked while MTS owns it

    juce::String loadedPresetName; // empty = no preset is currently being tracked for drift
    std::vector<std::pair<juce::String, float>> loadedPresetSnapshot; // paramID -> base value at load/save
    void snapshotLoadedPreset (const juce::String& name);

    // Chord-search worker state (worker writes, message thread reads/copies).
    std::unique_ptr<std::thread> chordThread;
    std::atomic<bool>  chordRunning  { false }, chordCancel { false };
    std::atomic<float> chordProgress { 0.0f };
    std::atomic<int>   chordGeneration { 0 }, chordRequested { 0 }, chordFound { 0 };
    std::atomic<double> chordMinGap { 33.0 };
    mutable juce::CriticalSection chordLock;
    std::vector<ChordHit> chordResults;
    juce::String chordResultsPeriodLine { "2/1" }; // the period the PUBLISHED results were searched in
                                                   // (guarded by chordLock - a new search must not
                                                   // re-label old results until its worker publishes)
    juce::String chordPeriodSclLine { "2/1" };
    double       chordPeriodRatio = 2.0;

    // Spectrum-adaptation worker state.
    std::unique_ptr<std::thread> adaptThread;
    std::atomic<bool>  adaptCancel { false }, adaptRunning { false };
    std::atomic<float> adaptProgress { 0.0f };
    std::atomic<int>   adaptGeneration { 0 };
    mutable juce::CriticalSection adaptLock;
    AdaptInfo adaptResult;
    double    adaptPeriodRatio = 2.0;   // the period applied() writes as the Custom Engine's

    //==========================================================================
    // REALTIME link state (message thread only, driven from timerCallback).
    RtLink       rtLink = RtLink::Off;
    RtSettings   rtSet;
    juce::String rtRefusal;
    int  rtTick = 0, rtLastStart = -1000, rtStarts = 0, rtApplies = 0;
    bool rtPending = false;
    // THE MEASURED LANDING (see getLastRealtimeLandingMs). `rtMovedAtMs` is stamped the
    // first time the chord is seen to have MOVED since the last landing - the moment the
    // clock the lookahead budget is racing starts - and is cleared when the result lands,
    // so a chord that moves twice during one search is timed from its FIRST move (the
    // pessimistic, and honest, end of the interval). Wall clock, message thread only.
    double rtMovedAtMs = -1.0;
    double rtLandingMs = -1.0;
    unsigned int rtSeenSpecGen = 0;
    int  rtSeenChordGen = 0, rtSeenAdaptGen = 0;
    // The held chord the last adaptation was actually AIMED AT - the ratios and the root's
    // real pitch, not a hash. The follower re-adapts only when the notes under the fingers
    // move away from these (see kRtHeldMoveCents): a per-note bend wobbling by a hundredth
    // of a cent is not a new chord, and re-running the search for it produced a visually
    // identical spectrum four times a second and made the link feel stuck.
    std::vector<double> rtHeldTarget;
    double rtHeldF0 = 0.0;
    static constexpr double kRtHeldMoveCents = 1.0;
    double rtTuneSig = -1.0;            // the tuning shape the last adaptation was aimed at

    // WHAT A REALTIME LINK MUST NEVER DO: leave the patch somewhere the user cannot get
    // back from. Sixty transactions a second is not undo, it is a wall - so the follower
    // pushes NOTHING while it runs and instead snapshots the state it displaced when it
    // engaged. Switching the link off then commits ONE transaction from that snapshot to
    // whatever it ended on: Ctrl+Z afterwards restores exactly the tuning/spectrum you
    // had before you switched it on, in a single step, and doing nothing keeps the result.
    struct SpecSnapshot
    {
        juce::String timbreText;
        std::vector<spectrum::PartialGain> gains;
        bool manual = false;
        bool timbreOn = false;
        bool taken = false;
        // The spectrum's tuning provenance travels WITH the spectrum it describes:
        // undoing an adaptation back to a typed spectrum must also take the
        // "Load spectrum's tuning" button away, and redoing must bring it back.
        juce::String provName, provSource, provPeriod;
    };
    TuningManager::Snapshot rtTuneBase;
    int  rtTuneBaseMode = 0;
    bool rtTuneBaseTaken = false;
    SpecSnapshot rtSpecBase;
    void takeRealtimeBaseline (RtLink mode);
    void commitRealtimeBaseline();
    void applySpecSnapshot (const SpecSnapshot& s);
    SpecSnapshot captureSpecSnapshot() const;
    void driveRealtimeLink();

    //== THE PRESET PLAYGROUND (see the public API above) =======================
    // One corner. Everything a blend needs is extracted ONCE, when the preset is
    // dropped here: the parameter values, the custom scale, the per-partial amplitude
    // edits, and the compiled spectrum. Nothing is re-read from disk while dragging.
    struct PlaygroundCorner
    {
        juce::String name;                                   // empty = this corner is unassigned
        // The preset's REAL (denormalised) value for every parameter, indexed by the
        // parameter's position in getParameters() - NOT a map keyed by id. A drag reads
        // all four corners for every parameter, thirty times a second; an id lookup per
        // read is 200x200 string compares per frame for no benefit, because the index
        // order is identical for every corner by construction.
        //
        // TOTAL, ALWAYS. Every parameter has a value here the moment the corner is
        // assigned: the preset's own where the file carries it, the parameter's DEFAULT
        // where the preset PREDATES it (fillCornerDefaults). There used to be a parallel
        // `hasParam` flag meaning "this preset predates that parameter", and every reader
        // answered that state differently - push skipped it (so the corner-edit surface
        // kept the BLEND's value there and the pull absorbed it back into the corner,
        // hunt entry 10), the continuous blend renormalised it away (so the parameter was
        // ruled 100 % by the OTHER corner and the vertex became path-dependent, hunt
        // entry 11), the discrete branch skipped it (so a corner whose preset never
        // mentioned mono was left permanently monophonic, hunt entry 8), while MIX mode's
        // ParamBank::fill quietly used the default. FOUR readers, four answers.
        // Deleting the flag is what makes that unrepresentable: there is now ONE answer,
        // it is the default, it is decided once at assign time, and morph, mix and edit
        // cannot disagree about it because there is nothing left to disagree about.
        std::vector<float> params;
        std::vector<spectrum::PartialGain> gains;
        bool manual = false;                                  // "partialManual"
        juce::String timbreName, timbreSource;                // the Custom Engine's own scale
        // The corner spectrum's tuning provenance, read from the preset's TIMBRE child:
        // when this corner WINS the discrete state, its spectrum arrives with the origin
        // it was saved with - and a winner saved without one clears the button honestly.
        juce::String provName, provSource, provPeriod;
        int  rtLink = 0;                                      // the realtime/adaptive link it was saved with
        RtSettings rtSet;
        // The preset's MODULATOR ROUTING (the MODS child): target-id csv per modulator
        // and the drawn LFO waves. Morph mode never reads these (routing is live-patch
        // state there); MIX mode loads them into the corner's OWN ModEngine, because a
        // preset whose filter breathes under an LFO is not the same preset with the
        // routing stripped. Empty = that modulator drives nothing = it is simply off.
        juce::String modTargets[ModEngine::kMods];
        juce::String modWaves[ModEngine::kLfos];
        // ADAPTIVE-CORNER bookkeeping (message thread; see the public block). The aim is
        // the chord the last launched search was AIMED at - the same "has it moved?"
        // reference the plain follower keeps, per corner because each corner searches
        // with its own settings and on its own turn of the round-robin.
        std::vector<double> rtAimAlphas;
        double rtAimF0 = 0.0;
        bool   rtPend  = false;   // the chord moved: this corner owes a search
        bool   rtDead  = false;   // its settings REFUSED to run: stop retrying, say so
        bool rtAdaptive() const noexcept
        { return assigned() && rtLink == (int) RtLink::SpectrumFromChord && ! rtDead; }
        // shared_ptr, and const: a drag posts a blend job every 16 ms and the worker may
        // still be reading a corner the user has just replaced. Immutable + refcounted is
        // what makes reassigning a corner mid-blend safe without a lock.
        std::shared_ptr<const spectrum::SpectrumTemplate> tmpl;
        // CORNER EDIT (see beginPlaygroundCornerEdit). `edited` is the "*": this corner
        // no longer matches the .gsting it was loaded from. `rev` counts every change to
        // the corner's stored VALUES - the mix sub-engine keys its bank refill on it,
        // because a filter tweak changes no template pointer and would otherwise be
        // inaudible until something else happened to recompile the spectrum.
        bool edited = false;
        int  rev    = 0;
        bool assigned() const noexcept { return name.isNotEmpty(); }
        // THIS CORNER'S value for parameter #i. Always succeeds for a corner that has
        // been assigned (the store is total - see `params`); false only for an index
        // that is not a parameter at all, which is a programming error, not a preset
        // that predates something.
        bool value (int i, double& out) const noexcept
        {
            if (i < 0 || (size_t) i >= params.size()) return false;
            out = (double) params[(size_t) i];
            return true;
        }
    };
    std::array<PlaygroundCorner, kPlaygroundCorners> pgCorner;
    bool  pgActive   = false;   // >= 2 corners assigned: the morph is driving
    int   pgWinner   = -1;      // the corner every discrete setting comes from
    double pgLastX = -1.0, pgLastY = -1.0;
    bool  pgDirty    = true;    // force one full apply (a corner changed, or we just engaged)
    int   pgParamTick = 0;      // throttles the CONTROL writes (see drivePlayground)
    bool  pgParamsPending = false; // a throttled tick still owes the controls one write
    // THE DRAG'S FREQUENCY GLIDE. A drag posts a blend every tick, and every
    // kPgGlideTicks-th of them is published WITH a generation bump so the voices treat
    // it as a real spectrum change: with Morph on the partials BEND to the new blend
    // (the pitch glide morph does everywhere else in this app - the user's request),
    // with Morph off they cross the 50 ms template fade. 10 Hz, the same cadence the
    // adaptive corners already republish at ("a rate the 50 ms fade absorbs" -
    // kRtAdaptTicks); the frames in between still publish bump-less so the AMPLITUDES
    // track at the full 60 Hz. Bumping every frame instead is the documented click
    // machine (sixty morph restarts a second); bumping never is what the user reported:
    // ratios that only ever crossfade in place, "no pitch bend en los parciales".
    int   pgBlendTick = 0;         // frames since engage, clocks the glide throttle
    int   pgLastBump  = -1000000;  // pgBlendTick of the last bumped publish
    bool  pgGlidePending = false;  // a bump-less blend went out after the last bump
    static constexpr int kPgGlideTicks = kRtAdaptTicks;
    // What the realtime link was doing when the playground took the spectrum over, so
    // it can be handed back untouched when the playground lets go.
    RtLink pgSuspendedRt = RtLink::Off;
    bool   pgRtSuspended = false;

    //== CORNER EDIT (see the ownership block in the .cpp) ======================
    // The corner whose values the main editor's controls are currently standing in
    // for, or -1. Everything below exists so that leaving edit mode can hand the LIVE
    // patch back exactly as it was found - it is the frozen live patch, not the corner.
    int             pgEditCorner = -1;
    juce::ValueTree pgEditLiveState;                  // captureState() at entry
    float           pgEditLiveX = 0.0f, pgEditLiveY = 0.0f;   // the point, before pinning
    juce::String    pgEditLivePresetName;             // the live patch's preset identity
    std::vector<std::pair<juce::String, float>> pgEditLivePresetSnap;  // ...and its drift reference
    // The corner recompile is THROTTLED to the glide cadence (kPgGlideTicks, 10 Hz): a
    // full compileSpectrum per 60 Hz frame of a knob drag is the message thread's own
    // click machine. `pending` is the pgParamsPending lesson applied here - a drag that
    // STOPS on a skipped tick must still get its final compile, or the corner would be
    // left permanently one edit short of what the controls show.
    int  pgEditTick = 0;
    bool pgEditCompilePending = false;
    // THE FIRST PULL IS A BASELINE, NOT AN EDIT. Push writes only the parameters the
    // corner's preset actually HAS; pull reads every parameter a corner may own. So the
    // first pull always finds differences - the ones the preset predates, plus whatever
    // a normalise/denormalise round trip costs a skewed range - and without this the "*"
    // would appear the instant Edit was pressed, on a corner nobody had touched. That
    // makes the warning meaningless, which is worse than not having it.
    bool pgEditPrimed = false;

    //== RULE 7: A CORNER-EDIT SESSION IS NOT PART OF THE LIVE PATCH'S HISTORY ===
    //
    // The scratch surface writes through the SAME UndoManager as the live patch. Every
    // knob turned and every partial drawn while editing a corner therefore landed on the
    // live patch's undo stack as a step that WRITES INTO THE LIVE PATCH, and the whole
    // gesture is one the shipped GUI offers: Edit a corner, drag the pencil, click OK,
    // press Ctrl+Z. Measured before this fix:
    //     after leaving edit mode  gains=1 [190196:-9.00] cutoff=1611
    //     after one Ctrl+Z         gains=0               cutoff=6543
    //     after Ctrl+Y             gains=1 [336883:11.00] cutoff=1611
    // - the user's hand-drawn spectrum wiped, the corner's cutoff on the live patch, and
    // redo installing the CORNER's drawing there. Hand-drawn amplitudes exist nowhere
    // else, so that first keystroke destroyed work with no way back.
    //
    // The cure is that the session leaves the stack as it found it. Two halves:
    //   * the actions this file mints (gains, timbre) are simply not minted while a
    //     session is open - the corner's own rev carries the edit;
    //   * the APVTS parameter writes cannot be intercepted (APVTS holds the undo manager
    //     by const pointer and flushes on its own timer), so they are UNWOUND on the way
    //     out, back through a boundary marker pushed on the way in.
    //
    // Undoing is not the same as dropping, and dropping is what one would want - but
    // JUCE's UndoManager cannot drop transactions. clearUndoHistory() takes the user's
    // own history with it, which is the same data-loss bug wearing a different hat.
    //
    // The marker itself. It does nothing to the patch in either direction; it exists so
    // the unwind knows where the session's own transactions start and, above all, where
    // to STOP - undoing one transaction too many would eat the user's own last edit.
    struct CornerEditBoundaryAction : public juce::UndoableAction
    {
        explicit CornerEditBoundaryAction (FractalAdditiveAudioProcessor& p) : proc (p) {}
        bool perform() override { return true; }   // a marker performs nothing
        bool undo()    override { proc.pgEditUnwound = true; return true; }
        int  getSizeInUnits() override { return 16; }
        FractalAdditiveAudioProcessor& proc;
    };
    // ...and the ONE step the whole session leaves behind, in place of the dozens it used
    // to leave. It writes only the CORNER - never the live patch - so neither Ctrl+Z nor
    // Ctrl+Y can put somebody's corner on the user's sound. It is also the only way to
    // clear the redo stack the unwind fills: JUCE drops future transactions when a new
    // action is performed and by no other route. Undoing it takes back the corner edits,
    // which is a step this feature never had at all.
    struct CornerEditSessionAction : public juce::UndoableAction
    {
        CornerEditSessionAction (FractalAdditiveAudioProcessor& p, int c,
                                 PlaygroundCorner prev, PlaygroundCorner next)
            : proc (p), corner (c), after (std::move (next)), before (std::move (prev)) {}
        bool perform() override { return applyOne (after); }
        bool undo()    override { return applyOne (before); }
        int  getSizeInUnits() override
        { return (int) ((before.params.size() + after.params.size()) * sizeof (float)) + 512; }
        bool applyOne (const PlaygroundCorner& c)
        {
            auto& dst = proc.pgCorner[(size_t) corner];
            const int rev = dst.rev;
            dst = c;
            // The mix sub-engine keys its bank refill on rev, and the blend needs a
            // generation bump: without both, taking the edits back would be inaudible
            // until something else happened to recompile the spectrum.
            dst.rev = rev + 1;
            proc.pgDirty = true;
            return true;
        }
        FractalAdditiveAudioProcessor& proc;
        int corner;
        PlaygroundCorner after, before;
    };
    // Raised by the marker's undo(): "the unwind has reached the start of the session".
    bool pgEditUnwound = false;
    // The corner as it stood BEFORE the session, so leaving can hand the session to
    // CornerEditSessionAction as one undoable step.
    PlaygroundCorner pgEditCornerBefore;
    // How deep the undo stack was once the marker was on it. The unwind stops on the
    // marker; this stops it anyway if the marker has been dropped off the bottom of a
    // full stack, because unwinding past it would be eating the user's own history.
    int pgEditStackDepth = 0;

    // Write corner `corner`'s stored values onto the live control surface, and read the
    // surface back into the corner. Both skip every parameter presetmorph excludes -
    // the tuning, the reference, the transpose and the playground's own point are NOT
    // part of a corner and must survive a round trip untouched.
    void pushCornerToLiveSurface (int corner);
    void pullPlaygroundCornerEdit();
    // Every parameter at its DEFAULT - the base every corner store is built on, and the
    // one answer to "what does this corner mean for a parameter its preset predates?".
    void fillCornerDefaults (PlaygroundCorner& c) const;

    //== ADAPTIVE CORNERS (see the public block). Message thread except where noted. ==
    // One finished corner search. It carries its OWN identity (corner + preset name) and
    // the chord it was AIMED at, because by the time it is consumed the corner may have
    // been reassigned and the chord may have moved - both of which make it stale, and a
    // stale adaptation is DROPPED, never applied late (same rule as the plain follower).
    struct CornerAdaptResult
    {
        int corner = -1;
        juce::String cornerName;
        std::vector<double> ratios, amps;   // ascending, ratios[0] == 1.0
        double period = 2.0;
        std::vector<double> aimAlphas;
        double aimF0 = 0.0;
        bool valid = false;
    };
    CornerAdaptResult pgAdaptResult;        // guarded by adaptLock (worker writes, timer reads)
    std::atomic<int>  pgAdaptGeneration { 0 };
    int  pgSeenAdaptGen = 0;
    int  pgAdaptLastStart = -1000;          // in rtTick units: same launch floor as the follower
    int  pgAdaptNext = 0;                   // round-robin cursor over the adaptive corners
    int  pgAdaptInFlight = -1;              // corner whose search is running (-1 = none)
    int  pgAdaptStarts = 0, pgAdaptApplies = 0;
    juce::String pgAdaptRefusal;
    // A corner preset's value for a parameter, falling back to the parameter's DEFAULT
    // (never the live patch) when the preset predates it - shared by every corner
    // compile so old presets always mean the same thing.
    float cornerValueOr (const PlaygroundCorner& c, const char* id) const;
    // Launch one corner's adaptation on the shared worker ("never two at once" is
    // adaptRunning, exactly as the follower uses it).
    bool startCornerAdapt (int corner, const std::vector<double>& alphas, double f0,
                           juce::String& errorOut);
    // Write a finished result into its corner: adapted intervals as the corner's typed
    // spectrum, adapted amplitudes BAKED into the corner's compiled template.
    void applyCornerAdaptResult (PlaygroundCorner& c, const CornerAdaptResult& r);
    // Consume finished results, watch the held chord, launch what is owed. Called from
    // drivePlayground while the playground is engaged.
    void drivePlaygroundAdaptive();

    void drivePlayground();
    void applyPlaygroundDiscreteState (int corner);
    void applyPlaygroundContinuous (const double w[kPlaygroundCorners]);
    void postPlaygroundBlend (const double w[kPlaygroundCorners], bool bumpGeneration);
    void refreshPlaygroundActive();

    //== PLAYGROUND MIX MODE (see the public block). ============================
    // One corner's sub-engine. Declaration order IS destruction safety: the synth's
    // AdditiveVoices hold references to `spectrum`, `mods` and `bank`, so the synth
    // (declared last) must be destroyed first - same rule the processor's own members
    // follow for the main engine.
    struct MixCornerEngine
    {
        explicit MixCornerEngine (FractalAdditiveAudioProcessor& p)
            : bank (p), mods (p.apvts, &bank)
        {
            synth.setVoiceStealingEnabled (true);
            synth.enableLegacyMode (48);   // caller mirrors the live zone/bend right after
        }
        // Polyphony follows the VISIBLE "pgVoices" control (measured: five 32-voice
        // engines are ~3x a 48 kHz/512 block on dense spectra, so a fixed 32 would be
        // an inaudible-until-it-crackles trap and a fixed lower number a silent cap).
        // Message thread; MPESynthesiser's own voice lock serialises this against the
        // audio thread, and reduceNumVoices steals sounding voices properly.
        // Growing is free. SHRINKING has the same two audible faults applyMonoVoiceCount
        // documents at length, and for the same reason: reduceNumVoices() DELETES sounding
        // voices, and a deleted voice stops mid-waveform because the object that would
        // render a fade is gone. This is reached from the 60 Hz poll whenever the user
        // turns "Mix voices per corner" down, which they will do with keys held - four
        // sub-engines make it the control people reach for when the CPU meter climbs.
        //
        // Same two-step cure. FADE FIRST: every sounding voice above the new count is
        // ASKED to fade over its own 5 ms fast kill, through the atomic the voice consumes
        // at the top of its own render - beginFastKill() from this thread would mutate
        // ADSR state and currentlyPlayingNote underneath a block the audio thread is
        // rendering, and MPESynthesiser's voicesLock is not reachable from outside.
        // DELETE SECOND, on a later tick, once they are quiet: findFreeVoice() hands
        // reduceNumVoices the INACTIVE voices first, so the survivors are chosen inside
        // JUCE under its own lock rather than by index surgery from here.
        //
        // Unlike mono, there is no last-note contract to honour - this is a polyphony
        // budget, not an articulation - so the newest `want` voices stay and the oldest
        // fade, which is what a player running out of voices expects.
        void setVoices (FractalAdditiveAudioProcessor& p, int want)
        {
            want = juce::jlimit (1, 32, want);
            while (synth.getNumVoices() < want)
                synth.addVoice (new AdditiveVoice (p.apvts, spectrum, p.tuning, mods, &bank));
            if (synth.getNumVoices() <= want) { shrinkTicks = 0; return; }

            // Rank the sounding voices by age so the oldest are the ones asked to go.
            std::vector<juce::MPESynthesiserVoice*> live;
            for (int v = 0; v < synth.getNumVoices(); ++v)
                if (auto* mv = synth.getVoice (v); mv != nullptr && mv->isActive())
                    live.push_back (mv);

            if ((int) live.size() > want && shrinkTicks < kMixShrinkTicks)
            {
                ++shrinkTicks;
                std::sort (live.begin(), live.end(),
                           [] (auto* a, auto* b) { return a->noteOnTime > b->noteOnTime; });
                // Re-asked every tick, not once: a key pressed DURING the shrink takes the
                // newest slot and must not be the one that goes.
                for (size_t i = (size_t) want; i < live.size(); ++i)
                    if (auto* av = dynamic_cast<AdditiveVoice*> (live[i]))
                        av->requestFadeOut();
                return;   // the delete waits for the fades; re-entered from the 60 Hz tick
            }

            // Either the fades landed, or nothing is rendering them - a stopped transport,
            // a bypassed plugin - and the polyphony the user asked for must not wait for
            // ever on a voice that will never be asked for another sample.
            shrinkTicks = 0;
            synth.reduceNumVoices (want);
        }
        // Half a second of 60 Hz ticks; a 5 ms fade has landed a hundred times over.
        static constexpr int kMixShrinkTicks = 30;
        int shrinkTicks = 0;
        ParamBank                 bank;      // the corner preset's values (see ParamBank.h)
        spectrum::SpectrumEngine  spectrum;  // holds the corner's compiled template
        ModEngine                 mods;      // the corner's own modulators, reading `bank`
        juce::MPESynthesiser      synth;
        juce::AudioBuffer<float>  buf;       // render scratch, sized on the message thread
        // What the bank/routing/template currently describe, so a 60 Hz tick only
        // refreshes when the corner actually changed (assign, or an adaptive apply).
        std::shared_ptr<const spectrum::SpectrumTemplate> lastTmpl;
        juce::String lastName;
        // ...and which REVISION of the corner's stored values. A corner edit that moves
        // only voice parameters leaves the template pointer alone, so without this the
        // sub-engine would keep the values it was built with and the user would hear
        // nothing move (the exact "the store changed but the sound did not" trap).
        int lastRev = -1;
    };
    // The message thread OWNS the engines (built on demand, kept for the processor's
    // lifetime - never deleted mid-session, so the audio thread can hold a pointer
    // across a block without a lock); the atomics are the audio thread's view,
    // release-stored only once an engine is fully built.
    std::array<std::unique_ptr<MixCornerEngine>, kPlaygroundCorners> mixEng;
    std::array<std::atomic<MixCornerEngine*>, kPlaygroundCorners>    mixEngLive {};
    std::atomic<bool>  mixEngaged { false };                     // message -> audio
    std::atomic<float> mixGainTarget[kPlaygroundCorners] {};     // sqrt(w) per corner
    bool pgMixDriving  = false;   // message thread: the mix path ran this tick
    bool pgLastMixMode = false;   // detects morph<->mix flips (forces a full re-apply)
    // Audio-thread-only mix state (touched in renderSynthMix and nowhere else).
    float mixXfade = 0.0f;                       // 0 = main engine alone, 1 = corners alone
    float mixGainCur[kPlaygroundCorners] {};     // smoothed per-corner gains
    float mixApplied[kPlaygroundCorners] {};     // gain actually applied last block (ramp start)
    float mixMainApplied = 1.0f;
    bool  mixWasOn = false;                      // edge detect: engage re-injects held keys
    bool  mixKilled = false;                     // corner voices killed after fade-out
    int   mixTailBlocks = 0;                     // muted blocks so the 5 ms kill can finish
    double curBpm = -1.0, curPpq = -1.0, curF0 = 0.0; // this block's transport (audio thread)
    juce::AudioBuffer<float> mixScratch;         // the main engine's bus during a crossfade
    juce::MidiBuffer         mixInject;          // reused: held keys re-struck on engage
    int lastBlockSize = 512;

    void drivePlaygroundMix (const double w[kPlaygroundCorners]);
    void buildMixEngine (int corner);
    void refreshMixEngine (int corner);
    void renderSynthMix (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, int nSamp);
    // Mirror the live zone layout / bend range onto a sub-synth (corners follow the
    // LIVE tuning plumbing - the tuning is global, exactly as the morph promises).
    void applyZoneTo (juce::MPESynthesiser& s) const
    {
        if (lastZoneMpe)
        {
            juce::MPEZoneLayout zone;
            zone.setLowerZone (15, 48, 48);   // same numbers as applyTuningMode, same reason
            s.setZoneLayout (zone);
        }
        else
        {
            s.enableLegacyMode (lastBendApplied);
        }
    }
    // Where a parameter sits in getParameters(). -1 if there is no such parameter.
    int  paramIndexOf (const char* id) const;
    // The realtime/adaptive link as saved in a preset/project, and back again. It is
    // PART OF THE PRESET (a patch that follows what you play is not the same patch with
    // the follower off), so it travels in the state like everything else.
    juce::ValueTree realtimeSettingsToTree() const;
    RtSettings      realtimeSettingsFromTree (const juce::ValueTree&) const;
    // What the user asked for, which is not always what is running: the playground
    // stands the follower down while it owns the spectrum, and saving must record the
    // arming the user chose rather than the suspension.
    RtLink effectiveRealtimeLink() const noexcept
    { return pgRtSuspended ? pgSuspendedRt : rtLink; }
    // Build a corner's SpectrumSettings from ITS values (never the live ones), then
    // compile it. Shares settingsFromValues with currentSettings(), so a corner and the
    // live patch can never disagree about what a set of numbers means.
    void compileCornerTemplate (PlaygroundCorner& c);

    // One transaction that carries the whole SPECTRUM (Custom-Engine text + per-partial
    // amplitudes + the manual switch) from one snapshot to another. The individual pieces
    // already have undo actions of their own, but a realtime adaptation moves all three
    // at once and they have to travel as ONE step or a single Ctrl+Z would leave the
    // amplitudes of one spectrum sitting on the ratios of another.
    struct SpecSnapshotAction : public juce::UndoableAction
    {
        SpecSnapshotAction (FractalAdditiveAudioProcessor& p, SpecSnapshot prev, SpecSnapshot next)
            : proc (p), after (std::move (next)), before (std::move (prev)) {}
        bool perform() override { proc.applySpecSnapshot (after);  return true; }
        bool undo() override    { proc.applySpecSnapshot (before); return true; }
        int  getSizeInUnits() override
        { return (int) ((after.gains.size() + before.gains.size()) * sizeof (spectrum::PartialGain)
                        + (size_t) (after.timbreText.length() + before.timbreText.length()) * 2) + 64; }
        FractalAdditiveAudioProcessor& proc;
        SpecSnapshot after, before;
    };
    // The same for the TUNING, through TuningManager's own whole-tuning snapshot.
    struct TuningSnapshotAction : public juce::UndoableAction
    {
        TuningSnapshotAction (FractalAdditiveAudioProcessor& p, TuningManager::Snapshot prev, int prevMode,
                              TuningManager::Snapshot next, int nextMode)
            : proc (p), after (std::move (next)), before (std::move (prev)),
              afterMode (nextMode), beforeMode (prevMode) {}
        bool perform() override { return applyOne (after, afterMode); }
        bool undo() override    { return applyOne (before, beforeMode); }
        int  getSizeInUnits() override
        { return (int) (after.source.length() + before.source.length()) * 2 + 64; }
        bool applyOne (const TuningManager::Snapshot& s, int mode)
        {
            juce::String err;
            if (s.source.isNotEmpty() && ! proc.tuning.restoreTuning (s, err))
                return false;
            if (auto* p = proc.apvts.getParameter ("tuningMode"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) mode));
            ++proc.dissGeneration;      // the keyboard + the degree list are showing a scale
            return true;
        }
        FractalAdditiveAudioProcessor& proc;
        TuningManager::Snapshot after, before;
        int afterMode = 0, beforeMode = 0;
    };

    // n-ad worker state.
    std::unique_ptr<std::thread> nadThread;
    std::atomic<bool> nadCancel { false }, nadBusy { false }, nadOverBudget { false },
                      nadNoPeriod { false };
    std::atomic<int>  nadGeneration { 0 }, nadUsedD { 0 }, nadTotalD { 0 }, nadResultD { 0 };
    mutable juce::CriticalSection nadLock;
    std::vector<NadRow> nadResults;

    // Modulator engine state.
    ModEngine    modEngine { apvts };
    juce::String lfoWavePoints[3];   // drawn-wave breakpoints (for re-editing)
    juce::uint32 modNoteOns = 0;     // running note-on counter (envelope retrigger)
    int          modHeld    = 0;     // notes currently held (audio thread; envelope gate)
    std::array<std::array<bool, 128>, 16> modKeyHeld {}; // which (channel, key) are down
    std::vector<DissDegreeInfo> dissDegrees;
    double dissGreenH = 0.0, dissRedH = 1.0;
    int    dissRequested = 0, dissFound = 0, dissGeneration = 0;

    // "From spectrum's segment" selection state (message thread).
    std::set<int> segSel;              // selected partial indices in the live template
    int           segAnchor = -1;      // last clicked (shift-click range anchor)
    juce::String  segPeriodText = "2"; // period ratio/cents when not using the top partial
    bool          segUseTopPeriod = false;
    bool          segmentModeActive = false; // Spectrum-to-Tuning mode (persisted)
    std::vector<double> segSourceByDegree;   // degree -> source partial ratio n/1 (segment scales)
    juce::uint32  segRebuiltGen = 0;          // template generation the segment scale was built from
    // Drop the segment selection + source mapping when the active tuning becomes
    // something else (a loaded .scl, a Sethares scale, 12ED2) so the keyboard stops
    // showing stale "from n/1" labels and the timer stops rebuilding over it.
    void clearSegmentSource() noexcept { segSourceByDegree.clear(); segSel.clear(); segAnchor = -1; }

    float lastGain = 0.0f;
    double lastSr  = 0.0; // from prepareToPlay (getSampleRate() can be 0 in offline harnesses)
    int   lastBendApplied = 48; // matches enableLegacyMode(48) in the constructor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessor)
};
