#pragma once

//==============================================================================
// Additive MPE voice: renders the shared spectrum template as a bank of sine
// partials.
//
// Click-free design:
//   - Note-on from an IDLE voice: scatter phases, reset the envelope to 0, snap
//     the partial amplitudes to the template (crisp attack).
//   - Note-on that REUSES an active voice (voice stealing): MPESynthesiser calls
//     noteStarted() directly without a noteStopped(false), so we must NOT reset
//     here. We keep phases and envelope continuous, so the steal is click-free.
//   - Template swap while a note is held (the user edits a spectrum parameter):
//     partial FREQUENCIES snap (a frequency jump with continuous phase does not
//     click) but partial AMPLITUDES slew toward their new values over a few ms, so
//     the spectrum morphs smoothly instead of stepping (no zipper). Partials that
//     appear fade in from 0; partials that disappear fade out to 0.
//
// Fidelity: the ONLY partials skipped are those above Nyquist at the current pitch
// (they would alias). No audible partial is dropped here.
//==============================================================================

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "Spectrum.h"
#include "SpectrumEngine.h"
#include "CurvedEnv.h"
#include "SineTable.h"
#include "TuningManager.h"
#include "ModEngine.h"

#include <array>
#include <cmath>

class AdditiveVoice : public juce::MPESynthesiserVoice
{
public:
    AdditiveVoice (juce::AudioProcessorValueTreeState& state, const spectrum::SpectrumEngine& eng,
                   const TuningManager& tuningManager, const ModEngine& modulators)
        : apvts (state), engine (eng), tuning (tuningManager), mods (modulators),
          sine (spectrum::sineTable())
    {
        attackParam  = apvts.getRawParameterValue ("ampAttack");
        decayParam   = apvts.getRawParameterValue ("ampDecay");
        sustainParam = apvts.getRawParameterValue ("ampSustain");
        releaseParam = apvts.getRawParameterValue ("ampRelease");
        phaseModeParam = apvts.getRawParameterValue ("phaseMode");

        fCutoffParam = apvts.getRawParameterValue ("filterCutoff");
        fResParam    = apvts.getRawParameterValue ("filterResonance");
        fEnvAmtParam = apvts.getRawParameterValue ("filterEnvAmount");
        fAttackParam = apvts.getRawParameterValue ("filterAttack");
        fDecayParam  = apvts.getRawParameterValue ("filterDecay");
        fSustainParam= apvts.getRawParameterValue ("filterSustain");
        fReleaseParam= apvts.getRawParameterValue ("filterRelease");

        pEnvAmtParam = apvts.getRawParameterValue ("pitchEnvAmount");
        pAttackParam = apvts.getRawParameterValue ("pitchAttack");
        pDecayParam  = apvts.getRawParameterValue ("pitchDecay");
        pSustainParam= apvts.getRawParameterValue ("pitchSustain");
        pReleaseParam= apvts.getRawParameterValue ("pitchRelease");

        curveSlopeParam = apvts.getRawParameterValue ("curveSlope");
        manualParam     = apvts.getRawParameterValue ("partialManual");
        ampCurveA = apvts.getRawParameterValue ("ampCurveA");
        ampCurveD = apvts.getRawParameterValue ("ampCurveD");
        ampCurveR = apvts.getRawParameterValue ("ampCurveR");
        filtCurveA = apvts.getRawParameterValue ("filterCurveA");
        filtCurveD = apvts.getRawParameterValue ("filterCurveD");
        filtCurveR = apvts.getRawParameterValue ("filterCurveR");
        pitchCurveA = apvts.getRawParameterValue ("pitchCurveA");
        pitchCurveD = apvts.getRawParameterValue ("pitchCurveD");
        pitchCurveR = apvts.getRawParameterValue ("pitchCurveR");

        morphParam     = apvts.getRawParameterValue ("morph");
        morphTimeParam = apvts.getRawParameterValue ("morphTime");

        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    //==========================================================================
    void noteStarted() override
    {
        // An MTS-ESP master may leave this key unmapped -> it must not sound.
        if (tuning.shouldFilterNote ((int) currentlyPlayingNote.initialNote))
        {
            // If this note-on was a STEAL, the voice is still rendering the previous
            // note. clearCurrentNote() alone only tells the synthesiser we are free -
            // the envelope stays active, so renderNextBlock keeps producing sound for
            // a note nobody owns any more and no note-off can ever reach: a permanent
            // drone. Fade it out properly instead.
            if (adsr.isActive() || fastKill) beginFastKill();
            else                             finishNote();
            clearCurrentNote();
            return;
        }

        // Is this note-on REUSING a still-sounding voice (voice steal)? JUCE's
        // MPESynthesiser starts a stolen voice by calling noteStarted() directly
        // on the active voice - with no noteStopped() first. Hard-resetting here
        // (envelope to 0, phases scrambled) would step the output discontinuously
        // = an audible click/glitch at the tail of the stolen note, most likely
        // when the 32-voice pool runs dry during fast playing. So on reuse we keep
        // the envelope, phases and filter CONTINUOUS and just re-attack + slew the
        // amplitudes toward the new note: a click-free hand-off.
        const bool reuse = adsr.isActive() || fastKill;

        // Deterministic note-on (same note twice = identical). Two phase modes:
        //   Aligned  - every partial starts at its peak (cosine), so they form a coherent
        //              pulse at the difference-tone rate -> a defined tone, not noise.
        //   Scattered- pseudo-random phases decorrelate the partials into a soft/noisy bed.
        // Skipped on reuse (keeping the running phases avoids a discontinuity).
        if (! reuse)
        {
            const bool aligned = phaseModeParam->load() < 0.5f;
            for (int i = 0; i < spectrum::kMaxPartials; ++i)
            {
                if (aligned)
                    phase[(size_t) i] = 0.25; // sin(2*pi*0.25) = 1 -> all partials at their peak
                else
                {
                    const double h = std::sin ((double) i * 12.9898) * 43758.5453;
                    phase[(size_t) i] = h - std::floor (h);
                }
            }
        }

        // Velocity and pressure are GAIN terms, so on a steal they must glide like the
        // amplitudes do: assigning them outright stepped the output by up to ~15 dB
        // (velocity) or ~9 dB (pressure) in a single sample - exactly the click this
        // reuse path exists to avoid. On a cold start the envelope begins at zero, so
        // there they are set directly.
        velTarget = 0.10f + 0.90f * currentlyPlayingNote.noteOnVelocity.asUnsignedFloat();
        if (! reuse) velocityLevel = velTarget;
        // Do NOT seed from currentlyPlayingNote.pressure: under enableLegacyMode() JUCE
        // returns the CHANNEL'S sticky last-received aftertouch, which legacy mode never
        // resets on note-off - so a note on an aftertouch-touched channel would start up
        // to ~9 dB louder than one on a fresh channel. Start at rest; genuine aftertouch
        // still swells in through notePressureChanged().
        pressureTarget = 0.0f;
        if (! reuse)
        {
            pressure = 0.0f;
            // ...but if the pressure that BELONGS to this note arrives with it (an MPE
            // host sends it before the first block renders), take it immediately
            // instead of gliding up to it. Gliding turned every fresh note into a
            // 35%->100% swell over ~30 ms - an attack the envelope never asked for,
            // most obvious when several notes are struck at once. The steady level is
            // unchanged; only the ramp to it is gone. Anything arriving LATER is real
            // aftertouch and still glides.
            pressureSnap = true;
        }

        fastKill = false;
        // A steal that interrupts a fast kill keeps the faded gain and ramps it back
        // up with the same slew (jumping straight to 1 was a second step).
        if (! reuse) fastKillGain = 1.0f;
        snapAmps     = ! reuse; // cold: snap amps to the template; reuse: slew them (no jump)
        releasing    = false;   // fresh note: parameter re-apply is allowed again

        // Sync lastGen to the LIVE template's own stamp (what the first renderNextBlock
        // will read) so a steal is never mistaken for a spectrum change and no spurious
        // morph fires.
        if (const auto* t = engine.getLive()) lastGen = t->generation;
        else                                  lastGen = engine.getGeneration();
        // Reset the morph state on EVERY note-on, INCLUDING a steal. A stolen voice must
        // not inherit the previous note's in-flight morph (morphActive / morphN /
        // morphTgtLog / morphPos) - that would keep it gliding to the OLD note's target
        // spectrum, then snap abruptly when the stale morph finishes. prevN=0 also makes
        // the first block cleanly SNAP curRatio onto the current template (a frequency
        // snap with continuous phase = click-free) instead of gliding from a stale ratio.
        morphActive = false; morphPos = 1.0; prevN = 0; morphWeight = 0.0;
        morphWPos = 0.0; morphFadeOut = false;
        // The amp glide starts a note at the 7 ms de-click minimum, steal included: a
        // stolen voice's amplitudes must hand over to the NEW note in milliseconds, not
        // inherit a seconds-long constant from the old note's in-flight morph.
        ampGlideCur = 0.007; tmplGlideSamps = 0;

        // Retrigger this voice's modulators. A stolen voice retriggers too: it is a new
        // note, and its envelopes must start from the beginning like any other.
        modNewNote = true;
        modHeld    = true;

        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        updateAdsrParameters();
        adsr.setSampleRate (sr);
        if (! reuse) adsr.reset(); // cold: start from silence; reuse: keep the envelope continuous
        // The comment here always CLAIMED the reuse case re-attacks from the current
        // level, but noteOn() zeroed it unconditionally - so a steal dropped the amp
        // to silence in one sample, which is the click the whole reuse path exists to
        // avoid. Only the AMP envelope does this: handing `reuse` to the filter
        // envelope would let a stolen voice inherit filterEnvValue ~1 (its sustain
        // defaults to 1.0) and skip its sweep entirely.
        adsr.noteOn (reuse);

        updateFilterAdsrParameters();
        filterAdsr.setSampleRate (sr);
        if (! reuse) { filterAdsr.reset(); filter.reset(); filterEnvValue = 0.0f; }
        filterAdsr.noteOn();

        updatePitchAdsrParameters();
        pitchAdsr.setSampleRate (sr);
        if (! reuse) { pitchAdsr.reset(); pitchEnvValue = 0.0f; }
        pitchAdsr.noteOn();
    }

    void noteStopped (bool allowTailOff) override
    {
        modHeld = false; // this voice's modulator envelopes go to their release
        if (allowTailOff) { adsr.noteOff(); filterAdsr.noteOff(); pitchAdsr.noteOff(); releasing = true; }
        else              beginFastKill();
    }

    // The transport, pushed in once per block by the processor: a per-voice LFO can be
    // locked to the host timeline exactly like the global one.
    void setTransportForMod (double bpm, double ppq) noexcept { modBpm = bpm; modPpq = ppq; }

    // Aftertouch is a gain term too: set the TARGET so a jumpy 7-bit stream glides.
    void notePressureChanged() override
    {
        pressureTarget = currentlyPlayingNote.pressure.asUnsignedFloat();
        // Delivered together with the note-on (before its first block): that is this
        // note's OWN starting pressure, not a gesture, so it applies at once.
        if (pressureSnap) pressure = pressureTarget;
    }
    void notePitchbendChanged() override {}
    void noteTimbreChanged() override    {}
    void noteKeyStateChanged() override  {}

    void setCurrentSampleRate (double newRate) override
    {
        if (currentSampleRate != newRate)
        {
            currentSampleRate = newRate;
            const double sr = newRate > 0.0 ? newRate : 44100.0;
            adsr.setSampleRate (sr);
            filterAdsr.setSampleRate (sr);
            pitchAdsr.setSampleRate (sr);
            filter.prepare ({ sr, 8192, 1 });
            // CurvedEnv works in SECONDS, so a sample-rate change needs no re-apply
            // dance: the times are already right, only its sample rate had to change.
        }
    }

    //==========================================================================
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr.isActive() && ! fastKill)
            return;

        // THIS VOICE's modulators, advanced by one block. Its own envelopes, its own LFO
        // phases, retriggered by its own note-on - so a chord is six independent
        // envelopes, not one shared envelope restarted by the last key pressed.
        {
            double vals[ModEngine::kMods] = {};
            bool   act [ModEngine::kMods] = {};
            const double lfoF0 = tuning.frequencyForNote ((int) currentlyPlayingNote.initialNote);
            mods.evaluateMods (modState, numSamples, modNewNote, modHeld,
                               lfoF0, modBpm, modPpq, vals, act, false);
            mods.collectVoiceOffsets (vals, act, modOff, modOwner);
            modNewNote = false;
        }

        updateAdsrParameters();
        updateFilterAdsrParameters();
        updatePitchAdsrParameters();

        // Snapshot the live template: FREQUENCIES snap, AMPLITUDES are the slew target.
        // Pitch comes from the active tuning (12-TET / MTS-ESP / Scala) for the base
        // note, with any MPE per-note pitch bend AND the pitch envelope riding on top.
        // Pitch env: amount (semitones) * the envelope value [0..1], evaluated once
        // per block (like the filter cutoff) - the env itself advances per sample.
        int n = 0;
        // 0..1 crossfade weight from the running phases -> the coherent alignPhase.
        // It is a MEMBER because it must survive a template change: restarting the morph
        // clock (which happens on every new template) must never DROP it mid-fade, since
        // a falling weight is a per-partial waveform step. It rises monotonically during a
        // morph and is only released to 0 at the two safe points named in the invariant
        // below (weight already 1, or the fade deliberately ramped out by morphFadeOut).
        double& morphW = morphWeight;
        // Per-sample increment for that weight, filled in by the morph block below and
        // consumed inside the render loop. Advancing the weight per BLOCK was the cause of
        // the loud clicks: at any Morph Time shorter than one buffer it stepped 0 -> 1
        // between two samples.
        double morphWStep = 0.0;
        const auto* tmpl = engine.getLive();
        const double baseFreq = tuning.frequencyForNote ((int) currentlyPlayingNote.initialNote);
        const double pitchEnvSemis = (double) modded (ModEngine::PvPEnvAmt, pEnvAmtParam)
                                   * (double) pitchEnvValue;
        const double f0 = baseFreq * std::pow (2.0,
            ((double) currentlyPlayingNote.totalPitchbendInSemitones + pitchEnvSemis) / 12.0);

        // No infrasound gate: a very low fundamental (wide-period tunings can map a
        // key to a few Hz) is not silenced, because its HARMONICS - at 2x, 3x, ...
        // the fundamental - are audible even when the fundamental itself is not, so
        // muting the whole note would wrongly kill an audible tone. A sub-audio
        // fundamental just adds inaudible rumble; the note still sounds through its
        // partials. (The colour keyboard's range adapts to the period so such deep
        // keys are not normally offered in the first place.)
        if (tmpl != nullptr && currentSampleRate > 0.0 && f0 > 0.0)
        {
            const double maxRatio = (0.5 * currentSampleRate) / f0; // Nyquist / f0
            const auto&  ps       = tmpl->partials;
            const int    count    = std::min ((int) ps.size(), spectrum::kMaxPartials);

            // Sub-Nyquist partial count of the CURRENT template.
            int newN = 0;
            for (int i = 0; i < count; ++i)
            {
                if ((double) ps[(size_t) i].ratio > maxRatio) break; // sorted asc -> rest are higher
                newN = i + 1;
            }

            // Spectral MORPHING: when the template CHANGES, each partial glides from
            // where it is NOW to its new position over Morph Time (a per-partial pitch
            // bend) instead of snapping. Mapping is rank-wise (old rank j -> new rank j),
            // which always produces a real glide when the spectra differ; if the count
            // grew, the extra new partials appear from their nearest old one (split);
            // if it shrank, the extra old partials converge to their nearest new one and
            // fade (merge). A change mid-morph just restarts the glide FROM the current
            // (already-gliding) state - it never snaps. Morphing off -> instant snap.
            // Use the LIVE TEMPLATE'S OWN generation stamp (read via the same acquire
            // that got `tmpl`), NOT engine.getGeneration(): the latter is a separate
            // atomic that can read one publish ahead of `live`, which made a voice
            // morph toward the OLD template yet mark lastGen as the NEW one - so it
            // stuck on the wrong spectrum until re-triggered.
            const juce::uint32 gen = tmpl->generation;
            const bool morphOn = morphParam != nullptr && morphParam->load() > 0.5f;
            if (gen != lastGen)
            {
                if (morphOn && ! snapAmps && prevN > 0 && newN > 0)
                {
                    const int lo = std::min (prevN, newN);
                    const int hi = std::max (prevN, newN);

                    // THE invariant that makes a morph restart click-free:
                    //   alignPhase[] is only ever WRITTEN while the crossfade weight is 0,
                    //   phase[]      is only ever WRITTEN while that weight is 1.
                    // At those two points the two oscillator banks are respectively
                    // inaudible and numerically identical, so neither assignment can step
                    // the output. Anything in between is left strictly alone.
                    //
                    // The old code wrote both arrays at an arbitrary weight - re-seeding
                    // alignPhase whenever the weight was <= 0.5 and adopting it whenever it
                    // was > 0.5 - so EVERY template change stepped every partial at once,
                    // coherently. In Aligned mode alignPhase is the same constant 0.25 for
                    // all of them, which makes that step a maximal impulse rather than a
                    // smear, and two voices seeded identically stepped in phase and added.
                    // With an LFO on Base (a new template ~60x a second) that was a
                    // continuous train of near-full-scale clicks.
                    //
                    // This adoption runs BEFORE the glide below is re-aimed: the mappings
                    // must start from the ratios the listener is actually hearing.
                    const bool wasMorphing = morphActive;
                    const int  seedSpan    = std::max (hi, activeCount); // every slot the render loop touches
                    if (wasMorphing && morphWeight >= 0.999)
                    {
                        // Fully crossfaded: the output already IS the aligned bank, so hand
                        // BOTH its phases AND its (possibly still gliding) ratios back to the
                        // running bank and release the weight. Copying the phases alone and
                        // re-seeding the glide from curRatio - a ratio the listener stopped
                        // hearing when the weight reached 1 - jumped the audible frequencies
                        // BACKWARDS mid-glide on every adoption: one of the "trompicon"
                        // stumbles under the realtime-adaptive template stream, where
                        // templates land faster than the morph completes.
                        for (int j = 0; j < seedSpan; ++j)
                        {
                            phase[(size_t) j] = alignPhase[(size_t) j];
                            if (alignRatio[(size_t) j] > 1.0e-12) // a slot the aligned bank never held keeps its own ratio
                                curRatio[(size_t) j] = alignRatio[(size_t) j];
                        }
                        morphWeight = 0.0; morphWPos = 0.0;
                    }
                    else if (! wasMorphing)
                    {
                        // A morph that ENDED already adopted its phases (see the tail of
                        // renderNextBlock), so the weight can be released here without a step.
                        morphWeight = 0.0; morphWPos = 0.0;
                    }

                    for (int j = 0; j < lo; ++j) // shared ranks: straight old[j] -> new[j]
                    {
                        morphStartLog[(size_t) j] = std::log2 (std::max (1.0e-12, curRatio[(size_t) j]));
                        morphTgtLog  [(size_t) j] = std::log2 (std::max (1.0e-12, (double) ps[(size_t) j].ratio));
                    }
                    if (newN > prevN) // grew: extra new partials start from their nearest old
                    {
                        int k = 0;
                        for (int j = prevN; j < newN; ++j)
                        {
                            const double tr = (double) ps[(size_t) j].ratio;
                            while (k + 1 < prevN
                                   && std::abs (curRatio[(size_t)(k+1)] - tr) <= std::abs (curRatio[(size_t) k] - tr)) ++k;
                            morphStartLog[(size_t) j] = std::log2 (std::max (1.0e-12, curRatio[(size_t) k]));
                            morphTgtLog  [(size_t) j] = std::log2 (std::max (1.0e-12, tr));
                        }
                    }
                    else if (prevN > newN) // shrank: extra old partials converge to nearest new, then fade
                    {
                        int k = 0;
                        for (int j = newN; j < prevN; ++j)
                        {
                            const double srr = curRatio[(size_t) j];
                            while (k + 1 < newN
                                   && std::abs ((double) ps[(size_t)(k+1)].ratio - srr) <= std::abs ((double) ps[(size_t) k].ratio - srr)) ++k;
                            morphStartLog[(size_t) j] = std::log2 (std::max (1.0e-12, srr));
                            morphTgtLog  [(size_t) j] = std::log2 (std::max (1.0e-12, (double) ps[(size_t) k].ratio));
                        }
                    }
                    for (int j = 0; j < hi; ++j)
                        curRatio[(size_t) j] = std::exp2 (morphStartLog[(size_t) j]);

                    morphN = hi; morphActive = true; morphFadeOut = false;
                    // EVERY new target restarts the glide clock. Keeping the old
                    // morphPos was what made a continuous move (a knob drag, or a
                    // modulator: both re-emit a template ~60x a second) degenerate
                    // into a staircase - once morphPos had run up to ~1, each new
                    // template was applied at full strength immediately, i.e. a hard
                    // snap per rebuild. morphStartLog below is re-aimed from where the
                    // partials ACTUALLY are, so restarting the clock is seamless.
                    morphPos = 0.0;

                    // Coherent phase reference for the DESTINATION (as if the new
                    // spectrum were freshly played now); the audio crossfades from the
                    // current running phases to this over the morph, so the note ends
                    // phase-coherent (a real pulse in Aligned mode) instead of a drifted
                    // smear. Without this the sustain after a morph sounds "wrong".
                    // Seeded ONLY while the crossfade weight is 0 (see the invariant above):
                    // at any other weight this array is actively feeding the output, so
                    // writing it is a waveform step on every single template change.
                    if (morphWeight <= 0.0)
                    {
                        const bool alg = phaseModeParam->load() < 0.5f;
                        for (int j = 0; j < seedSpan; ++j)
                        {
                            if (alg) alignPhase[(size_t) j] = 0.25;
                            else { const double h = std::sin ((double) j * 12.9898) * 43758.5453;
                                   alignPhase[(size_t) j] = h - std::floor (h); }
                        }
                        // A fresh crossfade: the aligned bank is inaudible, so it can sit
                        // directly AT the destination frequencies for the whole glide
                        // (unchanged behaviour when templates come slower than the morph).
                        for (int j = 0; j < hi; ++j)
                            alignStartLog[(size_t) j] = morphTgtLog[(size_t) j];
                    }
                    else
                    {
                        // Mid-crossfade: the aligned bank is AUDIBLE at the current weight,
                        // so its frequencies must GLIDE to the new destination just like the
                        // running bank's. Snapping alignInc here (the old behaviour) was the
                        // loudest of the adaptive-stream stumbles: once the weight neared 1,
                        // the entire audible spectrum stepped onto each new template
                        // instantly - morph on, yet audibly not morphing.
                        for (int j = 0; j < hi; ++j)
                            alignStartLog[(size_t) j] = alignRatio[(size_t) j] > 1.0e-12
                                ? std::log2 (alignRatio[(size_t) j])
                                : morphTgtLog[(size_t) j]; // a slot the bank never held starts at target
                    }
                }
                else if (morphActive && morphWeight > 1.0e-6)
                {
                    // Snapping (Morph switched off, or an empty template) while a crossfade
                    // is part-way through: dropping the term outright removes
                    // morphWeight * (aligned - running) from every partial in one sample,
                    // which is the same coherent step as above. Ramp it out instead.
                    morphFadeOut = true;
                    if (! snapAmps) tmplGlideSamps = (int) (kTmplGlideHold * currentSampleRate);
                }
                else
                {
                    morphActive = false; morphFadeOut = false;
                    morphWeight = 0.0; morphWPos = 0.0;
                    // Morph OFF: the ratios snap below (phase-continuous, no click), but the
                    // AMPLITUDES get a real fade-out/fade-in of kTmplGlideSec instead of the
                    // bare 7 ms de-click slew. 7 ms is right for hiding a step; over a large
                    // timbre change it still READS as a hard cut ("una transicion subita y
                    // abrupta"). This is deliberately NOT the morph machinery - the user
                    // wants the two decoupled - just a longer target for the same amp slew.
                    // snapAmps (a cold note) is excluded: a fresh note starts ON its target.
                    if (! snapAmps) tmplGlideSamps = (int) (kTmplGlideHold * currentSampleRate);
                }
                lastGen = gen;
            }

            if (morphActive && morphFadeOut)
            {
                // Leaving a morph early: the ratios snap (a frequency move on a continuous
                // phase is inaudible) but the crossfade WEIGHT is walked down to 0 over
                // ~5 ms by the render loop, so the aligned bank is removed gradually
                // instead of vanishing between two samples.
                morphWStep = -1.0 / (0.005 * currentSampleRate);
                for (int j = 0; j < newN; ++j)
                    curRatio[(size_t) j] = (double) ps[(size_t) j].ratio;
            }
            else if (morphActive)
            {
                const double mt = std::max (0.001, (double) morphTimeParam->load());
                morphPos += (double) numSamples / (mt * currentSampleRate);
                const double t = std::min (1.0, morphPos);
                // Phase-lock PROGRESSIVELY across the WHOLE morph (gentle ease-in that
                // reaches perfect coherence exactly at the end - the Morph Time), so the
                // pulse comes into phase very gradually rather than snapping near the end.
                // The weight now advances PER SAMPLE in the render loop (see morphWStep);
                // only the ratio glide is evaluated once per block.
                //
                // The 5 ms floor applies to the WEIGHT ALONE. Morph Time keeps its full
                // 1 ms minimum for the ratios, but a phase hand-over shorter than ~5 ms
                // cannot be made click-free: it switches the entire partial bank onto the
                // coherent reference, and in Aligned mode every partial sits at its peak
                // there, so the step is a full-scale impulse rather than a smear.
                morphWStep = 1.0 / (std::max (0.005, mt) * currentSampleRate);
                for (int j = 0; j < morphN; ++j)
                    curRatio[(size_t) j] = std::exp2 (morphStartLog[(size_t) j]
                                            + (morphTgtLog[(size_t) j] - morphStartLog[(size_t) j]) * t);
                for (int j = morphN; j < newN; ++j)         // partials that appeared mid-glide
                    curRatio[(size_t) j] = (double) ps[(size_t) j].ratio;
            }
            else
            {
                for (int j = 0; j < newN; ++j)
                    curRatio[(size_t) j] = (double) ps[(size_t) j].ratio; // snapped
            }

            // Render up to morphN during a morph (old extras keep sounding as they
            // converge, then fade); the target amps come from the current template,
            // with any partial past its end fading to 0.
            n = morphActive ? std::max (morphN, newN) : newN;
            for (int j = 0; j < n; ++j)
                tgtAmp[(size_t) j] = (j < newN) ? ps[(size_t) j].amp : 0.0;

            // PER-VOICE SLOPE - the one spectrum control a voice can move on its own.
            // The compiler gives each base harmonic amp = (k/kMin)^(-slope) and the
            // fractal convolution only multiplies those, so a published amplitude is
            // exactly ratio^(-slope) times a factor that does NOT depend on slope.
            // Rescaling by ratio^-(dS) therefore lands on precisely the spectrum the
            // compiler would have produced at this voice's slope - no recompile, no
            // approximation, and nothing dropped: every partial is still here, just at a
            // different amplitude. The per-sample normalisation downstream is derived
            // from these targets, so loudness follows automatically.
            // MANUAL mode: the amplitudes belong to the user's own per-partial edits, so
            // slope does nothing at all - not the fader, not a modulator pointed at it.
            // The template was compiled at the frozen slope, so there is nothing to
            // rescale from either; skipping the whole thing is what makes it inert.
            const bool manualAmps = manualParam != nullptr && manualParam->load() > 0.5f;
            if (! manualAmps)
            {
                const double dS = (double) modded (ModEngine::PvSlope, curveSlopeParam)
                                - (double) tmpl->curveSlope;
                if (std::abs (dS) > 1.0e-6)
                    for (int j = 0; j < newN; ++j)
                        tgtAmp[(size_t) j] *= std::pow (std::max (1.0e-9, (double) ps[(size_t) j].ratio), -dS);
            }

            // The user's per-partial amplitude edits. Already RESOLVED against this exact
            // partial list when the template was published, so here it is one multiply
            // per partial with no searching and no allocation on the audio thread.
            //
            // Note what this does NOT do: it never removes a partial. A partial pulled to
            // silence is still in the template, still counted, and comes back the instant
            // the edit is undone or manual mode is switched off. And because the voice
            // rebuilds its normalisation from these targets every sample, pulling one
            // partial down lifts the rest - it is a redistribution, not an attenuation.
            if (! tmpl->gain.empty())
            {
                const int gn = std::min (newN, (int) tmpl->gain.size());
                for (int j = 0; j < gn; ++j)
                    tgtAmp[(size_t) j] *= (double) tmpl->gain[(size_t) j];
            }

            for (int j = 0; j < n; ++j) // clamp guards a partial pushed past Nyquist by pitch bend
                inc[(size_t) j] = std::min (curRatio[(size_t) j] * f0 / currentSampleRate, 0.4999);

            // Increment for the coherent reference. The aligned bank GLIDES to the
            // destination on the same clock as the running bank (alignStartLog is the
            // destination itself when the crossfade began at weight 0, so a lone morph
            // still advances at the true destination frequency throughout). It must not
            // simply SIT on ps[j].ratio: a template landing mid-crossfade would snap the
            // whole bank onto the new frequencies while it is audible at the current
            // weight - the audible stumble under the realtime-adaptive stream.
            // Bounded by the SAME span the render loop crossfades over, not by n: the
            // fading tails in [n, activeCount) are mixed in at weight morphW too, and
            // leaving their alignInc stale advanced them at a frequency belonging to an
            // older template - audible as a detuned ghost during every glide. Slots
            // outside the glide (appeared mid-glide, old static tails, or any fade-out)
            // follow the running ratio, which is exactly where the old code left them.
            if (morphActive)
            {
                const double tA     = std::min (1.0, morphPos);
                const int    incSpan = std::max (n, activeCount);
                for (int j = 0; j < incSpan; ++j)
                {
                    alignRatio[(size_t) j] = (! morphFadeOut && j < morphN)
                        ? std::exp2 (alignStartLog[(size_t) j]
                                     + (morphTgtLog[(size_t) j] - alignStartLog[(size_t) j]) * tA)
                        : curRatio[(size_t) j];
                    alignInc[(size_t) j] = std::min (alignRatio[(size_t) j] * f0 / currentSampleRate, 0.4999);
                }
            }

            prevN = n;
            engine.reportRendered (newN); // meter: the true (target) partial count

            // A cold note starts ON its target amplitudes - no fade-in. This MUST sit
            // inside the block that fills tgtAmp: outside it, a first block with no
            // usable template (none published yet, or f0 not resolved) consumed the
            // flag against empty targets, and every later block then slewed up from
            // silence - a ~15 ms fade-in on a note whose attack was set to 1 ms, worst
            // when several notes are struck at once.
            if (snapAmps)
            {
                for (int i = 0; i < n; ++i)           curAmp[(size_t) i] = tgtAmp[(size_t) i];
                for (int i = n; i < activeCount; ++i) curAmp[(size_t) i] = 0.0;
                activeCount = n;
                snapAmps = false;
            }
        }

        // Per-voice normalisation, recomputed PER SAMPLE from the amplitudes that are
        // ACTUALLY sounding. normGain must equal target/sum(curAmp) at EVERY sample: a
        // SLEWED normGain lags the amp slew, so on a steal/morph the product
        // sum(curAmp)*normGain overshoots many-fold -> "some notes suddenly much louder".
        // Since every partial slews with the SAME rate `slew`, sum(curAmp) and
        // sum(curAmp^2) have an exact closed form in rk = (1-slew)^k:
        //     sumCur(k) = sumTarget   + (sumCur0 - sumTarget) * rk
        //     sumSq (k) = sumTargetSq + (2*coefA + coefB*rk)  * rk
        // tracked with two scalar multiplies per sample - no O(n) re-sum, no lag, no clip.
        const int    span = std::max (n, activeCount);
        const double sr   = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        // AMPLITUDES glide over the Morph Time while a morph runs (a hardcoded 7 ms here
        // once meant only the FREQUENCIES ever glided), over the kTmplGlideSec fade for
        // a short window after a template change with Morph off (7 ms hides a step but
        // reads as a hard cut over a big timbre change), and at the 7 ms click-free
        // minimum the rest of the time.
        double glideTarget = 0.007;
        if (morphActive && morphTimeParam != nullptr)
            glideTarget = std::max (0.007, (double) morphTimeParam->load());
        else if (tmplGlideSamps > 0)
            glideTarget = kTmplGlideSec;
        if (tmplGlideSamps > 0) tmplGlideSamps = std::max (0, tmplGlideSamps - numSamples);
        // The glide CONSTANT itself must never jump FASTER while the amplitudes are still
        // moving: flipping from Morph Time straight to 7 ms the moment a morph completes
        // swept the remaining ~1/e of the amplitude move in a few milliseconds - an
        // audible knee ("trompicon") right as every realtime-adaptive stream settled.
        // Slower is adopted at once (the pending delta was just re-aimed anyway, and a
        // slowdown only softens); faster is approached over ~100 ms, so the knee becomes
        // a gradual accelerando. Constant WITHIN the block, as normGain's closed-form
        // sums require.
        if (glideTarget >= ampGlideCur) ampGlideCur = glideTarget;
        else ampGlideCur += (glideTarget - ampGlideCur)
                          * (1.0 - std::exp (-(double) numSamples / (0.100 * sr)));
        const double slew         = 1.0 - std::exp (-1.0 / (ampGlideCur * sr));
        const double oneMinusSlew = 1.0 - slew;
        // Velocity / pressure / the fast-kill recovery keep their own FIXED ~7 ms glide:
        // they are gain terms whose slew exists to hide steal hand-over steps, and tying
        // them to the amplitude glide made a stolen note's velocity swell in over an
        // entire Morph Time (seconds) instead of milliseconds.
        const float gainSlew = (float) (1.0 - std::exp (-1.0 / (0.007 * sr)));
        double sumCur0 = 0.0, sumTarget = 0.0, sumTargetSq = 0.0, coefA = 0.0, coefB = 0.0;
        for (int i = 0; i < span; ++i)
        {
            const double c0 = curAmp[(size_t) i];           // amps are non-negative
            const double tg = (i < n) ? tgtAmp[(size_t) i] : 0.0;
            const double d  = c0 - tg;
            sumCur0     += c0;
            sumTarget   += tg;
            sumTargetSq += tg * tg;
            coefA       += tg * d;
            coefB       += d  * d;
        }
        // Equal-loudness per voice = normalise to a fixed RMS. In Aligned mode a plain
        // peak-normalise (0.5/sum) equalises the PEAK, not the loudness, so sparse
        // (high-pitch, few-partial) notes sit ~11 dB above dense ones; use the RMS target
        // but CAP it so the coherent pulse peak never exceeds 0.5 (no clip) = min(RMS,peak).
        const bool   aligned = phaseModeParam->load() < 0.5f;
        double rk = 1.0; // (1-slew)^k, advanced every sample in the loop below

        // Filter cutoff TRACKS the played note: cutoff = f0 * 2^(cents/1200 + envOct).
        // Cents (not Hz) means the same INTERVAL above every note -> the same
        // partials are cut regardless of pitch (a partial at ratio r sits at
        // 1200*log2(r) cents). Refreshed every kCutoffChunk samples, NOT once per
        // block: at a 512-sample buffer a 5 ms filter envelope (or an LFO on the
        // cutoff) was never actually rendered, and the sound depended on the host's
        // buffer size.
        constexpr int kCutoffChunk = 32;
        int sinceCutoff = kCutoffChunk;
        const auto refreshCutoff = [&]
        {
            const double octs = (double) modded (ModEngine::PvFCutoff, fCutoffParam) / 1200.0
                              + (double) modded (ModEngine::PvFEnvAmt, fEnvAmtParam) * (double) filterEnvValue;
            const double co = f0 * std::exp2 (octs);
            filter.setCutoffFrequency ((float) juce::jlimit (20.0, 0.49 * sr, co));
            filter.setResonance (juce::jlimit (0.1f, 40.0f, modded (ModEngine::PvFRes, fResParam)));
        };
        refreshCutoff();

        const int    numChannels = output.getNumChannels();

        while (--numSamples >= 0)
        {
            if (--sinceCutoff <= 0) { refreshCutoff(); sinceCutoff = kCutoffChunk; }
            // Crossfade weight, advanced PER SAMPLE. Doing this once per block meant that
            // for any Morph Time below one buffer the weight went 0 -> 1 between two
            // samples, switching every partial onto the coherent reference simultaneously:
            // a full-scale impulse, i.e. the reported click. Two scalar ops per sample.
            if (morphActive)
            {
                if (morphFadeOut)
                    morphW = std::max (0.0, morphW + morphWStep);
                else
                {
                    morphWPos += morphWStep;
                    const double tw = std::min (1.0, morphWPos);
                    morphW = std::max (morphW, tw * tw); // ease-in, monotonic within a fade
                }
            }
            // Exact per-sample normalisation from the closed-form running sums (no lag).
            rk *= oneMinusSlew;
            const double sumCur = sumTarget   + (sumCur0 - sumTarget) * rk;
            const double sumSq  = sumTargetSq + (2.0 * coefA + coefB * rk) * rk;
            const double rmsN   = sumSq  > 1.0e-12 ? 0.25 / std::sqrt (sumSq) : 0.0;
            const double peakN  = sumCur > 1.0e-9  ? 0.5  / sumCur            : 0.0;
            normGain = (float) (aligned ? std::min (rmsN, peakN) : rmsN);

            double acc = 0.0;
            for (int i = 0; i < span; ++i)
            {
                const double target = (i < n) ? tgtAmp[(size_t) i] : 0.0;
                curAmp[(size_t) i] += (target - curAmp[(size_t) i]) * slew;

                double p = phase[(size_t) i] + inc[(size_t) i];
                if (p >= 1.0) p -= 1.0;
                phase[(size_t) i] = p;

                double s = (double) sine.lookup (p);
                if (morphActive) // crossfade the running phase -> the coherent destination phase
                {
                    double pa = alignPhase[(size_t) i] + alignInc[(size_t) i]; // advances at the DEST freq
                    if (pa >= 1.0) pa -= 1.0;
                    alignPhase[(size_t) i] = pa;
                    s += morphW * ((double) sine.lookup (pa) - s);
                }
                acc += curAmp[(size_t) i] * s;
            }

            // Velocity / pressure / the fast-kill recovery glide with their own fixed
            // ~7 ms constant (gainSlew), so a stolen voice hands over without a step
            // whatever the amplitude glide is currently doing.
            velocityLevel += (velTarget      - velocityLevel) * gainSlew;
            pressure      += (pressureTarget - pressure)      * gainSlew;
            if (! fastKill && fastKillGain < 1.0f)
                fastKillGain += (1.0f - fastKillGain) * gainSlew;

            const float env    = adsr.getNextSample();
            const float killG  = fastKillGain; // 1 normally; fading out, or ramping back after a steal
            const float gain   = env * killG * velocityLevel * (0.35f + 0.65f * pressure) * normGain;
            const float s      = filter.processSample (0, (float) acc * gain);
            filterEnvValue     = filterAdsr.getNextSample();
            pitchEnvValue      = pitchAdsr.getNextSample();

            for (int ch = 0; ch < numChannels; ++ch)
                output.addSample (ch, startSample, s);

            ++startSample;

            if (fastKill)
            {
                fastKillGain -= fastKillDelta;
                if (fastKillGain <= 0.0f) { finishNote(); break; }
            }
            else if (! adsr.isActive())
            {
                finishNote();
                break;
            }
        }

        // The note has now rendered: any pressure arriving from here on is a real
        // gesture during the note, so it glides rather than snapping.
        pressureSnap = false;

        // Morph finished this block: adopt the coherent destination phases (the
        // crossfade already reached them) so the sustained note carries on exactly
        // like a freshly-played new spectrum, not a phase-drifted version.
        //
        // Gated on the WEIGHT, not only on morphPos. With a Morph Time under the 5 ms
        // weight floor the ratios arrive before the phase hand-over has finished, and
        // adopting there would copy alignPhase over a bank that is still audible at
        // (1 - morphWeight) - exactly the step this whole path exists to avoid. Bounded
        // by `span`, not morphN: the render loop crossfades the fading tails too, and
        // leaving them un-adopted stepped them back onto their old phase.
        if (morphActive && ! morphFadeOut && morphPos >= 1.0 && morphWeight >= 0.999)
        {
            for (int i = 0; i < span; ++i) phase[(size_t) i] = alignPhase[(size_t) i];
            morphActive = false;
            morphWeight = 0.0; morphWPos = 0.0; // safe here: the two banks are identical
        }
        else if (morphFadeOut && morphWeight <= 0.0)
        {
            // The ramp-out reached silence: the aligned bank contributes nothing now, so
            // dropping it is a no-op.
            morphActive = false; morphFadeOut = false; morphWPos = 0.0;
        }

        // Keep fading-out tails alive until they are inaudible.
        int newActive = n;
        for (int i = n; i < span; ++i)
            if (curAmp[(size_t) i] > 1.0e-5)
                newActive = i + 1;
        activeCount = newActive;
    }

private:
    //==========================================================================
    // Push ADSR parameters to the envelope, but AVOID JUCE's release-rate bug:
    // JUCE's ADSR recomputes its release RATE from `sustain` inside every
    // setParameters() call (recalculateRates: sustain / (release * sr)), whereas
    // noteOff() derives the correct rate from the LIVE envelope value. So ANY
    // setParameters() during the release tail overwrites the honest rate and the
    // tail length jumps ("suddenly longer, more release"). Two guards close this:
    //   1. `releasing` - once the tail has begun, freeze the envelope entirely;
    //      parameter edits/automation take effect on the NEXT note, not this tail.
    //   2. change detection - even before release, only re-apply when a value
    //      actually changed, so we never touch the envelope needlessly.
    // (JUCE's own docs: "Do not change the parameters during playback.")
    void updateAdsrParameters()
    {
        if (releasing) return; // never disturb a release tail's rate
        adsr.setParameters (modded (ModEngine::PvAmpA,  attackParam),
                            modded (ModEngine::PvAmpD,  decayParam),
                            modded (ModEngine::PvAmpS,  sustainParam),
                            modded (ModEngine::PvAmpR,  releaseParam),
                            modded (ModEngine::PvAmpCA, ampCurveA),
                            modded (ModEngine::PvAmpCD, ampCurveD),
                            modded (ModEngine::PvAmpCR, ampCurveR));
    }

    void updateFilterAdsrParameters()
    {
        if (releasing) return;
        filterAdsr.setParameters (modded (ModEngine::PvFA,  fAttackParam),
                                  modded (ModEngine::PvFD,  fDecayParam),
                                  modded (ModEngine::PvFS,  fSustainParam),
                                  modded (ModEngine::PvFR,  fReleaseParam),
                                  modded (ModEngine::PvFCA, filtCurveA),
                                  modded (ModEngine::PvFCD, filtCurveD),
                                  modded (ModEngine::PvFCR, filtCurveR));
    }

    void updatePitchAdsrParameters()
    {
        if (releasing) return;
        pitchAdsr.setParameters (modded (ModEngine::PvPA,  pAttackParam),
                                 modded (ModEngine::PvPD,  pDecayParam),
                                 modded (ModEngine::PvPS,  pSustainParam),
                                 modded (ModEngine::PvPR,  pReleaseParam),
                                 modded (ModEngine::PvPCA, pitchCurveA),
                                 modded (ModEngine::PvPCD, pitchCurveD),
                                 modded (ModEngine::PvPCR, pitchCurveR));
    }

    void beginFastKill()
    {
        // Nothing is sounding: there is no tail to fade, so DO NOT arm the fade.
        // Arming it on an idle voice left fastKill stuck true for ever, because the
        // synthesiser never renders an inactive voice and only the render loop can
        // clear it. The next real note on that voice then saw fastKill and took the
        // voice-STEAL path: amplitudes and velocity slewed up from zero instead of
        // starting on target, i.e. a ~15 ms fade-in on every note however short the
        // attack was set. turnOffAllVoices() hits every voice, and JUCE calls it on
        // any zone-layout change - which happens as soon as the tuning mode is first
        // applied - so in practice this affected the first note on all 32 voices.
        if (! adsr.isActive())
        {
            finishNote();
            return;
        }
        constexpr double fadeSeconds = 0.005; // 5 ms
        const auto sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        fastKill      = true;
        fastKillDelta = (float) (1.0 / (fadeSeconds * sr));
    }

    void finishNote()
    {
        adsr.reset();
        filterAdsr.reset();
        pitchAdsr.reset();
        fastKill     = false;
        fastKillGain = 1.0f;
        releasing    = false;
        clearCurrentNote();
    }

    juce::AudioProcessorValueTreeState& apvts;
    const spectrum::SpectrumEngine&     engine;
    const TuningManager&                tuning;
    const ModEngine&                    mods;

    //==========================================================================
    // PER-VOICE MODULATION.
    //
    // This voice runs its OWN copy of the six modulators - its own envelopes, its own
    // LFO phases - retriggered by its own note-on and released when its own key comes
    // up. The settings live in one shared place (the user has one set of controls);
    // only the running state is per voice. That is how every other synth does it, and
    // it is why the global bank felt broken: one envelope for all the voices, restarted
    // by whichever note arrived last.
    //
    // The parameter itself is never written. Each read site asks modded() for
    // "base + this voice's modulation", so the on-screen control keeps showing the value
    // the user set - the honest thing to show when eight notes each have a different one.
    ModEngine::VoiceState       modState;
    float                       modOff  [ModEngine::PvCount] = {};
    juce::RangedAudioParameter* modOwner[ModEngine::PvCount] = {};
    bool   modNewNote = false;   // set by noteStarted, consumed by the first render block
    bool   modHeld    = false;   // this voice's own key state, gating its envelopes
    double modBpm = -1.0, modPpq = -1.0;  // transport, pushed in per block by the processor

    // base + this voice's modulation, converted back through the parameter's own range
    // (the same normalised-space maths the global path uses, so a control behaves
    // identically whichever route it takes). Unmapped targets cost one atomic load.
    float modded (int t, const std::atomic<float>* fallback) const noexcept
    {
        if (auto* p = modOwner[(size_t) t])
            return p->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, p->getValue() + modOff[(size_t) t]));
        return fallback->load();
    }
    const spectrum::SineTable&          sine;

    std::atomic<float>* attackParam    = nullptr;
    std::atomic<float>* decayParam     = nullptr;
    std::atomic<float>* sustainParam   = nullptr;
    std::atomic<float>* releaseParam   = nullptr;
    std::atomic<float>* phaseModeParam = nullptr;
    std::atomic<float>* fCutoffParam   = nullptr;
    std::atomic<float>* fResParam      = nullptr;
    std::atomic<float>* fEnvAmtParam   = nullptr;
    std::atomic<float>* fAttackParam   = nullptr;
    std::atomic<float>* fDecayParam    = nullptr;
    std::atomic<float>* fSustainParam  = nullptr;
    std::atomic<float>* fReleaseParam  = nullptr;
    std::atomic<float>* pEnvAmtParam   = nullptr;
    std::atomic<float>* pAttackParam   = nullptr;
    std::atomic<float>* pDecayParam    = nullptr;
    std::atomic<float>* pSustainParam  = nullptr;
    std::atomic<float>* pReleaseParam  = nullptr;
    std::atomic<float>* ampCurveA = nullptr, * ampCurveD = nullptr, * ampCurveR = nullptr;
    std::atomic<float>* filtCurveA = nullptr, * filtCurveD = nullptr, * filtCurveR = nullptr;
    std::atomic<float>* pitchCurveA = nullptr, * pitchCurveD = nullptr, * pitchCurveR = nullptr;
    std::atomic<float>* curveSlopeParam = nullptr; // base slope; per-voice modulated
    std::atomic<float>* manualParam     = nullptr; // manual per-partial amps -> slope inert
    std::atomic<float>* morphParam     = nullptr;
    std::atomic<float>* morphTimeParam = nullptr;

    spectrum::CurvedEnv adsr, filterAdsr, pitchAdsr; // per-segment curves (see CurvedEnv.h)
    // Last parameters actually pushed to each ADSR (sentinel = force first apply).
    // See updateAdsrParameters() for why we gate on change.
    juce::dsp::StateVariableTPTFilter<float> filter;
    float filterEnvValue = 0.0f;
    float pitchEnvValue   = 0.0f;

    std::array<double, spectrum::kMaxPartials> phase  {};
    std::array<double, spectrum::kMaxPartials> inc    {}; // frequency (cycles/sample), from curRatio
    std::array<double, spectrum::kMaxPartials> tgtAmp {}; // target amplitude (from template)
    std::array<double, spectrum::kMaxPartials> curAmp {}; // slewed amplitude actually rendered
    std::array<double, spectrum::kMaxPartials> curRatio      {}; // current (gliding) ratio per partial
    std::array<double, spectrum::kMaxPartials> morphStartLog {}; // log2 start ratio during a morph
    std::array<double, spectrum::kMaxPartials> morphTgtLog   {}; // log2 target ratio during a morph
    double morphWeight = 0.0; // see the reference bound to it in renderNextBlock
    std::array<double, spectrum::kMaxPartials> alignPhase    {}; // coherent destination phase (morph crossfade)
    std::array<double, spectrum::kMaxPartials> alignInc      {}; // increment for alignPhase (from alignRatio)
    // The aligned bank's own gliding ratio + its glide start (log2). It cannot simply
    // sit on the destination: a template landing mid-crossfade would snap the audible
    // aligned bank onto the new frequencies in one sample's time (see renderNextBlock).
    std::array<double, spectrum::kMaxPartials> alignRatio    {};
    std::array<double, spectrum::kMaxPartials> alignStartLog {};

    double        morphPos    = 1.0;   // 0..1 glide progress (>= 1 -> done)
    // Separate clock for the phase-crossfade WEIGHT, advanced per sample. It is NOT reset
    // by an incoming template (only morphPos is), so a stream of templates - a knob drag,
    // or a modulator on a spectrum control - lets the crossfade actually COMPLETE instead
    // of being pinned near zero for ever, which is what kept every voice permanently stuck
    // in the mid-fade state where any write to phase[]/alignPhase[] clicks.
    double        morphWPos   = 0.0;
    bool          morphActive = false;
    // Ramping the crossfade weight back OUT (Morph switched off, or an empty template)
    // rather than dropping the term in one sample.
    bool          morphFadeOut = false;
    int           morphN      = 0;     // partial count captured when the morph started
    int           prevN       = 0;     // partials rendered last block (nearest-neighbour source)
    juce::uint32  lastGen     = 0;     // last template generation seen (detects a spectrum change)

    int   activeCount = 0;     // slots to process (includes fading-out tails)
    bool  snapAmps    = true;  // snap amps to target this block (cold note start)
    float normGain    = 1.0f;  // per-voice normalisation gain (recomputed per sample)

    // Spectral de-click for template changes with Morph OFF: the amp slew's target
    // constant while the countdown runs, and how long it stays armed (~4 time constants,
    // so the fade completes to ~2% before the constant relaxes back to 7 ms). This is
    // NOT the morph machinery - the user explicitly wants the two decoupled - and it is
    // constant and modest by design: de-clicking, not an effect.
    static constexpr double kTmplGlideSec  = 0.050;
    static constexpr double kTmplGlideHold = 0.200;
    int    tmplGlideSamps = 0;     // samples the template glide stays armed for
    double ampGlideCur    = 0.007; // CURRENT amp glide constant (only ever relaxes faster; see renderNextBlock)

    float velocityLevel = 0.0f, velTarget = 0.0f;
    float pressure = 0.0f, pressureTarget = 0.0f;
    bool  pressureSnap = false; // take the note's own initial pressure instantly (see noteStarted)

    bool  fastKill = false;
    float fastKillGain = 1.0f;
    float fastKillDelta = 0.0f;
    bool  releasing = false; // in the note-off release tail: freeze ADSR params (see updateAdsrParameters)
};
