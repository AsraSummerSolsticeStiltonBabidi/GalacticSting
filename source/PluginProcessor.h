#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <set>
#include <thread>

#include "dsp/AdditiveVoice.h"
#include "dsp/Dissonance.h"
#include "dsp/ModEngine.h"
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
    // Write the CURRENT tuning as an AnaMark .tun (v2). This is the primary interchange
    // format now: unlike .scl it carries the reference frequency, so a scale exported
    // here reopens at the same absolute pitch instead of losing its A and being guessed
    // at by whatever loads it. Every MIDI note is written out, so a mapping of any size
    // survives - a .scl would need a .kbm alongside it to say the same thing.
    bool saveCurrentTuningTun (const juce::File& f, juce::String& errorOut)
    {
        const double f0 = tuning.frequencyForNote (0);
        if (! (f0 > 0.0) || ! std::isfinite (f0))
        { errorOut = "The current tuning has no usable pitch for MIDI note 0."; return false; }

        // What Fiblo exports is a SCALE whenever it HAS one, never a scale it merely
        // guessed at. A .tun can legitimately hold a non-repeating mapping - a stretched
        // piano curve, per-note tweaks - and Fiblo now holds exactly that as an absolute
        // 128-key table; for those the [Scale Begin] block is omitted rather than filled
        // with the estimated period, which would state as fact a structure the tuning
        // does not have. The 128 exact pitches below carry it losslessly either way, and
        // reloading the file lands back in table mode.
        const bool   isTable = tuning.usesAbsoluteTable();
        const int    degrees = tuning.degreesPerPeriod();
        const double period  = tuning.periodRatio();
        if (! isTable && (degrees <= 0 || ! (period > 1.0001) || ! std::isfinite (period)))
        {
            errorOut = "This tuning has no repeating scale behind it (an MTS-ESP master can "
                       "supply an arbitrary 128-key table), so it cannot be written as a scale. "
                       "Nothing was saved.";
            return false;
        }

        const auto centsAt = [this, f0] (int n)
        {
            const double fn = tuning.frequencyForNote (n);
            return (fn > 0.0 && std::isfinite (fn)) ? 1200.0 * std::log2 (fn / f0) : 0.0;
        };

        juce::String s;
        s << "; AnaMark TUN v2 - written by Fiblo Galactic\n"
          << "; " << getTuningStatus() << "\n\n";

        if (isTable)
        {
            // Said in a comment, so no reader can mistake it for structure: the period is
            // Fiblo's estimate (or the user's own choice), not something the file repeats at.
            const auto est = tuning.getTablePeriodEstimate();
            s << "; 128-key tuning table - this mapping does not repeat, so it declares no scale.\n"
              << "; period used inside Fiblo: " << juce::String (1200.0 * std::log2 (period), 6)
              << " cents (" << (tuning.tablePeriodIsUserSet() ? "chosen by the user" : "Fiblo's estimate")
              << ", " << juce::String (tuning.tablePeriodDevCents(), 2) << " cents off at worst"
              << (est.found ? "" : ", nothing to measure") << ")\n\n";
        }
        else
        {
            // Declare the SCALE, so the file says what it is instead of leaving a reader to
            // infer a period from 128 numbers. Degrees as cents above 1/1, then the period
            // as the closing interval - the same shape a .scl has, which is what makes this
            // a scale file rather than a table that happens to repeat.
            s << "[Scale Begin]\n"
              << "; " << degrees << " degrees per period of "
              << juce::String (1200.0 * std::log2 (period), 6) << " cents\n";
            for (int d = 1; d <= degrees; ++d)
            {
                const double r = d < degrees ? getDegreeRatio (d) : period;
                s << "degree " << d << "=" << juce::String (1200.0 * std::log2 (std::max (1.0e-9, r)), 6) << "\n";
            }
            s << "[Scale End]\n\n";
        }

        s << "[Tuning]\n";
        for (int n = 0; n < 128; ++n)
            s << "note " << n << "=" << juce::String ((int) std::llround (centsAt (n))) << "\n";
        // The exact block is what actually carries the pitch: integer cents above are
        // only the coarse fallback for readers that predate it.
        s << "\n[Exact Tuning]\nbasefreq=" << juce::String (f0, 10) << "\n";
        for (int n = 0; n < 128; ++n)
            s << "note " << n << "=" << juce::String (centsAt (n), 6) << "\n";

        if (! f.replaceWithText (s)) { errorOut = "Could not write " + f.getFullPathName(); return false; }
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
        // No scale to ask (MTS, an absolute 128-key table): read the KEY the degree lands
        // on, which for a table is the file's own pitch. Keys past 127 are walked DOWN by
        // whole periods rather than clamped - a table with a wide period spans more than
        // 68 keys per period, and clamping made every degree above that report key 127.
        int key = 60 + step, extra = 0;
        while (key > 127) { key -= D; ++extra; }
        while (key < 0)   { key += D; --extra; }
        const double f0 = tuning.frequencyForNote (60);
        const double f  = tuning.frequencyForNote (juce::jlimit (0, 127, key));
        return f0 > 0.0 ? (f / f0) * std::pow (per, (double) (wraps + extra)) : 1.0;
    }

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
        if (getTuningMode() == (int) TuningManager::Mode::Scala && ! tuning.usesAbsoluteTable())
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

    //==========================================================================
    // Timbre -> rhythm MIDI out (see RhythmEngine.h). The editor edits the slots
    // directly (atomics) and reads the live pulse rates for its panel.
    RhythmEngine& getRhythm() noexcept { return rhythm; }

    //==========================================================================
    // Shepard tone generator (Tuning panel). The engine itself is stateless about
    // the transport: processBlock resolves the quantized mode's beats against the
    // host BPM and hands the engine plain seconds. Exposed for the verifiers
    // (probe of the live components) and the editor (the honesty flag below).
    ShepardEngine& getShepard() noexcept { return shepard; }
    // TRUE while the rate control is in Sync mode but the host offers no BPM, so
    // the FREE (ms) value is what actually drives the rise. The editor shows this
    // state instead of letting the beats box claim a sync that is not happening.
    bool shepardSyncHasNoBpm() const noexcept { return shepSyncNoBpm.load (std::memory_order_relaxed); }
    const spectrum::SpectrumTemplate* getLiveTemplate() const noexcept { return spectrumEngine.getLive(); }
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
    void commitPartialGains (std::vector<spectrum::PartialGain> before,
                             std::vector<spectrum::PartialGain> after)
    {
        if (before == after) return;                 // a click that moved nothing
        undoManager.beginNewTransaction();
        undoManager.perform (new PartialGainsAction (*this, std::move (before), std::move (after)));
    }
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
    // The corner's compiled template, so a test can prove it CHANGES when the chord does.
    std::shared_ptr<const spectrum::SpectrumTemplate> getPlaygroundCornerTemplateForTests (int corner) const
    {
        return juce::isPositiveAndBelow (corner, kPlaygroundCorners) ? pgCorner[(size_t) corner].tmpl
                                                                     : nullptr;
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
    int  getNadUsedDegrees()  const noexcept { return nadUsedD.load(); }
    int  getNadTotalDegrees() const noexcept { return nadTotalD.load(); }
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
        // ONE NUMBER, and it is a COUNT OF SCALE STEPS: the range runs from the anchor key
        // (see registerAnchorKey) to `regSteps` degrees of the loaded tuning above it. Two
        // absolute bounds could not do this - they were pinned to fixed pitches, so the
        // adaptation answered with the same spectrum whichever register was actually being
        // played, and they had to be re-read every time the tuning changed. A step count is
        // the same musical width in any tuning (24 in 12-EDO, 62 in 31-EDO, 26 in
        // Bohlen-Pierce are all two periods) and it FOLLOWS the hands.
        //
        // < 0 means "the default width", which is kAdaptRegisterDefaultPeriods periods of
        // whatever scale is loaded - so a follower running with the window shut still spans
        // the register being played instead of collapsing to one pitch.
        int    regSteps  = -1;
    };
    // The KEY the register range hangs off: the lowest note being HELD, else the last one
    // played, else 60. In that order and for that reason - the hand-pressed Adapt must have
    // a defensible anchor without the user holding a key down while they reach for the
    // mouse, and "middle C" is only ever the answer before a single note has been played.
    int registerAnchorKey() const noexcept;
    // How many steps the DEFAULT range spans under the scale loaded right now.
    int defaultRegisterSteps() const noexcept;
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
    };
    void setRealtimeLink (RtLink mode, const RtSettings& s);
    void updateRealtimeSettings (const RtSettings& s) { rtSet = s; }
    RtLink getRealtimeLink() const noexcept { return rtLink; }
    // Non-empty when a link REFUSED to run (and switched itself off). Never silent.
    juce::String getRealtimeRefusal() const { return rtRefusal; }
    // Counters for the honesty read-out AND for the throttle tests: how many searches the
    // follower actually launched, and how many results it actually applied.
    int getRealtimeStarts()  const noexcept { return rtStarts; }
    int getRealtimeApplies() const noexcept { return rtApplies; }
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
        if (apvts.getRawParameterValue ("timbreTemper")->load() > 0.5f) return EngineInForce::Tempered;
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

    juce::String currentTuningAsScl (const juce::String& name) const; // rationals stay ratios, rest cents
    // What "put the tuning that is playing into the bank" must STORE. For a scale that is
    // the .scl Fibló writes. For an absolute 128-key table there is no .scl to write, so
    // the bank keeps the .tun text itself - currentTuningAsScl() would hand the bank a
    // flattened copy of the user's tuning under the user's own name for it.
    juce::String currentTuningAsBankSource (const juce::String& name) const
    { return tuning.usesAbsoluteTable() ? tuning.getScalaSource() : currentTuningAsScl (name); }
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
    // The Shepard tone (Tuning panel). Runs after the synth mix, before master gain,
    // so the master fader still scales it with everything else.
    ShepardEngine               shepard;
    std::atomic<bool>           shepSyncNoBpm { false }; // see shepardSyncHasNoBpm()
    // Set by restoreState() (any thread); consumed at the top of the next processBlock
    // (audio thread) - see there for why. A preset/project load must never leave a
    // note stuck sounding.
    std::atomic<bool>           pendingHardSilence { false };

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
    juce::String presetPendingTablePeriod;                     // its table period, if it had one
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
        std::vector<float> params;
        std::vector<char>  hasParam;   // 0 = this preset predates that parameter
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
        bool assigned() const noexcept { return name.isNotEmpty(); }
        // The preset's value for parameter #i, or false if this preset predates it.
        bool value (int i, double& out) const noexcept
        {
            if (i < 0 || (size_t) i >= params.size() || ! hasParam[(size_t) i]) return false;
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
    // What the realtime link was doing when the playground took the spectrum over, so
    // it can be handed back untouched when the playground lets go.
    RtLink pgSuspendedRt = RtLink::Off;
    bool   pgRtSuspended = false;

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
        void setVoices (FractalAdditiveAudioProcessor& p, int want)
        {
            want = juce::jlimit (1, 32, want);
            while (synth.getNumVoices() < want)
                synth.addVoice (new AdditiveVoice (p.apvts, spectrum, p.tuning, mods, &bank));
            if (synth.getNumVoices() > want)
                synth.reduceNumVoices (want);
        }
        ParamBank                 bank;      // the corner preset's values (see ParamBank.h)
        spectrum::SpectrumEngine  spectrum;  // holds the corner's compiled template
        ModEngine                 mods;      // the corner's own modulators, reading `bank`
        juce::MPESynthesiser      synth;
        juce::AudioBuffer<float>  buf;       // render scratch, sized on the message thread
        // What the bank/routing/template currently describe, so a 60 Hz tick only
        // refreshes when the corner actually changed (assign, or an adaptive apply).
        std::shared_ptr<const spectrum::SpectrumTemplate> lastTmpl;
        juce::String lastName;
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
