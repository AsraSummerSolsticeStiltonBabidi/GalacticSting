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

#include <array>
#include <atomic>
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
        }

        //----------------------------------------------------------------------
        // Audio thread: the current template (read-only) or nullptr before the first build.
        const SpectrumTemplate* getLive() const noexcept
        {
            const int i = live.load (std::memory_order_acquire);
            return i < 0 ? nullptr : &pool[(size_t) i];
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

        // How many edits actually found a partial in the live spectrum. Surfaced so the
        // editor can SAY that an edit is currently homeless rather than pretend it applied.
        int getGainMatchedCount() const noexcept { return gainMatched.load(); }

        // Audio thread: a voice reports how many partials it actually renders (after the
        // per-pitch Nyquist cull) so the meter can show the audible count, not the
        // (much larger, pitch-independent) template size.
        void reportRendered (int n) const noexcept { renderedCount.store (n, std::memory_order_relaxed); }
        int  getRenderedCount() const noexcept     { return renderedCount.load (std::memory_order_relaxed); }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                wakeUp.wait (-1);
                if (threadShouldExit()) break;

                SpectrumSettings s;
                bool have = false;
                unsigned long long epoch = 0;
                {
                    const std::lock_guard<std::mutex> lk (requestMutex);
                    if (hasRequest) { s = requested; hasRequest = false; have = true; epoch = pubEpoch; }
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
                        publish (std::move (t));
                }
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

        // Serialised between worker and buildNow (never called on the audio thread).
        void publish (SpectrumTemplate t)
        {
            const std::lock_guard<std::mutex> lk (buildMutex);
            const int w = (live.load() + 1) % (int) pool.size();
            const juce::uint32 g = generation.load (std::memory_order_relaxed) + 1;
            t.generation = g;                    // stamp BEFORE the slot is written + released
            fillGains (t);                       // no template is ever live without its gains
            pool[(size_t) w] = std::move (t);
            liveCount.store ((int) pool[(size_t) w].partials.size());
            rawCount .store (pool[(size_t) w].rawCount);
            truncated.store (pool[(size_t) w].truncated);
            // Keep the standalone counter in sync (used by getGeneration()); the voices
            // read the template's own stamp so the pair is always consistent.
            generation.store (g, std::memory_order_release);
            liveRev.fetch_add (1, std::memory_order_release);
            live.store (w, std::memory_order_release);
        }

        std::vector<PartialGain> overrides;      // the user's edits (message thread writes)
        bool                     overridesOn = false;
        std::mutex               overrideMutex;
        std::atomic<int>         gainMatched { 0 };

        static constexpr int kSlots = 8; // margin so a burst of publishes can't wrap onto a slot a voice is mid-read
        std::array<SpectrumTemplate, kSlots> pool;

        std::atomic<int>  live       { -1 };
        std::atomic<juce::uint32> generation { 0 };
        std::atomic<juce::uint32> liveRev    { 0 };
        std::atomic<int>  liveCount  { 0 };
        std::atomic<int>  rawCount   { 0 };
        std::atomic<bool> truncated  { false };
        std::atomic<bool> abortBuild { false };
        mutable std::atomic<int> renderedCount { 0 };

        std::mutex        requestMutex;
        std::mutex        buildMutex;
        SpectrumSettings  requested;
        bool              hasRequest = false;
        // Guarded by requestMutex. Bumped by publishNow so a build that started earlier
        // cannot publish an older spectrum over a newer one (see publishNow).
        unsigned long long pubEpoch = 0;
        juce::WaitableEvent wakeUp;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumEngine)
    };
}
