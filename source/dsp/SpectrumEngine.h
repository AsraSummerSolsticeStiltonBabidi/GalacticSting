#pragma once

//==============================================================================
// Real-time-safe delivery of the compiled spectrum to the voices.
//
//   - A dedicated low-priority worker thread runs compileSpectrum() OFF the audio
//     thread (a deep fractal can be a heavy build).
//   - Results are published into a small pool of preallocated slots via an atomic
//     index (acquire/release). The audio thread only ever READS the published slot
//     for the duration of one block; the worker never writes the published slot,
//     and with 4 slots a freed slot is not reused for several publishes, so an
//     in-flight block can never see a torn write.
//   - No std::shared_ptr and no allocation/free ever happen on the audio thread.
//==============================================================================

#include <juce_core/juce_core.h>
#include "Spectrum.h"
#include "PartialGains.h"
#include "PhaseSets.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace spectrum
{
    class SpectrumEngine : private juce::Thread
    {
    public:
        SpectrumEngine() : juce::Thread ("SpectrumBuilder")
        {
            startThread (juce::Thread::Priority::low);
        }

        ~SpectrumEngine() override
        {
            abortBuild.store (true);   // let an in-flight heavy compile bail out fast
            signalThreadShouldExit();
            wakeUp.signal();
            stopThread (5000);         // should return almost immediately now
        }

        //----------------------------------------------------------------------
        // Message thread: ask for a rebuild (coalesced; the worker builds the latest).
        void requestRebuild (const SpectrumSettings& s)
        {
            {
                const std::lock_guard<std::mutex> lk (requestMutex);
                requested = s;
                hasRequest = true;
            }
            wakeUp.signal();
        }
        // Build synchronously (call once from prepareToPlay so a template always exists).
        void buildNow (const SpectrumSettings& s)
        {
            publish (compileSpectrum (s));
            // NO planLive() HERE, AND THE REASON IS MEASURED. buildNow() is the message
            // thread, and the live adaptation reaches it while notes are held: with the
            // plan built here the editor's worst timer tick went to 93.9 ms against a
            // 33.6 ms frame - the window stops following the mouse. 38 ms is simply too
            // much to spend anywhere the message thread can wait for it.
            //
            // The worker plans its own publishes behind the release-store instead, so a
            // template compiled at prepareToPlay has no plan until the first real
            // recompile - and a voice with no plan gets today's hash, which is the sound
            // this synth already ships.
        }

        //----------------------------------------------------------------------
        // Audio thread: the current template (read-only) or nullptr before the first build.
        const SpectrumTemplate* getLive() const noexcept
        {
            const int i = live.load (std::memory_order_acquire);
            return i < 0 ? nullptr : &pool[(size_t) i];
        }

        // The phase plan measured on the live template, or nullptr when there is none.
        //
        // ONE acquire-load for both, so a caller that wants the pair takes the slot index
        // once: reading getLive() and then getLivePlan() can straddle a publish and hand
        // back a plan measured on a template the caller is not looking at. The pair is
        // what the guarantee is about - a plan is only ever "not worse than the hash" for
        // the amplitudes it was built on.
        const spectrum::phaseset::PhasePlan* getLivePlan() const noexcept
        {
            const int i = live.load (std::memory_order_acquire);
            if (i < 0) return nullptr;
            // Only if it was measured on the template that is live NOW.
            const juce::uint32 g = pool[(size_t) i].generation;
            if (g == 0 || planStamp[(size_t) i].load (std::memory_order_acquire) != g) return nullptr;
            return &planPool[(size_t) i];
        }
        const spectrum::phaseset::PhaseReport* getLiveReport() const noexcept
        {
            const int i = live.load (std::memory_order_acquire);
            if (i < 0) return nullptr;
            const juce::uint32 g = pool[(size_t) i].generation;
            if (g == 0 || planStamp[(size_t) i].load (std::memory_order_acquire) != g) return nullptr;
            return &reportPool[(size_t) i];
        }
        struct LivePair { const SpectrumTemplate* t = nullptr; const spectrum::phaseset::PhasePlan* p = nullptr; };
        LivePair getLivePair() const noexcept
        {
            const int i = live.load (std::memory_order_acquire);
            if (i < 0) return {};
            return { &pool[(size_t) i], &planPool[(size_t) i] };
        }

        // The rate the voices render at, for the plan's analysis. Set from prepareToPlay;
        // a plan built at the wrong rate would analyse partials that cannot sound, or
        // miss ones that can. Templates published before this is set use the 48 kHz
        // default and are re-planned by the recompile prepareToPlay already triggers.
        void setRenderRate (double sr) noexcept
        { if (sr > 0.0) renderRate.store (sr, std::memory_order_relaxed); }

        // Message thread: is a build or a blend queued, or in flight right now?
        //
        // This exists for the offline verifiers. They have no message loop and no ear, so
        // they have to KNOW that a publish they asked for has happened before they measure
        // what is sounding - and the worker is Priority::low, so "sleep a bit and hope" is
        // a race that a busy machine loses. Measured: 1907 ms and 2309 ms to publish while
        // a verifier ran back-to-back processBlock calls against it, against 3.0 ms of
        // actual compile work. That race is most of why the rhythm suite could report
        // 2, 3, 11, 12, 17 and 19 failures from the same binary at the same commit.
        //
        // ASK THE WORKER TO PLAN THE LIVE TEMPLATE, without compiling anything.
        //
        // prepareToPlay compiles SYNCHRONOUSLY (buildNow) and deliberately does not plan:
        // 38 ms on the message thread is a lost frame, measured. That left the template a
        // session opens with - the one the FIRST note plays - as the only one with no
        // plan, until the user happened to change something. This closes that without
        // putting a millisecond back on the message thread: the worker wakes, plans the
        // slot that is already live, and publishes nothing.
        //
        // Counted in isBuildPending() so awaitSpectrumIdle() and the verifiers wait for it
        // like they wait for a compile - a plan that lands after the check looked is a
        // plan the check will call missing.
        void requestPlan()
        {
            {
                const std::lock_guard<std::mutex> lk (requestMutex);
                hasPlanRequest = true;
            }
            wakeUp.signal();
        }

        // The pending flag and `working` are read together under the request lock, and the
        // worker sets `working` under that same lock as it clears the flag, so there is no
        // instant where a job is neither pending nor in flight.
        bool isBuildPending() const
        {
            const std::lock_guard<std::mutex> lk (requestMutex);
            return hasRequest || hasBlend || hasPlanRequest || working.load (std::memory_order_acquire);
        }

        int  getPartialCount() const noexcept { return liveCount.load(); }
        int  getRawCount()     const noexcept { return rawCount.load(); }
        bool isTruncated()     const noexcept { return truncated.load(); }

        // Bumped on every publish, so a voice can tell the template CHANGED (and
        // start a morph) rather than just re-reading it. Read on the audio thread.
        juce::uint32 getGeneration() const noexcept { return generation.load (std::memory_order_acquire); }

        // Bumped on EVERY swap of the live slot - a full publish AND a gains-only
        // republish, which deliberately leaves the generation alone. The two counters
        // together are how a test can tell "one spectrum change" from "one spectrum
        // change plus two invisible amplitude edits either side of it": the second kind
        // never starts a morph and never arms the template fade, so it lands on the ear
        // as a bare 7 ms amplitude step. See run 42.
        juce::uint32 getLiveRevision() const noexcept { return liveRev.load (std::memory_order_acquire); }

        //----------------------------------------------------------------------
        // Message thread: hand over the user's per-partial amplitude edits and republish
        // the live template with them resolved.
        //
        // The generation is deliberately LEFT ALONE. A voice starts a spectral morph
        // whenever the generation changes, so bumping it on every drag frame would
        // restart the glide clock and re-seed the phase reference ~60 times a second -
        // exactly the staircase that produced the loud clicks. The voice refills its
        // amplitude targets from the template every block regardless, so a new gain array
        // is picked up immediately and glides in over the usual 7 ms: click-free, and no
        // morph in sight.
        //
        // `republish == false` STORES the edits and stops there. It exists for the one
        // caller that is about to publish a template of its own in the same call (the
        // spectrum adaptation, which changes the partial RATIOS and their AMPLITUDES
        // together): republishing first would swap in the OLD partial list carrying gain
        // keys that were computed for the NEW one, so every edit would miss, the previous
        // adaptation's shaping would be stripped, and the ear would get that as a bare
        // 7 ms amplitude step - a thump - milliseconds before the real change. The stored
        // edits are resolved by the very next publish (see fillGains), so nothing is lost.
        void setOverrides (std::vector<PartialGain> ov, bool on, bool republish = true)
        {
            {
                const std::lock_guard<std::mutex> lk (overrideMutex);
                overrides   = std::move (ov);
                overridesOn = on;
            }
            if (! republish) return;
            const std::lock_guard<std::mutex> lk (buildMutex);
            const int cur = live.load();
            if (cur < 0) return;                       // nothing published yet; the next publish picks them up
            const int w = (cur + 1) % (int) pool.size();
            pool[(size_t) w] = pool[(size_t) cur];     // message thread only - allocation is fine here
            fillGains (pool[(size_t) w]);
            liveRev.fetch_add (1, std::memory_order_release);
            live.store (w, std::memory_order_release); // same generation: no morph restart
        }

        // Message thread: publish a template the CALLER already compiled, right now.
        //
        // The worker exists so a deep fractal never blocks anything, and the normal route
        // (requestRebuild) is the right one for every knob and modulator. This is for the
        // caller that has the finished template in its hand ALREADY - the adaptation
        // compiles one to resolve its amplitudes against - and would otherwise throw it
        // away, wait a whole 16.7 ms timer tick for currentSettings() to notice, and then
        // wait again for the worker to build the identical thing. That round trip was pure
        // latency between the player's hands and the spectrum they asked for.
        //
        // It also SUPERSEDES the worker. A rebuild queued or already running was compiled
        // from settings captured BEFORE this template existed, so letting it publish
        // afterwards would put the older spectrum back - and the caller has by then told
        // the poll that this state is already pushed, so nothing would ever correct it:
        // the adaptation would simply vanish. The queued request is dropped and an epoch is
        // bumped, which the worker checks before publishing (see run()). Nothing is lost by
        // it: the caller builds from currentSettings(), so its template already carries
        // every parameter the discarded one did.
        void publishNow (SpectrumTemplate t)
        {
            {
                const std::lock_guard<std::mutex> lk (requestMutex);
                hasRequest = false;
                ++pubEpoch;
            }
            publish (std::move (t));
        }

        //----------------------------------------------------------------------
        // THE PRESET PLAYGROUND'S BLEND, run on this same worker.
        //
        // Corner templates are compiled ONCE when a preset is dropped on a corner and
        // are immutable afterwards, so the job carries shared_ptrs to them: a drag
        // posts a new job every 16 ms and copying four 16k-partial lists per frame
        // would be half a megabyte of memcpy for nothing, while a raw pointer would be
        // a lifetime bug the moment a corner is reassigned mid-blend.
        struct BlendJob
        {
            std::shared_ptr<const SpectrumTemplate> corner[4];
            double w[4] = { 0.0, 0.0, 0.0, 0.0 };  // bilinear weights, already renormalised
            float  curveSlope = 1.0f;              // stamped on the result (see blendTemplates)
            // WHETHER THE VOICES SHOULD TREAT THIS AS A SPECTRUM CHANGE.
            //
            // false is most frames of a drag, and it is the SAME trick setOverrides
            // uses: publishing without bumping the generation means no voice restarts
            // its spectral morph - the amplitudes track the moving blend at 60 Hz over
            // the usual 7 ms slew. Bumping all sixty frames a second would re-seed
            // every voice's morph clock and phase reference per frame: the staircase
            // that produced the loud clicks.
            //
            // true is for a DISCONTINUITY - engaging the playground, a corner being
            // (re)assigned, a discrete setting flipping, or automation jumping the point
            // across the pad - and, THROTTLED to 10 Hz, for the frames of a drag itself
            // (drivePlayground's glide block): each bumped publish is what makes the
            // voices BEND their partial frequencies to the blend (Morph on) or cross
            // the 50 ms template fade (Morph off). Without any bumps a drag could only
            // ever crossfade bin amplitudes in place - the reported "morph does not
            // pitch-bend the partials".
            bool   bumpGeneration = true;
        };

        void requestBlend (BlendJob job)
        {
            {
                const std::lock_guard<std::mutex> lk (requestMutex);
                // Supersede anything the worker was about to publish, for the same
                // reason publishNow does: a rebuild queued from the pre-morph settings
                // would otherwise land AFTER this blend and put the un-morphed spectrum
                // back, with nothing left to correct it.
                hasRequest = false;
                blend      = std::move (job);
                hasBlend   = true;
                ++pubEpoch;
            }
            wakeUp.signal();
        }

        // How many edits actually found a partial in the live spectrum. Surfaced so the
        // editor can SAY that an edit is currently homeless rather than pretend it applied.
        int getGainMatchedCount() const noexcept { return gainMatched.load(); }

        // Audio thread: a voice reports how many partials it actually renders (after the
        // per-pitch Nyquist cull) so the meter can show the audible count, not the
        // (much larger, pitch-independent) template size.
        // WRITTEN ONLY BY A SOUNDING VOICE, WHICH IS WHY THE COUNT ALONE IS NOT A FACT.
        //
        // renderedCount is 0 in two states that could not be further apart: a cold
        // instrument nobody has played yet, and an instrument that is playing and
        // producing nothing. The editor's meter read the number without that distinction
        // and printed "0 partials audible" in the healthy green in BOTH - photographed on
        // a cold instrument at 1600x900, where the synth is completely fine, and again in
        // the silent-warning state, where it is not. One string and one colour cannot be
        // right for both.
        //
        // One more relaxed store of a bool is the whole cost on the audio thread: no
        // allocation, no lock, and no ordering imposed on anything, because both values
        // are advisory read-outs that the DSP never reads back. Never cleared: the
        // question is "has this instrument made a sound since it was built", and letting
        // go of a note must not turn the meter back into a question mark.
        //
        // REJECTED: a -1 sentinel inside renderedCount. The verifier reads
        // getRenderedCount() as a plain count in two places, and a negative would have to
        // be understood at both, off screen, by whoever edits them next.
        void reportRendered (int n) const noexcept
        {
            renderedCount.store (n, std::memory_order_relaxed);
            everRendered.store (true, std::memory_order_relaxed);
        }
        int  getRenderedCount() const noexcept     { return renderedCount.load (std::memory_order_relaxed); }
        bool hasEverRendered()  const noexcept     { return everRendered.load (std::memory_order_relaxed); }

        // MEASURE THE LIVE SLOT'S PLAN, AFTER IT IS ALREADY SOUNDING.
        //
        // buildPlan is 38 ms at the compile budget. Done before the release-store, that
        // is 38 ms of extra latency on EVERY recompile - a patch tweak under a held note
        // taking that much longer to be heard - and it broke four verifier scenarios that
        // render a fixed number of blocks after a change. Done inside buildMutex it was
        // worse still: buildNow() runs on the message thread, and the editor's worst timer
        // tick went from 0.2 ms to 41.1 ms against a 33.6 ms frame.
        //
        // So the template is published at exactly the speed it always was, and the plan
        // arrives afterwards. The header says why that is sound: "a late upgrade is
        // inaudible on notes already sounding, because a plan only reaches a voice at
        // note-on". Notes that start in the gap get today's hash, which is the sound this
        // synth already ships.
        //
        // THE STAMP IS THE WHOLE SAFETY ARGUMENT. A plan is only ever valid for the exact
        // amplitudes it was measured on, so the slot records WHICH template generation it
        // was built for and getLivePlan() hands back nothing unless that matches the live
        // template's own stamp. A publish that lands mid-measurement therefore discards
        // the plan instead of pairing it with a spectrum it never saw.
        void planLive()
        {
            const int w = live.load (std::memory_order_acquire);
            if (w < 0) return;
            const juce::uint32 g = pool[(size_t) w].generation;
            if (planStamp[(size_t) w].load (std::memory_order_acquire) == g && g != 0) return;
            spectrum::phaseset::PhasePlan   plan;
            spectrum::phaseset::PhaseReport rep = planFor (pool[(size_t) w], plan);
            // Still the same template? If not, this plan describes a spectrum nobody is
            // playing and is dropped rather than published against the wrong one.
            if (live.load (std::memory_order_acquire) != w || pool[(size_t) w].generation != g) return;
            planPool[(size_t) w]   = std::move (plan);
            reportPool[(size_t) w] = rep;
            planStamp[(size_t) w].store (g, std::memory_order_release);
        }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                wakeUp.wait (-1);
                if (threadShouldExit()) break;

                SpectrumSettings s;
                BlendJob job;
                bool have = false, haveBlend = false, havePlan = false;
                unsigned long long epoch = 0;
                {
                    const std::lock_guard<std::mutex> lk (requestMutex);
                    // A blend takes priority: while the playground owns the spectrum
                    // there are no ordinary requests, and if one did race in it is by
                    // definition from the settings the blend supersedes.
                    if (hasBlend)        { job = blend;     hasBlend   = false; haveBlend = true; epoch = pubEpoch; }
                    else if (hasRequest) { s   = requested; hasRequest = false; have      = true; epoch = pubEpoch; }
                    // A bare plan request is the lowest priority of the three: a compile
                    // or a blend will publish a NEW template and plan it anyway, which
                    // makes this one moot.
                    if (! have && ! haveBlend && hasPlanRequest) { hasPlanRequest = false; havePlan = true; }
                    // Taken under the SAME lock that clears the pending flag, so there is
                    // no instant at which isBuildPending() can see neither the request nor
                    // the work - which is the whole point of it (see that function).
                    working.store (have || haveBlend || havePlan, std::memory_order_release);
                }

                if (havePlan && ! abortBuild.load()) planLive();

                if (haveBlend)
                {
                    auto t = runBlend (job);
                    bool superseded = false;
                    {
                        const std::lock_guard<std::mutex> lk (requestMutex);
                        superseded = pubEpoch != epoch;   // the point moved again mid-blend
                    }
                    if (! abortBuild.load() && ! superseded)
                    {
                        if (job.bumpGeneration) publish (std::move (t));
                        else                    publishKeepingGeneration (std::move (t));
                    }
                }

                if (have)
                {
                    auto t = compileSpectrum (s, &abortBuild);
                    bool superseded = false;
                    {
                        const std::lock_guard<std::mutex> lk (requestMutex);
                        superseded = pubEpoch != epoch;   // publishNow overtook this build
                    }
                    if (! abortBuild.load() && ! superseded)   // also discards a build cancelled at teardown
                    {
                        publish (std::move (t));
                        // ...and measure its plan BEHIND the release, only on this path.
                        //
                        // NOT after a playground blend: a drag posts a job every 16 ms and
                        // buildPlan is 38 ms, so planning every frame leaves the worker
                        // permanently behind the drag - measured, it broke the round-trip
                        // level check. The header says the same thing (drag budget while a
                        // control moves, one re-run when it settles), and until that settle
                        // pass exists a blended template simply has no plan: the stamp will
                        // not match, getLivePlan() hands back nullptr, and a voice falls
                        // back to today's hash - the sound this synth already ships.
                        if (! abortBuild.load()) planLive();
                    }
                }

                working.store (false, std::memory_order_release);
            }
        }

        // Resolve the user's per-partial edits against THIS template's partial list.
        // Done here, once per publish, rather than in the compiler: the compiler would
        // respond to an edit by deleting partials (see PartialGains.h), and doing it in
        // the voice would repeat the same merge join 32 times a block.
        void fillGains (SpectrumTemplate& t)
        {
            const std::lock_guard<std::mutex> lk (overrideMutex);
            if (! overridesOn || overrides.empty())
            { t.gain.clear(); gainMatched.store (0); return; }
            gainMatched.store (resolveGains (t.partials, overrides, t.gain));
        }

        // The four-corner blend, hierarchically: the top pair by x, the bottom pair by
        // y's counterpart x, then those two rows by y - which is exactly the bilinear
        // weights the caller sent, re-expressed as three crossfades (bilinear IS
        // separable, and a blend is linear in the amplitudes, so the two agree exactly).
        // Corners with no weight are skipped entirely, so a two-preset line costs ONE
        // blend and an unassigned corner never contributes a partial.
        static SpectrumTemplate runBlend (const BlendJob& job)
        {
            const auto row = [&job] (int lo, int hi, SpectrumTemplate& out) -> double
            {
                const double a = job.corner[lo] != nullptr ? job.w[lo] : 0.0;
                const double b = job.corner[hi] != nullptr ? job.w[hi] : 0.0;
                if (a <= 0.0 && b <= 0.0) return 0.0;
                if (b <= 0.0) { out = *job.corner[lo]; return a; }
                if (a <= 0.0) { out = *job.corner[hi]; return b; }
                out = blendTemplates (*job.corner[lo], *job.corner[hi], b / (a + b));
                return a + b;
            };

            SpectrumTemplate top, bottom, result;
            const double wTop = row (0, 1, top);
            const double wBot = row (2, 3, bottom);
            if (wTop <= 0.0 && wBot <= 0.0) return {};
            if (wBot <= 0.0)      result = std::move (top);
            else if (wTop <= 0.0) result = std::move (bottom);
            else                  result = blendTemplates (top, bottom, wBot / (wTop + wBot));

            // The caller's slope, not the blend of the corners' - see blendTemplates.
            result.curveSlope = job.curveSlope;
            return result;
        }

        // Publish WITHOUT advancing the generation: the live template is replaced, the
        // voices pick up the new ratios and amplitudes on their next block, and not one
        // of them starts a spectral morph. See BlendJob::bumpGeneration.
        void publishKeepingGeneration (SpectrumTemplate t)
        {
            fillGains (t);
            const std::lock_guard<std::mutex> lk (buildMutex);
            const int cur = live.load();
            const int w   = (cur + 1) % (int) pool.size();
            t.generation = generation.load (std::memory_order_relaxed);
            pool[(size_t) w] = std::move (t);
            // The plan is measured AFTER the release below - see planLive().
            planStamp[(size_t) w].store (0, std::memory_order_relaxed);
            liveCount.store ((int) pool[(size_t) w].partials.size());
            rawCount .store (pool[(size_t) w].rawCount);
            truncated.store (pool[(size_t) w].truncated);
            liveRev.fetch_add (1, std::memory_order_release);
            live.store (w, std::memory_order_release);
        }

        // Serialised between worker and buildNow (never called on the audio thread).
        void publish (SpectrumTemplate t)
        {
            // GAINS AND PLAN BEFORE THE LOCK, NOT INSIDE IT.
            //
            // buildMutex serialises the worker against buildNow(), and buildNow() runs on
            // the message thread. Measuring a phase plan inside that lock therefore adds
            // its whole cost to a message-thread wait: written that way first and
            // measured, the editor's worst timer tick under a held chord went from 0.2 ms
            // to 41.1 ms against a 33.6 ms frame - the window stops following the mouse.
            // Neither call needs the lock (fillGains takes its own), and the plan is
            // measured on `t`, which nothing else can see yet.
            fillGains (t);                       // no template is ever live without its gains
            const std::lock_guard<std::mutex> lk (buildMutex);
            const int w = (live.load() + 1) % (int) pool.size();
            const juce::uint32 g = generation.load (std::memory_order_relaxed) + 1;
            t.generation = g;                    // stamp BEFORE the slot is written + released
            pool[(size_t) w] = std::move (t);
            // The plan is measured AFTER the release below - see planLive().
            planStamp[(size_t) w].store (0, std::memory_order_relaxed);
            liveCount.store ((int) pool[(size_t) w].partials.size());
            rawCount .store (pool[(size_t) w].rawCount);
            truncated.store (pool[(size_t) w].truncated);
            // Keep the standalone counter in sync (used by getGeneration()); the voices
            // read the template's own stamp so the pair is always consistent.
            generation.store (g, std::memory_order_release);
            liveRev.fetch_add (1, std::memory_order_release);
            live.store (w, std::memory_order_release);
        }


        //======================================================================
        // THE PHASE PLAN, PUBLISHED IN LOCKSTEP WITH THE TEMPLATE IT MEASURES.
        //
        // It lives here and not inside SpectrumTemplate because PhaseSets.h includes
        // Spectrum.h - a plan member would close the cycle. A parallel pool costs the
        // same guarantee for nothing: slot w's plan is written BEFORE the release-store
        // of `live`, so a voice that acquire-loads the live slot sees the template and
        // the plan it was measured on as one pair, exactly as it already does for the
        // generation stamp.
        //
        // WHAT IT IS MEASURED ON. The amplitudes the voice will actually RENDER - the
        // template's own amps multiplied by the resolved per-partial gains - and at the
        // slope the template was compiled at, which is what those amps already embody.
        // The header calls this a precondition rather than a refinement: a plan built on
        // the pre-edit amplitudes measured 2.13 dB WORSE than today's hash after a single
        // pencil edit.
        //
        // WHAT IT COSTS. buildPlan is the expensive call - 38 ms worst case at the
        // compile budget on the suite's own box - and this is the background compile
        // thread, next to a compile that is heavier. It is NOT on the audio thread and
        // NOT per frame.

        //======================================================================

        spectrum::phaseset::PhaseReport planFor (const SpectrumTemplate& t,
                                                 spectrum::phaseset::PhasePlan& out) const
        {
            const int n = (int) t.partials.size();
            if (n <= 0) { out = spectrum::phaseset::PhasePlan(); return {}; }
            // The rendered amplitudes, in the template's own partial order.
            std::vector<spectrum::Partial> rendered (t.partials);
            if (t.gain.size() == t.partials.size())
                for (size_t i = 0; i < rendered.size(); ++i)
                    rendered[i].amp *= t.gain[i];
            spectrum::phaseset::Options opt;
            opt.sampleRate = renderRate.load (std::memory_order_relaxed);
            // TWICE THE COMPILE BUDGET, and the reason is measured rather than guessed.
            //
            // At kWorkBudgetCompile the report comes back with `reachReduced` set: the
            // budget, not the pitch, was cutting how far the analysis got. Doubling it
            // clears that flag AND takes the crest from 1.6078 to 1.5517 against a hash of
            // 2.0830 - 2.25 dB becomes 2.55. Four times and eight times give 1.5517 to the
            // last digit, so this is the knee and not a slope: the search has found
            // everything it can find at this reach.
            //
            // WHAT IT COSTS is latency-to-plan, not audio and not the editor: this runs on
            // the worker BEHIND the release-store (see planLive), so a recompile's plan
            // simply lands about twice as late, and a note started in that window gets
            // today's hash - which is the documented fallback and the sound this synth
            // already shipped. The budget is a deterministic WORK counter, so a slower
            // machine pays proportionally and nothing changes about what is produced.
            //
            // AND NOT MORE REACH. Asking the analysis to cover C1 instead of C2 was
            // measured at 4x and 16x: the reach is reached (32.7 Hz, and at 16x the budget
            // stops being the limit at all) and the crest win vanishes completely - chosen
            // 2.1451 against a hash of 2.1451. The advantage lives at this prefix; spread
            // over a longer one there is nothing to find, and the engine says so by handing
            // the hash back.
            opt.workBudget = 2LL * spectrum::phaseset::kWorkBudgetCompile;
            return spectrum::phaseset::buildPlan (rendered.data(), n, opt, out);
        }

        std::vector<PartialGain> overrides;      // the user's edits (message thread writes)
        bool                     overridesOn = false;
        std::mutex               overrideMutex;
        std::atomic<int>         gainMatched { 0 };

        static constexpr int kSlots = 8; // margin so a burst of publishes can't wrap onto a slot a voice is mid-read
        std::array<SpectrumTemplate, kSlots> pool;
        std::array<spectrum::phaseset::PhasePlan, kSlots> planPool;
        // Kept beside the plan so the integration can be CHECKED, not just used: the
        // engine's guarantee is "never worse than the hash", and a number nobody reads
        // is a guarantee nobody keeps.
        std::array<spectrum::phaseset::PhaseReport, kSlots> reportPool;
        // Which template generation each slot's plan was measured on. 0 = none yet.
        std::array<std::atomic<juce::uint32>, kSlots> planStamp {};
        std::atomic<double> renderRate { 48000.0 };

        std::atomic<int>  live       { -1 };
        std::atomic<juce::uint32> generation { 0 };
        std::atomic<juce::uint32> liveRev    { 0 };
        std::atomic<int>  liveCount  { 0 };
        std::atomic<int>  rawCount   { 0 };
        std::atomic<bool> truncated  { false };
        std::atomic<bool> abortBuild { false };
        mutable std::atomic<int> renderedCount { 0 };
        mutable std::atomic<bool> everRendered { false }; // see reportRendered: 0 is not a diagnosis

        mutable std::mutex requestMutex;
        std::mutex        buildMutex;
        SpectrumSettings  requested;
        bool              hasRequest = false;
        bool              hasPlanRequest = false;   // plan the live slot, compile nothing
        BlendJob          blend;                  // guarded by requestMutex
        bool              hasBlend   = false;     // coalesced: the latest point wins
        // Guarded by requestMutex. Bumped by publishNow so a build that started earlier
        // cannot publish an older spectrum over a newer one (see publishNow).
        unsigned long long pubEpoch = 0;
        juce::WaitableEvent wakeUp;
        // True from the moment the worker takes a job until it has finished publishing it.
        // Read together with the two pending flags by isBuildPending().
        std::atomic<bool> working { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumEngine)
    };
}
