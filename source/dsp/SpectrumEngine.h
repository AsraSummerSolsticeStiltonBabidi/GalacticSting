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
                {
                    const std::lock_guard<std::mutex> lk (requestMutex);
                    if (hasRequest) { s = requested; hasRequest = false; have = true; }
                }

                if (have)
                {
                    auto t = compileSpectrum (s, &abortBuild);
                    if (! abortBuild.load())   // discard a build that was cancelled at teardown
                        publish (std::move (t));
                }
            }
        }

        // Serialised between worker and buildNow (never called on the audio thread).
        void publish (SpectrumTemplate t)
        {
            const std::lock_guard<std::mutex> lk (buildMutex);
            const int w = (live.load() + 1) % (int) pool.size();
            const juce::uint32 g = generation.load (std::memory_order_relaxed) + 1;
            t.generation = g;                    // stamp BEFORE the slot is written + released
            pool[(size_t) w] = std::move (t);
            liveCount.store ((int) pool[(size_t) w].partials.size());
            rawCount .store (pool[(size_t) w].rawCount);
            truncated.store (pool[(size_t) w].truncated);
            // Keep the standalone counter in sync (used by getGeneration()); the voices
            // read the template's own stamp so the pair is always consistent.
            generation.store (g, std::memory_order_release);
            live.store (w, std::memory_order_release);
        }

        static constexpr int kSlots = 4;
        std::array<SpectrumTemplate, kSlots> pool;

        std::atomic<int>  live       { -1 };
        std::atomic<juce::uint32> generation { 0 };
        std::atomic<int>  liveCount  { 0 };
        std::atomic<int>  rawCount   { 0 };
        std::atomic<bool> truncated  { false };
        std::atomic<bool> abortBuild { false };
        mutable std::atomic<int> renderedCount { 0 };

        std::mutex        requestMutex;
        std::mutex        buildMutex;
        SpectrumSettings  requested;
        bool              hasRequest = false;
        juce::WaitableEvent wakeUp;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumEngine)
    };
}
