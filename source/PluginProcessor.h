#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <set>
#include <thread>

#include "dsp/AdditiveVoice.h"
#include "dsp/ModEngine.h"
#include "dsp/NoteGate.h"
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
    // Host reset (transport re-arm, state clear): flush any sounding rhythm-MIDI
    // notes so none hang in the receiving track.
    void reset() override { rhythm.flushOpenGates(); }
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
    juce::String getTimbreName()   const { return timbreName; }
    // The exact text a typed Spectrum-Builder spectrum was entered as. It is saved in
    // the state, but the box that edits it had no way to read it back, so a reopened
    // window showed an empty field and the spectrum could only be edited by retyping
    // it from memory.
    juce::String getTimbreSource() const { return timbreSource; }
    int  getTimbreNoteCount() const noexcept { return (int) timbreDegrees.size(); }

    // Degrees per period of the active tuning (for the colour keyboard).
    int getScaleDegrees() const noexcept { return tuning.degreesPerPeriod(); }

    // True when EVERY degree sits one equal step from the last (any N-EDO, or any
    // scale that happens to be equal-stepped). In such a tuning a chord shape has
    // the SAME intervals wherever you start it, so a result found on degree 0 is
    // valid from every degree - which is what the n-ad list shows with "any".
    // Same 0.5-cent criterion the n-ad search itself uses to root at 0.
    bool tuningIsEqualStep() const
    {
        const int D = tuning.degreesPerPeriod();
        if (D < 2) return false;
        const double periodC = 1200.0 * std::log2 (std::max (1.0e-9, tuning.periodRatio()));
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

    // Frequency ratio of scale degree `d` relative to degree 0 (the tonic on
    // key 60), for the keyboard's just-interval labels. Degree d is mapped to an
    // IN-RANGE key of the same degree class (walking down whole periods) rather
    // than clamping the MIDI note, so dense scales (72-EDO etc.) label correctly.
    double getDegreeRatio (int d) const
    {
        const int D = juce::jmax (1, tuning.degreesPerPeriod());
        const double per = tuning.periodRatio();
        // Degree index -> (period count, step inside the period), floored so negative
        // degrees work too. The PERIOD factor is applied explicitly, never inferred
        // from where the degree happens to land on the keyboard.
        const int wraps = (int) std::floor ((double) d / (double) D);
        const int step  = d - wraps * D;
        // Scala: read the scale's own degree ratio. The 128-key table only holds 128
        // degree classes, so a scale with more degrees than that CANNOT be read back
        // through it (degrees 68..D-61 used to all collapse onto key 127's value).
        double r = 0.0;
        if (tuning.scalaDegreeRatio (step, r))
            return r * std::pow (per, (double) wraps);
        const double f0 = tuning.frequencyForNote (60);
        const double f  = tuning.frequencyForNote (juce::jlimit (0, 127, 60 + step));
        return f0 > 0.0 ? (f / f0) * std::pow (per, (double) wraps) : 1.0;
    }

    // Genuine JI test BY SOURCE: is scale degree `d` (0..getScaleDegrees(), where the
    // top index is the period) a rational-sourced interval, and if so its exact p/q?
    //  - Scala mode (incl. segment + dissonance scales built as .scl): from the .scl's
    //    per-degree source, so a ratio degree "3/2" is genuine but a cents degree
    //    "701.955" is NOT (it only approximates one).
    //  - Any other mode (equal divisions, MTS, MPE): only the unison and an exact 2/1
    //    octave are rational; every tempered step is shown as cents (+ "≈p/q *").
    // The one exact cents interval, 1200c = 2/1, is the period case handled here.
    bool getDegreeIsRational (int d, long long& p, long long& q) const
    {
        const int D = juce::jmax (1, tuning.degreesPerPeriod());
        if (getTuningMode() == (int) TuningManager::Mode::Scala)
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
    void clearTimbre();
    // The Custom Engine's on/off is a real PARAMETER, so it is automatable and -
    // the reason it moved here - Ctrl+Z undoes it like every other control. It only
    // counts as on while a scale is actually loaded.
    bool isTimbreActive() const noexcept
    { return ! timbreDegrees.empty() && apvts.getRawParameterValue ("timbreOn")->load() > 0.5f; }

    // Recently-used .scl files (persisted across sessions, newest first, max 3).
    juce::StringArray getRecentSclPaths() const { return recentSclPaths; }

    //==========================================================================
    // Timbre -> rhythm MIDI out (see RhythmEngine.h). The editor edits the slots
    // directly (atomics) and reads the live pulse rates for its panel.
    RhythmEngine& getRhythm() noexcept { return rhythm; }
    const spectrum::SpectrumTemplate* getLiveTemplate() const noexcept { return spectrumEngine.getLive(); }

    // Per-partial amplitude edits. The authoritative copy lives HERE, because it has to
    // be saved with the project; the spectrum engine only ever holds a resolved
    // index-aligned copy for the voices. Message thread.
    void setPartialGains (std::vector<spectrum::PartialGain> ov, bool on)
    {
        partialGains = ov;
        spectrumEngine.setOverrides (std::move (ov), on);
    }
    const std::vector<spectrum::PartialGain>& getPartialGains() const noexcept { return partialGains; }
    // How many of those edits currently sit on a partial that actually exists. The rest
    // are not lost - their partial is simply not in this spectrum right now.
    int getPartialGainsMatched() const noexcept { return spectrumEngine.getGainMatchedCount(); }
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
    // Every loaded tuning joins the bank. Returns the 0-based slot it landed in (or
    // an existing duplicate's), -1 if the bank is full.
    int  bankAutoAdd (const juce::String& name, const juce::String& source);
    void syncBankSelector (int slot0); // point the selector at a slot without re-loading it
    void bankClearSlot (int slot0);                                          // the bank's X button

    //==========================================================================
    // File presets (.gsting). Called from the editor.
    static juce::File getPresetFolder();
    juce::StringArray getPresetNames();
    juce::String savePreset (const juce::String& name); // returns the stored name, or empty on failure
    bool loadPreset (const juce::String& name);
    // True once ANY parameter's BASE value (a knob turned, host automation, undo...)
    // no longer matches what was in the loaded preset - modulation from the
    // preset's OWN envelopes/LFOs never counts: it writes only the raw atomic
    // (see ModEngine.h), never the base value this compares against.
    bool presetHasDrifted() const;
    juce::String getLoadedPresetName() const { return loadedPresetName; }

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
    bool loadChordAsScale (int index, juce::String& errorOut);

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
    int  getNadUsedDegrees()  const noexcept { return nadUsedD.load(); }
    int  getNadTotalDegrees() const noexcept { return nadTotalD.load(); }
    std::vector<NadRow> getNadResults() const
    {
        const juce::ScopedLock sl (nadLock);
        return nadResults;
    }

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

    // Does the Custom-Engine scale REPLACE the harmonic engine (temper mode off)?
    // With temper ON both run together, so the engine's controls stay live.
    bool timbreReplacesEngine() const noexcept
    { return isTimbreActive() && ! (apvts.getRawParameterValue ("timbreTemper")->load() > 0.5f); }

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

    juce::String currentTuningAsScl (const juce::String& name) const; // rationals stay ratios, rest cents
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
    NoteGate                    noteGate; // quantize-to-tempo / mono / quantize-to-pulse
    // Set by restoreState() (any thread); consumed at the top of the next processBlock
    // (audio thread) - see there for why. A preset/project load must never leave a
    // note stuck sounding.
    std::atomic<bool>           pendingHardSilence { false };

    // Scala-as-timbre state (message thread; read into SpectrumSettings each poll).
    std::vector<float> timbreDegrees;
    float              timbrePeriod = 2.0f;
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
    juce::String presetPendingSclSource, presetPendingSclName; // see hasPendingPresetTuning()
    // Self-play routing as of the last block: a CHANGE strands notes (see processBlock).
    // `known` starts false so the very first block only records the state - flushing
    // there would kill a note that arrived in that same block.
    bool wasSelfPlay = false, selfPlayKnown = false;
    int  lastTempoSent = -1; // last CC20 BPM sent by the tempo sync (see timerCallback)
    int  undoTxnTick   = 0;  // groups edits into ~1 s undo transactions (see timerCallback)

    // MTS-ESP adoption is a ONE-SHOT check at load (see timerCallback), never repeated.
    // Manual per-partial amplitudes: slope is frozen at the value it had when manual was
    // switched on, so switching is silent and the fader is genuinely inert while it lasts.
    std::vector<spectrum::PartialGain> partialGains;

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

    // n-ad worker state.
    std::unique_ptr<std::thread> nadThread;
    std::atomic<bool> nadCancel { false }, nadBusy { false }, nadOverBudget { false };
    std::atomic<int>  nadGeneration { 0 }, nadUsedD { 0 }, nadTotalD { 0 };
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
