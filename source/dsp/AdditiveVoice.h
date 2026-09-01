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
//
// SHEPARD MODE (see ShepardEngine.h for the rules and the bounds). When it is on,
// the voice renders its spectrum at several rise-interval-spaced transpositions of
// the played pitch AT ONCE, under a fixed raised-cosine window in absolute
// log-frequency, all rising together. Everything else about the voice - the three
// envelopes, the filter, the per-voice modulators, the morph, the manual amps - is
// untouched: the copies MULTIPLY the partial set, they do not bypass the voice.
// The copies reuse this voice's own oscillator arrays (slot * kStride offsets), so
// Shepard mode costs no extra memory and can never exceed the oscillator budget the
// voice already had.
//==============================================================================

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "Spectrum.h"
#include "PhaseSets.h"
#include "SpectrumEngine.h"
#include "CurvedEnv.h"
#include "SineTable.h"
#include "TuningManager.h"
#include "ModEngine.h"
#include "ShepardEngine.h"

#include <array>
#include <cmath>

class AdditiveVoice : public juce::MPESynthesiserVoice
{
public:
    // `bank` selects the value source (see ParamBank.h): nullptr - the live patch,
    // identical behaviour to always; a corner's ParamBank - this voice belongs to one
    // of the playground MIX mode's sub-engines and reads THAT preset's values. One
    // voice class either way: forking it would let the two drift apart.
    AdditiveVoice (juce::AudioProcessorValueTreeState& state, const spectrum::SpectrumEngine& eng,
                   const TuningManager& tuningManager, const ModEngine& modulators,
                   ParamBank* bank = nullptr)
        : src { &state, bank }, engine (eng), tuning (tuningManager), mods (modulators),
          sine (spectrum::sineTable())
    {
        attackParam  = src.raw ("ampAttack");
        decayParam   = src.raw ("ampDecay");
        sustainParam = src.raw ("ampSustain");
        releaseParam = src.raw ("ampRelease");
        phaseModeParam = src.raw ("phaseMode");

        fCutoffParam = src.raw ("filterCutoff");
        fResParam    = src.raw ("filterResonance");
        fEnvAmtParam = src.raw ("filterEnvAmount");
        fAttackParam = src.raw ("filterAttack");
        fDecayParam  = src.raw ("filterDecay");
        fSustainParam= src.raw ("filterSustain");
        fReleaseParam= src.raw ("filterRelease");

        pEnvAmtParam = src.raw ("pitchEnvAmount");
        pAttackParam = src.raw ("pitchAttack");
        pDecayParam  = src.raw ("pitchDecay");
        pSustainParam= src.raw ("pitchSustain");
        pReleaseParam= src.raw ("pitchRelease");

        curveSlopeParam = src.raw ("curveSlope");
        manualParam     = src.raw ("partialManual");
        ampCurveA = src.raw ("ampCurveA");
        ampCurveD = src.raw ("ampCurveD");
        ampCurveR = src.raw ("ampCurveR");
        filtCurveA = src.raw ("filterCurveA");
        filtCurveD = src.raw ("filterCurveD");
        filtCurveR = src.raw ("filterCurveR");
        pitchCurveA = src.raw ("pitchCurveA");
        pitchCurveD = src.raw ("pitchCurveD");
        pitchCurveR = src.raw ("pitchCurveR");

        morphParam     = src.raw ("morph");
        morphTimeParam = src.raw ("morphTime");
        xfadeParam     = src.raw ("dryWet");
        xfadeTimeParam = src.raw ("dryWetTime");

        // SHEPARD MODE. Read from THIS voice's source like every other control, so a
        // playground corner whose preset is a Shepard patch really is one - the mode
        // is part of what a preset sounds like, not a global switch bolted on top.
        shepOnParam   = src.raw ("shepOn");
        shepModeParam = src.raw ("shepMode");
        shepTimeParam = src.raw ("shepTime");
        shepDivParam  = src.raw ("shepDiv");
        shepRiseParam = src.raw ("shepRise");

        // MONOPHONIC GLIDE. These two are read from the LIVE state, NOT through `src`,
        // and that is deliberate: mono is a rule about the NOTE STREAM, and there is
        // exactly one note stream feeding the main engine and every playground MIX
        // sub-engine at once. If a corner's stored preset were allowed to disagree,
        // the same run of note-ons would be a legato takeover in one engine and a
        // restrike in another - the mix would be silently non-mono for that corner,
        // which is the one outcome this feature is not allowed to have.
        monoGlideParam = state.getRawParameterValue ("monoGlide");
        glideMsParam   = state.getRawParameterValue ("glideTime");

        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    // TESTS ONLY (message thread, voice idle between blocks): the ratios the
    // oscillators are rendering RIGHT NOW - curRatio, the gliding values, not the
    // template's targets. It exists so an offline probe can assert that a playground
    // drag really BENDS the partial frequencies through intermediate values instead of
    // snapping them (the run-47 glide check); nothing but the verifiers may call it.
    int curRatiosForTests (double* out, int maxN) const noexcept
    {
        const int n = std::min (maxN, activeCount);
        for (int i = 0; i < n; ++i) out[i] = curRatio[(size_t) i];
        return n;
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

        //== MONOPHONIC GLIDE: is this note-on a LEGATO TAKEOVER? ==================
        // Three things have to be true at once, and each one rules out a case that
        // would otherwise glide when it must not:
        //   - the mode is on (otherwise a plain voice steal from a full 32-voice pool
        //     would start bending the pitch of unrelated notes);
        //   - this note-on arrived as a STEAL of a voice that is still sounding
        //     (`reuse`) - in mono the pool is one voice, so every steal is a takeover;
        //   - that voice was still SUSTAINING, not in its release tail (`releasing`,
        //     read here before the fresh note clears it). A note pressed after every
        //     key came up is a new phrase, not a legato move: it starts AT its pitch
        //     and retriggers everything, which is what every mono synth does and what
        //     a player means by lifting their hand.
        //
        // THE RETRIGGER RULE, stated once: a legato takeover retriggers NOTHING - not
        // the three voice envelopes, not this voice's modulators. Legato IS the glide;
        // re-attacking the amplitude in the middle of a portamento is a second
        // articulation nobody played, and the pitch envelope restarting mid-bend would
        // fight the bend itself. Velocity still follows the new note (it glides over
        // the usual 7 ms), and a note struck from silence retriggers normally.
        // (`fastKill` excluded on top of `releasing`: a voice being hard-silenced -
        // turnOffAllVoices from a preset load or a self-play routing change - is not a
        // note anybody is holding, and gliding out of its corpse would be a swoop from
        // whatever the project used to be playing.)
        const bool monoGlide = monoGlideParam != nullptr && monoGlideParam->load() > 0.5f;
        const bool legato    = monoGlide && reuse && ! releasing && ! fastKill && pitchLogValid;
        {
            // The glide is carried as a log2 OFFSET on this voice's base pitch, which
            // is why it survives everything riding on top: a per-note bend arriving
            // mid-glide, a tuning change, the pitch envelope - each still applies
            // exactly as it always did, and the portamento is the distance between the
            // two TUNED frequencies, never a walk through 12-ET semitones.
            double off = 0.0;
            int    len = 0;
            if (legato)
            {
                const double tgt = std::log2 (std::max (1.0e-9,
                                       tuning.frequencyForNote ((int) currentlyPlayingNote.initialNote)))
                                 + (double) currentlyPlayingNote.totalPitchbendInSemitones / 12.0;
                off = lastPitchLog - tgt;      // where we ARE, relative to where we are going
                const double sr0 = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
                const double ms  = glideMsParam != nullptr ? (double) glideMsParam->load() : 0.0;
                len = (int) std::lround (std::max (0.0, ms) * 0.001 * sr0);
            }
            if (len > 0 && std::abs (off) > 1.0e-9) { glideOff = off; glideStep = -off / (double) len; glideLeft = len; }
            else                                    { glideOff = 0.0; glideStep = 0.0;                 glideLeft = 0;   }
            glideBaked = glideOff;
        }

        // SHEPARD MODE IS LATCHED PER NOTE. It is a PLAYED mode: the note you press
        // either is a Shepard or is not, and it stays what it was struck as for its
        // whole life. Reading the switch every block instead would mean flipping it
        // under a held chord had to add or remove seven transpositions between two
        // samples - a step in every partial at once, i.e. the loudest click the whole
        // design exists to avoid. Automation therefore takes effect on the NEXT note,
        // which is exactly how the mode reads to a player, and the tooltip says so.
        const bool wantShep = shepOnParam != nullptr && shepOnParam->load() > 0.5f;
        if (! reuse || wantShep != shepActive)
        {
            // A cold note, or a STEAL that changes the mode: start the rise from the
            // pressed pitch (g = 0 means the m = 0 copy IS the key's own tuned pitch)
            // and let the band/rate be adopted instantly instead of glided up from a
            // stale value, which would be an audible swoop out of nothing.
            shepG = 0.0;
            shepFresh = true;
            for (auto& u : shepUsed) u = false;
        }
        shepActive = wantShep;

        // Deterministic note-on (same note twice = identical). ONE CONTINUOUS AXIS now,
        // not two modes:
        //   Phase 0  - every partial starts at its peak (cosine), so they form a coherent
        //              pulse at the difference-tone rate -> a defined tone, not noise.
        //   Phase 1  - pseudo-random phases decorrelate the partials into a soft/noisy bed.
        //   between  - each partial takes the shortest signed path between the two, so the
        //              crest factor slides rather than switching.
        // Skipped on reuse (keeping the running phases avoids a discontinuity).
        if (! reuse)
        {
            // The WHOLE array, every Shepard slot included (slot s lives at
            // s * kStride): a copy claimed later in the note starts from a phase this
            // note chose, not from whatever the previous note left in that slot.
            //
            // Seeded by GLOBAL INDEX, which is what decorrelates the Shepard copies from
            // each other: a copy is the same partial list transposed, so giving every
            // copy the same phases would stack their peaks and multiply the coherent
            // peak by the copy count. That property has to survive the fader, and it
            // does - seedScattered carries hashPhaseAt(index) toward the aligned phase,
            // so the copies stay as decorrelated at every fader position as they are at
            // the top of the travel.
            seedRange (phase, 0, spectrum::kMaxPartials, scatterAmount());
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
        morphWPos = 0.0; morphFadeOut = false; xfadeActive = false; xfadeAmpSnap = false;
        // The amp glide starts a note at the 7 ms de-click minimum, steal included: a
        // stolen voice's amplitudes must hand over to the NEW note in milliseconds, not
        // inherit a seconds-long constant from the old note's in-flight morph.
        ampGlideCur = 0.007; tmplGlideSamps = 0;

        // Retrigger this voice's modulators. A stolen voice retriggers too: it is a new
        // note, and its envelopes must start from the beginning like any other - EXCEPT
        // a mono legato takeover, which is one continuous note being bent (see the
        // retrigger rule above). modHeld stays true either way: the key is down.
        if (! legato) modNewNote = true;
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
        // A legato takeover does not re-attack: the envelope simply carries on where it
        // was (sustain, or wherever the attack/decay had got to), which is the whole
        // point of playing legato.
        if (! legato) adsr.noteOn (reuse);

        updateFilterAdsrParameters();
        filterAdsr.setSampleRate (sr);
        if (! reuse) { filterAdsr.reset(); filter.reset(); filterEnvValue = 0.0f; }
        if (! legato) filterAdsr.noteOn();

        updatePitchAdsrParameters();
        pitchAdsr.setSampleRate (sr);
        if (! reuse) { pitchAdsr.reset(); pitchEnvValue = 0.0f; }
        if (! legato) pitchAdsr.noteOn();
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
        // A ZERO-LENGTH render is not a block. JUCE's MPESynthesiser calls one on every
        // sounding voice when a MIDI event lands on sample 0, and letting it through
        // consumed this voice's note-on flag against no samples at all - and, since the
        // Shepard layout is rebuilt per block, blanked the copy list a real block had
        // just published.
        if (numSamples <= 0)
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
        // ...times the MONOPHONIC GLIDE offset, which is a factor on the whole pitch and
        // therefore carries the partials, the Shepard copies, the filter's key tracking
        // and the rhythm-engine-free parts of this voice with it. NOT const any more: the
        // glide advances INSIDE the render loop (see advanceGlide) so a portamento is a
        // continuous bend rather than one step per audio buffer - at a 256-sample block a
        // 100 ms octave glide updated per block would be a 19-step chromatic staircase.
        double f0 = baseFreq * std::pow (2.0,
            ((double) currentlyPlayingNote.totalPitchbendInSemitones + pitchEnvSemis) / 12.0)
                  * std::exp2 (glideOff);
        glideBaked = glideOff;   // the offset the increments below are about to be built at

        // THE SHEPARD LAYOUT, planned BEFORE anything else touches the partials: the
        // base partial COUNT depends on the lowest copy's root (a copy below the played
        // pitch has MORE partials under Nyquist than the played pitch does), and the
        // morph's phase-seeding loops below have to walk every copy's oscillators, not
        // just the first bank. With the mode off this is one copy at unity gain and the
        // whole file behaves exactly as it did.
        ShepLayout sl;
        planShepard (sl, tmpl, f0, numSamples);

        // No infrasound gate: a very low fundamental (wide-period tunings can map a
        // key to a few Hz) is not silenced, because its HARMONICS - at 2x, 3x, ...
        // the fundamental - are audible even when the fundamental itself is not, so
        // muting the whole note would wrongly kill an audible tone. A sub-audio
        // fundamental just adds inaudible rumble; the note still sounds through its
        // partials. (The colour keyboard's range adapts to the period so such deep
        // keys are not normally offered in the first place.)
        if (tmpl != nullptr && currentSampleRate > 0.0 && f0 > 0.0)
        {
            // Nyquist / the LOWEST sounding root. Without Shepard that root is f0 and
            // this is the old line unchanged; with it, the deepest copy sits
            // sl.lowestRootMul below (or above) the key, and its partials are the ones
            // that decide how long the shared base list has to be.
            const double maxRatio = (0.5 * currentSampleRate) / (f0 * sl.lowestRootMul);
            const auto&  ps       = tmpl->partials;
            const int    count    = std::min ((int) ps.size(),
                                              sl.windowed ? shepard::kStride : spectrum::kMaxPartials);

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
            // DRY/WET TRANSITION - the crossfade alternative to Morph's pitch glide (see
            // PluginProcessor.cpp, where the two parameters are declared, for the decision
            // and its reason). MORPH WINS when both are on: they are one machine, and a
            // hidden priority is worse than a stated one - the panel greys this out and says
            // so, so nothing on screen claims two smoothings are running.
            const bool xfadeOn = ! morphOn && xfadeParam != nullptr && xfadeParam->load() > 0.5f;
            // A fade already in flight is never CONVERTED from one kind to the other (see
            // xfadeActive): at the two endpoints the banks are silent or identical and either
            // kind may start, in between re-placing them is a write to an audible bank. So a
            // mode change part-way through falls through to the ramp-out branch instead.
            const bool settled = ! morphActive || morphWeight <= 1.0e-6 || morphWeight >= 0.999;
            const bool canCross = ! snapAmps && prevN > 0 && newN > 0;
            if (gen != lastGen)
            {
                if (morphOn && canCross && (! xfadeActive || settled))
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
                    // Every slot the render loop touches - but never past ONE COPY's
                    // width. A voice STOLEN from a dense ordinary note arrives here with
                    // activeCount far beyond kStride, and one stride further is the next
                    // copy's phases (or, from the top slot, PAST THE END of the array:
                    // slot 9's base is 9*1638 and 9*1638 + 16384 is a wild write).
                    const int  seedSpan    = std::min (std::max (hi, activeCount), sl.oscCap());
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
                        // Per COPY: with Shepard on, every transposition has its own
                        // oscillators, and handing only the first bank's phases over
                        // would leave the other seven stepping onto a reference they
                        // never crossfaded to.
                        for (int c = 0; c < sl.numCopies; ++c)
                            for (int j = 0; j < seedSpan; ++j)
                                phase[(size_t) (sl.off[c] + j)] = alignPhase[(size_t) (sl.off[c] + j)];
                        for (int j = 0; j < seedSpan; ++j)
                            if (alignRatio[(size_t) j] > 1.0e-12) // a slot the aligned bank never held keeps its own ratio
                                curRatio[(size_t) j] = alignRatio[(size_t) j];
                        adoptXfadeAmps (seedSpan);
                        morphWeight = 0.0; morphWPos = 0.0;
                    }
                    else if (! wasMorphing)
                    {
                        // A morph that ENDED already adopted its phases (see the tail of
                        // renderNextBlock), so the weight can be released here without a step.
                        morphWeight = 0.0; morphWPos = 0.0;
                    }
                    xfadeActive = false;   // this is a MORPH from here on

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
                        const double scatter = scatterAmount();
                        for (int c = 0; c < sl.numCopies; ++c)
                            seedRange (alignPhase, sl.off[c], seedSpan, scatter);
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
                else if (xfadeOn && canCross && (xfadeActive || settled))
                {
                    // ================= THE DRY/WET TRANSITION =========================
                    // A simultaneous fade-out of the PREVIOUS spectrum and fade-in of the
                    // NEW one, over Dry/Wet Time, with NO frequency movement anywhere. That
                    // is the whole difference from Morph, which bends every partial from
                    // where it is to where it is going and never has two spectra sounding.
                    //
                    // It reuses this voice's ONE crossfade machine rather than adding a
                    // second: the running bank (phase[], curRatio[], curAmp[]) keeps the old
                    // spectrum and the aligned bank (alignPhase[], alignRatio[], tgtAmp[])
                    // holds the new one, mixed by the same weight. So the invariant above
                    // holds unchanged and word for word - alignPhase[] is written only at
                    // weight 0, phase[] only at weight 1 - and violating it here would
                    // produce the same full-scale impulse it produces for a morph.
                    // No lo/hi PAIR here, unlike the morph above: a morph has to MAP old
                    // ranks onto new ones because every partial travels, and a crossfade
                    // maps nothing - each bank keeps its own list where it is.
                    const int hi = std::max (prevN, newN);
                    const int seedSpan = std::min (std::max (hi, activeCount), sl.oscCap());
                    const bool wasFading = morphActive;
                    if (wasFading && morphWeight >= 0.999)
                    {
                        // The previous transition finished: the output IS the aligned bank,
                        // so hand its phases, ratios AND amplitudes back and release the
                        // weight. Exactly the morph path's hand-over plus the amplitudes,
                        // which a crossfade needs and a morph does not (a morph's two banks
                        // always share one amplitude - they are the same partials).
                        for (int c = 0; c < sl.numCopies; ++c)
                            for (int j = 0; j < seedSpan; ++j)
                                phase[(size_t) (sl.off[c] + j)] = alignPhase[(size_t) (sl.off[c] + j)];
                        for (int j = 0; j < seedSpan; ++j)
                            if (alignRatio[(size_t) j] > 1.0e-12)
                                curRatio[(size_t) j] = alignRatio[(size_t) j];
                        adoptXfadeAmps (seedSpan);
                        morphWeight = 0.0; morphWPos = 0.0;
                    }
                    else if (! wasFading)
                    {
                        morphWeight = 0.0; morphWPos = 0.0;
                    }

                    // WHERE THE TWO BANKS SIT. Neither of them moves for the whole
                    // transition - these are the only writes, and they are constants:
                    //   shared ranks      : running keeps the OLD ratio, aligned takes the NEW.
                    //   ranks only in the NEW spectrum : BOTH take the new ratio, and the
                    //       partial simply fades in from silence (curAmp is at 0 there). Left
                    //       at its stale value the running bank would sound a phantom partial
                    //       at an unrelated old frequency and fade it out - audible, and a
                    //       frequency the user never asked for.
                    //   ranks only in the OLD spectrum : both keep the old ratio and fade out
                    //       together, which is exactly what an outgoing spectrum should do.
                    for (int j = 0; j < hi; ++j)
                    {
                        const double nr = j < newN ? (double) ps[(size_t) j].ratio : curRatio[(size_t) j];
                        if (j >= prevN) curRatio[(size_t) j] = nr;      // new-only: no phantom
                        morphStartLog[(size_t) j] = std::log2 (std::max (1.0e-12, curRatio[(size_t) j]));
                        morphTgtLog  [(size_t) j] = std::log2 (std::max (1.0e-12, nr));
                        alignStartLog[(size_t) j] = morphTgtLog[(size_t) j];
                    }
                    morphN = hi; morphActive = true; xfadeActive = true; morphFadeOut = false;
                    // No glide clock to restart: the ratios are already where both banks
                    // belong. morphPos is parked AT the end so the hand-over at the tail of
                    // renderNextBlock is gated on the WEIGHT alone, which is the only thing
                    // that moves during a crossfade.
                    morphPos = 1.0;

                    // The destination bank's phases, seeded ONLY at weight 0 - the same half
                    // of the invariant, for the same reason. A template landing mid-transition
                    // does NOT re-seed: it re-aims the ratios above (a frequency change on a
                    // continuous phase is inaudible) and lets the weight carry on rising, so
                    // a stream of templates crossfades towards the newest one instead of
                    // restarting and stalling half-way.
                    if (morphWeight <= 0.0)
                    {
                        // PHASE-LOCKED TO THE BANK IT IS REPLACING, not seeded to the
                        // coherent reference a morph uses. A crossfade's two banks share
                        // every partial the two spectra have in common - and a spectrum
                        // change usually moves some partials and leaves others exactly
                        // where they were. Seeded independently, such a partial would be
                        // sounding TWICE at one frequency with an arbitrary phase between
                        // the copies: constructive at one transition, a near-null at the
                        // next, on partials that did not change at all. Starting the new
                        // bank AT the old bank's phase makes every unchanged partial
                        // exactly itself for the whole transition - it simply carries on -
                        // and leaves only the partials that really moved to cross over.
                        // (The morph's coherent seeding is for the other thing: a glide
                        // that must ARRIVE phase-aligned. Nothing glides here.)
                        for (int c = 0; c < sl.numCopies; ++c)
                            for (int j = 0; j < seedSpan; ++j)
                                alignPhase[(size_t) (sl.off[c] + j)] = phase[(size_t) (sl.off[c] + j)];
                        // ...and its amplitudes, which are equally free to write at weight 0
                        // and equally forbidden at any other. Deferred by one flag because
                        // tgtAmp does not hold the NEW template's amplitudes yet - the slope
                        // rescale and the per-partial edits are applied to it further down.
                        xfadeAmpSnap = true;
                    }
                }
                else if (morphActive && morphWeight > 1.0e-6)
                {
                    // Snapping (Morph or Dry/Wet switched off, one of them taken over
                    // mid-fade, or an empty template) while a crossfade is part-way through:
                    // dropping the term outright removes the aligned bank's whole
                    // contribution from every partial in one sample, which is the same
                    // coherent step as above. Ramp it out instead, and leave xfadeActive
                    // set: the ramp still renders the incoming bank at its own amplitudes,
                    // and the hand-over at the end still has to give them back.
                    morphFadeOut = true;
                    if (! snapAmps) tmplGlideSamps = (int) (kTmplGlideHold * currentSampleRate);
                }
                else
                {
                    morphActive = false; morphFadeOut = false; xfadeActive = false;
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
            else if (morphActive && xfadeActive)
            {
                // A DRY/WET TRANSITION: nothing but the weight moves. No ratio glide, no
                // morphPos - the two banks were placed where they belong when the template
                // arrived and neither is allowed to shift, because a partial that shifted
                // would be a morph. Only the crossfade clock advances.
                //
                // The same 5 ms floor as the morph weight, and for the same reason: below it
                // the phase hand-over is a step rather than a fade. Dry/Wet Time keeps its
                // full range on the fader - a 1 ms setting simply lands as the shortest
                // click-free transition this machine can make, which is what the panel's
                // hover says.
                const double dt = xfadeTimeParam != nullptr
                                    ? std::max (0.001, (double) xfadeTimeParam->load()) : 0.3;
                morphWStep = 1.0 / (std::max (0.005, dt) * currentSampleRate);
                for (int j = morphN; j < newN; ++j)   // partials that appeared mid-transition
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

            // The oscillator INCREMENTS (this bank's and the aligned reference's) are
            // filled per COPY by fillShepardCopies below, once `span` is known: each
            // transposition multiplies these ratios by its own root and has its own
            // Nyquist edge to cull at. With Shepard off that is the one line this used
            // to be, for the one copy at unity.

            // Ratio for the coherent reference. The aligned bank GLIDES to the
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
                }
            }

            prevN = n;

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
        // A DRY/WET TRANSITION renders the aligned bank at tgtAmp - the NEW spectrum's own
        // amplitudes, which is what makes the far endpoint exactly the new spectrum instead
        // of an amplitude slew's 63%-of-the-way. Every other reader of tgtAmp guards with
        // (i < n); this one is indexed by the render span, so the tail has to be really
        // zero rather than whatever an older, longer template left there - otherwise a
        // fading-out slot would sound at a stale amplitude for the whole transition.
        const bool xfMix = morphActive && xfadeActive;
        for (int i = n; i < span && i < (int) tgtAmp.size(); ++i)
            tgtAmp[(size_t) i] = 0.0;
        // The deferred snap (see xfadeAmpSnap): tgtAmp is only NOW the new template's own
        // amplitudes, slope and per-partial edits included, and the bank that is about to
        // start fading in has to begin exactly there.
        if (xfadeAmpSnap)
        {
            for (int i = 0; i < span && i < (int) xfadeAmp.size(); ++i)
                xfadeAmp[(size_t) i] = tgtAmp[(size_t) i];
            xfadeAmpSnap = false;
        }

        // Each copy's own partial count, increments and near-Nyquist taper. This is
        // also where the meter's number comes from: with Shepard on it reports the
        // partials REALLY sounding across every transposition, because that is the
        // load the machine is carrying and hiding it would be the one dishonest
        // number on the panel.
        fillShepardCopies (sl, span, n, f0, sr);
        engine.reportRendered (sl.total);
        // AMPLITUDES glide over the Morph Time while a morph runs (a hardcoded 7 ms here
        // once meant only the FREQUENCIES ever glided), over the kTmplGlideSec fade for
        // a short window after a template change with Morph off (7 ms hides a step but
        // reads as a hard cut over a big timbre change), and at the 7 ms click-free
        // minimum the rest of the time.
        double glideTarget = 0.007;
        if (xfMix && xfadeTimeParam != nullptr)
            // A crossfade does not NEED the running bank's amplitudes to move at all - the
            // two banks carry their own - but curAmp is still slewing towards the new
            // template from before the transition began, and yanking it there in 7 ms would
            // pull the OUTGOING spectrum's amplitudes onto the incoming one's while it is
            // still audible. Over the transition's own time it barely moves before the
            // hand-over copies the real values in.
            glideTarget = std::max (0.007, (double) xfadeTimeParam->load());
        else if (morphActive && morphTimeParam != nullptr)
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
        // The SAME three coefficients for the incoming bank, which slews toward the same
        // targets at the same rate and therefore obeys the same closed form. Only filled
        // during a crossfade; otherwise xfadeAmp is not rendered and these stay at the
        // running bank's values, which is what the non-crossfade branch below reads.
        double sumAlg0 = 0.0, coefA2 = 0.0, coefB2 = 0.0;
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
            if (xfMix)
            {
                const double a0 = xfadeAmp[(size_t) i];
                const double e  = a0 - tg;
                sumAlg0 += a0; coefA2 += tg * e; coefB2 += e * e;
            }
        }
        // Equal-loudness per voice = normalise to a fixed RMS. In Aligned mode a plain
        // peak-normalise (0.5/sum) equalises the PEAK, not the loudness, so sparse
        // (high-pitch, few-partial) notes sit ~11 dB above dense ones; use the RMS target
        // but CAP it so the coherent pulse peak never exceeds 0.5 (no clip) = min(RMS,peak).
        //
        // AND THE FADER HAS TO BLEND THE TWO LAWS, NOT PICK ONE. This used to read
        // `aligned ? min(rmsN, peakN) : rmsN`, decided by a threshold at 0.5. The two
        // laws are up to 20.70 dB apart (measured across the twelve reference spectra,
        // worst on FLAT n=512, printed by PhaseSetTests), so a continuous control
        // crossing 0.5 while a chord is held would step EVERY sounding voice by that
        // much IN ONE SAMPLE - a bang in the middle of the gesture the fader exists for.
        //
        // The blend weight is (1 - scatter), not scatter: the CAPPED law belongs to the
        // ALIGNED end, which is the LOW end of the travel. Reading the taper the other
        // way round leaves both endpoints looking right in a code review and inverts the
        // whole fader, so it is written out rather than left to be re-derived.
        //
        // Blended on the RECIPROCALS, not linearly in gain, and that is a measurement
        // rather than a preference. A linear blend, rmsN + w*(cappedN - rmsN), is exact
        // at both ends and still wrong in between: with the two laws 16 dB apart on the
        // N=8 default patch, FftVerify's fader sweep measured 7.54 dB of the 16.1 dB
        // span crammed into the FIRST 5% of the travel. That is the dead zone this
        // user already complained about on the Fractal fader, arriving at the other end
        // of the same control. Blending 1/gain spreads it: the same sweep now reads its
        // biggest single step at a fraction of that, and the check refuses more than
        // 3 dB so the defect cannot come back quietly.
        //
        // Not a geometric (dB-linear) blend, which would be the textbook answer: this is
        // evaluated PER SAMPLE inside the closed form below, and a std::pow per sample is
        // not affordable here. It is evaluated per sample rather than slewed because a
        // slewed normGain overshoots on a voice steal - the "some notes are suddenly
        // much louder" bug the closed form exists to prevent.
        const double alignWeight = 1.0 - scatterAmount();
        const double alignRest   = 1.0 - alignWeight;
        // The two ends are taken by a branch on a BLOCK CONSTANT rather than by the
        // blend, because 1.0 / (1.0 / c) is not bit-identical to c in floating point,
        // and "the fader at 0 sounds exactly like the Aligned mode you saved" has to be
        // an identity, not a rounding.
        const bool fullyAligned   = alignWeight >= 1.0;
        const bool fullyScattered = alignWeight <= 0.0;
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
        // SAMPLES ACTUALLY RENDERED since the portamento last moved, which is the only
        // thing the glide is allowed to be driven by.
        //
        // It used to ride on sinceCutoff alone, and that counter is a best-effort CUTOFF
        // refresh: a LOCAL, re-seeded to 32 at the top of every renderNextBlock, so its
        // branch is entered for the first time on the 32nd sample and a sub-block of 31
        // samples or fewer never entered it at all. Skipping a cutoff refresh is
        // inaudible and always was allowed; skipping the glide meant glideLeft never
        // decremented and glideOff never reached zero - measured at exactly the cliff, a
        // 31-sample sub-block left the note sitting 700 cents flat and STILL there three
        // seconds later. lastPitchLog carries glideOff, so the next takeover then started
        // from the stuck pitch too and mono mode lost pitch control after the first note.
        //
        // Sub-blocks under 32 samples are ROUTINE at any host buffer size:
        // MPESynthesiserBase::renderNextBlock renders 1 sample for the first event of a
        // block and always renders the trailing remainder whatever its size, so ordinary
        // CC/MPE traffic alone stretched a 200 ms glide to 320 ms at a 256-sample buffer.
        int sinceGlide = 0;
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

        // THE SHEPARD WINDOW, per sample. Each copy's window position u advances by the
        // same tiny increment (du does not depend on which copy it is), so one complex
        // rotation constant drives all of them: cos/sin are seeded exactly at the block
        // start and rotated per sample, which is smooth to machine precision. Updating
        // the window once per block instead would put a ~5% amplitude kink on every
        // buffer boundary at the fastest rise rate - a 94 Hz buzz, not a Shepard.
        double wc[shepard::kSlots], cx[shepard::kSlots], sx[shepard::kSlots], uu[shepard::kSlots];
        double cd = 1.0, sd = 0.0;
        if (sl.windowed)
        {
            const double d = juce::MathConstants<double>::twoPi * sl.du;
            cd = std::cos (d); sd = std::sin (d);
            for (int c = 0; c < sl.numCopies; ++c)
            {
                const double th = juce::MathConstants<double>::twoPi * sl.u0[(size_t) c];
                cx[c] = std::cos (th); sx[c] = std::sin (th); uu[c] = sl.u0[(size_t) c];
            }
        }
        else { wc[0] = 1.0; }

        while (--numSamples >= 0)
        {
            ++sinceGlide;
            if (--sinceCutoff <= 0)
            {
                // The portamento moves first, so the cutoff's key tracking reads the
                // pitch this chunk is actually about to sound at. Charged with the
                // samples COUNTED above (exactly kCutoffChunk when the gate fires) and
                // never with the constant, so the flush after the loop cannot
                // double-count what this branch already paid for.
                if (glideLeft > 0) { advanceGlide (sinceGlide, sl, span, f0); sinceGlide = 0; }
                refreshCutoff();
                sinceCutoff = kCutoffChunk;
            }

            // Window values for this sample, and the two sums the normalisation needs.
            // Both scale EXACTLY: every oscillator of copy c carries the same factor
            // w_c, so sum(amp) over the whole stack is sum(w) * sum(amp) over the base
            // list and sum(amp^2) is sum(w^2) * sum(amp^2). The closed-form running
            // sums below therefore stay exact - a Shepard voice is normalised as
            // honestly as a plain one, and its loudness does not breathe with how many
            // copies happen to be inside the window.
            double Sw = 1.0, Sw2 = 1.0;
            if (sl.windowed)
            {
                Sw = 0.0; Sw2 = 0.0;
                for (int c = 0; c < sl.numCopies; ++c)
                {
                    const double nc = cx[c] * cd - sx[c] * sd;
                    const double ns = sx[c] * cd + cx[c] * sd;
                    cx[c] = nc; sx[c] = ns;
                    uu[c] += sl.du;
                    // ONE definition of the window's shape (ShepardEngine.h). Outside
                    // the band it is EXACTLY zero, and so is its slope there, so the
                    // gate can never introduce a step - which is what makes a copy
                    // leave the top and re-enter the bottom silent. Writing the formula
                    // out a second time here is how a sabotage of the rule would leave
                    // every test green.
                    const double w = shepard::windowFromCos (uu[c], nc);
                    wc[c] = w; Sw += w; Sw2 += w * w;
                }
            }
            // Crossfade weight, advanced PER SAMPLE. Doing this once per block meant that
            // for any Morph Time below one buffer the weight went 0 -> 1 between two
            // samples, switching every partial onto the coherent reference simultaneously:
            // a full-scale impulse, i.e. the reported click. Two scalar ops per sample.
            if (morphActive)
            {
                if (morphFadeOut)
                    morphW = std::max (0.0, morphW + morphWStep);
                else if (xfMix)
                {
                    // A crossfade runs its clock STRAIGHT. The morph's ease-in below is
                    // there to bring its coherent phase reference in gently; a crossfade
                    // starts phase-locked and has no reference to bring in, and a squared
                    // ramp would simply spend most of the time sitting on the old spectrum
                    // and then hurry - which is not "fade out while the other fades in".
                    morphWPos += morphWStep;
                    morphW = std::max (morphW, std::min (1.0, morphWPos));
                }
                else
                {
                    morphWPos += morphWStep;
                    const double tw = std::min (1.0, morphWPos);
                    morphW = std::max (morphW, tw * tw); // ease-in, monotonic within a fade
                }
            }
            // THE TWO BANKS' GAINS - ONE law for both features, and deliberately the linear
            // one. Equal-power (sqrt) is the textbook choice for crossfading two UNCORRELATED
            // sources, and it was wrong here: a crossfade's banks are phase-locked (see the
            // seeding above), so every partial the two spectra share is one partial sounding
            // through both gains, and sqrt would make it swell 3 dB in the middle of a
            // transition it is not even part of. Linear keeps every unchanged partial exactly
            // constant; the partials that really do cross over are the only ones that dip,
            // and they are the ones the listener is hearing change anyway. Bit-for-bit the
            // expression this replaces when both banks share one amplitude, i.e. under a
            // morph, and exactly (1, 0) at weight 0 and (0, 1) at weight 1 - which is what
            // makes a transition's two endpoints the old and the new spectrum exactly.
            const double algG = morphW;
            const double runG = 1.0 - morphW;
            // Exact per-sample normalisation from the closed-form running sums (no lag).
            rk *= oneMinusSlew;
            const double curSum = sumTarget   + (sumCur0 - sumTarget) * rk;
            const double curSq  = sumTargetSq + (2.0 * coefA + coefB * rk) * rk;
            // Under a crossfade the two banks carry DIFFERENT amplitudes (the running one
            // curAmp, the incoming one xfadeAmp), so the sums the normalisation is derived
            // from have to be the mixture of both. Interpolated BY THE WEIGHT, not by its
            // square: with phase-locked banks a partial common to both spectra really is
            // one partial at its own amplitude the whole way through, and w-squared would
            // model it as two independent ones and duck the normalisation 3 dB under it.
            // Exact at both endpoints either way, which is what keeps them the old and the
            // new spectrum. Without this the voice normalises to the OUTGOING spectrum
            // alone and drifts as the incoming one arrives.
            double sumCur = curSum * Sw, sumSq = curSq * Sw2;
            if (xfMix)
            {
                const double algCur = sumTarget   + (sumAlg0 - sumTarget) * rk;
                const double algSq  = sumTargetSq + (2.0 * coefA2 + coefB2 * rk) * rk;
                sumCur = (runG * curSum + algG * algCur) * Sw;
                sumSq  = (runG * curSq  + algG * algSq)  * Sw2;
            }
            const double rmsN   = sumSq  > 1.0e-12 ? 0.25 / std::sqrt (sumSq) : 0.0;
            const double peakN  = sumCur > 1.0e-9  ? 0.5  / sumCur            : 0.0;
            // scatter 0 -> min(rmsN, peakN), scatter 1 -> rmsN, both bit-exact, so the
            // two ends of the fader are the two levels this synth already shipped.
            // cappedN > 0 implies rmsN > 0 (it is a min of two non-negative numbers, one
            // of them rmsN), so the guard covers both denominators; at silence both laws
            // already returned 0 and so does this.
            const double cappedN = std::min (rmsN, peakN);
            normGain = (float) (fullyScattered ? rmsN
                              : fullyAligned   ? cappedN
                              : cappedN > 0.0  ? 1.0 / (alignWeight / cappedN + alignRest / rmsN)
                                               : 0.0);

            // The amplitude slew is its own pass now: every copy reads the SAME base
            // amplitudes (only the window factor differs), so slewing them once and
            // reusing them is both cheaper and the only way the closed-form sums above
            // can stay exact. With one copy at unity this is bit-for-bit the loop it
            // replaced.
            for (int i = 0; i < span; ++i)
            {
                const double target = (i < n) ? tgtAmp[(size_t) i] : 0.0;
                curAmp[(size_t) i] += (target - curAmp[(size_t) i]) * slew;
                // The incoming bank moves on the same clock, so the closed-form sums above
                // describe it exactly too. A no-op unless a template landed mid-transition:
                // xfadeAmp was snapped ONTO these targets when the transition began.
                if (xfMix) xfadeAmp[(size_t) i] += (target - xfadeAmp[(size_t) i]) * slew;
            }

            // One oscillator bank per copy, at slot * kStride. The advance is identical
            // for all of them - only the increments (this copy's root) and the window
            // factor differ.
            // `j` is the oscillator slot (per Shepard copy), `i` the partial rank the two
            // amplitudes are indexed by. It returns the sample ALREADY scaled, because under
            // a Dry/Wet transition the two banks no longer share one amplitude: the running
            // one carries the outgoing spectrum's (curAmp) and the aligned one the incoming
            // spectrum's (tgtAmp). Under a morph both gains multiply the same curAmp, so
            // this is curAmp * ((1-w)*s + w*sa) - the expression it replaces, factored.
            const double* algAmp = xfMix ? xfadeAmp.data() : curAmp.data();
            const auto advance = [&] (int j, int i) -> double
            {
                double p = phase[(size_t) j] + inc[(size_t) j];
                if (p >= 1.0) p -= 1.0;
                phase[(size_t) j] = p;
                const double s = (double) sine.lookup (p);
                if (morphActive) // crossfade the running phase -> the coherent destination phase
                {
                    double pa = alignPhase[(size_t) j] + alignInc[(size_t) j]; // advances at the DEST freq
                    if (pa >= 1.0) pa -= 1.0;
                    alignPhase[(size_t) j] = pa;
                    return curAmp[(size_t) i] * runG * s
                         + algAmp[(size_t) i] * algG * (double) sine.lookup (pa);
                }
                return curAmp[(size_t) i] * s;
            };

            double acc = 0.0;
            for (int c = 0; c < sl.numCopies; ++c)
            {
                const double w = wc[c];
                if (w <= 0.0) continue;   // a copy sitting on a window zero costs nothing
                const int off = sl.off[(size_t) c];
                const int nf  = sl.tapFrom[(size_t) c];
                const int nc  = sl.count[(size_t) c];
                double accC = 0.0;
                for (int i = 0; i < nf; ++i)
                    accC += advance (off + i, i);
                // The copy's own near-Nyquist taper (see fillShepardCopies): partials
                // leave a rising copy by FADING through it, never by disappearing
                // between two blocks.
                for (int i = nf; i < nc; ++i)
                    accC += sl.tap[(size_t) c][(size_t) (i - nf)] * advance (off + i, i);
                acc += w * accC;
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

        // THE REMAINDER. Whatever the chunk gate above did not cover - the whole of a
        // sub-block shorter than kCutoffChunk, or the last partial chunk of a long one -
        // still SOUNDED, so the glide has to be charged for it before lastPitchLog is
        // written below. Without this the portamento simply stopped wherever the
        // chunking left it (see sinceGlide). Rescaling inc[] one extra time is free:
        // it is refilled from curRatio at the top of every block.
        if (glideLeft > 0 && sinceGlide > 0) advanceGlide (sinceGlide, sl, span, f0);

        // The note has now rendered: any pressure arriving from here on is a real
        // gesture during the note, so it glides rather than snapping.
        pressureSnap = false;

        // WHERE THIS VOICE IS SOUNDING, for the next mono takeover to glide FROM. It is
        // the tuned pitch plus its per-note bend plus whatever is left of the current
        // glide - i.e. the real frequency - and deliberately NOT the pitch envelope: the
        // envelope is per-note articulation that rides on top, and folding it in would
        // make a takeover start from wherever the previous note's envelope happened to
        // be and then have the new note's envelope apply on top of that a second time.
        // Written at the END of the block, which is exactly the state a note-on landing
        // on the next block (or on the next split of this one) takes over from.
        lastPitchLog = std::log2 (std::max (1.0e-9, baseFreq))
                     + (double) currentlyPlayingNote.totalPitchbendInSemitones / 12.0
                     + glideOff;
        pitchLogValid = true;

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
            // Clamped to ONE copy's bank: with Shepard on, slot 9's base is
            // 9 * kStride and a span inherited from a dense ordinary note (activeCount
            // near kMaxPartials, before this note's first Shepard block trimmed it)
            // would write thousands of doubles past the end of the array.
            const int adoptSpan = std::min (span, sl.oscCap());
            for (int c = 0; c < sl.numCopies; ++c)
                for (int i = 0; i < adoptSpan; ++i)
                    phase[(size_t) (sl.off[c] + i)] = alignPhase[(size_t) (sl.off[c] + i)];
            // A DRY/WET transition also hands its AMPLITUDES over. The next block renders one
            // bank at curAmp, and the last sample of the transition was the aligned bank at
            // tgtAmp; without this copy the two differ by however far the amp slew still had
            // to go, and that difference lands in a single sample as a step. Free exactly
            // here and nowhere else: at weight 1 the running bank is silent.
            // (curRatio needs no hand-over: with morphActive false the next block snaps it
            // onto this same template, which is where alignRatio already sat.)
            adoptXfadeAmps (adoptSpan);
            morphActive = false;
            morphWeight = 0.0; morphWPos = 0.0; // safe here: the two banks are identical
        }
        else if (morphFadeOut && morphWeight <= 0.0)
        {
            // The ramp-out reached silence: the aligned bank contributes nothing now, so
            // dropping it is a no-op.
            morphActive = false; morphFadeOut = false; morphWPos = 0.0; xfadeActive = false;
        }

        // Keep fading-out tails alive until they are inaudible.
        int newActive = n;
        for (int i = n; i < span; ++i)
            if (curAmp[(size_t) i] > 1.0e-5)
                newActive = i + 1;
        activeCount = newActive;

        // Snapshot for the verifiers (see shepProbeForTests): the copies as they stood
        // at the END of this block, which is the state the next block continues from.
        shepProbeCount = sl.windowed ? sl.numCopies : 0;
        for (int c = 0; c < shepProbeCount; ++c)
        {
            shepProbeM   [(size_t) c] = sl.m[(size_t) c];
            shepProbeRoot[(size_t) c] = f0 * sl.rootMul[(size_t) c];
            shepProbeW   [(size_t) c] = wc[c];
            shepProbeN   [(size_t) c] = sl.count[(size_t) c];
        }
    }

    //== Test probe (single-threaded verifiers only) ===========================
    // The Shepard copies this voice rendered last block: grid index, ROOT frequency
    // (the transposed played pitch this copy's whole spectrum is built on), the
    // WINDOW amplitude at the end of the block, and how many partials it carried.
    // The window value is the number the silent-wrap rule is stated in.
    int shepProbeForTests (long long* ms, double* roots, double* wins, int* counts, int maxN) const noexcept
    {
        const int k = std::min (maxN, shepProbeCount);
        for (int c = 0; c < k; ++c)
        {
            if (ms     != nullptr) ms    [c] = shepProbeM   [(size_t) c];
            if (roots  != nullptr) roots [c] = shepProbeRoot[(size_t) c];
            if (wins   != nullptr) wins  [c] = shepProbeW   [(size_t) c];
            if (counts != nullptr) counts[c] = shepProbeN   [(size_t) c];
        }
        return k;
    }
    // The rise interval this voice is ACTUALLY using (the glided value), as a ratio:
    // 2.0 for the octave default, the tuning's period when the user chose it.
    // The pitch this voice ended its last block on, in Hz (tuned + bend + glide), and
    // whether a portamento is still running. Verifiers only - the audible check is a
    // zero-crossing f0 probe on the rendered stream, and this exists so a failure can
    // say WHICH of the two disagrees.
    double soundingHzForTests() const noexcept
    { return pitchLogValid ? std::exp2 (lastPitchLog) : 0.0; }
    bool   glidingForTests() const noexcept { return glideLeft > 0; }

    double shepRiseRatioForTests() const noexcept { return std::exp2 (smShepP); }
    bool   shepActiveForTests()    const noexcept { return shepActive; }

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

    // HAND A FINISHED DRY/WET TRANSITION'S AMPLITUDES OVER, in one place because there are
    // three points where a transition can end (a new template arriving on the morph path,
    // one arriving on the crossfade path, and the transition simply completing) and every
    // one of them has to do it. During a transition the running bank renders the OUTGOING
    // spectrum at curAmp while the aligned bank renders the INCOMING one at xfadeAmp; once
    // the weight has reached 1 the running bank is silent, the aligned bank is the whole
    // output, and the next block renders ONE bank at curAmp. So curAmp must BE xfadeAmp by
    // then, or the first sample after the hand-over steps between the two.
    // Called only at weight >= 0.999 - the same half of the invariant that governs phase[].
    // A no-op when the fade in flight is a morph, whose two banks share one amplitude.
    void adoptXfadeAmps (int span) noexcept
    {
        if (! xfadeActive) return;
        const int lim = std::min (span, (int) curAmp.size());
        for (int i = 0; i < lim; ++i)
            curAmp[(size_t) i] = xfadeAmp[(size_t) i];
        xfadeActive = false;
    }

    // The two phase modes, in ONE place so the note-on seeding and the morph's
    // coherent reference cannot drift apart. `j` is the GLOBAL oscillator index, so
    // in Scattered mode two Shepard copies of the same partial are decorrelated
    // rather than stacked on identical phases.
    // WHERE THE FADER IS, AS THE PHASE ENGINE WANTS IT.
    //
    // The parameter is the RAW travel u; every consumer wants x = u^kTaperExponent.
    // Without the taper the first HALF of the fader buys 18% of a 10.29 dB span - a
    // dead zone, and it is the exact complaint this user already made about the
    // Fractal fader, so shipping a second one would be shipping the same bug twice.
    //
    // The taper lives HERE and not in PhaseSets.h on purpose: the header refuses to
    // apply it so that a caller which already has its own curve is not tapered twice.
    // Applying it once, in one place, is what keeps the phases and the normalisation
    // law (the two consumers below) moving on ONE curve - if they moved on two, the
    // gain and the crest would disagree in the middle of the travel and the fader
    // would get louder and quieter on its way across.
    //
    // No allocation, no lock, no String: one atomic load, one clamp, one std::pow -
    // and the two endpoints skip the pow entirely so scatter 0 and scatter 1 are
    // bit-exact rather than "0.9999999 of the way there".
    double scatterAmount() const noexcept
    {
        if (phaseModeParam == nullptr) return 0.0;
        const double u = (double) phaseModeParam->load();
        if (! (u > 0.0)) return 0.0;   // also catches NaN: today's Aligned, exactly
        if (u >= 1.0)    return 1.0;   // today's Scattered hash, exactly
        return std::pow (u, (double) spectrum::phaseset::kTaperExponent);
    }

    // Seed [first, first + count) of one phase array at the fader position `scatter`.
    //
    // At scatter 0 this writes 0.25 into every slot - sin(2*pi*0.25) = 1, every partial
    // at its peak - and at scatter 1 it writes sin(j*12.9898)*43758.5453 frac, index for
    // index the value the old seedPhase() wrote. Both endpoints are therefore the sound
    // this synth already shipped; in between, each partial walks the SHORTEST signed way
    // from its aligned phase to its hash phase, so the crest slides instead of jumping.
    //
    // The hash formula now has exactly ONE copy in the build (phaseset::hashPhaseAt).
    // The old seedPhase() was a second one, and a second copy of this rule is how the
    // phase engine's Failure 2 hid for a whole version - two spellings that agreed until
    // one of them was edited.
    //
    // The count is clamped to the end of the array. That is a BOUNDS CHECK and not an
    // answer - it replaces the isPositiveAndBelow guard the old seedPhase carried, so a
    // caller that miscounts still cannot write into the next Shepard copy's phases
    // (which are audible) or off the end of the voice.
    void seedRange (std::array<double, spectrum::kMaxPartials>& arr,
                    int first, int count, double scatter) const noexcept
    {
        if (first < 0 || first >= spectrum::kMaxPartials || count <= 0) return;
        const int e = std::min (count, spectrum::kMaxPartials - first);
        spectrum::phaseset::seedScattered (first, e, scatter, arr.data() + (size_t) first);
    }

    // Declared here so advanceGlide below can take it by reference; defined with the
    // rest of the Shepard bookkeeping a few dozen lines down.
    struct ShepLayout;

    //==========================================================================
    // MONOPHONIC GLIDE: advance the portamento by `ns` samples, INSIDE the render loop.
    //
    // The glide is linear in log2 frequency, so it spends the same number of cents per
    // millisecond wherever it is - "a fixed TIME between the two notes", which is what
    // was asked for, rather than a fixed rate that would take four times as long for
    // two octaves as for one.
    //
    // The oscillator increments were built once at the top of the block from the pitch
    // at glideBaked; rescaling them by the ratio between then and now is exact and
    // costs one exp2 plus one multiply per oscillator per chunk (~3% of the inner loop
    // at kCutoffChunk = 32, and only while a glide is actually running). Rebuilding
    // them from curRatio instead would repeat the per-copy Nyquist walk eight times a
    // block for no gain.
    //
    // The 0.4999 ceiling is the SAME clamp fillShepardCopies applies for a partial
    // pushed past Nyquist by bend: a partial that crosses Nyquist part-way through a
    // rising glide is pinned there for at most the rest of the block, and the next
    // block's own count (rebuilt from the new pitch) drops it properly with the taper.
    // Nothing accumulates, because inc[] is refilled from curRatio every block.
    void advanceGlide (int ns, const ShepLayout& sl, int span, double& f0) noexcept
    {
        const int k = std::min (ns, glideLeft);
        glideLeft -= k;
        // Landing EXACTLY on zero rather than on the accumulated sum: a residue of a
        // few thousandths of a cent would sit on the note for its whole life and would
        // be inherited by the next takeover.
        glideOff = (glideLeft == 0) ? 0.0 : glideOff + glideStep * (double) k;
        const double ratio = std::exp2 (glideOff - glideBaked);
        glideBaked = glideOff;
        f0 *= ratio;

        const int e = std::min (span, sl.oscCap());
        for (int c = 0; c < sl.numCopies; ++c)
        {
            const int off = sl.off[(size_t) c];
            for (int i = 0; i < e; ++i)
                inc[(size_t) (off + i)] = std::min (inc[(size_t) (off + i)] * ratio, 0.4999);
            if (morphActive)
                for (int i = 0; i < e; ++i)
                    alignInc[(size_t) (off + i)] = std::min (alignInc[(size_t) (off + i)] * ratio, 0.4999);
        }
    }

    //==========================================================================
    // SHEPARD MODE. The rules, the bounds and the pure maths live in
    // ShepardEngine.h; these two functions are the per-block bookkeeping.
    //
    // The whole layout is one struct filled at the top of every block, because the
    // rest of renderNextBlock needs it: the base partial count depends on the LOWEST
    // copy's root, and every loop that writes phases has to walk all of them.
    struct ShepLayout
    {
        bool   windowed  = false;   // false = one copy at unity gain: the ordinary voice
        int    numCopies = 1;
        int    total     = 0;       // oscillators rendered across every copy (the meter's number)
        double du        = 0.0;     // window position advance per SAMPLE (same for every copy)
        double lowestRootMul = 1.0; // lowest copy root / f0 anywhere in this block
        double blockLogMove  = 0.0; // how far (log2) a root travels during this block
        int       off    [shepard::kSlots] = {};   // oscillator base index (slot * kStride)
        long long m      [shepard::kSlots] = {};   // grid index
        double    rootMul[shepard::kSlots] = {};   // copy root / f0 at the block midpoint
        double    u0     [shepard::kSlots] = {};   // window position at the block start
        int       count  [shepard::kSlots] = {};   // partials this copy renders (its own Nyquist)
        int       tapFrom[shepard::kSlots] = {};   // first index carrying the near-Nyquist taper
        bool      fresh  [shepard::kSlots] = {};   // slot (re)claimed this block -> re-seed its phases
        double    tap    [shepard::kSlots][shepard::kTaperMax]; // deliberately NOT zeroed: 5 kB a block
        // How far into ONE copy's bank any loop may walk. With the mode off the whole
        // array is one bank; with it on, a slot is kStride wide and a step further is
        // the next copy's phases.
        int oscCap() const noexcept { return windowed ? shepard::kStride : spectrum::kMaxPartials; }
    };

    // Where the copies are this block. With the mode off (or before any template
    // exists) this leaves ONE copy at unity gain and offset 0, which is bit-for-bit
    // the voice this file always was.
    void planShepard (ShepLayout& sl, const spectrum::SpectrumTemplate* tmpl,
                      double f0, int numSamples) noexcept
    {
        sl.windowed = false; sl.numCopies = 1; sl.total = 0;
        sl.off[0] = 0; sl.m[0] = 0; sl.rootMul[0] = 1.0; sl.u0[0] = 0.5;
        sl.count[0] = 0; sl.tapFrom[0] = 0; sl.fresh[0] = false;
        sl.du = 0.0; sl.lowestRootMul = 1.0; sl.blockLogMove = 0.0;
        if (! shepActive || tmpl == nullptr || tmpl->partials.empty()
            || currentSampleRate <= 0.0 || f0 <= 0.0 || numSamples <= 0)
            return;

        const double sr  = currentSampleRate;
        const double nyq = 0.5 * sr;
        const double dt  = (double) numSamples / sr;

        // THE RISE INTERVAL: ONE OCTAVE by default in every tuning; the tuning's own
        // period only when the user explicitly picks it (see shepard::riseRatio).
        const double R = shepard::riseRatio (
            (shepRiseParam != nullptr && shepRiseParam->load() > 0.5f) ? shepard::RisePeriod
                                                                      : shepard::RiseOctave,
            tuning.periodRatio());
        const double pTarget = std::log2 (R);

        // THE RATE. Sync resolves against the host clock this voice was handed
        // (setTransportForMod - the same number its LFOs lock to), so the rise is
        // sample-locked to the transport without any new plumbing. No BPM on offer and
        // the free ms value takes over; the panel says so from the processor's flag.
        const bool sync = shepModeParam != nullptr && shepModeParam->load() > 0.5f;
        const double riseSec = shepard::riseSeconds (sync,
            shepTimeParam != nullptr ? (double) shepTimeParam->load() : 8000.0,
            shepDivParam  != nullptr ? (int)    shepDivParam->load()  : 9,
            modBpm);
        const double rateTarget = 1.0 / juce::jmax (0.05, riseSec);

        // THE DENSITY FLOOR. A copy below the played pitch has MORE partials under
        // Nyquist than the key does, and each copy's oscillators live in one kStride
        // slot. Rather than cut a partial, the window's BOTTOM EDGE lifts until the
        // deepest copy fits: the band narrows (glided, so copies leave through a zero)
        // exactly as it narrows for a very small rise interval.
        const auto& ps = tmpl->partials;
        const int   np = (int) ps.size();
        const auto floorFor = [&] (int idx) -> double
        {
            if (np <= idx) return -1.0e9;                       // never binds
            const double r = (double) ps[(size_t) idx].ratio;
            return r > 1.0e-9 ? std::log2 (nyq / r) : -1.0e9;
        };
        const auto band = shepard::bandFor (sr, pTarget, floorFor (shepard::kDensityGuard));

        if (shepFresh)
        {
            // Cold note: adopt the targets INSTANTLY. The glides exist to bend a
            // RUNNING Shepard; a new note gliding up from the last note's band would
            // be a swoop nobody played.
            smShepP = pTarget; smShepLo = band.lo; smShepSpan = band.span; smShepRate = rateTarget;
            shepFresh = false;
        }
        else
        {
            const double a = 1.0 - std::exp (-dt / 0.15);   // ~150 ms, as v1 used
            smShepP    += a * (pTarget    - smShepP);
            smShepLo   += a * (band.lo    - smShepLo);
            smShepSpan += a * (band.span  - smShepSpan);
            smShepRate += a * (rateTarget - smShepRate);
        }
        // The HARD floor is applied AFTER the glide rather than glided towards: a
        // template that suddenly turns dense must not spend 150 ms with a bottom copy
        // that needs more partials than its slot can hold.
        smShepLo = juce::jmax (smShepLo, floorFor (shepard::kDensityHard));

        const double P      = juce::jmax (1.0e-6, smShepP);
        const double W      = juce::jmax (1.0e-6, smShepSpan);
        const double A      = std::log2 (f0);
        const double gStart = shepG;
        const double gEnd   = shepG + smShepRate * dt;

        // Which grid indices are inside the window at ANY moment of this block: the
        // bottom bound taken at the END of the block (a copy entering underneath), the
        // top bound at the START (one still on its way out). Both bounds slide down as
        // g rises, so the union is at most kMaxCopies + 2 == kSlots wide - which is
        // exactly why hashing the grid index by kSlots can never collide.
        long long mLo = (long long) std::ceil  ((smShepLo     - A) / P - gEnd);
        long long mHi = (long long) std::floor ((smShepLo + W - A) / P - gStart);
        if (mHi < mLo) mHi = mLo;                                   // always at least the root copy
        if (mHi - mLo + 1 > shepard::kSlots) mHi = mLo + shepard::kSlots - 1; // defensive; never fires

        // Deactivate BEFORE claiming: with the union exactly kSlots wide, the slot a
        // copy just vacated at one edge is the one the copy entering the other edge
        // needs, and claiming first would orphan a live slot.
        for (int s = 0; s < shepard::kSlots; ++s)
            if (shepUsed[(size_t) s] && (shepM[(size_t) s] < mLo || shepM[(size_t) s] > mHi))
                shepUsed[(size_t) s] = false;

        const double gMid = 0.5 * (gStart + gEnd);
        int c = 0;
        for (long long mm = mLo; mm <= mHi && c < shepard::kSlots; ++mm, ++c)
        {
            const long long S = (long long) shepard::kSlots;
            const int slot = (int) (((mm % S) + S) % S);
            sl.m[c]       = mm;
            sl.off[c]     = slot * shepard::kStride;
            sl.rootMul[c] = std::exp2 ((gMid + (double) mm) * P);
            sl.u0[c]      = ((A + (gStart + (double) mm) * P) - smShepLo) / W;
            // A slot claimed for a NEW grid index is claimed at a window edge, where
            // the window is ~0: the copy re-entering at the bottom starts SILENT. That
            // is the no-reset guarantee, and it is what makes re-seeding it inaudible.
            sl.fresh[c] = ! shepUsed[(size_t) slot] || shepM[(size_t) slot] != mm;
            shepUsed[(size_t) slot] = true;
            shepM[(size_t) slot]    = mm;
        }
        sl.numCopies     = c;
        sl.windowed      = true;
        sl.du            = smShepRate * P / W / sr;
        sl.blockLogMove  = smShepRate * P * dt;
        sl.lowestRootMul = std::exp2 ((gStart + (double) mLo) * P);

        // Advance the rise. Whole steps are renumbered away in multiples of kSlots so
        // the slot hash is untouched (the same trick v1 used on its sine grid).
        shepG = gEnd;
        if (shepG >= 4096.0)
        {
            const long long d = (long long) shepard::kSlots
                              * (long long) (shepG / (double) shepard::kSlots);
            shepG -= (double) d;
            for (int s = 0; s < shepard::kSlots; ++s) shepM[(size_t) s] += d;
        }
    }

    // Each copy's partial count, oscillator increments and near-Nyquist taper.
    // Called once `span` (the base list including fading tails) is known.
    void fillShepardCopies (ShepLayout& sl, int span, int n, double f0, double sr) noexcept
    {
        const double nyq = 0.5 * sr;
        const double scatter = scatterAmount();
        const double T   = shepard::taperWidth (sl.blockLogMove);
        const int    cap = sl.oscCap();      // one copy's bank; see ShepLayout::oscCap
        sl.total = 0;
        int prevCount = std::min (span, cap);

        for (int c = 0; c < sl.numCopies; ++c)
        {
            const int    off  = sl.off[(size_t) c];
            const double root = f0 * sl.rootMul[(size_t) c];
            const double maxR = root > 1.0e-9 ? nyq / root : 0.0;

            int cnt = span;
            if (sl.windowed)
            {
                // The copies are handed over in ascending root order, so their counts
                // DESCEND: walking down from the previous copy's count costs O(1) per
                // copy instead of rescanning the whole partial list eight times.
                const int hiN = std::min (span, cap);
                cnt = std::min (prevCount, hiN);
                while (cnt > 0   && curRatio[(size_t) (cnt - 1)] > maxR) --cnt;
                while (cnt < hiN && curRatio[(size_t) cnt]      <= maxR) ++cnt;
            }
            sl.count[(size_t) c] = cnt;
            sl.total += cnt;

            // A freshly claimed slot may hold another grid index's phases and
            // increments, so it is filled and seeded over the WHOLE base span - the
            // fading tails included, or a tail would sound at a stale frequency.
            const double k      = root / sr;
            const int    fillTo = sl.fresh[(size_t) c] ? cnt : std::min (cnt, n);
            for (int i = 0; i < fillTo; ++i) // clamp guards a partial pushed past Nyquist by bend
                inc[(size_t) (off + i)] = std::min (curRatio[(size_t) i] * k, 0.4999);
            if (morphActive)
                for (int i = 0; i < cnt; ++i)
                    alignInc[(size_t) (off + i)] = std::min (alignRatio[(size_t) i] * k, 0.4999);
            // ...but never past this slot's own width: `span` can still carry a dense
            // ordinary note's activeCount on the first Shepard block after a steal, and
            // one step past `cap` is the NEXT copy's phases (which are audible).
            // BOTH BANKS, ALWAYS, AND AT THE SAME FADER POSITION. Seeding one and not the
            // other is not a cosmetic slip: at a crossfade weight where both are audible,
            // a partial whose two copies sit half a cycle apart sums to ZERO - the partial
            // disappears for the length of the fade and comes back when the weight lands.
            if (sl.fresh[(size_t) c])
            {
                const int e = std::min (span, cap);
                seedRange (phase,      off, e, scatter);
                seedRange (alignPhase, off, e, scatter);
            }

            // THE NEAR-NYQUIST TAPER. A rising copy loses its top partials one by one;
            // dropping them outright is a step of that partial's whole amplitude, and
            // at seconds-per-octave that is several faint ticks a second. Instead each
            // partial fades over the top T octaves of the copy's usable range, and
            // because T is a distance in FREQUENCY the weights move continuously with
            // the root - nothing ever appears or vanishes between two blocks. The zone
            // is capped at kTaperMax partials so it stays a short tail loop; a spectrum
            // dense enough to hit the cap simply gets a narrower (still smooth) taper.
            int tf = cnt;
            if (sl.windowed && cnt > 0)
            {
                const double thresh = maxR * std::exp2 (-T);
                while (tf > 0 && curRatio[(size_t) (tf - 1)] > thresh
                       && (cnt - tf) < shepard::kTaperMax) --tf;
                const double lMax = std::log2 (std::max (1.0e-12, maxR));
                for (int i = tf; i < cnt; ++i)
                    sl.tap[(size_t) c][(size_t) (i - tf)] = juce::jlimit (0.0, 1.0,
                        (lMax - std::log2 (std::max (1.0e-12, curRatio[(size_t) i]))) / T);
            }
            sl.tapFrom[(size_t) c] = tf;
            prevCount = cnt;
        }
    }

    ParamSource                         src;   // live APVTS or a corner's ParamBank
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
        // The BASE comes from this voice's source (the live knob, or the corner's
        // stored value); the parameter object supplies only the range conversion.
        if (auto* p = modOwner[(size_t) t])
            return p->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, src.normOf (p) + modOff[(size_t) t]));
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
    std::atomic<float>* xfadeParam     = nullptr;  // Dry/Wet Transition on/off
    std::atomic<float>* xfadeTimeParam = nullptr;  // ...and how long one takes

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
    // THE INCOMING BANK'S OWN AMPLITUDES, during a Dry/Wet transition only. A morph does
    // not need them - its two banks are the same partials and share curAmp - but a
    // crossfade's two banks are two different spectra, and the incoming one has to be at
    // the NEW amplitudes from the first sample or the far endpoint is not the new spectrum,
    // it is wherever an amplitude slew had got to.
    //
    // SNAPPED at weight 0 (free: the bank is inaudible) and SLEWED thereafter. The slew is
    // not decoration: under a stream of templates - an adaptive follower, a modulator on a
    // spectrum control - a new one lands mid-transition while this bank is audible at
    // sqrt(weight), and writing its amplitudes straight in would step the loudest half of
    // the output. The frequencies may snap there (a frequency move on a continuous phase is
    // inaudible); the amplitudes may not.
    std::array<double, spectrum::kMaxPartials> xfadeAmp {};
    bool xfadeAmpSnap = false;   // consumed once tgtAmp holds the NEW template's amplitudes
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
    // WHICH KIND of fade is in flight, latched when it STARTS and never changed while it
    // runs. Morph and Dry/Wet share one crossfade machine and one gain law, but they place
    // the two banks completely differently: a morph has both banks GLIDING towards one
    // destination and seeds the incoming one on the coherent phase reference, while a
    // crossfade pins each bank to a standing spectrum and phase-locks the incoming one to
    // the outgoing bank. Re-placing the banks half way through means writing alignPhase[]
    // or alignRatio[] while they are audible, which is exactly the full-scale step the
    // invariant above exists to forbid. So a mode change mid-fade does not convert the fade:
    // it takes the ramp-out path below, and the NEXT template change starts the other kind
    // cleanly from weight 0.
    bool          xfadeActive = false;
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

    //== MONOPHONIC GLIDE state (see noteStarted and advanceGlide) =============
    // Both parameters come from the LIVE state, never from a corner's ParamBank -
    // see the constructor for why the mono rule cannot be per-preset.
    std::atomic<float>* monoGlideParam = nullptr;
    std::atomic<float>* glideMsParam   = nullptr;
    double glideOff   = 0.0;   // log2 offset still riding on this voice's pitch (octaves)
    double glideStep  = 0.0;   // per-sample change of glideOff (linear in log2 = constant cents/s)
    int    glideLeft  = 0;     // samples of portamento left to travel
    double glideBaked = 0.0;   // the offset the oscillator increments were last built at
    // log2 of the frequency this voice SOUNDED at, at the end of its last block: tuned
    // pitch + per-note bend + the live glide, pitch envelope deliberately excluded.
    // It is what the next takeover glides FROM, and it is only valid once a block has
    // actually rendered (a voice that has never sounded has no pitch to leave from).
    double lastPitchLog  = 0.0;
    bool   pitchLogValid = false;

    //== SHEPARD MODE state (see ShepardEngine.h and planShepard) ==============
    std::atomic<float>* shepOnParam   = nullptr, * shepModeParam = nullptr;
    std::atomic<float>* shepTimeParam = nullptr, * shepDivParam  = nullptr;
    std::atomic<float>* shepRiseParam = nullptr;
    bool   shepActive = false;   // LATCHED at note-on: this note either is a Shepard or is not
    bool   shepFresh  = true;    // adopt band/rate instantly (a cold note, not a glide)
    double shepG      = 0.0;     // the rising grid phase, in rise intervals
    double smShepP    = 1.0;     // smoothed log2(rise ratio)
    double smShepLo   = 5.0;     // smoothed window bottom edge (log2 Hz)
    double smShepSpan = 8.0;     // smoothed window span (log2)
    double smShepRate = 0.125;   // smoothed rise rate (intervals per second)
    std::array<long long, shepard::kSlots> shepM    {};  // grid index held by each slot
    std::array<bool,      shepard::kSlots> shepUsed {};
    // Last block's copies, for the verifiers only (see shepProbeForTests).
    int shepProbeCount = 0;
    std::array<long long, shepard::kSlots> shepProbeM    {};
    std::array<double,    shepard::kSlots> shepProbeRoot {};
    std::array<double,    shepard::kSlots> shepProbeW    {};
    std::array<int,       shepard::kSlots> shepProbeN    {};
};
