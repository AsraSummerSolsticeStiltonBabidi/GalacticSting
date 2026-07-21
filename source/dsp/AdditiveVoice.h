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
#include "SineTable.h"
#include "TuningManager.h"

#include <array>
#include <cmath>

class AdditiveVoice : public juce::MPESynthesiserVoice
{
public:
    AdditiveVoice (juce::AudioProcessorValueTreeState& state, const spectrum::SpectrumEngine& eng,
                   const TuningManager& tuningManager)
        : apvts (state), engine (eng), tuning (tuningManager), sine (spectrum::sineTable())
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

        velocityLevel = 0.10f + 0.90f * currentlyPlayingNote.noteOnVelocity.asUnsignedFloat();
        pressure      = currentlyPlayingNote.pressure.asUnsignedFloat();

        fastKill     = false;
        fastKillGain = 1.0f;
        snapAmps     = ! reuse; // cold: snap amps to the template; reuse: slew them (no jump)
        snapNorm     = ! reuse;
        releasing    = false;   // fresh note: parameter re-apply is allowed again

        // Morphing state: on a cold start snap to the current template (no glide);
        // on reuse keep the running frequencies continuous. Sync lastGen either way
        // so a steal is never mistaken for a spectrum change.
        lastGen = engine.getGeneration();
        if (! reuse) { morphActive = false; morphPos = 1.0; prevN = 0; }

        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        updateAdsrParameters();
        adsr.setSampleRate (sr);
        if (! reuse) adsr.reset(); // cold: start from silence; reuse: keep the envelope continuous
        adsr.noteOn();             // reuse: re-attacks from the CURRENT level -> no step

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
        if (allowTailOff) { adsr.noteOff(); filterAdsr.noteOff(); pitchAdsr.noteOff(); releasing = true; }
        else              beginFastKill();
    }

    void notePressureChanged() override  { pressure = currentlyPlayingNote.pressure.asUnsignedFloat(); }
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
            // The ADSR rates are sample-rate dependent, so force a re-apply on the
            // next block instead of skipping it as an unchanged parameter set.
            ampParamsApplied  = { -1.0f, -1.0f, -1.0f, -1.0f };
            filtParamsApplied = { -1.0f, -1.0f, -1.0f, -1.0f };
            pitchParamsApplied= { -1.0f, -1.0f, -1.0f, -1.0f };
        }
    }

    //==========================================================================
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr.isActive() && ! fastKill)
            return;

        updateAdsrParameters();
        updateFilterAdsrParameters();
        updatePitchAdsrParameters();

        // Snapshot the live template: FREQUENCIES snap, AMPLITUDES are the slew target.
        // Pitch comes from the active tuning (12-TET / MTS-ESP / Scala) for the base
        // note, with any MPE per-note pitch bend AND the pitch envelope riding on top.
        // Pitch env: amount (semitones) * the envelope value [0..1], evaluated once
        // per block (like the filter cutoff) - the env itself advances per sample.
        int n = 0;
        double morphW = 0.0; // 0..1 crossfade weight from running phases -> coherent alignPhase
        const auto* tmpl = engine.getLive();
        const double baseFreq = tuning.frequencyForNote ((int) currentlyPlayingNote.initialNote);
        const double pitchEnvSemis = (double) pEnvAmtParam->load() * (double) pitchEnvValue;
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
            const juce::uint32 gen = engine.getGeneration();
            const bool morphOn = morphParam != nullptr && morphParam->load() > 0.5f;
            if (gen != lastGen)
            {
                if (morphOn && ! snapAmps && prevN > 0 && newN > 0)
                {
                    const int lo = std::min (prevN, newN);
                    const int hi = std::max (prevN, newN);
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
                    morphN = hi; morphPos = 0.0; morphActive = true;

                    // Coherent phase reference for the DESTINATION (as if the new
                    // spectrum were freshly played now); the audio crossfades from the
                    // current running phases to this over the morph, so the note ends
                    // phase-coherent (a real pulse in Aligned mode) instead of a drifted
                    // smear. Without this the sustain after a morph sounds "wrong".
                    const bool alg = phaseModeParam->load() < 0.5f;
                    for (int j = 0; j < hi; ++j)
                    {
                        if (alg) alignPhase[(size_t) j] = 0.25;
                        else { const double h = std::sin ((double) j * 12.9898) * 43758.5453;
                               alignPhase[(size_t) j] = h - std::floor (h); }
                    }
                }
                else
                    morphActive = false; // snap
                lastGen = gen;
            }

            if (morphActive)
            {
                const double mt = std::max (0.001, (double) morphTimeParam->load());
                morphPos += (double) numSamples / (mt * currentSampleRate);
                const double t = std::min (1.0, morphPos);
                // Clean pitch-bend glide for almost all of the morph; only the last
                // ~40 ms crossfade to the coherent destination phase (the partial is
                // by then right at its target, so there is no audible beating), so the
                // note ENDS as a proper pulse instead of a phase-drifted smear.
                const double xf = std::min (0.5, 0.04 / mt);
                morphW = (t <= 1.0 - xf) ? 0.0 : std::min (1.0, (t - (1.0 - xf)) / xf);
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

            for (int j = 0; j < n; ++j) // clamp guards a partial pushed past Nyquist by pitch bend
                inc[(size_t) j] = std::min (curRatio[(size_t) j] * f0 / currentSampleRate, 0.4999);

            // Increment for the coherent reference: the DESTINATION frequency (not the
            // gliding one), so alignPhase converges to a proper pulse for the new spectrum.
            if (morphActive)
                for (int j = 0; j < n; ++j)
                {
                    const double dr = (j < newN) ? (double) ps[(size_t) j].ratio : curRatio[(size_t) j];
                    alignInc[(size_t) j] = std::min (dr * f0 / currentSampleRate, 0.4999);
                }

            prevN = n;
            engine.reportRendered (newN); // meter: the true (target) partial count
        }

        if (snapAmps)
        {
            for (int i = 0; i < n; ++i)               curAmp[(size_t) i] = tgtAmp[(size_t) i];
            for (int i = n; i < activeCount; ++i)     curAmp[(size_t) i] = 0.0;
            activeCount = n;
            snapAmps = false;
        }

        // Per-voice normalisation over the ACTUALLY-RENDERED (sub-Nyquist) partials, so
        // every note/patch has a consistent level no matter how many partials survive the
        // Nyquist cull, AND so the summed peak never clips.
        double sumAmp = 0.0, sumSq = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double a = tgtAmp[(size_t) i]; // amps are non-negative
            sumAmp += a;
            sumSq  += a * a;
        }
        const bool aligned = phaseModeParam->load() < 0.5f;
        // Aligned: the pulse peak is sum|amp|, so normalise against it -> guaranteed no clip
        // (dense patches simply get quieter). Scattered: RMS-normalise for even loudness.
        const float tgtNorm = aligned ? (sumAmp > 1.0e-9  ? (float) (0.5  / sumAmp)            : 0.0f)
                                      : (sumSq  > 1.0e-12 ? (float) (0.25 / std::sqrt (sumSq)) : 0.0f);
        if (snapNorm) { normGain = tgtNorm; snapNorm = false; }

        const double sr   = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        const int    span = std::max (n, activeCount);
        const double slew     = 1.0 - std::exp (-1.0 / (0.007 * sr));
        const double normSlew = 1.0 - std::exp (-1.0 / (0.020 * sr));

        // Filter cutoff = base * 2^(envAmount * filterEnv). Updated per block; the filter
        // envelope itself advances every sample below.
        {
            const double octs = (double) fEnvAmtParam->load() * (double) filterEnvValue;
            const double co = (double) fCutoffParam->load() * std::pow (2.0, octs);
            filter.setCutoffFrequency ((float) juce::jlimit (20.0, 0.49 * sr, co));
            filter.setResonance (juce::jlimit (0.1f, 40.0f, fResParam->load()));
        }

        const int    numChannels = output.getNumChannels();

        while (--numSamples >= 0)
        {
            normGain += (float) ((tgtNorm - normGain) * normSlew);

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

            const float env    = adsr.getNextSample();
            const float killG  = fastKill ? fastKillGain : 1.0f;
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

        // Morph finished this block: adopt the coherent destination phases (the
        // crossfade already reached them) so the sustained note carries on exactly
        // like a freshly-played new spectrum, not a phase-drifted version.
        if (morphActive && morphPos >= 1.0)
        {
            for (int i = 0; i < morphN; ++i) phase[(size_t) i] = alignPhase[(size_t) i];
            morphActive = false;
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
    static bool paramsEqual (const juce::ADSR::Parameters& a, const juce::ADSR::Parameters& b) noexcept
    {
        return a.attack == b.attack && a.decay == b.decay
            && a.sustain == b.sustain && a.release == b.release;
    }

    void updateAdsrParameters()
    {
        if (releasing) return; // never disturb a release tail's rate
        juce::ADSR::Parameters p;
        p.attack  = attackParam->load();
        p.decay   = decayParam->load();
        p.sustain = sustainParam->load();
        p.release = releaseParam->load();
        if (! paramsEqual (p, ampParamsApplied))
        {
            ampParamsApplied = p;
            adsr.setParameters (p);
        }
    }

    void updateFilterAdsrParameters()
    {
        if (releasing) return; // never disturb a release tail's rate
        juce::ADSR::Parameters p;
        p.attack  = fAttackParam->load();
        p.decay   = fDecayParam->load();
        p.sustain = fSustainParam->load();
        p.release = fReleaseParam->load();
        if (! paramsEqual (p, filtParamsApplied))
        {
            filtParamsApplied = p;
            filterAdsr.setParameters (p);
        }
    }

    void updatePitchAdsrParameters()
    {
        if (releasing) return; // never disturb a release tail's rate
        juce::ADSR::Parameters p;
        p.attack  = pAttackParam->load();
        p.decay   = pDecayParam->load();
        p.sustain = pSustainParam->load();
        p.release = pReleaseParam->load();
        if (! paramsEqual (p, pitchParamsApplied))
        {
            pitchParamsApplied = p;
            pitchAdsr.setParameters (p);
        }
    }

    void beginFastKill()
    {
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
    std::atomic<float>* morphParam     = nullptr;
    std::atomic<float>* morphTimeParam = nullptr;

    juce::ADSR adsr, filterAdsr, pitchAdsr;
    // Last parameters actually pushed to each ADSR (sentinel = force first apply).
    // See updateAdsrParameters() for why we gate on change.
    juce::ADSR::Parameters ampParamsApplied   { -1.0f, -1.0f, -1.0f, -1.0f };
    juce::ADSR::Parameters filtParamsApplied  { -1.0f, -1.0f, -1.0f, -1.0f };
    juce::ADSR::Parameters pitchParamsApplied { -1.0f, -1.0f, -1.0f, -1.0f };
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
    std::array<double, spectrum::kMaxPartials> alignPhase    {}; // coherent destination phase (morph crossfade)
    std::array<double, spectrum::kMaxPartials> alignInc      {}; // destination-frequency increment for alignPhase

    double        morphPos    = 1.0;   // 0..1 glide progress (>= 1 -> done)
    bool          morphActive = false;
    int           morphN      = 0;     // partial count captured when the morph started
    int           prevN       = 0;     // partials rendered last block (nearest-neighbour source)
    juce::uint32  lastGen     = 0;     // last template generation seen (detects a spectrum change)

    int   activeCount = 0;     // slots to process (includes fading-out tails)
    bool  snapAmps    = true;  // snap amps to target this block (cold note start)
    float normGain    = 1.0f;  // per-voice RMS-normalisation gain (smoothed)
    bool  snapNorm    = true;  // snap the normalisation gain on note start

    float velocityLevel = 0.0f;
    float pressure = 0.0f;

    bool  fastKill = false;
    float fastKillGain = 1.0f;
    float fastKillDelta = 0.0f;
    bool  releasing = false; // in the note-off release tail: freeze ADSR params (see updateAdsrParameters)
};
