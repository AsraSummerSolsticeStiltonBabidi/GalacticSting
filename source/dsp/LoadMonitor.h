#pragma once

//==============================================================================
// THE LOAD WATCH - "me gustaria que hubiese una opcion en el menu que por default este on
// que te avise de cuando estas cerca de que haya un freeze" (2026-09-19).
//
// A freeze in this synth has two shapes, and this watches both:
//   * THE AUDIO THREAD RUNS OUT OF TIME. Every block has a budget - numSamples / sampleRate -
//     and the cost scales with sounding notes x partials (up to 8 copies with Shepard, x2 while a
//     morph crossfades, + up to four corner engines in MIX). Past 100% of the budget the
//     host drops audio, and a host starved long enough stops answering too.
//   * THE INTERFACE THREAD IS BLOCKED. A few steps run synchronously on the host's message
//     thread (realtime-adapt compiles at high Fractal, a corner engine growing its voices);
//     while one runs, this window and the host's own screen are frozen.
//
// NOTHING HERE LIMITS ANYTHING. It measures and says so; the fix is the user's (the
// project's rule: no silent caps). The audio side is two clock reads and a handful of
// relaxed atomics per block - no allocation, no lock, no string.
//
// "Hot for long enough" is counted in AUDIO seconds on the audio side, not in wall-clock
// time on the message side, so a test that feeds synthetic blocks gets exactly the same
// verdict as a machine that is really overloaded. The calm that ends a warning is counted
// by the Policy from the time it is handed, for the same reason (a test hands it exact
// seconds) - and because a host that stops calling the audio at all must still end it.
//==============================================================================

#include <atomic>
#include <cmath>

namespace fibload
{
    // "Near" is 70%: the host needs its own share of every block, so one plugin at 70% is
    // already close to the edge. It has to hold for half a second, so one busy block (a
    // chord's attack, the OS scheduling something else) is not a warning. An episode ends
    // after two seconds in which NOTHING came close - no trigger of any kind.
    //
    // NOT "two seconds under 50%", which is what this was first: a load that settled at
    // 55-65% then never ended its episode, and since a notice closed with X stays closed
    // for the rest of its episode, one X at a 60% plateau muted every later overload for
    // as long as the user kept playing (review of 2026-09-19).
    inline constexpr float  kHotRatio    = 0.70f;
    inline constexpr double kHotSeconds  = 0.5;
    inline constexpr double kCoolSeconds = 2.0;
    inline constexpr double kEmaSeconds  = 0.3;   // smoothing time of the running load
    // BLOCKS OVER BUDGET, counted as a BURST. One block over its budget is one click - the
    // OS taking the thread away for a moment does that too, and a warning for every such
    // click would be turned off within the hour. Each overrun adds 1 to a count that decays
    // with a one-second time constant, and the warning needs 2.5: three overruns inside
    // about 0.4 s, or one every half second kept up (the count settles at 2.54). One a
    // second (1.58) is not a warning; that load shows up in the running average instead.
    inline constexpr double kBurstSeconds = 1.0;
    inline constexpr float  kBurstCount   = 2.5f;
    // A step on the interface thread longer than this is a freeze you can feel - a quarter
    // of a second in which a click visibly does not answer. Not less: loading a preset or
    // opening a panel is allowed a noticeable moment of its own without being called a
    // freeze, and a warning that fires on every preset load is a warning nobody reads.
    inline constexpr double kStallMs      = 250.0;
    // WHAT ONE VERDICT IS ABOUT. A host may split its device period into several calls - FL
    // Studio at every automation point, others at loop ends - and a 4-sample remainder still
    // pays every per-call cost (the per-voice plans, the partial loops) against a budget of
    // four samples, so it reads as 300% and as a "click" that never happened: the host's
    // deadline is the whole period. Calls shorter than the judge period are ADDED to the ones
    // after them until they cover it, and judged as one. The processor sets it from
    // prepareToPlay: the prepared block, but never more than 5 ms, so a host that prepares
    // for 4096 and plays 128 still has each of its real periods judged on its own.
    inline constexpr double kMaxJudgeSeconds = 0.005;

    class Monitor
    {
    public:
        struct Snapshot
        {
            float ema = 0.0f;          // the smoothed load, 1.0 = the whole block budget
            float peak = 0.0f;         // the worst single block since the last take
            float hotSeconds = 0.0f;   // the longest run at or above kHotRatio since the last take
            float overrunBurst = 0.0f; // the highest the overrun burst count reached (kBurstCount)
            float stallMs = 0.0f;      // the longest interface-thread step since the last take
            int   overruns = 0;        // blocks that took longer than their budget
            int   blocks = 0;          // processBlock calls since the last take (0 = no audio)
            int   oscillators = 0;     // the most oscillators sounding at once (notes x partials)
            int   mixOscillators = 0;  // ...of which in the Preset Playground's MIX corner engines
        };

        // NOT concurrently with noteBlock (prepareToPlay: see kMaxJudgeSeconds).
        void setJudgeSeconds (double s) noexcept
        {
            judge = s > 0.0 && std::isfinite (s) ? (s < kMaxJudgeSeconds ? s : kMaxJudgeSeconds) : 0.0;
            accCost = accTime = 0.0;
        }

        // AUDIO THREAD, once per block: what the block cost and what it was allowed.
        void noteBlock (double costSeconds, double blockSeconds) noexcept
        {
            if (! (blockSeconds > 0.0) || ! std::isfinite (costSeconds) || costSeconds < 0.0)
                return;
            blocks.fetch_add (1, std::memory_order_relaxed);   // every CALL: audio is flowing
            accCost += costSeconds;
            accTime += blockSeconds;
            if (accTime < judge)
                return;                                        // a split period: judged with what follows
            costSeconds  = accCost;
            blockSeconds = accTime;
            accCost = accTime = 0.0;
            const float r = (float) (costSeconds / blockSeconds);
            const float a = (float) (1.0 - std::exp (-blockSeconds / kEmaSeconds));
            float e = ema.load (std::memory_order_relaxed);
            e += a * (r - e);
            ema.store (e, std::memory_order_relaxed);
            raise (peak, r);
            burst *= std::exp (-blockSeconds / kBurstSeconds);
            if (r >= 1.0f)
            {
                overruns.fetch_add (1, std::memory_order_relaxed);
                burst += 1.0;
            }
            raise (burstMax, (float) burst);
            hot = e >= kHotRatio ? hot + blockSeconds : 0.0;
            raise (hotMax, (float) hot);
        }
        // AUDIO THREAD: the average number of oscillators this block ran (see SpectrumEngine).
        void noteOscillators (int n, int mix = 0) noexcept
        {
            int cur = osc.load (std::memory_order_relaxed);
            while (n > cur && ! osc.compare_exchange_weak (cur, n, std::memory_order_relaxed)) {}
            int curMix = mixOsc.load (std::memory_order_relaxed);
            while (mix > curMix && ! mixOsc.compare_exchange_weak (curMix, mix, std::memory_order_relaxed)) {}
        }
        // MESSAGE THREAD: how long one of this plugin's own steps held the interface thread.
        void noteStallMs (double ms) noexcept
        {
            if (std::isfinite (ms)) raise (stall, (float) ms);
        }

        // MESSAGE THREAD: everything since the last take (peaks reset, running values kept).
        Snapshot take() noexcept
        {
            Snapshot s;
            s.ema         = ema.load (std::memory_order_relaxed);
            s.peak        = peak.exchange (0.0f, std::memory_order_relaxed);
            s.hotSeconds  = hotMax.exchange (0.0f, std::memory_order_relaxed);
            s.overrunBurst = burstMax.exchange (0.0f, std::memory_order_relaxed);
            s.stallMs     = stall.exchange (0.0f, std::memory_order_relaxed);
            s.overruns    = overruns.exchange (0, std::memory_order_relaxed);
            s.blocks      = blocks.exchange (0, std::memory_order_relaxed);
            s.oscillators = osc.exchange (0, std::memory_order_relaxed);
            s.mixOscillators = mixOsc.exchange (0, std::memory_order_relaxed);
            return s;
        }

    private:
        static void raise (std::atomic<float>& a, float v) noexcept
        {
            float cur = a.load (std::memory_order_relaxed);
            while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
        }
        std::atomic<float> ema { 0.0f }, peak { 0.0f }, hotMax { 0.0f }, burstMax { 0.0f }, stall { 0.0f };
        std::atomic<int>   overruns { 0 }, blocks { 0 }, osc { 0 }, mixOsc { 0 };
        double hot = 0.0, burst = 0.0;   // audio thread only
        double accCost = 0.0, accTime = 0.0, judge = 0.0;
    };

    // MESSAGE THREAD: when an EPISODE is on, and what it has seen since it began. An episode
    // starts with the first trigger and ends after kCoolSeconds with no trigger at all. With
    // no audio arriving there is nothing to trigger, so a host that stopped calling the
    // plugin ends it as well - and endedWithoutAudio says which of the two it was, so the
    // notice does not claim the load came down when it simply stopped being measured.
    struct Policy
    {
        bool  showing = false;
        float worstEma = 0.0f, worstPeak = 0.0f, worstStallMs = 0.0f, nowEma = 0.0f;
        int   overruns = 0, oscillators = 0, mixOscillators = 0;
        bool  sawLoad = false, sawOverruns = false, sawStall = false;   // which triggers fired
        bool  endedWithoutAudio = false;
        double calmSeconds = 0.0;
        bool  calmHadAudio = false;

        // Which kinds of trigger this episode has seen, as bits (1 load, 2 overruns, 4 stall).
        int kinds() const noexcept { return (sawLoad ? 1 : 0) | (sawOverruns ? 2 : 0) | (sawStall ? 4 : 0); }

        static bool loadTrigger  (const Monitor::Snapshot& s) noexcept { return s.hotSeconds >= (float) kHotSeconds; }
        static bool burstTrigger (const Monitor::Snapshot& s) noexcept { return s.overrunBurst >= kBurstCount; }
        static bool stallTrigger (const Monitor::Snapshot& s) noexcept { return s.stallMs >= (float) kStallMs; }

        // Returns whether an episode is on after this snapshot; dtSeconds is the time since the last.
        bool update (const Monitor::Snapshot& s, double dtSeconds) noexcept
        {
            const bool trigger = loadTrigger (s) || burstTrigger (s) || stallTrigger (s);
            if (s.blocks > 0) nowEma = s.ema;
            if (! showing && trigger)
            {
                showing = true;
                worstEma = worstPeak = worstStallMs = 0.0f;
                overruns = oscillators = mixOscillators = 0;
                sawLoad = sawOverruns = sawStall = false;
                calmSeconds = 0.0;
                calmHadAudio = endedWithoutAudio = false;
            }
            if (showing)
            {
                if (s.blocks > 0 && s.ema > worstEma) worstEma = s.ema;
                if (s.peak > worstPeak)          worstPeak = s.peak;
                if (s.stallMs > worstStallMs)    worstStallMs = s.stallMs;
                if (s.oscillators > oscillators) oscillators = s.oscillators;
                if (s.mixOscillators > mixOscillators) mixOscillators = s.mixOscillators;
                overruns += s.overruns;
                sawLoad     = sawLoad     || loadTrigger (s);
                sawOverruns = sawOverruns || burstTrigger (s);
                sawStall    = sawStall    || stallTrigger (s);
                const double dt = dtSeconds > 0.0 && std::isfinite (dtSeconds) ? dtSeconds : 0.0;
                if (trigger)  { calmSeconds = 0.0; calmHadAudio = false; }
                else          { calmSeconds += dt; calmHadAudio = calmHadAudio || s.blocks > 0; }
                if (calmSeconds >= kCoolSeconds)
                {
                    showing = false;
                    endedWithoutAudio = ! calmHadAudio;
                }
            }
            return showing;
        }
    };
}
