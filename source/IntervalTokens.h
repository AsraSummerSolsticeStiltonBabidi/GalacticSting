#pragma once

//==============================================================================
// Where each interval token STARTS and STOPS in the Custom Engine's text box, and
// which entry of a colour palette it gets.
//
// WHY IT IS NOT IN THE EDITOR. Colouring the box means rebuilding its text run by
// run, and a run whose range is one character out paints the wrong characters -
// silently, because the wrong colour is still perfectly legible text. The editor
// itself cannot be unit tested (the verifiers build the PROCESSOR only), so the
// arithmetic lives here, JUCE-free, and SpectrumTests exercises it.
//
// PRESENTATION ONLY. Nothing here changes what a token means or how it parses -
// which is exactly why the separator set below is the parser's own set, copied
// with its source named: if the two ever drift, the colours would draw token
// boundaries the parser does not agree with, which is worse than no colour at all.
//==============================================================================

#include <vector>

namespace intervaltokens
{
    // The set loadTimbreText() splits on (PluginProcessor.cpp: addTokens with
    // " \t\r\n,;"). Anything else - digits, '/', '.', 'c', a sign - is token body.
    inline bool isSeparator (int cp) noexcept
    {
        return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == ',' || cp == ';';
    }

    struct Span
    {
        int start  = 0; // index of the token's first character
        int length = 0; // how many characters it spans
        int colour = 0; // index into the caller's palette
    };

    // One span per token, in order, with a palette index that is never the same as
    // the previous token's - which is the whole point of the feature: "3/2" must not
    // wear the colour of the value before it or the one after it. With paletteSize
    // entries the cycle is i % paletteSize, so a palette of at least 2 guarantees it
    // (0,1,0,1,...); the editor's is 5, so a run of five is distinct.
    //
    // Indices are CHARACTER indices, not bytes: juce::TextEditor ranges count
    // characters, so the caller hands us the text as code points.
    inline std::vector<Span> spans (const std::vector<int>& text, int paletteSize)
    {
        std::vector<Span> out;
        if (paletteSize < 2) paletteSize = 2; // 1 would colour every token the same

        const int n = (int) text.size();
        int i = 0;
        while (i < n)
        {
            while (i < n && isSeparator (text[(size_t) i])) ++i;
            if (i >= n) break;
            const int start = i;
            while (i < n && ! isSeparator (text[(size_t) i])) ++i;
            out.push_back ({ start, i - start, (int) out.size() % paletteSize });
        }
        return out;
    }
}
