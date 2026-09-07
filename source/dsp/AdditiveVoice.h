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

#include <algorithm>
#include <array>
#include <cmath>

// The live re-phase reads one slope per oscillator slot out of the phase engine's own
// table, so the table has to be at least as long as this voice's oscillator array.
static_assert (spectrum::phaseset::kScatterDeltaCount >= spectrum::kMaxPartials,
               "phaseset::scatterDeltas() must cover every oscillator slot a voice owns");

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
        shepDownParam = src.raw ("shepDown");

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
    // "FADE YOURSELF OUT." Message thread -> audio thread, for the MONO SHRINK: when the
    // Monophonic Glide switch is thrown under a held chord the voice pool has to drop to
    // one, and the notes that go must leave over a fade rather than be deleted
    // mid-waveform. It is a REQUEST, not the act: calling beginFastKill() from the message
    // thread would mutate this voice's ADSR state and currentlyPlayingNote underneath a
    // block the audio thread is in the middle of rendering, and MPESynthesiser's
    // voicesLock - the thing that makes its own paths safe - is not reachable from
    // outside. So the flag is atomic and the VOICE consumes it, at the top of its own
    // render, where nothing else can be touching it.
    void requestFadeOut() noexcept { fadeRequest.store (true, std::memory_order_release); }

    void noteStarted() override
    {
        // A new note VOIDS any pending fade request: the shrink that asked for it was
        // about the note this voice used to hold. Cleared here because an inactive voice
        // is never rendered, so a request that arrived just as the old note ended would
        // otherwise sit armed and fast-kill the next note's attack - the same stuck-flag
        // trap beginFastKill() documents.
        fadeRequest.store (false, std::memory_order_relaxed);

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
            const double s0 = scatterAmount();
            // THE PLAN, WHEN THERE IS ONE. The engine measures a phase set per compiled
            // template that is never worse than today's hash at the prefix this note will
            // actually render; without one - a template the worker has not reached yet, a
            // playground blend - `pl` is null and this is the same call it always was.
            //
            // TWO NUMBERS THAT ARE NOT THE SAME NUMBER (the header's Failure 6). The SET
            // is chosen by the RENDERED PREFIX: how many partials of this template fall
            // under Nyquist at THIS pitch, which is why a low note and a high note do not
            // get the same phases. The SPAN is how many oscillator slots to write, which
            // is a bank size and routinely ten times larger. Collapsing them measured 636
            // of 1851 cases WORSE than the hash; the types no longer allow it.
            //
            // AND THE SPAN STOPS AT THE FIRST SHEPARD SLOT. Copies live in this same array
            // at slot * kStride and are decorrelated by having the hash of their GLOBAL
            // index - that is what stops their peaks stacking. The plan's phases are for
            // the base partial list, so past kStride they would be neither measured for
            // that copy nor the hash it relies on. seedFromPlan is total in its span, so
            // the tail past the plan is today's hash either way; the rest of the array is
            // seeded exactly as before.
            // THE PLAN THIS NOTE WILL USE, chosen BEFORE anything is seeded - because
            // seedRange reads it, and every bank and every Shepard copy goes through
            // seedRange.
            //
            // TWO NUMBERS THAT ARE NOT THE SAME NUMBER (the header's Failure 6). The SET
            // is chosen by the RENDERED PREFIX: how many partials of this template fall
            // under Nyquist at THIS pitch, which is why a low note and a high note do not
            // get the same phases. The HEAD is how far into the oscillator array that set
            // reaches - a bank span, and routinely ten times larger. Collapsing them
            // measured 636 of 1851 cases WORSE than the hash; the typed accessor is what
            // keeps them apart here.
            //
            // AND THE HEAD STOPS AT THE FIRST SHEPARD SLOT. Copies live in this same array
            // at slot * kStride and are decorrelated by carrying the hash of their GLOBAL
            // index - that is what stops their peaks stacking. The plan's phases are for
            // the base partial list, so past kStride they would be neither measured for
            // that copy nor the hash it relies on.
            const auto pair = engine.getLivePair();
            notePlanSet = nullptr; notePlanHead = 0;
            if (pair.p != nullptr && pair.p->valid() && pair.t != nullptr && ! pair.t->partials.empty())
            {
                const double f0Hz = tuning.frequencyForNote ((int) currentlyPlayingNote.initialNote);
                const int rp = spectrum::phaseset::renderedPrefix (pair.t->partials.data(),
                                                                   (int) pair.t->partials.size(),
                                                                   f0Hz, getSampleRate());
                const int span = shepActive ? shepard::kStride : spectrum::kMaxPartials;
                notePlanHead = std::min (span, pair.p->n);
                // COPIED, NOT POINTED AT. See notePlanPhases: the pointer phasesFor()
                // returns aims into a std::vector the worker replaces, and the ring wraps
                // back to this slot after eight recompiles - which a modulator on any
                // spectrum control reaches in seconds. One memcpy at note-on and the
                // voice owns every byte it will read for the life of the note.
                if (const double* src = pair.p->phasesFor (spectrum::phaseset::RenderedPrefix { rp }))
                {
                    const int h = juce::jlimit (0, (int) notePlanPhases.size(), notePlanHead);
                    std::memcpy (notePlanPhases.data(), src, (size_t) h * sizeof (double));
                    notePlanSet  = notePlanPhases.data();
                    notePlanHead = h;
                }
                else { notePlanSet = nullptr; notePlanHead = 0; }
            }
            seedRange (phase, 0, spectrum::kMaxPartials, s0);
            // ...AND REMEMBER WHERE ON THE FADER THAT PUT THEM. Everything downstream that
            // wants to know "how scattered is this voice" must ask THIS number and not the
            // live parameter, because the two stop agreeing the moment the fader moves
            // under a held note. See soundingScatter() in renderNextBlock.
            runScatter = algScatter = s0;
            // A cold note starts ON the fader, so it owes nothing: any debt left over from
            // the note this voice used to hold would bend the new note's partials for no
            // reason. The whole array, for the same reason seedRange above covers it - a
            // Shepard slot claimed later in the note must not inherit a stale debt either.
            rephaseDest = s0; rephaseLeft = 0; rephaseK = 0.0; rephaseAimed = 0;
            rephaseWorstRel = 0.0;
            std::fill (rephaseRem.begin(), rephaseRem.end(), 0.0);
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
        //
        // ...but THE CROSSFADE WEIGHT IS NOT PART OF THAT RESET when the voice is
        // sounding. This file states one invariant about it - alignPhase[] may be written
        // only at weight 0 and phase[] only at weight 1 - because at any weight in between
        // BOTH banks are audible, and zeroing the weight outright removes
        // morphWeight * (aligned - running) from every partial in a single sample. That is
        // exactly the coherent step the fade-out path a few hundred lines below exists to
        // avoid, and its comment already says so.
        //
        // The reset predates the crossfade and for years nothing hit it, because a steal
        // re-attacked the amplitude envelope and the re-attack masked the step. MONO GLIDE
        // removed both excuses at once: one voice means every second note is a steal, and
        // the legato rule deliberately suppresses that re-attack - while templates now
        // arrive in streams (10 Hz drag bumps, the realtime follower), so a note-on landing
        // mid-crossfade is the ordinary case rather than a coincidence. Measured on a
        // mono-glide takeover 213 ms into a 300 ms morph (weight 0.506): the jump across
        // the note-on was 0.00172 against 0.00116 for the biggest jump the same waveform
        // made in the millisecond either side of it, and 0.00008 for the identical takeover
        // with Morph off - 21x, on a one-sample discontinuity no window RMS can see.
        //
        // So: ramp it out instead. morphFadeOut makes the render loop walk the weight down
        // over 5 ms while the new note's ratios snap in, and the block-end handler clears
        // the rest of the morph state the moment it lands. xfadeActive is deliberately left
        // standing for the same reason the fade-out path leaves it: under a Dry/Wet
        // transition the ramp still renders the incoming bank at its own amplitudes.
        // Guarded on `reuse` - a voice that is not sounding has no step to make, and a cold
        // note re-seeds phase[] from scratch, so keeping a stale aligned bank alive across
        // it would be a ghost of the previous note rather than a fade of this one.
        if (reuse && morphActive && morphWeight > 1.0e-6)
        {
            morphFadeOut = true;
            morphPos = 1.0; prevN = 0; morphWPos = 0.0;
        }
        else
        {
            morphActive = false; morphPos = 1.0; prevN = 0; morphWeight = 0.0;
            morphWPos = 0.0; morphFadeOut = false; xfadeActive = false; xfadeAmpSnap = false;
        }
        // The amp glide starts a note at the 7 ms de-click minimum, steal included: a
        // stolen voice's amplitudes must hand over to the NEW note in milliseconds, not
        // inherit a seconds-long constant from the old note's in-flight morph.
        ampGlideCur = 0.007; tmplGlideSamps = 0;

        // Retrigger this voice's modulators. A stolen voice retriggers too: it is a new
        // note, and its envelopes must start from the beginning like any other - EXCEPT
        // a mono legato takeover, which is one continuous note being bent (see the
        // retrigger rule above). modHeld stays true either way: the key is down.
        // ...and the pitch ramp forgets the previous note with them. A stolen voice that
        // kept pEnvAmtPrev would spend its first block gliding from the OLD note's
        // modulated amount to the new one's - 48 semitones over one buffer at the default
        // Range - which is a swoop on every single note-on. See pEnvAmtPrimed.
        if (! legato) { modNewNote = true; pEnvAmtPrimed = false; }
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
    // ONE PLAN CANNOT OUTLAST ITS SLOTS. planShepard gives every grid index whose window
    // is non-zero anywhere in the plan its own slot, and there are only kSlots of them.
    // A LIVE buffer never gets near that (the fastest rise takes 2400 samples to move one
    // interval at 48 kHz), but an OFFLINE RENDER buffer does: at 8192 samples the grid
    // moves 3.4 intervals, the plan asks for 12 slots, and the clamp that used to say
    // "defensive; never fires" deleted a copy whose window was still 0.269 - a partial
    // dropped in silence AND a step, in one event.
    //
    // So the BLOCK is split on the horizon the slots can honestly cover (see
    // shepard::maxPlanSamples) rather than the STACK being trimmed to fit the block.
    // With the mode off, or at any rate a live buffer can outrun, `limit` is at least
    // numSamples and this is exactly one call - bit-for-bit the block this file always
    // rendered. Nothing is allocated: renderChunk is the same function under a new name.
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        int limit = numSamples;
        if (shepActive && currentSampleRate > 0.0)
            limit = shepard::maxPlanSamples (currentSampleRate, shepardRiseSeconds());
        while (numSamples > 0)
        {
            const int nS = std::min (numSamples, limit);
            renderChunk (output, startSample, nS);
            startSample += nS;
            numSamples  -= nS;
            // finishNote() inside renderChunk ends the note mid-buffer; the remaining
            // chunks would be no-ops anyway (renderChunk returns immediately on an
            // inactive voice), but stopping here says so out loud.
            if (! adsr.isActive() && ! fastKill)
                break;
        }
    }

    // The rise rate this voice is being driven at, RIGHT NOW, in seconds per interval.
    // ONE reader for the two places that need it - the block splitter above and
    // planShepard - because a second copy of these three parameter reads is how the
    // splitter and the plan would come to disagree about how fast the stack is moving,
    // which is exactly the disagreement the split exists to prevent.
    double shepardRiseSeconds() const noexcept
    {
        return shepard::riseSeconds (shepModeParam != nullptr && shepModeParam->load() > 0.5f,
                                     shepTimeParam != nullptr ? (double) shepTimeParam->load() : 8000.0,
                                     shepDivParam  != nullptr ? (int)    shepDivParam->load()  : 9,
                                     modBpm);
    }

private:
    void renderChunk (juce::AudioBuffer<float>& output, int startSample, int numSamples)
    {
        // The mono shrink's fade request (see requestFadeOut), consumed on the only thread
        // that is allowed to touch this voice's state. Skipped when a fade is already
        // running: beginFastKill() on a voice whose envelope has already finished calls
        // finishNote(), which would cut a fade already in progress short - the very step
        // the request exists to avoid.
        if (fadeRequest.exchange (false, std::memory_order_acquire) && ! fastKill)
            beginFastKill();
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
        // Pitch env: amount (semitones) * the envelope value [0..1]. Both halves are now
        // re-aimed every kPitchChunk samples inside the render loop (see advancePitchMod),
        // NOT once per block - see the staircase note below.
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
        //======================================================================
        // THE PITCH MODULATOR WAS A STAIRCASE, AND HERE IS THE ARITHMETIC.
        //
        // "SEGUN COMO SE MODULA ALMENOS AVECES QUIZAS SIEMPRE, EL CAMBIO DE TONO DEL
        //  PITCH MODULATOR ES ESCALONADO NO CONTINUO."
        //
        // He was right, and "a veces, quizás siempre" is the giveaway: the step size
        // depended on the HOST's buffer size, so the same patch was smooth at 64 samples
        // and chromatic at 1024. This line used to read the modulated amount ONCE and
        // bake it into every oscillator increment for the whole block; nothing in the
        // per-sample loop revisited the pitch except the portamento, and that is gated on
        // a glide being in flight. So the sounding pitch was a zero-order hold at the
        // BLOCK rate, on top of a modulator whose own value is advanced a whole block at
        // a time (dt = numSamples / sr, ModEngine.h) and held across it.
        //
        // For a sine modulator of peak-to-peak span D semitones at f Hz the steepest
        // slope is pi*D*f semitones/second, so one buffer of B samples at SR holds the
        // pitch still across
        //        step = 100 * pi * D * f * B / SR   cents.
        // At the SHIPPED DEFAULT Range of 0.5 the span the fader buys is D = 48 st
        // (pitchEnvAmount is +-48 st, so a normalised span of 96 st times 0.5), and
        //        1 Hz, B = 256, SR = 44100  ->  100*pi*48*1*256/44100 = 87.5 cents,
        //        1 Hz, B = 1024, SR = 44100 ->  350 cents.
        // 87 cents is very nearly a semitone: an audible chromatic staircase on the
        // first thing anyone tries. Re-aimed on the 32-sample grid below the same patch
        // steps 10.9 cents and, more importantly, steps the SAME 10.9 cents at every
        // buffer size the host picks - the number stops depending on the host at all.
        // A 1-semitone 5 Hz vibrato at B = 512 goes from 16.8 cents per block to 1.05
        // cents per chunk, under the just-noticeable difference.
        //
        // WHAT WAS TRIED AND REJECTED, so nobody re-proposes it:
        //  - "It is the 0.01-semitone INTERVAL on pitchEnvAmount snapping the modulated
        //    value" (a real defect, fixed in modded() below). Rejected AS THE CAUSE by
        //    arithmetic: that quantiser's tread is amount * pitchEnvValue with
        //    pitchEnvValue <= 1, so it is hard-capped at 1.000 cent at any setting, any
        //    depth, any rate, any buffer. One cent is at or below the pitch JND; it is
        //    the floor under the staircase, not the staircase.
        //  - Interpolating only the modulation OFFSET and re-reading the base fresh.
        //    Rejected: a host automation lane or a mouse drag on the Pitch Env Amount
        //    fader would still step at block rate. The whole modded() value is ramped.
        //  - Folding the pitch into glideOff / renaming glideBaked to carry both.
        //    Rejected: glideOff is an ACCUMULATOR that must land exactly on zero, and a
        //    stuck pitch offset is the failure this file already survived once (the
        //    31-sample sub-block that left a note 700 cents flat). pitchBaked below is a
        //    block LOCAL, re-derived from scratch every block, so nothing can accumulate.
        //  - A juce::SmoothedValue on the pitch. Rejected: it is a one-pole/linear ramp
        //    towards a target and would ADD latency to a modulator that is already
        //    exactly reconstructible - the two block endpoints are known, so a straight
        //    line between them is the right answer, not an approach curve.
        //
        // STILL BLOCK-RATE ON PURPOSE: the LFO's own phase (this interpolates BETWEEN its
        // samples, leaving only smooth curvature error), the pitch ADSR's parameters, the
        // filter's modulator, and every spectrum-side pitch destination (Base N and
        // friends, which go through a 60 Hz timer and an (int) cast).
        const float  pEnvAmtNow = modded (ModEngine::PvPEnvAmt, pEnvAmtParam);
        if (! pEnvAmtPrimed) { pEnvAmtPrev = pEnvAmtNow; pEnvAmtPrimed = true; }
        const double pitchAmtStart = (double) pEnvAmtPrev;
        // GUARDED AGAINST A ZERO-LENGTH SUB-BLOCK. A host or a block split that hands this
        // voice numSamples == 0 renders nothing, but an unguarded divide would put an
        // infinity in pitchAmtStep, and the remainder flush below would then evaluate
        // inf * 0 = NaN, hand a NaN ratio to rescaleIncs and leave every increment in the
        // voice NaN for the rest of the note. A block that renders nothing owes nothing.
        const double pitchAmtStep  = numSamples > 0
                                   ? ((double) pEnvAmtNow - pitchAmtStart) / (double) numSamples
                                   : 0.0;
        pEnvAmtPrev = pEnvAmtNow;   // this block's right-hand end is the next block's left
        // numSamples is still the WHOLE sub-block here; the render loop decrements it.
        const double pitchEnvSemis = pitchAmtStart * (double) pitchEnvValue;
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
        // The TOTAL pitch-env offset in OCTAVES that inc[] is about to be built at. A
        // LOCAL, deliberately not a member: it is re-derived from scratch every block, so
        // a sub-block that never reaches the 32-sample gate simply sounds at its own
        // left-hand value and the next one starts from exactly there. glideOff is the
        // accumulator; this is not, and that is the whole difference.
        double pitchBaked = pitchEnvSemis / 12.0;

        // THE SHEPARD LAYOUT, planned BEFORE anything else touches the partials: the
        // base partial COUNT depends on the lowest copy's root (a copy below the played
        // pitch has MORE partials under Nyquist than the played pitch does), and the
        // morph's phase-seeding loops below have to walk every copy's oscillators, not
        // just the first bank. With the mode off this is one copy at unity gain and the
        // whole file behaves exactly as it did.
        ShepLayout sl;
        planShepard (sl, tmpl, f0, numSamples);

        // WHICH TRANSITION IS IN FORCE, hoisted out of the template-change branch below
        // because the PHASE FADER borrows the same two controls: re-phasing a held note IS
        // a spectrum change, and Morph / Dry/Wet exist to say how a spectrum change is made
        // smooth (see the "THERE IS NO phaseGlide PARAMETER" note in PluginProcessor.cpp).
        // MORPH WINS when both are on, exactly as it does for the spectrum itself - a voice
        // has ONE crossfade between two banks, so there is only one answer available.
        const bool morphOn = morphParam != nullptr && morphParam->load() > 0.5f;
        const bool xfadeOn = ! morphOn && xfadeParam != nullptr && xfadeParam->load() > 0.5f;
        // Read ONCE per block. Every consumer that used to call scatterAmount() separately
        // now shares this number, so a fader that moves between two reads inside one block
        // cannot leave the phases seeded at one position and the level derived from another.
        const double sNow = scatterAmount();

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
            // THE ONE SILENT CUT LEFT IN THIS FILE, AND IT IS MEASURED. `count` above is
            // clamped to kStride with the mode on, so if the walk reaches the end without
            // meeting Nyquist, the deepest copy wanted MORE partials than its slot holds
            // and the rest are simply absent - not tapered, not faded, gone. The density
            // floor exists so this cannot happen, which is exactly why the claim needs a
            // number rather than a comment: run 53 asserts this counter stays at zero
            // through a mid-note density change, which is the one moment the floor is
            // catching up and the cut could bite.
            if (sl.windowed && newN == count && count < (int) ps.size())
            {
                ++shepEvCutBlocks;
                shepEvCutWorst = std::max (shepEvCutWorst, (int) ps.size() - count);
                // ...AND IT IS ON SCREEN WHILE IT LASTS. A cap that changes what is heard
                // may not be silent, and this one is reachable: for the ~16 ms in which the
                // band's bottom edge is travelling up to a density change (see planShepard)
                // the deepest copies are still audible and cannot fit their slot. Held for
                // half a second because the event is far shorter than one frame of the
                // editor's 60 Hz poll, and a flag nobody can see is not visible.
                shepCapHold = (int) (0.5 * currentSampleRate);
            }
            shepCapped.store (shepCapHold > 0, std::memory_order_relaxed);
            shepCapHold = std::max (0, shepCapHold - numSamples);

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
            // morphOn / xfadeOn are read once at the top of the block now (the Phase fader
            // needs the same two answers). DRY/WET TRANSITION - the crossfade alternative to
            // Morph's pitch glide (see PluginProcessor.cpp, where the two parameters are
            // declared, for the decision and its reason). MORPH WINS when both are on: they
            // are one machine, and a hidden priority is worse than a stated one - the panel
            // greys this out and says so, so nothing on screen claims two smoothings run.
            //
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
                        // The running bank IS the aligned bank now, so it is at the aligned
                        // bank's fader position too. Forgetting this leaves the level law
                        // describing phases that were replaced two lines ago.
                        runScatter = algScatter;
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
                        // SEEDED WHERE THE SOUNDING PHASES ARE, NOT WHERE THE FADER IS. If a
                        // live re-phase is part-way through, runScatter is behind the
                        // parameter, and seeding the incoming bank AT the parameter would
                        // hand it the destination while the running bank is still walking
                        // there - so the shared correction below (which reaches both banks,
                        // as it must) would carry the incoming bank a second time and
                        // overshoot it by the whole outstanding move. Placed here instead,
                        // the two banks travel together and both land exactly on target;
                        // that is also what makes the wholesale alignPhase -> phase copy at
                        // morph completion harmless to a correction in flight.
                        for (int c = 0; c < sl.numCopies; ++c)
                            seedRange (alignPhase, sl.off[c], seedSpan, runScatter);
                        // The incoming bank's own fader position. The level law is blended
                        // between the two banks by the same crossfade weight the audio is,
                        // because at any weight in between BOTH are what the listener has.
                        algScatter = runScatter;
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
                        runScatter = algScatter;   // see the same hand-over on the morph path
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
                        // Copied phases means a copied fader position: a Dry/Wet transition's
                        // incoming bank starts exactly as scattered as the outgoing one, so
                        // the level law must not budge when the transition begins.
                        algScatter = runScatter;
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

            //== THE PHASE FADER'S OWN CROSSFADE (the Dry/Wet answer) ==================
            //
            // Dry/Wet says a change is made smooth by fading the old out while the new
            // fades in, with NO frequency movement anywhere, and that promise covers a
            // re-phase exactly as it covers a spectrum change - see the "THERE IS NO
            // phaseGlide PARAMETER" note in PluginProcessor.cpp. So under Dry/Wet the
            // Phase fader does NOT take Morph's momentary detune: the old phases fade out
            // and the new ones fade in. Nothing is detuned, which is the reason to choose
            // it - there is no pitch blip on a low partial however short the time is set.
            //
            // It is the SAME crossfade machine, placed the same way and under the same
            // invariant, which is why it is written here rather than as a second one:
            // alignPhase[] is WRITTEN ONLY AT WEIGHT 0. So it starts only when the machine
            // is free, and when it is not it simply is not started - runScatter is what
            // remembers that the fader has moved and has not been answered, so the next
            // block tries again and nothing is lost. Re-placing a bank that is audible is
            // the full-scale impulse the whole file exists to avoid.
            //
            // WHAT THE TWO BANKS ARE. Both are THIS spectrum at THESE frequencies; only
            // the phases differ, by the fader's own per-partial delta. So they share
            // curRatio (nothing glides), they are given identical amplitudes (a partial
            // common to both must not swell or duck - it is one partial sounding through
            // two gains, which is exactly why the gain law here is linear and not
            // equal-power), and the level law blends across the weight because the two
            // banks really are at two different fader positions at once.
            //
            // A DRAG therefore lands in steps of one transition each, since the incoming
            // bank's phases may not be re-placed while it is audible. Morph's detune tracks
            // a drag continuously. That is a real difference between the two modes and not
            // a defect in either.
            // (rephaseLeft: never while one of Morph's detunes is still in flight - the
            // mode was switched mid-gesture, and running both would move the phases twice.)
            else if (xfadeOn && ! morphActive && morphWeight <= 0.0 && ! snapAmps
                     && rephaseLeft <= 0 && newN > 0 && std::abs (sNow - runScatter) > 1.0e-12)
            {
                const int seedSpan = std::min (std::max (newN, activeCount), sl.oscCap());
                const double* dtab = spectrum::phaseset::scatterDeltas();
                const double  shift = sNow - runScatter;
                for (int c = 0; c < sl.numCopies; ++c)
                {
                    const int off = sl.off[c];
                    const int lim = std::min (seedSpan, spectrum::kMaxPartials - off);
                    for (int i = 0; i < lim; ++i)
                    {
                        // The SAME bank, moved bodily to the new fader position: each
                        // partial's own aligned-to-scattered delta, scaled by how far the
                        // fader travelled. Taken off the RUNNING phase rather than re-seeded
                        // from the hash, so a partial the move barely touches (a small delta,
                        // or a small travel) carries straight on through the transition
                        // instead of crossing over to an unrelated phase and back.
                        // ...along the slope THIS NOTE is on. dtab is the hash's; a note
                        // seeded from a plan travels a different one, and using the table
                        // here left the whole incoming bank on the hash while a note born
                        // at the destination was on the plan - all 218 partials disagreed.
                        double q = phase[(size_t) (off + i)] + shift * rephaseSlopeAt (off + i);
                        q -= std::floor (q);
                        alignPhase[(size_t) (off + i)] = q;
                    }
                }
                // Neither bank moves in frequency: park both glide endpoints on the ratios
                // that are already sounding, so the alignRatio fill below lands on exactly
                // curRatio and morphPos has nothing left to do.
                for (int j = 0; j < seedSpan; ++j)
                {
                    const double l = std::log2 (std::max (1.0e-12, curRatio[(size_t) j]));
                    morphStartLog[(size_t) j] = morphTgtLog[(size_t) j] = alignStartLog[(size_t) j] = l;
                }
                // Amplitude-identical banks. NOT xfadeAmpSnap (which aims the incoming bank
                // at tgtAmp): the two banks here are the same spectrum, so any difference
                // between them would be a level step the listener hears as the transition
                // starting, on a control that is only supposed to move phases.
                //
                // Over the RENDER SPAN and not seedSpan, which is a narrower thing (one
                // Shepard copy's bank). The normalisation reads xfadeAmp across the whole
                // span to build the incoming bank's closed-form sums, so a tail left at an
                // older template's amplitude there is a wrong gain on every sample of the
                // transition - not a wrong phase somewhere inaudible.
                const int ampSpan = std::min (std::max (newN, activeCount), (int) xfadeAmp.size());
                for (int i = 0; i < ampSpan; ++i)
                    xfadeAmp[(size_t) i] = curAmp[(size_t) i];
                morphN = newN; morphActive = true; xfadeActive = true; morphFadeOut = false;
                morphPos = 1.0; morphWeight = 0.0; morphWPos = 0.0;
                algScatter = sNow;      // the incoming bank IS at the new position
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

        // LIVE RE-PHASING. Runs AFTER fillShepardCopies because it needs this block's
        // increments (a correction may not push a partial into the Nyquist clamp) and
        // this block's `fresh` flags (a re-entering Shepard copy has just been seeded and
        // must not be moved twice). See updateRephase for the whole rule.
        // MORPH ONLY. Dry/Wet's answer to the same gesture is the crossfade placed above -
        // that is the mode's whole promise ("the old fades out while the new fades in, and
        // nothing is detuned"), and running both machines on one gesture would move every
        // partial twice.
        updateRephase (sl, span, sNow, morphOn,
                       morphTimeParam != nullptr ? (double) morphTimeParam->load() : 0.3,
                       sr, numSamples);
        const bool   rephasing = rephaseK > 0.0;
        // How far the SOUNDING fader position moves per sample while that runs - the
        // number the normalisation law is ramped along, so the level lands exactly when
        // the phases do. Zero (a literal, so the arithmetic below is exact) when idle.
        const double sndStep   = rephasing ? rephaseK * (rephaseDest - runScatter) : 0.0;
        double       sndBase   = runScatter;
        const int    startSample0 = startSample;   // how many samples this block really renders
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
        //
        // AND THE LAW IS ASKED WHERE THE *PHASES* ARE, NOT WHERE THE FADER IS. This read
        // used to be scatterAmount() - the live parameter - and that is the defect the
        // user reported in these words: "cuando muevo el fader de fase mientras el sonido
        // suena el volumen aumenta pero no se produce el efecto". He is describing a
        // consequence arriving without its cause. A held note's partials do not re-phase
        // the instant the fader moves (they cannot: a phase can only be moved by detuning
        // the partial for a while, which is what the re-phase below does over the panel's
        // transition time, and which does not happen at all when both transitions are
        // off). Reading the live fader here therefore moved the LEVEL by up to 20.70 dB
        // while the crest factor - the thing the level is compensating for - had not
        // moved at all, so the gain change was pure arbitrary loudness. The rule is one
        // line: THE LEVEL FOLLOWS THE PHASES AND NEVER LEADS THEM.
        //
        // Under a crossfade the two banks sit at two fader positions (runScatter and
        // algScatter) and BOTH are audible, so the law is blended between them by the
        // same weight the audio is - see sndScatter inside the loop.
        const double dAlgScatter = morphActive ? (algScatter - runScatter) : 0.0;
        double rk = 1.0; // (1-slew)^k, advanced every sample in the loop below

        // Filter cutoff TRACKS the played note: cutoff = f0 * 2^(cents/1200 + envOct).
        // Cents (not Hz) means the same INTERVAL above every note -> the same
        // partials are cut regardless of pitch (a partial at ratio r sits at
        // 1200*log2(r) cents). Refreshed every kCutoffChunk samples, NOT once per
        // block: at a 512-sample buffer a 5 ms filter envelope (or an LFO on the
        // cutoff) was never actually rendered, and the sound depended on the host's
        // buffer size.
        constexpr int kCutoffChunk = 32;
        // THE PITCH RIDES THE SAME GRID, and it is named separately so it can be
        // tightened later without touching the filter. 32 samples is 1378 Hz at 44.1 kHz
        // and 1500 Hz at 48 kHz - the zero-order-hold artefact of a pitch sweep sits
        // there, far above anything the sweep itself contains. One counter and one
        // branch serve both; the static_assert is what makes that sharing explicit
        // rather than accidental.
        constexpr int kPitchChunk = kCutoffChunk;
        static_assert (kPitchChunk == kCutoffChunk, "one counter drives both refreshes");
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
        // RE-AIM THE PITCH at sample `done` of this block (see the staircase note at the
        // top of the function). `done` is startSample - startSample0, the samples that
        // have REALLY sounded, so a block the loop broke out of early is charged for what
        // it played and no more.
        //
        // It returns the frequency RATIO instead of touching inc[] itself, so the
        // portamento and the pitch modulator share ONE pass over the oscillators and meet
        // the 0.4999 Nyquist clamp ONCE. Clamping twice would leave a partial pinned by
        // one factor and then scaled by the other - a silently detuned partial at
        // 0.4999*r, which is exactly the kind of thing this file must never do.
        //
        // Both halves of the pitch envelope are re-read here: pitchEnvValue is the member
        // the render loop advances per sample, and (pitchAmtStart + pitchAmtStep*done) is
        // the modulated AMOUNT walking from the previous block's end to this one's. So
        // this one expression fixes the modulator AND the plain pitch envelope, which was
        // stepping at block rate with no mod matrix involved at all.
        const auto advancePitchMod = [&] (int done) noexcept -> double
        {
            const double now   = (pitchAmtStart + pitchAmtStep * (double) done)
                               * (double) pitchEnvValue / 12.0;
            const double ratio = std::exp2 (now - pitchBaked);
            pitchBaked = now;
            f0 *= ratio;          // so the cutoff's key tracking reads the pitch about to sound
            return ratio;
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

        // WHAT THE STACK IS NORMALISED BY, and it is a CONSTANT for the block.
        //
        // This used to be the window's own running sum, recomputed every sample - and
        // dividing by that divides the window straight back out. On the ordinary
        // 8-interval band nothing showed, because a raised cosine over a uniform grid of
        // q copies sums to exactly q/2 and its square to exactly 3q/8, so the running sum
        // WAS this constant (4.0 and 3.0) to rounding. But when a dense spectrum or a wide
        // rise interval narrows the band to one interval, the running sum IS the single
        // copy's window: the divisor went to zero exactly where the window did, the voice
        // held full level all the way to the edge, and the silent wrap the whole illusion
        // rests on stopped happening. Measured at a 16/1-period tuning with Base N at 64:
        // as loud at a window sum of 0.098 as at 0.95 - a ratio of 0.90 where it should
        // have been 10.2. See shepard::windowSumPeak for why there are two of them.
        const double Sw  = sl.windowed ? sl.normSw  : 1.0;   // feeds the PEAK law
        const double Sw2 = sl.windowed ? sl.normSw2 : 1.0;   // feeds the RMS law

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
                //
                // ONE product, ONE pass, ONE clamp: the glide's ratio and the pitch
                // modulator's are multiplied and handed to rescaleIncs together, so a
                // patch doing both costs exactly what a patch doing one of them costs.
                double ratio = 1.0;
                if (glideLeft > 0) { ratio = advanceGlide (sinceGlide, f0); sinceGlide = 0; }
                ratio *= advancePitchMod (startSample - startSample0);
                rescaleIncs (ratio, sl, span);
                refreshCutoff();
                sinceCutoff = kCutoffChunk;
            }

            // Window values for this sample. Every oscillator of copy c carries the same
            // factor w_c, so sum(amp) over the whole stack is (sum of w) * sum(amp) over
            // the base list, and sum(amp^2) is (sum of w^2) * sum(amp^2). The closed-form
            // running sums below therefore stay exact.
            if (sl.windowed)
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
                    wc[c] = shepard::windowFromCos (uu[c], nc);
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
            // WHERE THE PHASES THIS SAMPLE IS MADE OF ACTUALLY SIT ON THE FADER: the
            // running bank's position, plus the aligned bank's difference at the weight
            // that bank is being mixed in at. Evaluated PER SAMPLE for the same reason
            // the weight itself is - a level that stepped once per buffer would be a
            // zipper on exactly the gesture this control exists for.
            //
            // Costs one multiply and two adds while nothing is moving, and the two ends
            // stay BIT-EXACT: dAlgScatter is a literal 0.0 unless a crossfade is running,
            // and runScatter + 0.0 is runScatter. That matters because the branches below
            // exist to keep "the fader at 0 sounds exactly like the Aligned mode you
            // saved" an identity rather than a rounding (1.0 / (1.0 / c) is not c).
            sndBase += sndStep;
            const double sndScatter  = sndBase + morphW * dAlgScatter;
            const double alignWeight = 1.0 - sndScatter;
            const double alignRest   = 1.0 - alignWeight;
            normGain = (float) (alignWeight <= 0.0 ? rmsN
                              : alignWeight >= 1.0 ? cappedN
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
                // THE LIVE RE-PHASE OFFSET, in cycles per sample. A phase is the integral
                // of a frequency, so the ONLY way to move one without a discontinuity is
                // to detune the partial briefly - this is that detune, and it is a
                // read-only per-partial quantity folded in AT THE POINT OF USE because
                // inc[] and alignInc[] are ASSIGNED (=, not +=) from curRatio/alignRatio
                // every block and cannot hold anything of their own.
                //
                // IT REACHES BOTH BANKS ON THE SAME CLOCK, and that is not tidiness. At a
                // crossfade weight near 0.5 both banks are audible at once; a partial
                // whose two copies drift half a cycle apart sums to ZERO, so applying the
                // offset to one bank only NULLS that partial for the length of the fade
                // and brings it back when the weight lands. Measured, not hypothetical.
                //
                // Constant within the block and decayed once per block (see updateRephase)
                // while it is applied per SAMPLE: a step in a FREQUENCY offset leaves the
                // phase continuous, so it is inaudible, and paying for the decay per
                // sample would be a read-modify-write of a 16 k array in the hot loop.
                const double rp = rephasing ? rephaseK * rephaseRem[(size_t) j] : 0.0;
                double p = phase[(size_t) j] + inc[(size_t) j] + rp;
                if (p >= 1.0) p -= 1.0;
                else if (p < 0.0) p += 1.0;   // rp may be negative; inc alone never is
                phase[(size_t) j] = p;
                const double s = (double) sine.lookup (p);
                if (morphActive) // crossfade the running phase -> the coherent destination phase
                {
                    double pa = alignPhase[(size_t) j] + alignInc[(size_t) j] + rp; // DEST freq + the same detune
                    if (pa >= 1.0) pa -= 1.0;
                    else if (pa < 0.0) pa += 1.0;
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
        //
        // THE PITCH IS CHARGED HERE TOO, and that is NOT symmetry for its own sake. Two of
        // the three judges argued this flush could be skipped because pitchBaked is a
        // block local and the main bank is rebuilt from curRatio every block. That is true
        // of the MAIN bank and false of the fading tails: fillShepardCopies refills inc[]
        // only for i < min(cnt, n) on a copy that is not freshly claimed, so the tail
        // slots in [n, cnt) are NOT rebuilt and carry nothing but the ratios that were
        // applied to them. Leaving the last <32 samples' worth of ramp off them every
        // block would let a fading tail drift against the bank it is fading out of. The
        // gate fires on samples 31, 63, ... so there is a remainder on essentially every
        // block, and it costs nothing: advancePitchMod is idempotent (asked for the same
        // `done` twice it returns exactly 1.0 and rescaleIncs early-outs), and inc[] is
        // refilled next block anyway.
        double flush = 1.0;
        if (glideLeft > 0 && sinceGlide > 0) flush = advanceGlide (sinceGlide, f0);
        flush *= advancePitchMod (startSample - startSample0);
        rescaleIncs (flush, sl, span);

        // Charge the re-phase for the samples that were REALLY rendered (the loop can
        // break early on a finished envelope), which is exactly what startSample counted.
        drainRephase (sl, span, startSample - startSample0);

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
            runScatter = algScatter;   // the phases came from there, so the level law does too
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

public:
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
    // The morph/crossfade weight this voice ended its last block on: 0 = the running bank
    // alone, 1 = the aligned bank alone, anything between = BOTH audible. Verifiers only,
    // and it exists because the invariant this file states ("alignPhase written only at
    // weight 0, phase only at weight 1") is otherwise unmeasurable - a test that fires a
    // note-on mid-crossfade has to be able to say the crossfade really was mid-flight,
    // rather than measuring nothing and calling it a pass.
    double morphWeightForTests() const noexcept { return morphActive ? morphWeight : 0.0; }

    // WHERE THE FADER IS FOR THE PHASES THAT ARE ACTUALLY SOUNDING (0 = aligned, 1 =
    // scattered), which is what the normalisation law is derived from. It is NOT the
    // parameter: under a held note the two disagree for the length of a re-phase, and
    // the whole point of this feature is that they are allowed to.
    double soundingScatterForTests() const noexcept { return runScatter; }
    // The biggest frequency offset a live re-phase has actually applied on this note,
    // in CENTS of the partial it was applied to - the only honest unit for it, because
    // the same absolute offset is a swoop on a 55 Hz partial and nothing at 4.4 kHz.
    double rephaseWorstCentsForTests() const noexcept
    { return rephaseWorstRel > 0.0 ? 1200.0 * std::log2 (1.0 + rephaseWorstRel) : 0.0; }
    // The running bank's phases, slot for slot. Verifiers only, and it exists so
    // "the partials really arrived at the interpolated targets" can be a measured
    // number of degrees rather than a claim: a control voice played at the destination
    // fader position from its note-on advances at the same increments, so the two
    // arrays must agree once the move has landed.
    // `base` is the oscillator slot to start at - 0 for an ordinary voice, and
    // slot * shepard::kStride to read ONE Shepard copy's bank. It matters which: the
    // copies do not all carry the same number of partials (a risen copy has fewer under
    // Nyquist), and slot 0 is whichever grid index happens to hash there, so a verifier
    // that always read slot 0 would compare 26 partials on one run and 383 on the next.
    int phasesForTests (double* out, int maxN, int base = 0) const noexcept
    {
        if (base < 0 || base >= spectrum::kMaxPartials) return 0;
        const int k = std::min (std::min (maxN, activeCount), spectrum::kMaxPartials - base);
        for (int i = 0; i < k; ++i) out[i] = phase[(size_t) (base + i)];
        return k;
    }

    double shepRiseRatioForTests() const noexcept { return std::exp2 (smShepP); }
    bool   shepActiveForTests()    const noexcept { return shepActive; }
    // The meter's honesty flag: this voice's deepest Shepard copy wanted more partials
    // than one kStride slot holds, so some of them are not sounding. See the comment at
    // shepCapHold - the ONE cap left that changes what is heard, and it says so on screen.
    bool   shepardSlotCapped()     const noexcept { return shepCapped.load (std::memory_order_relaxed); }

    //== The Shepard's three STATED invariants, made measurable ==================
    // Prose cannot be sabotaged, so none of these is left to a comment. Each is the
    // worst case seen since shepEvidenceResetForTests(), and each is a number the
    // rules in ShepardEngine.h are already written in:
    //   claimW  - the window a slot was (re)claimed at. The rule is "a slot is claimed
    //             at a window edge, where the window is ~0, and that is what makes
    //             re-seeding it inaudible"; a re-seed writes phase[] AND alignPhase[],
    //             so this number IS the amplitude step the re-seed can cause.
    //   claimX  - the crossfade weight at that same moment. The file's other invariant
    //             is that alignPhase[] is only ever WRITTEN while the weight is 0; at
    //             weight w a write contributes w x delta.
    //   planned - the widest grid union any block asked for BEFORE the kSlots clamp.
    //             > kSlots means the clamp deleted a copy, and dropW says how loud it
    //             was when it went. Both exist because "defensive; never fires" is a
    //             claim, and a claim nothing measures is worth nothing.
    //   loStep  - the biggest ONE-BLOCK jump of the band's bottom edge, in log2 Hz. The
    //             band is what every copy's window position is measured against, so a
    //             jump here steps every sounding copy at once.
    void shepEvidenceResetForTests() noexcept
    {
        shepEvClaimW = 0.0; shepEvClaimX = 0.0; shepEvDropW = 0.0; shepEvLoStep = 0.0;
        shepEvPlanned = 0; shepEvDropped = 0; shepEvMinCopies = 1 << 30; shepEvBlocks = 0;
        shepEvCutBlocks = 0; shepEvCutWorst = 0;
        shepPrevLoValid = false;
    }
    void shepEvidenceForTests (double& claimW, double& claimX, double& dropW, double& loStep,
                               int& planned, int& dropped, int& minCopies, int& blocks,
                               int& cutBlocks, int& cutWorst) const noexcept
    {
        claimW = shepEvClaimW; claimX = shepEvClaimX; dropW = shepEvDropW; loStep = shepEvLoStep;
        planned = shepEvPlanned; dropped = shepEvDropped;
        minCopies = shepEvMinCopies == (1 << 30) ? 0 : shepEvMinCopies; blocks = shepEvBlocks;
        cutBlocks = shepEvCutBlocks; cutWorst = shepEvCutWorst;
    }

    //==========================================================================
    // THE NEAR-NYQUIST TAPER, AS TWO PURE FUNCTIONS.
    //
    // Production maths, not a probe: fillShepardCopies calls exactly these, so the
    // verifier that calls them is measuring the shipping arithmetic and not a second
    // copy of it. They are public for that reason and for no other. (A per-voice
    // counter would have been the house style, but the taper is a number no processor
    // accessor reaches, and this file's own rule is that a claim nothing can measure is
    // worth nothing - so the maths comes out to where a measurement can get at it.)
    //
    // WHAT THE ZONE IS. Each copy fades its top partials over the last T octaves of its
    // usable range, so a partial rising past the copy's Nyquist leaves by fading rather
    // than by vanishing between two blocks. The zone is capped at kTaperMax partials so
    // the tail loop stays short.
    //
    // WHAT THE CAP USED TO DO, AND THE PROMISE IT BROKE. The comment at the call site
    // has always said that a spectrum dense enough to hit the cap "simply gets a
    // narrower (still smooth) taper". It did not. The walk stopped after kTaperMax
    // partials but the weights were still divided by the FULL T, so the zone was
    // TRUNCATED: the lowest tapered partial started at (lMax - log2 r)/T, well below 1,
    // with the partial one step under it at full amplitude.
    //
    // That is not a static notch, it is the very tick the taper exists to abolish. As
    // the copy rises, cnt falls and the zone's floor follows it, so the partial LEAVING
    // the zone jumps from that reduced weight straight back to 1.0 in a single block.
    // Measured on a harmonic spectrum at the fastest rise, where T saturates at one
    // octave: the lowest tapered partial started at 0.35, i.e. a 0.65 step, exactly
    // where the fade was supposed to be smoothest.
    //
    // WHAT IT DOES NOW. When - and only when - the cap is what ended the walk, the
    // weights are divided by the zone that actually FIT instead of by the zone that was
    // asked for. weight(from) is then exactly 1.0, continuous with the untapered
    // partials below it, and the fade still reaches 0 at the copy's Nyquist: a narrower
    // taper, which is what the sentence promised. The zone is dense by definition when
    // the cap binds (kTaperMax partials inside T octaves), so what is left of the
    // per-block movement is one partial spacing out of kTaperMax - about 1.6% of a
    // weight, against the 65% it replaces.
    //
    // THE UNCAPPED PATH IS UNTOUCHED: width == T to the bit, so the default octave band
    // is exactly the instrument it was.
    struct TaperZone
    {
        int    from   = 0;      // first index carrying the taper (== cnt when there is none)
        double width  = 0.0;    // the octave span the weights are divided by
        bool   capped = false;  // kTaperMax, and not the threshold, is what ended the walk
    };
    static TaperZone taperZoneFor (const double* ratios, int cnt, double maxR, double T) noexcept
    {
        TaperZone z; z.from = cnt; z.width = T;
        if (ratios == nullptr || cnt <= 0) return z;
        const double thresh = maxR * std::exp2 (-T);
        while (z.from > 0 && ratios[(size_t) (z.from - 1)] > thresh)
        {
            if ((cnt - z.from) >= shepard::kTaperMax) { z.capped = true; break; }
            --z.from;
        }
        if (z.capped)
            z.width = std::max (1.0e-9,
                                std::log2 (std::max (1.0e-12, maxR))
                                - std::log2 (std::max (1.0e-12, ratios[(size_t) z.from])));
        return z;
    }
    // `lMax` is log2(maxR), hoisted by the caller: the render path computes it once per
    // copy and this is called once per tapered partial, so recomputing it here would put
    // a std::log2 per partial per copy per block on the audio thread for nothing.
    static double taperWeightAt (double lMax, double ratio, double width) noexcept
    {
        return juce::jlimit (0.0, 1.0,
                             (lMax - std::log2 (std::max (1.0e-12, ratio))) / width);
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
        // The meter's Shepard cap flag is held for half a second so a human can see it,
        // and a voice that stops rendering stops counting that half second down - so it
        // is dropped HERE, where every note ends. Otherwise a single dense-patch tweak
        // would leave "(safety cap)" on the meter for the rest of the session, which is
        // the same dishonesty as hiding it, pointing the other way.
        shepCapHold = 0;
        shepCapped.store (false, std::memory_order_relaxed);
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
    // HOW FAR PARTIAL `g` STILL HAS TO TRAVEL PER UNIT OF FADER, for THIS note.
    //
    // scatterDeltas() answers that for today's hash, from a table, in one multiply. The
    // moment a note is SEEDED FROM A PLAN the table is aiming somewhere the note is not
    // going: measured, a note moved to fader 1 landed 179.5 degrees from a note born
    // there, because the born one used the plan and the moved one integrated the hash's
    // deltas. Run 53 pins those two as equal and is right to - that equality IS the
    // fader.
    //
    // So inside the plan's head, on the copy the plan describes, the slope comes from the
    // plan's own phase for that partial; everywhere else - past the head, and on every
    // Shepard copy, whose decorrelation is the hash of its GLOBAL index - it is the table.
    // Three operations rather than a lookup, and no std::sin: the same cost argument the
    // table was built for still holds.
    double rephaseSlopeAt (int g) const noexcept
    {
        if (notePlanSet != nullptr && g < notePlanHead)
            return spectrum::phaseset::scatterDeltaTo (notePlanSet[g]);
        return spectrum::phaseset::scatterDeltas()[g];
    }

    void seedRange (std::array<double, spectrum::kMaxPartials>& arr,
                    int first, int count, double scatter) const noexcept
    {
        if (first < 0 || first >= spectrum::kMaxPartials || count <= 0) return;
        const int e = std::min (count, spectrum::kMaxPartials - first);
        // THE PLAN, WHERE THIS NOTE HAS ONE. Both banks and every Shepard copy come
        // through here, so making this one function plan-aware is what keeps them
        // consistent: a note seeded from the plan and then MOVED by the fader has to walk
        // toward the same phases, and the aligned bank has to be seeded at its own fader
        // position from the same set. Done anywhere less central, one of the three sites
        // goes on aiming at the hash - measured, that left a moved note 179.5 degrees from
        // a note born at the destination.
        //
        // Past notePlanHead, and on every Shepard copy (whose `first` is at or past the
        // base copy's span), it is today's hash by GLOBAL index - which is exactly what
        // decorrelates the copies from each other.
        if (notePlanSet != nullptr && first < notePlanHead)
        {
            const int h = std::min (first + e, notePlanHead) - first;
            spectrum::phaseset::interpolatePhases (notePlanSet + first, h, scatter,
                                                   arr.data() + (size_t) first);
            if (e > h)
                spectrum::phaseset::seedScattered (first + h, e - h, scatter,
                                                   arr.data() + (size_t) (first + h));
            return;
        }
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
    // Returns the frequency RATIO this advance owes the oscillators and deliberately does
    // NOT apply it: the caller multiplies it by the pitch modulator's ratio and hands the
    // product to rescaleIncs once. The arithmetic below is unchanged - only the loop moved
    // out - so the portamento is bit-identical to what run 48 already measures.
    double advanceGlide (int ns, double& f0) noexcept
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
        return ratio;
    }

    // ONE pass over the sounding oscillators, shared by the portamento and the pitch
    // modulator. The exact-1.0 early-out is what keeps the default patch free: with
    // pitchEnvAmount at 0.0 and nothing mapped, the modulator's `now` and `pitchBaked` are
    // both exactly 0.0, exp2 returns exactly 1.0, and this whole function is one compare.
    // A patch running a portamento AND a pitch modulation costs +0% over the portamento
    // alone, because the two ratios are multiplied and share this single pass.
    void rescaleIncs (double ratio, const ShepLayout& sl, int span) noexcept
    {
        if (ratio == 1.0) return;
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
    // LIVE RE-PHASING: THE PHASE FADER MOVED WHILE THIS NOTE WAS SOUNDING.
    //
    // "cuando muevo el fader de fase mientras el sonido suena el volumen aumenta pero no
    //  se produce el efecto o no del todo." He was right, and precisely right: the level
    // followed the fader immediately while the PHASES did not move until the next
    // note-on, so he was hearing the consequence without the cause. The rule this file
    // now keeps is one line - THE LEVEL FOLLOWS THE PHASES AND NEVER LEADS THEM - and
    // this function is the half that makes the phases actually go.
    //
    // THE MATHS IS STATELESS. seedScattered places partial j at
    // wrap01(kAlignedPhase + s * d_j), so moving the fader by ds owes every partial
    // exactly ds * d_j cycles, where d_j is that partial's own aligned-to-scattered
    // delta. That is phaseset::scatterDeltas()[j] - the header's OWN curve, tabulated,
    // never a second copy of it (testScatterDeltaIsTheCurve re-derives interpolatePhases
    // from the table so the two cannot drift apart).
    //
    // WHAT MOVES A PHASE. Only a frequency offset: phase is the integral of frequency, so
    // a partial walks to its new phase by being briefly detuned and nothing else can do
    // it without a step. rephaseRem[j] is the debt still owed IN CYCLES and rephaseK the
    // fraction of it to pay per sample, so the offset is rephaseK * rephaseRem[j] - a
    // block constant times a per-partial number, folded in at the point of use because
    // inc[]/alignInc[] are re-ASSIGNED from curRatio/alignRatio every block and cannot
    // park anything of their own. Both banks get the same offset (see advance()).
    //
    // WHAT A SINGLE TIME CANNOT DO, SAID OUT LOUD. Half a cycle in T seconds is an offset
    // of 0.5/T Hz WHATEVER the partial's own frequency is, so the same move is a large
    // INTERVAL down low and an inaudible one up high: at T = 5 ms it is +1794 cents on a
    // 55 Hz partial and +39 cents on a 4400 Hz one. No single T is inaudible across a
    // whole bank, and pretending otherwise would be the lie. What is done about it:
    //   - the time is the panel's own transition Time, which the user sets, floored at
    //     kRephaseMinSec for the same reason the crossfade weight is floored there;
    //   - a DRAG re-aims every block, so the offset is proportional to the drag RATE and
    //     not to 0.5/T at all (a 0.5 s drag at 110 Hz measures a few cents). It is a JUMP
    //     - an automation step, a preset recall - that costs, and it costs once;
    //   - the worst offset actually applied is measured and reportable
    //     (rephaseWorstCentsForTests), so this stays a number and not a hope.
    //
    // AND NO PARTIAL IS EVER PINNED. The offset must not push a partial into the 0.4999
    // Nyquist clamp (silently freezing a partial's frequency is the same class of
    // violation as silently dropping it) nor through zero into a reversed one. It never
    // has to: a phase debt is defined MODULO ONE CYCLE, so rephaseRem[j] and
    // rephaseRem[j] +/- 1 reach the identical phase by opposite routes. A partial whose
    // upward route would clip takes the downward one instead, and vice versa - it arrives
    // at the same time as everyone else, just by the other side. That is legal at ANY
    // moment (it changes the route, never the destination), which is why the check can
    // simply run every block against this block's real increments.
    void updateRephase (const ShepLayout& sl, int span, double sNow, bool allowed,
                        double tSec, double sr, int numSamples) noexcept
    {
        const int e = std::min (span, sl.oscCap());
        if (e <= 0) { rephaseK = 0.0; return; }

        // A NEW AIM. Gated on `allowed`: with BOTH transitions off there is nothing that
        // says how a mid-note phase move should be made smooth, so it is not made at all
        // and the new phases wait for the next note-on - which is what the panel promises,
        // and which is honest precisely because the LEVEL now waits with them. A move
        // already in flight is never abandoned when the mode is switched off mid-gesture:
        // stopping it would strand the partials between two fader positions for ever.
        // With nothing in flight the destination IS where the phases are. Re-stated rather
        // than assumed, because runScatter also moves on its own: a Dry/Wet phase crossfade
        // hands it over when the fade completes, and a stale rephaseDest would then be a
        // phantom debt for the next Morph gesture to deliver on top of its own.
        if (rephaseLeft <= 0) rephaseDest = runScatter;
        const double ds = (allowed && std::abs (sNow - rephaseDest) > 1.0e-12)
                            ? sNow - rephaseDest : 0.0;
        if (ds != 0.0)
        {
            rephaseDest = sNow;
            // EVERY new aim restarts the clock, exactly as the morph's own glide does and
            // for the same reason: keeping the old countdown would let a continuous drag
            // (a fresh value ~60x a second) run the remaining time down to nothing and
            // turn the last stretch of the gesture into a snap.
            rephaseLeft = (int) std::lround (std::max (kRephaseMinSec, tSec) * sr);
            if (rephaseLeft < 1) rephaseLeft = 1;
        }
        if (rephaseLeft <= 0) { rephaseK = 0.0; return; }

        const double  outstanding = rephaseDest - runScatter;
        const double* dtab = spectrum::phaseset::scatterDeltas();
        for (int c = 0; c < sl.numCopies; ++c)
        {
            const int off = sl.off[(size_t) c];
            const int lim = std::min (e, spectrum::kMaxPartials - off);
            // A FRESHLY SEEDED SHEPARD COPY OWES NOTHING. fillShepardCopies has just placed
            // it AT the destination (it was silent, so it was free to place anywhere), and
            // handing it the correction as well is the trap: it would travel the distance
            // twice. Zeroed rather than left alone, because the slot may still be carrying
            // the debt of the grid index that used to live in it.
            //
            // Otherwise: a slot this move has never covered - a rank the template only grew
            // into after the move began - is WRITTEN with the whole outstanding debt rather
            // than added to, since it has none of its own yet.
            const bool whole = sl.fresh[(size_t) c];
            const int  had   = whole ? 0 : std::min (lim, rephaseAimed);
            for (int i = 0; i < had; ++i)
                rephaseRem[(size_t) (off + i)] += ds * rephaseSlopeAt (off + i);
            for (int i = had; i < lim; ++i)
                rephaseRem[(size_t) (off + i)] = outstanding * rephaseSlopeAt (off + i);
        }
        rephaseAimed = std::max (rephaseAimed, e);

        // THE RATE. Stretched to the block when the move would finish inside it, so the
        // offset is applied for exactly as long as the debt lasts and can never overshoot
        // past the target - at most one buffer later than asked, which is under the 5 ms
        // floor anyway.
        const int over = std::max (numSamples, rephaseLeft);
        rephaseK = 1.0 / (double) over;

        // NO PARTIAL PINNED, NO PARTIAL REVERSED. Checked against BOTH banks (they glide
        // at different frequencies during a morph and share one offset), over exactly the
        // slots the render loop will touch.
        for (int c = 0; c < sl.numCopies; ++c)
        {
            const int off = sl.off[(size_t) c];
            const int cnt = std::min (sl.count[(size_t) c], std::min (e, spectrum::kMaxPartials - off));
            for (int i = 0; i < cnt; ++i)
            {
                const size_t j = (size_t) (off + i);
                const double o  = rephaseK * rephaseRem[j];
                const double hi = morphActive ? std::max (inc[j], alignInc[j]) : inc[j];
                const double lo = morphActive ? std::min (inc[j], alignInc[j]) : inc[j];
                if      (hi + o >= kIncCeil)  rephaseRem[j] -= 1.0;   // same phase, the long way DOWN
                else if (lo + o <= kIncFloor) rephaseRem[j] += 1.0;   // same phase, the long way UP
                // The biggest detune this note has been asked to make, as a fraction of
                // the partial's own frequency - the honest form of "how big a pitch blip
                // was that", because the same absolute offset is enormous low and nothing
                // high. Costs one multiply while a move runs and nothing at all otherwise.
                const double a = std::abs (rephaseK * rephaseRem[j]);
                if (lo > 1.0e-12 && a > rephaseWorstRel * lo) rephaseWorstRel = a / lo;
            }
        }
    }

    // Charge the move for `rendered` samples: drain the debt by the fraction that was
    // actually paid, and walk the SOUNDING fader position the same fraction of the way.
    // Both banks moved by the identical offset, so both positions advance by the identical
    // amount and their difference - which the level law blends across - is untouched.
    void drainRephase (const ShepLayout& sl, int span, int rendered) noexcept
    {
        if (rephaseK <= 0.0 || rendered <= 0) return;
        const double f = std::min (1.0, rephaseK * (double) rendered);
        // Over the WHOLE span, including the slots a Shepard copy is not currently
        // sounding (its own count stops below span once its root has risen). Those slots
        // did not move, so their debt is being written off - deliberately, because the
        // invariant everything here rests on is that rem[j] IS (rephaseDest - runScatter)
        // times that partial's delta, and a slot carrying a private, staler debt would
        // break it for every later aim. Nothing is heard: a partial only comes back into a
        // copy's range through the near-Nyquist taper, i.e. from an amplitude of zero.
        const int    e = std::min (span, sl.oscCap());
        const double dAbs = (rephaseDest - runScatter) * f;
        for (int c = 0; c < sl.numCopies; ++c)
        {
            const int off = sl.off[(size_t) c];
            const int lim = std::min (e, spectrum::kMaxPartials - off);
            for (int i = 0; i < lim; ++i) rephaseRem[(size_t) (off + i)] *= (1.0 - f);
        }
        runScatter += dAbs;
        algScatter += dAbs;
        if (f >= 1.0)
        {
            // Landed. EXACTLY on the destination rather than on the accumulated sum, for
            // the same reason the portamento lands exactly on zero: a residue of a
            // thousandth of a fader unit would sit on the note for its whole life and the
            // level law would be derived from it.
            const double snap = rephaseDest - runScatter;
            runScatter += snap; algScatter += snap;   // both banks moved together; both land together
            rephaseLeft = 0; rephaseK = 0.0;
        }
        else rephaseLeft = std::max (1, rephaseLeft - rendered);
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
        // What the normalisation divides the stack by. GEOMETRY, not the window's
        // current value - see shepard::windowSumPeak / windowSumMeanSq and the block
        // in renderChunk that uses them.
        double    normSw = 1.0, normSw2 = 1.0;
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
        // ...read through the SAME function renderNextBlock's block splitter uses, so the
        // two can never disagree about how fast the stack is moving.
        // ...AND WHICH WAY. A descending Shepard is the same illusion with the sign of the
        // rise flipped: the grid walks down instead of up, the raised-cosine window slides
        // the other way, and copies leave through the zero at the BOTTOM edge exactly as
        // they left through the top. Everything downstream is already symmetric - du is a
        // signed per-sample advance, taperWidth takes the absolute value of the block's
        // travel, and the band is centred rather than anchored to either end - so this is
        // one negation and not a second code path.
        //
        // Applied to the SMOOTHING TARGET rather than to shepG, so flipping the arrow while
        // a note is sounding decelerates through zero and accelerates the other way over
        // the same ~one-block smoothing the rate already has. Flipping shepG's increment
        // directly would reverse the stack in a single sample - a click, and the one thing
        // this engine exists to avoid.
        const double rateTarget = (shepDownParam != nullptr && shepDownParam->load() > 0.5f ? -1.0 : 1.0)
                                / juce::jmax (shepard::kMinRiseSec, shepardRiseSeconds());

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
            shepLo  = juce::jmax (band.lo, floorFor (shepard::kDensityHard));
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

        const double P      = juce::jmax (1.0e-6, smShepP);
        const double W      = juce::jmax (1.0e-6, smShepSpan);
        const double A      = std::log2 (f0);
        const double gStart = shepG;
        const double gEnd   = shepG + smShepRate * dt;

        // THE BOTTOM EDGE MOVES ACROSS THE BLOCK, NOT BETWEEN TWO BLOCKS.
        //
        // The HARD density floor still binds INSTANTLY - it has to. It is what keeps the
        // deepest copy inside one kStride slot, and the alternative is the base list
        // truncating at kStride, which is thousands of partials cut in silence: the one
        // thing this synth may never do. But applying it to the edge itself, between two
        // blocks, moved the band 4.42 octaves in one buffer under a held key (Base N
        // pushed to 64 mid-note). Every sounding copy's window position stepped at once,
        // slots were deactivated and re-claimed wherever the window happened to be
        // (measured: a slot claimed at window 0.972, where the file's own rule says a
        // slot is claimed "at a window edge, where the window is ~0 ... and that is what
        // makes re-seeding it inaudible"), and the re-seed writes BOTH phase[] and
        // alignPhase[] - the second of which this file promises is only ever written at
        // crossfade weight 0.
        //
        // So the edge travels from where the window actually stands (shepLo) to where the
        // floor demands, AS A RAMP INSIDE THE BLOCK, folded into the same per-sample
        // window motion the rise already uses. That is what makes the guarantee hold
        // rather than merely be claimed: while the edge rises every copy's u FALLS, so a
        // copy retiring leaves through u = 0 and a copy arriving enters through u = 1 -
        // and windowFromCos is exactly zero at both.
        const double loStart = shepLo;
        const double loWant  = juce::jmax (smShepLo, floorFor (shepard::kDensityHard));
        // How far the edge may travel in ONE plan. The plan needs
        //   (span/period) + |grid drift| + 1  slots and it has kSlots, so the drift budget
        // is kSlots - span/period - 1 intervals. Taken from the band's ACTUAL width rather
        // than from the kMaxCopies worst case, because a narrow band leaves far more room
        // and a density change is exactly when the band is narrowing: at 8 intervals the
        // budget is 1, at the 3.52 a dense template asks for it is 5.5.
        // The rise spends part of it and the edge spends the rest, and the two pull
        // OPPOSITE ways on u - which is why the two bounds are not the same. A move
        // bigger than this finishes over the next few plans; 4.42 octaves takes a handful
        // of 256-sample blocks, and every one of them is smooth.
        // THE BUDGET IS SIGNED, because the grid can now walk either way.
        //
        // The invariant the plan needs is |(loEnd - loStart)/P - (gEnd - gStart)| <= budget:
        // the bottom edge's travel measured in intervals, minus the grid's, is exactly the
        // number of extra slots the plan will ask for, and kSlots is what there is.
        //
        // Written with jmax (0, gEnd - gStart), which is the rise and nothing else, that
        // clamp maps the quantity onto [-budget, +budget] going UP and onto
        // [-budget + d, budget + d] going DOWN - over budget by the whole of d. The
        // consequence is not cosmetic: `planned` reaches kSlots + 1, shepEvDropped fires,
        // and a copy whose window is NOT zero is deleted - a partial dropped in silence
        // plus a step, which is the one thing this engine is not allowed to do and the
        // reason maxPlanSamples exists at all.
        //
        // It needs the whole coincidence to bite - descending, the band at full width so
        // the budget is 1, the bottom edge saturating its bound, and a buffer past
        // maxPlanSamples (an offline bounce) - which is why no test caught it: the descend
        // run measures grid-phase drift, and the two runs that DO stress the plan both use
        // the default direction.
        //
        // Signed, the two bounds are mirror images and the arithmetic is the same one
        // sentence in both directions.
        const double drift   = gEnd - gStart;            // signed: + rising, - falling
        const double budget  = juce::jmax (1.0, (double) shepard::kSlots - W / P - 1.0);
        const double upMax   = P * juce::jmax (0.0, budget + drift);   // edge rising: cancels the grid
        const double downMax = P * juce::jmax (0.0, budget - drift);   // edge falling: adds to it
        const double loEnd   = juce::jlimit (loStart - downMax, loStart + upMax, loWant);
        shepLo = loEnd;

        // Which grid indices are inside the window at ANY moment of this block. u rises
        // with g and FALLS as the bottom edge lifts, so neither bound can be assumed to
        // move one way any more: take both at both ends of the block and use the union.
        // With the drift budget above that union is at most kMaxCopies + 2 == kSlots
        // wide - which is exactly why hashing the grid index by kSlots cannot collide.
        const double bot0 = (loStart     - A) / P - gStart, bot1 = (loEnd     - A) / P - gEnd;
        const double top0 = (loStart + W - A) / P - gStart, top1 = (loEnd + W - A) / P - gEnd;
        long long mLo = (long long) std::ceil  (std::min (bot0, bot1));
        long long mHi = (long long) std::floor (std::max (top0, top1));
        if (mHi < mLo) mHi = mLo;                                   // always at least the root copy
        // EVIDENCE, not prose. The line below used to carry the comment "defensive;
        // never fires" - and at an offline-render block size it fired, deleting a copy
        // whose window was still a third of full scale. What makes it true now is the
        // chunking in renderNextBlock (see shepard::maxPlanSamples); what PROVES it is
        // this counter, which run 46 asserts stays at zero.
        {
            const int planned = (int) (mHi - mLo + 1);
            if (planned > shepEvPlanned) shepEvPlanned = planned;
            if (planned > shepard::kSlots)
            {
                ++shepEvDropped;
                // How loud the deleted copies were at ANY point of this block: the
                // number the "no partial leaves except through a window zero" rule is
                // written in. Only reached when the clamp really bites, so the cos()
                // calls cost nothing on the path that matters.
                for (long long mm = mLo + shepard::kSlots; mm <= mHi; ++mm)
                {
                    const double uA = ((A + (gStart + (double) mm) * P) - loStart) / W;
                    const double uB = ((A + (gEnd   + (double) mm) * P) - loEnd)   / W;
                    for (int q = 0; q <= 16; ++q)
                    {
                        const double u = uA + (uB - uA) * ((double) q / 16.0);
                        const double w = shepard::windowFromCos (u, std::cos (juce::MathConstants<double>::twoPi * u));
                        if (w > shepEvDropW) shepEvDropW = w;
                    }
                }
                mHi = mLo + shepard::kSlots - 1;
            }
        }

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
            sl.u0[c]      = ((A + (gStart + (double) mm) * P) - loStart) / W;
            // A slot claimed for a NEW grid index is claimed at a window edge, where the
            // window is ~0: the copy re-entering at the bottom starts SILENT. That is the
            // no-reset guarantee, and it is what makes re-seeding it inaudible. It holds
            // because mLo and mHi are the UNION over the block: a grid index that was not
            // inside the window at the block's start is outside [0,1] here, and
            // windowFromCos is exactly zero there. shepEvClaimW below is the number that
            // says so, and run 53 asserts on it - the sentence alone was worth nothing
            // while the bottom edge could jump 4.42 octaves between two blocks.
            sl.fresh[c] = ! shepUsed[(size_t) slot] || shepM[(size_t) slot] != mm;
            if (sl.fresh[c])
            {
                // The window the re-seed is about to happen at. The rule says ~0; this
                // is the number that says whether it really was.
                const double u = sl.u0[c];
                const double w = shepard::windowFromCos (u, std::cos (juce::MathConstants<double>::twoPi * u));
                // The two numbers the two invariants are written in, tracked SEPARATELY
                // because they are separate claims about separate banks. The worst window
                // any claim happened at is the amplitude step re-seeding phase[] can cause;
                // the worst crossfade weight any claim happened at is what the file's other
                // rule ("alignPhase[] is only ever WRITTEN while the weight is 0") is about.
                // A max taken over the same event would let one hide behind the other.
                if (w > shepEvClaimW) shepEvClaimW = w;
                if (morphActive && morphWeight > shepEvClaimX) shepEvClaimX = morphWeight;
            }
            shepUsed[(size_t) slot] = true;
            shepM[(size_t) slot]    = mm;
        }
        if (c < shepEvMinCopies) shepEvMinCopies = c;
        ++shepEvBlocks;
        if (shepPrevLoValid)
        {
            const double step = std::abs (loEnd - shepPrevLo);
            if (step > shepEvLoStep) shepEvLoStep = step;
        }
        shepPrevLo = loEnd; shepPrevLoValid = true;
        sl.numCopies     = c;
        sl.windowed      = true;
        // The normalisation's divisors, from the BAND's shape alone (see
        // shepard::windowSumPeak). Constant for the block, so the window survives into
        // the output instead of being divided out of it.
        sl.normSw        = shepard::windowSumPeak   (W / P);
        sl.normSw2       = shepard::windowSumMeanSq (W / P);
        // The window position's advance per SAMPLE. It is the same number for every copy
        // - which is what lets one complex rotation drive all of them - and it now
        // carries BOTH motions: the grid rising (u up) and the band's bottom edge lifting
        // (u down). Folding the edge in here is the whole S3 fix: the edge's travel
        // becomes part of the smooth per-sample window instead of a step between blocks.
        sl.du            = ((gEnd - gStart) * P - (loEnd - loStart)) / W / (double) numSamples;
        sl.blockLogMove  = smShepRate * P * dt;
        // "lowest copy root / f0 ANYWHERE IN THIS BLOCK" - which is what the member says
        // it is, and what renderNextBlock sizes the shared base list from (see the
        // maxRatio line there). It was read at gStart alone, and gStart is only the
        // block's lowest grid position while the stack is RISING.
        //
        // A DESCENDING Shepard walks shepG DOWN (the arrow flips the rate's sign, see
        // rateTarget above), so gEnd < gStart and the deepest copy spends the block
        // getting DEEPER. Sized at gStart the base list is therefore short by the whole
        // block's travel: maxRatio comes out too small, newN too small, and
        // fillShepardCopies' count walk hits `hiN = min(span, cap)` instead of the copy's
        // own Nyquist - so the partials between the two are simply not rendered. Not
        // tapered, not faded, and NOT COUNTED: shepEvCutBlocks only fires on the kStride
        // cap, so this was a partial drop with no number anywhere, which is the one thing
        // this engine may not do.
        //
        // HOW BIG. |gEnd - gStart| is the block's travel in rise intervals; at the panel's
        // fastest rise (kMinRiseSec, 0.05 s per interval) and a 256-sample block at 48 kHz
        // that is 0.107 intervals, i.e. 7.7% of the frequency the deepest copy could
        // actually reach - tens of partials on a dense template, and four times that at a
        // 16/1-period tuning. At an 8 s rise it is a partial or two at the boundary. Both
        // are drops, and the first is audible.
        //
        // min() and not a second branch: rising, gStart IS the minimum and this line is
        // the old one unchanged, so the default direction is bit-for-bit what it was.
        // rootMul[c] is taken at the block MIDPOINT (gMid above) and the midpoint lies
        // between the two ends, so covering both ends covers every copy's own count too.
        //
        // MEASURED, not argued: the last block of the RhythmVerify descending-Shepard test
        // reads the deepest copy's ROOT and its partial COUNT back through
        // voiceShepardForTests and recomputes the count that root's own Nyquist asks for.
        // With the line read at gStart the two disagree; with it read here they agree
        // exactly, and the ascending run agrees either way - which is the whole shape of
        // this defect in one comparison.
        sl.lowestRootMul = std::exp2 ((std::min (gStart, gEnd) + (double) mLo) * P);

        // Advance the rise. Whole steps are renumbered away in multiples of kSlots so
        // the slot hash is untouched (the same trick v1 used on its sine grid).
        shepG = gEnd;
        // ...IN EITHER DIRECTION. The renumbering only ever looked UP, because shepG only
        // ever grew. A descending voice walks it negative instead, and left alone it would
        // run to -1e15 over a long note and lose its low bits - the same precision loss
        // this guard exists to prevent, reached from the other side. Symmetric now, and
        // the two branches share one line of arithmetic: the truncation toward zero that
        // (long long) does is exactly what both want, because the multiple of kSlots is
        // subtracted rather than floored.
        if (shepG >= 4096.0 || shepG <= -4096.0)
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
        // THE FADER POSITION OF THE BANK THIS COPY IS JOINING, not the live parameter.
        // A slot re-entering the window has to be as scattered as its seven siblings or
        // the voice has two crest factors at once and its ONE normalisation number can
        // only be honest about one of them. Identical to the old scatterAmount() read
        // whenever the fader has not moved under a held note, which is every case that
        // existed before this number did.
        const double scatter = runScatter;
        // WHERE A COPY THAT IS NOT SOUNDING GETS PLACED, which is a different question and
        // it took a measurement to see that. A copy is (re)seeded only while its window is
        // exactly zero - newly claimed, or sitting on an edge - so it is free to be put
        // ANYWHERE, and the right answer is not "where the sounding phases are" but "where
        // they are going". A copy placed at the current position and then handed the
        // outstanding correction moves twice; placed there and handed nothing, it rises
        // into the band wearing the fader position it was claimed at while its seven
        // siblings wear the new one, and the voice has ONE normalisation number and cannot
        // describe two crest factors. Measured before this line existed, RhythmVerify run
        // 53's Shepard case at a 500 ms rise: the copies claimed during the gesture read
        // back at fader 0.01 and 0.02 while the rest had arrived at 1.00.
        //
        // The two banks get their OWN destinations, because they legitimately have two:
        // under a Dry/Wet phase crossfade the running bank is still the old phase set and
        // the aligned bank is already the new one, and the hand-over at the end is what
        // moves the voice. With nothing in flight all three numbers are equal and this is
        // the read it replaces, unchanged.
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
                // ...and the ALIGNED bank at ITS position, which is a different number
                // during a Dry/Wet phase crossfade: the running bank is still the old
                // phase set and the incoming one is already the new one. Seeded at the
                // running bank's position instead, this copy would be handed over to the
                // old phases when the fade completes - the whole stack would have moved
                // except the copy that happened to enter the window during the gesture.
                seedRange (alignPhase, off, e, algScatter);
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
                // ONE definition of where the zone starts and how wide it really is, and
                // the verifier calls the SAME function rather than a second copy of it -
                // see taperZoneFor, which is where the renormalisation and its measurement
                // live.
                const TaperZone z = taperZoneFor (curRatio.data(), cnt, maxR, T);
                tf = z.from;
                const double lMax = std::log2 (std::max (1.0e-12, maxR));
                for (int i = tf; i < cnt; ++i)
                    sl.tap[(size_t) c][(size_t) (i - tf)]
                        = taperWeightAt (lMax, curRatio[(size_t) i], z.width);
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
    // THE PLAN THIS NOTE WAS SEEDED FROM, held for the note's life.
    //
    // The pointer, not the engine: a re-publish plans a DIFFERENT slot (kSlots is 8, for
    // exactly this margin), so re-reading the live plan mid-gesture could hand a held note
    // the slope of a spectrum it is not playing. `head` is how much of the array the plan
    // actually covers for THIS note - the rendered prefix clamped to the plan and to the
    // base copy's own span - so everything past it is the hash, which is what the Shepard
    // copies rely on.
    const double* notePlanSet  = nullptr;
    int           notePlanHead = 0;
    bool   modNewNote = false;   // set by noteStarted, consumed by the first render block
    bool   modHeld    = false;   // this voice's own key state, gating its envelopes
    // THE LEFT-HAND END OF THE PITCH RAMP: the modulated Pitch Env Amount this voice
    // produced at the END of the previous block, which is the value that block finished
    // sounding at. ModEngine::evaluateMods advances the LFO phase by dt = numSamples/sr
    // and evaluates AFTER the advance (ModEngine.h:429-431), so the number it returns is
    // the modulator's value at the block's LAST sample - which makes the previous block's
    // copy the value at THIS block's FIRST sample, and a straight line between the two an
    // exact reconstruction of the sampled trajectory with zero added latency.
    //
    // `Primed` is false until the first render block of a non-legato note, so a fresh or
    // STOLEN voice starts flat instead of swooping in from whatever the previous note left
    // behind: at the shipped default Range of 0.5 (a 48-semitone span) an un-primed steal
    // would ramp up to 48 semitones across one buffer, i.e. 5.8 ms at 256/44100.
    float  pEnvAmtPrev   = 0.0f;
    bool   pEnvAmtPrimed = false;
    double modBpm = -1.0, modPpq = -1.0;  // transport, pushed in per block by the processor

    // base + this voice's modulation, converted back through the parameter's own range
    // (the same normalised-space maths the global path uses, so a control behaves
    // identically whichever route it takes). Unmapped targets cost one atomic load.
    float modded (int t, const std::atomic<float>* fallback) const noexcept
    {
        // The BASE comes from this voice's source (the live knob, or the corner's
        // stored value); the parameter object supplies only the range conversion.
        if (auto* p = modOwner[(size_t) t])
        {
            // THE OFFSET CANNOT ASK FOR MORE ROOM THAN THE PARAMETER HAS. Clamped
            // against the headroom instead of clamping the sum, for the same reason as
            // the global path in ModEngine: an offset that overshoots does not make a
            // deeper modulation, it makes a FLAT one - the wave moves for part of its
            // cycle and stands still for the rest. This is the per-voice half of it,
            // and it is the half a user meets first, because pitch is per-voice.
            const float base = src.normOf (p);
            const float off  = juce::jlimit (-base, 1.0f - base, modOff[(size_t) t]);
            // THE VALUE GRID UNDER THE STAIRCASE, and it is NOT the same bug as the block
            // rate above - it is the floor underneath it.
            //
            // juce::RangedAudioParameter::convertFrom0to1 ends in
            //   range.snapToLegalValue (range.convertFrom0to1 (jlimit (0, 1, v)))
            // (juce_RangedAudioParameter.cpp:54-57), which rounds the result to the
            // parameter's declared INTERVAL. pitchEnvAmount is declared with an interval
            // of 0.01 semitone (PluginProcessor.cpp:352-354), so however smooth the
            // modulator was, every modulated pitch landed on an exact 1-cent grid. That
            // tread is amount * pitchEnvValue with pitchEnvValue <= 1, so it is
            // hard-capped at 1.000 cent at any depth, rate or buffer size - invisible next
            // to an 87-cent block step, and the ENTIRE remaining staircase once the block
            // step is gone. At a shallow Range (6 cents of sweep) it left seven reachable
            // pitches across the whole LFO cycle.
            //
            // The NormalisableRange's own conversion keeps the mapping (skew included -
            // timeRange() uses setSkewForCentre, which sets `skew` rather than a lambda,
            // and this honours it) and drops only the grid. Checked, not assumed: all 26
            // per-voice ids in ModEngine::perVoiceTargetIndex are AudioParameterFloat with
            // a plain interval and no snapToLegalValueFunction, and every consumer takes
            // them as continuous floats.
            //
            // DELIBERATELY NOT DONE on the GLOBAL path (ModEngine.h, the write-back loop).
            // Pitch never travels it - that loop skips every per-voice target and
            // pitchEnvAmount is one - and the targets that DO live there include
            // AudioParameterInt/Choice whose consumers do `(int) get("baseN")` and
            // std::lround. De-snapping those turns rounding into truncation and shifts
            // every step boundary by half a step.
            //
            // The jlimit stays. NormalisableRange::convertFrom0to1 calls clampTo0To1,
            // which carries a jassert that fires in Debug on a single ULP over 1.0f, and
            // base + jlimit(-base, 1-base, off) is not provably <= 1.0f in float. The
            // wrapper we are stepping around was doing that clamp for us.
            return p->getNormalisableRange().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, base + off));
        }
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

    // THE NOTE'S PHASE PLAN, COPIED - because a pointer into it is a pointer into a
    // std::vector the WORKER OWNS AND REPLACES.
    //
    // noteStarted used to keep the raw pointer phasesFor() returns and read it every
    // block for the life of the note (see rephaseSlopeAt and seedRange). That pointer
    // aims into PhasePlan::phase, a std::vector<double>; SpectrumEngine publishes with
    // `planPool[w] = std::move (plan)`, whose move-assignment FREES the old buffer. The
    // plan pool is a ring of 8, so eight template recompiles under one held note - which
    // a modulator on any spectrum control produces in a second or two - wrap the ring
    // back to that voice's slot and free the array it is still reading. The audio thread
    // then feeds freed heap to interpolatePhases: partials jump to arbitrary phases, and
    // on a hardened allocator the host crashes.
    //
    // getLivePair() made it reachable sooner than that, because unlike getLivePlan() it
    // does NOT check planStamp against the template's generation - it hands back
    // &planPool[i] unconditionally - so a voice could take a pointer into a slot that was
    // mid-publish. That check is restored in the engine; this copy is what makes the
    // LIFETIME safe, and the two are separate bugs.
    //
    // A copy, not a lock and not a refcount: this is the audio thread, the read is a
    // straight memcpy of at most the plan's own n doubles at note-on, and after it the
    // voice owns every byte it will read for the life of the note - which is also exactly
    // the guarantee the surrounding comment already claims ("re-reading the live plan
    // mid-gesture could hand a held note the slope of a spectrum it is not playing").
    // It costs one array the same size as `phase`, which this voice already carries four
    // of.
    std::array<double, spectrum::kMaxPartials> notePlanPhases {};
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

    //== WHERE THE PHASE FADER IS FOR THE PHASES THAT ARE ACTUALLY SOUNDING =====
    // Not the parameter. The parameter is where the USER has put the control; these two
    // are where the two oscillator banks were PLACED, and under a held note they are
    // allowed to disagree with it - a phase cannot be moved without detuning the partial
    // for a while, so the move takes the panel's transition time (or, with both
    // transitions off, does not happen until the next note-on at all).
    //
    // Every consumer of "how scattered is this voice" reads these, never the parameter:
    //   - the normalisation law, so the LEVEL follows the phases instead of leading them
    //     (the reported symptom: the fader got louder without doing anything);
    //   - a Shepard copy re-entering the window, so it joins the bank it is joining;
    //   - the aligned bank's coherent seeding, so a morph starts from what is sounding.
    // runScatter belongs to phase[], algScatter to alignPhase[]; they are handed over
    // wherever one bank is copied onto the other, and blended by the crossfade weight
    // wherever both are audible.
    double runScatter = 0.0;
    double algScatter = 0.0;

    //== LIVE RE-PHASING state (see updateRephase / drainRephase) ==============
    // The phase each oscillator slot still OWES, in cycles, signed, and defined modulo
    // one cycle - which is what lets a partial that would be pushed into the Nyquist
    // clamp take the same journey the other way round instead of being pinned.
    // Per-slot rather than a single scalar because that modulo choice is per partial and
    // has to stay made for the rest of the move, or the fraction already delivered and
    // the fraction still to come would be two different journeys.
    std::array<double, spectrum::kMaxPartials> rephaseRem {};
    double rephaseDest = 0.0;   // the fader position the phases are travelling TO
    double rephaseK    = 0.0;   // fraction of the debt paid per sample (0 = nothing in flight)
    int    rephaseLeft = 0;     // samples of the move still to run
    int    rephaseAimed = 0;    // slots this move has already been aimed over (see updateRephase)
    // The biggest detune the move has asked for, as a fraction of the partial's own
    // frequency. Reported, never acted on - it is how "how big was the pitch blip" stays
    // a measured number instead of an estimate from the formula.
    double rephaseWorstRel = 0.0;
    // The same 5 ms floor the crossfade WEIGHT uses, and for a related reason: below it
    // the frequency offset needed to walk half a cycle stops being a detune and becomes a
    // chirp (0.5/T Hz - at 5 ms that is +1794 cents on a 55 Hz partial). Morph Time and
    // Dry/Wet Time keep their full range for everything else they drive.
    static constexpr double kRephaseMinSec = 0.005;
    // The ceiling is the SAME 0.4999 fillShepardCopies clamps at (a partial may not be
    // pushed to or past Nyquist) and the floor keeps every frequency strictly positive (a
    // reversed partial is not the partial the user asked for either).
    static constexpr double kIncCeil  = 0.4999;
    static constexpr double kIncFloor = 1.0e-9;

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
    // Set by the message thread (requestFadeOut), consumed by renderNextBlock. The ONLY
    // cross-thread write to this voice, and it is one atomic bool.
    std::atomic<bool> fadeRequest { false };
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
    std::atomic<float>* shepRiseParam = nullptr, * shepDownParam = nullptr;
    bool   shepActive = false;   // LATCHED at note-on: this note either is a Shepard or is not
    bool   shepFresh  = true;    // adopt band/rate instantly (a cold note, not a glide)
    double shepG      = 0.0;     // the rising grid phase, in rise intervals
    double smShepP    = 1.0;     // smoothed log2(rise ratio)
    double smShepLo   = 5.0;     // smoothed window bottom edge TARGET (log2 Hz)
    // ...and the edge actually IN FORCE at the end of the last block. Separate from the
    // glide's own state because the HARD density floor clamps what is in force without
    // being allowed to yank the glide with it: the edge travels to the clamp across the
    // block (see planShepard), the glide keeps heading for the comfortable target, and
    // the two meet without any copy's window ever stepping.
    double shepLo     = 5.0;
    double smShepSpan = 8.0;     // smoothed window span (log2)
    double smShepRate = 0.125;   // smoothed rise rate (intervals per second)
    std::array<long long, shepard::kSlots> shepM    {};  // grid index held by each slot
    std::array<bool,      shepard::kSlots> shepUsed {};
    // WHERE EACH SLOT'S BANK SITS ON THE PHASE FADER. Normally every slot equals
    // runScatter - they all take the same correction - but a copy the render loop skips
    // (window exactly zero) takes none of it, and this is what notices. See
    // fillShepardCopies.
    // Last block's copies, for the verifiers only (see shepProbeForTests).
    int shepProbeCount = 0;
    std::array<long long, shepard::kSlots> shepProbeM    {};
    std::array<double,    shepard::kSlots> shepProbeRoot {};
    std::array<double,    shepard::kSlots> shepProbeW    {};
    std::array<int,       shepard::kSlots> shepProbeN    {};
    // Worst cases since the last reset, for shepEvidenceForTests. Plain scalars written
    // from the audio thread and read by a single-threaded verifier: no allocation, no
    // lock, no string, and nothing here is ever read by the audio path itself.
    double shepEvClaimW = 0.0, shepEvClaimX = 0.0, shepEvDropW = 0.0, shepEvLoStep = 0.0;
    int    shepEvPlanned = 0, shepEvDropped = 0, shepEvMinCopies = 1 << 30, shepEvBlocks = 0;
    int    shepEvCutBlocks = 0, shepEvCutWorst = 0;   // the base list hit the kStride cap
    // ...and the same event as the METER sees it. Written on the audio thread, read by the
    // editor's timer: a relaxed atomic, so no lock and no allocation, and a frame of
    // staleness on a meter is not a correctness question. See shepCapHold above.
    std::atomic<bool> shepCapped { false };
    int               shepCapHold = 0;   // samples left to keep the flag up
    double shepPrevLo = 0.0; bool shepPrevLoValid = false;  // for shepEvLoStep
};
