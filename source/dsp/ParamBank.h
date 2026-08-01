#pragma once

//==============================================================================
// ParamBank + ParamSource: ONE voice/mod implementation, TWO value sources.
//
// The playground's MIX mode runs up to four corner presets as real sub-engines,
// each with its own AdditiveVoices and its own ModEngine - but those two classes
// were written against the ONE live APVTS: they cache raw std::atomic<float>
// pointers at construction and read the parameter objects' normalised base values
// for per-voice modulation. A corner must read ITS OWN preset's values instead.
//
// The wrong fix is a second voice class ("MixAdditiveVoice") reading a different
// store: the two would drift apart the first time anyone fixed a bug in one of
// them. The right fix is this indirection:
//
//   ParamBank   - an array of std::atomic<float>, one per parameter, indexed in
//                 getParameters() order (exactly how PlaygroundCorner::params is
//                 indexed) and looked up by the same ids the APVTS uses. Filled
//                 from a corner's stored preset values on the message thread;
//                 read - and, for the corner ModEngine's global targets, written -
//                 on the audio thread. It stores BOTH the denormalised value (what
//                 getRawParameterValue would hand out) and the normalised base
//                 (what RangedAudioParameter::getValue would), because the voice's
//                 per-voice modulation works in normalised space.
//
//   ParamSource - the two-way switch AdditiveVoice and ModEngine construct from.
//                 bank == nullptr means "the live patch": every call lands on the
//                 APVTS/parameter objects exactly as before, so the main engine's
//                 behaviour is bit-for-bit unchanged. bank != nullptr means "this
//                 corner": raw pointers come from the bank, and the normalised
//                 BASE comes from the bank too - the live parameter object is
//                 still used, but only for its RANGE (convertFrom0to1 etc.),
//                 which is identical for every corner by construction (one
//                 parameter layout).
//==============================================================================

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ParamBank
{
public:
    // Enumerate the processor's parameters ONCE and pin the id -> index map. Indexing
    // matches getParameters() order, which is also how a playground corner stores its
    // preset's values - so filling the bank from a corner is a straight array walk.
    explicit ParamBank (const juce::AudioProcessor& proc)
    {
        const auto& all = proc.getParameters();
        n = all.size();
        denormV.reset (new std::atomic<float>[(size_t) n]);
        normV.reset (new std::atomic<float>[(size_t) n]);
        prms.resize ((size_t) n, nullptr);
        for (int i = 0; i < n; ++i)
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
            {
                prms[(size_t) i] = p;
                byId.emplace (p->paramID.toStdString(), i);
                const float nv = p->getDefaultValue();
                normV[(size_t) i].store (nv);
                denormV[(size_t) i].store (p->convertFrom0to1 (nv));
            }
    }

    // Fill every slot from a corner's values. `value(i, out)` returns false when the
    // corner's preset predates parameter #i, in which case the parameter's DEFAULT is
    // used - never the live patch's value, so a corner sounds the same whatever was
    // loaded when it was assigned (the same rule cornerValueOr documents).
    // Message thread; the stores are atomic so the audio thread reads torn-free.
    void fill (const std::function<bool (int, double&)>& value)
    {
        for (int i = 0; i < n; ++i)
        {
            auto* p = prms[(size_t) i];
            if (p == nullptr) continue;
            double v = 0.0;
            if (! value (i, v)) v = (double) p->convertFrom0to1 (p->getDefaultValue());
            denormV[(size_t) i].store ((float) v);
            normV[(size_t) i].store (juce::jlimit (0.0f, 1.0f, p->convertTo0to1 ((float) v)));
        }
    }

    // The bank's atomic for `id` (the corner engine's stand-in for
    // getRawParameterValue). The corner ModEngine WRITES its global targets here -
    // base + modulation, exactly as the live engine writes the live atomics - which is
    // why this is not const data.
    std::atomic<float>* raw (const juce::String& id) const
    {
        const auto it = byId.find (id.toStdString());
        return it == byId.end() ? nullptr : denormV.get() + it->second;
    }

    // The corner's normalised BASE for parameter #paramIndex (the stand-in for
    // RangedAudioParameter::getValue in per-voice modulation maths).
    float norm (int paramIndex) const noexcept
    {
        return juce::isPositiveAndBelow (paramIndex, n) ? normV[(size_t) paramIndex].load()
                                                        : 0.0f;
    }

private:
    int n = 0;
    std::unique_ptr<std::atomic<float>[]> denormV, normV;
    std::vector<juce::RangedAudioParameter*> prms;
    std::unordered_map<std::string, int> byId;

    JUCE_DECLARE_NON_COPYABLE (ParamBank)
};

//==============================================================================
struct ParamSource
{
    juce::AudioProcessorValueTreeState* apvts = nullptr;
    ParamBank*                          bank  = nullptr;   // nullptr = the live patch

    // The value atomic the DSP reads (and the corner ModEngine writes).
    std::atomic<float>* raw (const juce::String& id) const
    {
        return bank != nullptr ? bank->raw (id) : apvts->getRawParameterValue (id);
    }

    // The parameter OBJECT - always the live one. For a corner it supplies only the
    // range maths (identical across corners: one layout); its VALUE must never be
    // read directly for a corner, which is what normOf below is for.
    juce::RangedAudioParameter* param (const juce::String& id) const
    {
        return apvts->getParameter (id);
    }

    // The normalised base value of `p` under THIS source: the live knob for the main
    // engine, the corner's stored value for a corner engine.
    float normOf (const juce::RangedAudioParameter* p) const
    {
        return bank != nullptr ? bank->norm (p->getParameterIndex()) : p->getValue();
    }

    bool isLive() const noexcept { return bank == nullptr; }
};
