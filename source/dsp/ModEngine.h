#pragma once

//==============================================================================
// ModEngine: 6 modulators (3 custom envelopes + 3 LFOs), each routable to ANY
// number of parameters at once. Stepped ONCE PER AUDIO BLOCK on the audio thread
// and written into the parameters' RAW VALUE ATOMICS (the values the DSP reads),
// NOT through setValueNotifyingHost:
//   - smooth (block-rate, not 30 Hz timer steps), no zipper at UI level
//   - no host-automation spam, RT-safe (every audio-thread access is an atomic
//     load/store on pointers cached at construction - no String building, no map
//     lookups, no allocation)
//   - the parameter itself keeps the user's BASE setting; modulation rides on it
//     (base + sum of contributions, clamped to the range). Restoration is done
//     BY THE AUDIO THREAD: any target written last block but not this block gets
//     its base back (see lastW), so unmapping/switching off can never race the
//     audio thread into a stuck value. NOTE: the raw atomics DO carry base+mod,
//     so (a) state capture re-writes the BASE for every parameter (captureState in
//     PluginProcessor.cpp) and (b) while the user is SETTING a mapped parameter,
//     this engine hands that atomic back to its plain base for ~300 ms - long
//     enough for JUCE's own 10 Hz flush to copy the intended value (not a snapshot
//     of the modulation) into the ValueTree and the undo history.
//
// Envelopes: ADSR, retriggered by EVERY note-on (so each new key restarts the
// movement), released when no note is held; a note that starts and ends inside
// one block still releases (never parks at sustain).
// LFOs: sine / triangle / saw up / saw down / square / custom drawn wave; rate
// modes: BPM-divided, fundamental-divided (default; follows the lowest held
// note's frequency), or free Hz; phase modes: restart at each note-on (default)
// or locked to the host transport (BPM / free-Hz rates; the fundamental rate has
// no transport-stable phase, so it falls back to note-restart).
//
// Routing edits happen on the message thread under routingLock; the audio thread
// never takes the lock - it only reads the per-slot atomic pointers. Slots are
// NEVER compacted: removal nulls the slot (the audio loop skips nulls), so an
// in-flight block can at worst apply one stale block of the old routing.
//==============================================================================

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cmath>

class ModEngine : private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr int kEnvs = 3, kLfos = 3, kMods = kEnvs + kLfos; // 0..2 env, 3..5 lfo
    static constexpr int kMaxTargets = 8;    // params one modulator can drive
    static constexpr int kWaveN = 256;       // custom-wave table resolution

    ModEngine (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        // Cache every modulator parameter's raw atomic once: processBlock then
        // runs with ZERO string building / map lookups on the audio thread.
        for (int i = 0; i < kMods; ++i)
        {
            const auto pre = ownPrefix (i);
            const auto rv  = [this, &pre] (const char* suf) { return apvts.getRawParameterValue (pre + suf); };
            auto& q = prm[(size_t) i];
            q.on = rv ("On"); q.range = rv ("Range");
            onParam[(size_t) i] = apvts.getParameter (pre + "On");
            if (i < kEnvs) { q.a = rv ("A"); q.d = rv ("D"); q.s = rv ("S"); q.r = rv ("R");
                             q.cA = rv ("CurveA"); q.cD = rv ("CurveD"); q.cR = rv ("CurveR"); }
            else
            {
                q.wave = rv ("Wave"); q.rateMode = rv ("RateMode"); q.divN = rv ("Div");
                q.hz = rv ("Hz"); q.phaseMode = rv ("PhaseMode"); q.phase = rv ("Phase");
            }
        }
    }

    ~ModEngine() override
    {
        for (auto& w : watched)
            if (auto* p = w.p.load()) p->removeListener (this);
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (auto& e : env) { e = Env{}; }
        // The processor zeroes its note counter here too, so this stays in step
        // WITHOUT swallowing the first note-on: adopting the counter lazily on the
        // first block ate the retrigger of a note that arrived in that same block.
        noteSeen = 0;
    }

    // Message thread: routing management. A modulator may drive OTHER modulators'
    // parameters (rates, ranges, ADSR...) but never its own (direct self-loop).
    // The per-block evaluation order is fixed (mod 0..5), so cross-modulation is
    // well-defined with one block of latency at most.
    static juce::String ownPrefix (int mod)
    { return (mod < kEnvs ? "modE" : "modL") + juce::String (mod < kEnvs ? mod + 1 : mod - kEnvs + 1); }

    void addTarget (int mod, const juce::String& pid)
    {
        if (! juce::isPositiveAndBelow (mod, kMods) || pid.startsWith (ownPrefix (mod))) return;
        auto* p   = apvts.getParameter (pid);
        auto* raw = apvts.getRawParameterValue (pid);
        if (p == nullptr || raw == nullptr) return;
        const juce::ScopedLock sl (routingLock);
        auto& m = mods[(size_t) mod];
        for (auto& id : m.tgtId) if (id == pid) return;
        for (int i = 0; i < kMaxTargets; ++i)
            if (m.tgtId[(size_t) i].isEmpty())
            {
                m.tgtId[(size_t) i] = pid;
                m.slot[(size_t) i].raw.store (raw);                                   // raw first,
                m.slot[(size_t) i].param.store (p, std::memory_order_release);        // param last
                if (mapCount (pid) == 1) watchAdd (pid, p);
                return;
            }
    }
    void removeTarget (int mod, const juce::String& pid)
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return;
        const juce::ScopedLock sl (routingLock);
        auto& m = mods[(size_t) mod];
        for (int i = 0; i < kMaxTargets; ++i)
            if (m.tgtId[(size_t) i] == pid)
            {
                m.slot[(size_t) i].param.store (nullptr); // audio skips null slots; it
                m.slot[(size_t) i].raw.store (nullptr);   // restores the base next block
                m.tgtId[(size_t) i].clear();
                if (mapCount (pid) == 0) watchRemove (pid);
                return;
            }
    }
    void unmapAll (int mod)
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return;
        const juce::ScopedLock sl (routingLock);
        auto& m = mods[(size_t) mod];
        for (int i = 0; i < kMaxTargets; ++i)
        {
            const auto pid = m.tgtId[(size_t) i];
            m.slot[(size_t) i].param.store (nullptr);
            m.slot[(size_t) i].raw.store (nullptr);
            m.tgtId[(size_t) i].clear();
            if (pid.isNotEmpty() && mapCount (pid) == 0) watchRemove (pid);
        }
    }
    int numTargets (int mod) const
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return 0;
        const juce::ScopedLock sl (routingLock);
        int n = 0;
        for (auto& id : mods[(size_t) mod].tgtId) n += id.isNotEmpty() ? 1 : 0;
        return n;
    }
    juce::String targetId (int mod, int i) const // i-th mapped id (skips empty slots)
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return {};
        const juce::ScopedLock sl (routingLock);
        for (auto& id : mods[(size_t) mod].tgtId)
            if (id.isNotEmpty() && i-- == 0) return id;
        return {};
    }
    bool isMappedTo (const juce::String& pid, int mod) const
    {
        if (! juce::isPositiveAndBelow (mod, kMods)) return false;
        const juce::ScopedLock sl (routingLock);
        for (auto& id : mods[(size_t) mod].tgtId) if (id == pid) return true;
        return false;
    }

    // Custom LFO wave table (message thread writes; audio reads - float tearing is
    // transient and inaudible).
    void setCustomWave (int lfo, const float* table256)
    {
        if (! juce::isPositiveAndBelow (lfo, kLfos)) return;
        std::copy (table256, table256 + kWaveN, customWave[(size_t) lfo].begin());
    }
    const float* getCustomWave (int lfo) const
    { return juce::isPositiveAndBelow (lfo, kLfos) ? customWave[(size_t) lfo].data() : nullptr; }

    // True while an LFO's requested rate exceeds what a block-stepped LFO can
    // represent (the UI says so rather than the engine capping in silence).
    bool isRateLimited (int lfo) const noexcept
    { return juce::isPositiveAndBelow (lfo, kLfos) && rateLimited[(size_t) lfo].load(); }

    // How many of this modulator's 8 target slots are in use (the map menu shows it).
    bool isFull (int mod) const { return numTargets (mod) >= kMaxTargets; }

    // Audio thread: called once per block from processBlock BEFORE the synth renders.
    //   noteOnsSoFar - running note-on counter (retrigger detection)
    //   anyHeld      - is any note currently held (accurate THIS block)
    //   fundamental  - lowest held note's Hz (0 if none)
    //   bpm/ppq      - host transport (bpm <= 0 = unknown; ppq < 0 = not playing)
    void processBlock (int numSamples, juce::uint32 noteOnsSoFar, bool anyHeld,
                       double fundamental, double bpm, double ppq)
    {
        if (numSamples <= 0) return; // an empty block must not freeze the envelopes at 0
        lastBlock = numSamples;      // used to size the parameter-hold window
        const bool newNote = noteOnsSoFar != noteSeen;
        noteSeen = noteOnsSoFar;
        const double dt = (double) numSamples / sr;

        // Pass 1: every modulator's output value (0..1). Fixed order 0..5, so a
        // modulator driving ANOTHER modulator's parameters acts with at most one
        // block of latency and never re-enters itself.
        double vals[kMods] = {};
        bool   act[kMods]  = {};
        for (int i = 0; i < kMods; ++i)
        {
            auto& m = mods[(size_t) i];
            const auto& q = prm[(size_t) i];
            bool anyTarget = false;
            for (int t = 0; t < kMaxTargets; ++t)
                if (m.slot[(size_t) t].param.load (std::memory_order_relaxed) != nullptr) { anyTarget = true; break; }
            // Gate on the USER's On setting, never on a modulated copy of it: two
            // modulators cross-mapped onto each other's On would otherwise form a
            // block-rate limit cycle (both off -> restored -> both on -> ...).
            act[i] = (onParam[(size_t) i] != nullptr ? onParam[(size_t) i]->getValue() > 0.5f
                                                     : q.on->load() > 0.5f) && anyTarget;

            if (i < kEnvs)
            {
                // The envelope keeps RUNNING even while unmapped/off so its state is
                // sane the moment it is engaged. It is generated here rather than by
                // juce::ADSR because each segment carries its own CURVE (concave <->
                // convex): a straight line ramp is what made the decay feel like it
                // "drops" instead of falling away.
                auto& e = env[(size_t) i];
                if (newNote) { e.stage = Env::attack; e.pos = 0.0; e.from = e.value; }
                if (! anyHeld && (e.held || newNote))
                { e.stage = Env::release; e.pos = 0.0; e.from = e.value; }
                e.held = anyHeld;

                const double dt = (double) numSamples / sr;
                const double S  = juce::jlimit (0.0, 1.0, (double) q.s->load());
                const auto adv = [&e, dt] (float secs)
                { e.pos += dt / std::max (1.0e-4, (double) secs); return e.pos >= 1.0; };

                switch (e.stage)
                {
                    case Env::attack:
                        if (adv (q.a->load())) { e.stage = Env::decay; e.pos = 0.0; e.value = 1.0; }
                        else e.value = e.from + (1.0 - e.from) * shape (e.pos, q.cA->load());
                        break;
                    case Env::decay:
                        if (adv (q.d->load())) { e.stage = Env::sustain; e.pos = 0.0; e.value = S; }
                        else e.value = 1.0 + (S - 1.0) * shape (e.pos, q.cD->load());
                        break;
                    case Env::sustain: e.value = S; break;
                    case Env::release:
                        if (adv (q.r->load())) { e.stage = Env::idle; e.pos = 0.0; e.value = 0.0; }
                        else e.value = e.from * (1.0 - shape (e.pos, q.cR->load()));
                        break;
                    case Env::idle:
                    default: e.value = 0.0; break;
                }
                vals[i] = juce::jlimit (0.0, 1.0, e.value);
            }
            else
            {
                auto& l = lfo[(size_t) (i - kEnvs)];
                const int  rateMode  = (int) q.rateMode->load();
                const int  divN      = (int) q.divN->load();
                const int  phaseMode = (int) q.phaseMode->load();
                const double phOff   = (double) q.phase->load();
                double rate = 0.0;
                if (rateMode == 0)      rate = bpm > 1.0 ? (bpm / 60.0) / std::exp2 ((double) divN) : 0.0;
                else if (rateMode == 1) rate = fundamental > 0.0 ? fundamental / std::exp2 ((double) divN) : 0.0;
                else                    rate = (double) q.hz->load();
                // The phase steps once per block, so the honest ceiling is the BLOCK
                // rate's Nyquist, not an arbitrary 40 Hz: with a 64-sample buffer a
                // fundamental-tracking LFO can run into the hundreds of Hz. The
                // limit is reported to the UI (lfoRateLimited) instead of hiding.
                const double ceiling = std::max (1.0, 0.5 * sr / (double) numSamples);
                if (rate > ceiling) { rate = ceiling; rateLimited[(size_t) (i - kEnvs)] = true; }
                else                { rateLimited[(size_t) (i - kEnvs)] = false; }
                rate = std::max (0.0, rate);

                if (phaseMode == 1 && rateMode != 1 && ppq >= 0.0 && bpm > 1.0)
                {
                    // Transport-locked. Cycles elapsed since the timeline start:
                    //   BPM mode : beats / 2^div
                    //   free Hz  : seconds * Hz  (seconds = ppq * 60 / bpm)
                    const double cycles = rateMode == 0 ? ppq / std::exp2 ((double) divN)
                                                        : ppq * (60.0 / bpm) * rate;
                    l.phase = std::fmod (cycles + phOff, 1.0);
                }
                else
                {
                    // Note-restart (default). The fundamental rate has no transport-
                    // stable phase, so transport mode also restarts per note there.
                    if (newNote && (phaseMode == 0 || rateMode == 1)) l.phase = phOff;
                    l.phase = std::fmod (l.phase + rate * dt, 1.0);
                }
                vals[i] = evalWave (i - kEnvs, (int) q.wave->load(), l.phase);
            }
        }

        // Pass 2: SUM every active modulator's contribution per UNIQUE target, then
        // write base + sum once (two modulators on one knob add, they don't fight).
        struct W { juce::RangedAudioParameter* p; std::atomic<float>* raw; float sum; };
        W writes[kMods * kMaxTargets];
        int nW = 0;
        for (int i = 0; i < kMods; ++i)
        {
            if (! act[i]) continue;
            auto& m = mods[(size_t) i];
            const float contrib = (float) vals[i] * prm[(size_t) i].range->load();
            for (int t = 0; t < kMaxTargets; ++t)
            {
                auto* p   = m.slot[(size_t) t].param.load (std::memory_order_acquire);
                auto* raw = m.slot[(size_t) t].raw.load();
                if (p == nullptr || raw == nullptr) continue;
                int f = -1;
                for (int k = 0; k < nW; ++k) if (writes[k].raw == raw) { f = k; break; }
                if (f >= 0) writes[f].sum += contrib;
                else if (nW < kMods * kMaxTargets) writes[nW++] = { p, raw, contrib };
            }
        }
        // A target the user is CURRENTLY setting is held at its plain base for a
        // moment (see parameterChanged), so the value JUCE stores in its tree - and
        // therefore the value undo/redo and the saved project see - is exactly the
        // one that was set, never a snapshot of the modulation riding on it.
        for (auto& h : holds)
        {
            if (h.blocks.load() <= 0) continue;
            h.blocks.fetch_sub (1);
            auto* hp = h.param.load();
            if (hp == nullptr) continue;
            for (int k = 0; k < nW; ++k)
                if (writes[k].p == hp) writes[k].sum = 0.0f;
            if (h.blocks.load() <= 0) h.param.store (nullptr); // free the slot
        }
        for (int k = 0; k < nW; ++k)
        {
            const float norm = juce::jlimit (0.0f, 1.0f, writes[k].p->getValue() + writes[k].sum);
            writes[k].raw->store (writes[k].p->convertFrom0to1 (norm));
        }
        // Whatever was modulated LAST block but not this one returns to its own
        // value - the audio thread is always the last writer, so an unmap or a
        // modulator switched off can never leave a parameter stuck displaced.
        for (int k = 0; k < nLastW; ++k)
        {
            bool still = false;
            for (int j = 0; j < nW; ++j) if (writes[j].raw == lastW[k].raw) { still = true; break; }
            if (! still && lastW[k].p != nullptr)
                lastW[k].raw->store (lastW[k].p->convertFrom0to1 (lastW[k].p->getValue()));
        }
        nLastW = nW;
        for (int k = 0; k < nW; ++k) lastW[k] = { writes[k].p, writes[k].raw };
    }

private:
    // How many slots currently hold this id (routingLock is held by the caller).
    // The parameter listener is added when the FIRST slot takes an id and removed
    // when the LAST one lets it go.
    int mapCount (const juce::String& pid) const
    {
        int n = 0;
        for (auto& m : mods)
            for (auto& id : m.tgtId)
                if (id == pid) ++n;
        return n;
    }

    // The user is TOUCHING a mapped parameter (a UI gesture: drag, double-click,
    // wheel, typed value - JUCE's attachments wrap all of them in begin/end
    // gesture). JUCE's APVTS copies the parameter's raw atomic into its ValueTree
    // ~100 ms later THROUGH the undo manager, and that atomic is where this engine
    // writes base+modulation. So while the gesture lasts - and for a moment after -
    // the atomic is handed back to its plain base value: what gets stored, undone
    // and redone is exactly the value that was set. HOST AUTOMATION does not emit
    // gestures, so it never pauses the modulation (automating a knob and modulating
    // it at the same time keeps working).
    // Called from the message thread (UI) or the audio thread (automation), so it
    // only does atomic loads/stores over a fixed array - no lookup, no allocation.
    void parameterGestureChanged (int index, bool starting) override
    {
        auto* p = watchedByIndex (index);
        if (p == nullptr) return;
        setHold (p, starting ? kHoldWhileGrabbed : holdBlocks());
    }
    void parameterValueChanged (int index, float) override
    {
        auto* p = watchedByIndex (index);          // renew only while the user holds it,
        if (p == nullptr) return;                  // so automation is never throttled
        for (auto& h : holds)
            if (h.param.load() == p && h.blocks.load() > 0)
            { h.blocks.store (kHoldWhileGrabbed); return; }
    }

    juce::RangedAudioParameter* watchedByIndex (int index) const
    {
        for (auto& w : watched)
            if (w.index.load() == index) { if (auto* p = w.p.load()) return p; }
        return nullptr;
    }
    void setHold (juce::RangedAudioParameter* p, int blocks)
    {
        for (auto& h : holds)
        {
            juce::RangedAudioParameter* expected = nullptr;
            if (h.param.load() == p || h.param.compare_exchange_strong (expected, p))
            { h.blocks.store (blocks); return; }
        }
    }

    // index -> parameter cache for the listener above (message thread maintains it).
    void watchAdd (const juce::String& pid, juce::RangedAudioParameter* p)
    {
        for (auto& w : watched)
            if (w.p.load() == nullptr)
            {
                w.id = pid; w.index.store (p->getParameterIndex()); w.p.store (p);
                p->addListener (this);
                return;
            }
    }
    void watchRemove (const juce::String& pid)
    {
        for (auto& w : watched)
            if (w.p.load() != nullptr && w.id == pid)
            {
                if (auto* p = w.p.load()) p->removeListener (this);
                w.p.store (nullptr); w.index.store (-1); w.id.clear();
                return;
            }
    }
    int holdBlocks() const noexcept { return juce::jlimit (1, 4096, (int) (0.3 * sr / std::max (1.0, (double) lastBlock))); }
    // While the control is grabbed the hold is simply re-armed every event.
    static constexpr int kHoldWhileGrabbed = 1 << 20;

    struct Slot
    {
        std::atomic<juce::RangedAudioParameter*> param { nullptr };
        std::atomic<std::atomic<float>*>         raw   { nullptr };
    };
    struct Mod
    {
        juce::String tgtId[kMaxTargets]; // message thread only (under routingLock)
        Slot         slot[kMaxTargets];  // audio-thread view of the routing
    };
    // Curve shaper shared by every envelope segment: c = 0 is a straight line,
    // c < 0 bends it concave (slow start, fast finish), c > 0 convex (fast start,
    // long tail - the classic "natural" decay). Same family as the drawn waves.
    static double shape (double t, float c) noexcept
    {
        t = juce::jlimit (0.0, 1.0, t);
        const double k = std::pow (2.0, -3.0 * (double) juce::jlimit (-1.0f, 1.0f, c));
        return std::pow (t, k);
    }

    struct Env
    {
        enum Stage { idle, attack, decay, sustain, release };
        Stage  stage = idle;
        double pos = 0.0;    // 0..1 through the current segment
        double value = 0.0;  // current output
        double from = 0.0;   // level the segment started from (retrigger / release)
        bool   held = false;
    };
    struct Lfo { double phase = 0.0; };
    // Deterministic little RNG for the Random wave (no allocation, no global state).
    struct Rnd
    {
        juce::uint32 state = 0x9E3779B9u;
        double from = 0.5, target = 0.5, lastPhase = 0.0;
        double next() noexcept
        {
            state = state * 1664525u + 1013904223u;
            return (double) ((state >> 8) & 0xFFFFFF) / (double) 0xFFFFFF;
        }
    };
    struct Ptrs
    {
        std::atomic<float>* on = nullptr, * range = nullptr;
        std::atomic<float>* a = nullptr, * d = nullptr, * s = nullptr, * r = nullptr;   // envs
        std::atomic<float>* cA = nullptr, * cD = nullptr, * cR = nullptr;              // segment curves
        std::atomic<float>* wave = nullptr, * rateMode = nullptr, * divN = nullptr,     // lfos
                          * hz = nullptr, * phaseMode = nullptr, * phase = nullptr;
    };

    double evalWave (int lfoIdx, int wave, double ph)
    {
        switch (wave)
        {
            case 0:  return 0.5 + 0.5 * std::sin (ph * juce::MathConstants<double>::twoPi); // sine
            case 1:  return ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0;                            // triangle
            case 2:  return ph;                                                              // saw up
            case 3:  return 1.0 - ph;                                                        // saw down
            case 4:  return ph < 0.5 ? 1.0 : 0.0;                                            // square
            case 6:  // RANDOM: a new level each cycle, glided into (sample & hold that
            {       //          does not step - the target changes, the value walks to it)
                auto& rs = rnd[(size_t) lfoIdx];
                if (ph < rs.lastPhase) { rs.from = rs.target; rs.target = rs.next(); } // wrapped
                rs.lastPhase = ph;
                return rs.from + (rs.target - rs.from) * (ph * ph * (3.0 - 2.0 * ph)); // smoothstep
            }
            default: // custom drawn wave (linear interp over the 256-point table)
            {
                const auto& w = customWave[(size_t) lfoIdx];
                const double x = ph * (kWaveN - 1);
                const int i0 = (int) x;
                const double f = x - i0;
                return (1.0 - f) * w[(size_t) i0] + f * w[(size_t) juce::jmin (kWaveN - 1, i0 + 1)];
            }
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    mutable juce::CriticalSection routingLock; // message-thread routing edits/reads; never audio
    Mod  mods[kMods];
    Ptrs prm[kMods];
    juce::RangedAudioParameter* onParam[kMods] = {}; // base (unmodulated) On state
    Env  env[kEnvs];
    Lfo  lfo[kLfos];
    Rnd  rnd[kLfos];   // per-LFO random-wave state
    struct Last { juce::RangedAudioParameter* p = nullptr; std::atomic<float>* raw = nullptr; };
    Last lastW[kMods * kMaxTargets];
    int  nLastW = 0;
    struct Hold { std::atomic<juce::RangedAudioParameter*> param { nullptr }; std::atomic<int> blocks { 0 }; };
    struct Watch { juce::String id; std::atomic<juce::RangedAudioParameter*> p { nullptr }; std::atomic<int> index { -1 }; };
    Watch watched[kMods * kMaxTargets];  // id -> param, for the RT-safe listener
    Hold holds[16];              // parameters temporarily handed back to their base
    int  lastBlock = 256;        // last block size, for the hold duration
    std::array<std::atomic<bool>, kLfos> rateLimited {};
    std::array<std::array<float, kWaveN>, kLfos> customWave {};
    double sr = 44100.0;
    juce::uint32 noteSeen = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModEngine)
};
