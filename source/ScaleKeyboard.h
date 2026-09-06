#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>

//==============================================================================
// ScaleKeyboard: replaces the piano keyboard with one equal-width cell per MIDI
// key (no black/white distinction). Each cell is coloured by its scale DEGREE,
// sweeping the visible-light order red (low) -> violet (high) across one period
// and repeating every period, and is labelled with the degree number (0-based,
// resetting to 0 at each period). Click / drag to play; held notes (from any
// source, incl. the computer keyboard) are highlighted.
//
// The number of degrees per period comes from the active tuning (12 for 12-TET,
// the loaded scale's count for Scala, the master's map size for MTS-ESP).
//==============================================================================
class ScaleKeyboard : public juce::Component,
                      public juce::TooltipClient,
                      private juce::Timer
{
public:
    ScaleKeyboard (juce::MidiKeyboardState& s, std::function<int()> degreesProvider)
        : state (s), getDegrees (std::move (degreesProvider))
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false); // keep computer-keyboard play alive after a click
        startTimerHz (30); // reflect held notes + tuning changes live
    }

    ~ScaleKeyboard() override { stopTimer(); }

    // The visible key window: lowest MIDI note at the left edge (the reference key
    // that defines degree 0 - matches TuningManager::kRefKey = 60 so every tonic
    // reads "0") and how many keys to show. `numKeys <= 0` restores the automatic
    // ~26 px-per-cell count. Adapting the window to the tuning's period keeps wide
    // scales from exposing sub-audio / ultrasonic keys.
    // The full playable range [lowestKey, lowestKey+numKeys). If more keys fit than
    // the minimum readable width allows, only a window is shown and left/right arrows
    // scroll through the rest (so a dense scale like 31-EDO never gets microscopic).
    void setRange (int lowestKey, int numKeys = 0)
    {
        const int lo = juce::jlimit (0, 127, lowestKey);
        const int cnt = numKeys > 0 ? juce::jmin (numKeys, 128 - lo) : juce::jmax (1, getWidth() / 30);
        if (lo != rangeLo || cnt != rangeCount)
        {
            rangeLo = lo; rangeCount = cnt;
            // Keep the current view; clampView() below slides it the MINIMUM amount
            // into the new range (never teleport to the far end of the keyboard).
        }
        clampView();
        repaint();
    }
    int  getBaseKey() const noexcept { return baseKey; }

    // Scroll the visible window (arrow keys / UI). Public so the editor can bind
    // the computer keyboard's left/right arrows to it.
    void scrollView (int deltaKeys) { scrollBy (deltaKeys); }

    // Auto-follow: shift the visible window the MINIMUM amount that brings the
    // played range [lo..hi] on screen (no jump when it already is).
    void ensureVisible (int lo, int hi)
    {
        const int cells = numCells();
        if (! isScrollable() || hi < lo) return;
        if (hi - lo + 1 > cells) return; // span wider than the window: following one
                                         // end would ping-pong at timer rate - stay put
        if (lo < baseKey)                 scrollBy (lo - baseKey);
        else if (hi >= baseKey + cells)   scrollBy (hi - (baseKey + cells - 1));
    }

    // Screen position of the hovered key's eye (top-centre of its strip cell), so the
    // floating hover window can park right above it with a pointer down to the key.
    // Returns (0,0) when nothing is hovered.
    juce::Point<int> getHoverAnchorScreen() const
    {
        if (hoverCell < 0) return {};
        // Off cellRect, the one function that knows where the keys start (see
        // keysOriginX): with the keys centred in a wider window, an anchor built from
        // arrowW alone parks the hover callout over a different key than the one hovered.
        return localPointToGlobal (juce::Point<int> ((int) cellRect (hoverCell).getCentreX(), 0));
    }
    bool isHoverActive() const noexcept { return hoverArmed && hoverCell >= 0; }

    // Test hook: the span the keys REALLY occupy, in this component's own coordinates,
    // built from cellRect - the very function paint() fills from. Not a reconstruction:
    // a probe with its own copy of the rule would go on passing after paint() changed,
    // which is the whole reason "the keyboard is centred" was never checkable before.
    // Empty when the tuning has no playable key in the window at all.
    juce::Rectangle<float> drawnKeysSpan() const
    {
        const int n = liveCells();
        if (n <= 0) return {};
        return cellRect (0).getUnion (cellRect (n - 1));
    }
    int drawnKeyCount() const { return liveCells(); }
    int windowKeyCount() const { return numCells(); }

    // Optional: a short just-intonation label per degree (e.g. "3/2"), drawn
    // under the degree number - the interval each Sethares minimum lands on.
    void setDegreeLabeller (std::function<juce::String(int)> fn) { getLabel = std::move (fn); }
    std::function<juce::String(int)> getDegreeLabeller() const { return getLabel; }

    //== DEGREE NAMES instead of degree numbers ================================
    // Optional, off unless the editor turns it on. `fn` returns the NAME of a degree
    // (empty string = this tuning has none), `face` is the typeface those names must be
    // drawn in. The face is not decoration: in notes.otf the accidentals ride on ordinary
    // ASCII letters, so C sharp is the two characters "Cc" and drawing them in any other
    // font produces the literal letters C and c - a wrong note name, not a missing glyph,
    // and therefore invisible to every check that only looks at rectangles. Pass a null
    // provider or a null face to go back to the numbers.
    void setDegreeNamer (std::function<juce::String(int)> fn, juce::Typeface::Ptr face)
    {
        getDegreeName = std::move (fn);
        nameFace      = std::move (face);
        ++namerGen;   // the widest-label cache is keyed on this - see refreshWidestCache
        repaint();
    }
    // Is a namer installed AND does this tuning have a name for every degree? A tuning
    // half-named would put "C" on one key and "7" on the next, so it is all or nothing.
    // Cached on the same key as the widest label, and for the same reason: paint() asks
    // twice per frame and the walk is O(degrees). See refreshWidestCache.
    bool namerActive() const { refreshWidestCache(); return cachedNamerActive; }
    // Do this tuning's names fit? Measured at kCellW - the NARROWEST key the keyboard
    // ever draws, which by construction is never the one on screen - against the widest
    // name the tuning produces, in the face they are drawn in. A name that does not fit
    // is not clipped by drawFittedText: it is squeezed horizontally until it ellipsises,
    // which looks deliberate rather than broken. Below kMinNameFont the keyboard keeps
    // its numbers, and the menu says which of the two reasons it was.
    bool namesFitTheKeys() const { return namerActive() && nameFontAtMinKey() >= kMinNameFont; }
    // The height the NAMES would be drawn at on the narrowest key - the number
    // namesFitTheKeys() judges, exposed so a check (and the menu's explanation) can
    // report how close to the line a tuning is instead of only "yes" or "no".
    float nameFontAtMinKey() const
    {
        return getDegreeName == nullptr ? 0.0f
             : fittedLabelFontForCell ((float) kCellW, keysHeight(), widestNameText(), nameFace);
    }
    // ...and the height whatever is ACTUALLY drawn gets there, names or numbers.
    float labelFontAtMinKey() const
    {
        return fittedLabelFontForCell ((float) kCellW, keysHeight(), widestDegreeText(), labelFace());
    }
    // Is the keyboard actually SHOWING names right now?
    bool showingNames() const { return namerActive() && namesFitTheKeys(); }

    // The font height a whole ROW of degree labels gets on a `cellW` x `cellH` key: the
    // height the band allows, brought down until `widest` fits the key. ONE height for
    // the whole row on purpose - fitting each key on its own put a 21 pt "C" next to a
    // 9 pt "wwwGa", which reads as a rendering fault rather than a scale. Public so a
    // check can ask about the smallest key instead of whichever one a test window made.
    float fittedLabelFontForCell (float cellW, float cellH, const juce::String& widest,
                                  juce::Typeface::Ptr face) const
    {
        auto cell = juce::Rectangle<float> (0.0f, 0.0f, cellW, cellH).reduced (0.6f);
        const juce::String label = getLabel ? getLabel (0) : juce::String();
        float fontH = 0.0f;
        const auto band = degreeArea (cell, label, cellW, fontH);
        return shrinkToWidth (fontH, widest, maxLabelWidth (band), face);
    }
    // The smallest a NAME may be drawn before the keyboard gives up and keeps its
    // numbers. Below this the accidentals - a half sharp against a three-quarter sharp -
    // stop being distinguishable, which is worse than an honest number.
    static constexpr float kMinNameFont = 8.5f;

    // What one degree WEARS, and the widest of them: the name when names are showing,
    // the number otherwise. Every measurement goes through these two, so a probe can
    // never be sizing text the screen has stopped drawing.
    juce::String degreeText (int degree) const
    {
        return showingNames() ? getDegreeName (degree) : juce::String (degree);
    }
    juce::String widestDegreeText() const
    {
        return showingNames() ? widestNameText() : widestNumberText();
    }
    juce::Typeface::Ptr labelFace() const
    {
        return showingNames() ? nameFace : juce::Typeface::Ptr();
    }

    // Optional hover text per degree (ratio -> cents, or cents -> nearest just
    // interval). Shown ONLY after the mouse rests ~0.5 s on a key, never while
    // that key is being clicked/dragged.
    // THE WORD OVER THE REFERENCE KEY, injected like every other string this component
    // draws. It was the literal "REFERENCE" in paint(), which made it the one user-visible
    // string in the window that never changed language - and invisible to the rig's
    // "is it translated?" gate, because that gate walks the layout probe's lists and this
    // component paints its own text. It is now in the probe too (buttonTexts), so the
    // gate sees it like every other word.
    // THE WORD ABOVE THE REFERENCE KEY, in the two numbers paint() uses to draw it, so a
    // probe can ask whether it fits without owning a second copy of the rule. The band is
    // jmax (keyWidth, kReferenceBandMinWidth) wide, so this constant is the width the
    // word is GUARANTEED to have: a measurement against it is conservative at any key
    // width, and it is drawn with drawText (useEllipsis = false), which clips in silence.
    static constexpr float kReferenceBandMinWidth = 74.0f;
    static constexpr float kReferenceFontHeight   = 9.5f;

    void setReferenceWord (juce::String w) { referenceWord = std::move (w); repaint(); }
    juce::String getReferenceWord() const { return referenceWord; }
    void setHoverTextProvider (std::function<juce::String(int)> fn) { getHoverText = std::move (fn); }
    std::function<juce::String(int)> getHoverTextProvider() const { return getHoverText; }

    // THE THREE INFO ROWS ABOVE THE KEYS GET TITLES, ON THE LEFT.
    //
    // "PON UN TITULO A LA IZQUIERDA DE LAS TRES FILAS DE INFORMACION QUE HAY ENCIMA DE
    //  LAS TECLAS: DIFERENCIA DE PASOS, DIFERENCIA DE CENTS, BATIDOS. HAZ SITIO A LA
    //  IZQUIERDA ENCOGIENDO UN POCO EL TECLADO SI HACE FALTA. CADA TITULO EN NEGRITA/
    //  SUBRAYADO, QUE SE DISTINGA DEL TEXTO QUE VA SOBRE LAS TECLAS."
    //
    // WHY THE GUTTER IS TAKEN FROM BOTH SIDES. The keys are checked to be CENTRED inside
    // this component - RhythmVerify measures (keysDrawn.x - box.x) against
    // (box.right - keysDrawn.right) and allows 2 px, at four window widths and on two
    // tunings. A gutter taken from the left alone is not a caption column, it is the
    // keyboard going off centre, and it would fail that check for an honest reason. So
    // both sides give the same 62 px and only the left one is written in. The right one
    // is the price of the left one being free.
    static constexpr int   kCaptionW          = 62;
    static constexpr float kCaptionFontHeight = 9.5f;
    void setRowCaptions (juce::String steps, juce::String cents, juce::String beating)
    { capSteps = std::move (steps); capCents = std::move (cents);
      capBeating = std::move (beating); repaint(); }

    // THE FOUR BANDS OF THE INFO STRIP, top to bottom: step difference, cent difference,
    // the just ratio when there is one, and the beating value. ONE function answers it,
    // because the per-key painter fills those bands and the caption gutter labels them -
    // and a caption laid out from its own arithmetic drifts off the row it names the
    // first time either side is touched. It also fixed a real problem on the way: the
    // deltas used to be ONE drawFittedText of a 2- or 3-line string, vertically centred
    // in a 36 px box, so a hovered pair with no just ratio floated its two lines to a
    // different height than a pair with one. Nothing could have been labelled.
    static constexpr float kBeatBandH = 14.0f;
    struct StripBands { juce::Rectangle<float> line[3], beat; };
    static StripBands stripBands (juce::Rectangle<float> band)
    {
        StripBands b;
        b.beat = band.removeFromBottom (kBeatBandH);
        const float h = band.getHeight() / 3.0f;
        for (auto& r : b.line) r = band.removeFromTop (h);
        return b;
    }

    // Below-keyboard hover strip: while a key is hovered, each OTHER key shows its
    // distance to it via getDeltaLabel(hoveredNote, otherNote) (multi-line: steps,
    // cents, and a just ratio when exact); the hovered key shows getSourceLabel(note)
    // (e.g. "from 5/1" for spectrum-segment scales) drawn in `sourceColour`.
    void setDeltaProviders (std::function<juce::String(int,int)> delta,
                            std::function<juce::String(int)> source, juce::Colour srcColour)
    { getDeltaLabel = std::move (delta); getSourceLabel = std::move (source); sourceColour = srcColour; }
    void setNoteHzProvider (std::function<double(int)> fn) { getNoteHz = std::move (fn); }

    // While hovering, every OTHER key's strip cell also shows the Sethares sensory
    // dissonance of that key AGAINST the hovered one, coloured green (consonant) ->
    // red (rough) like the interval list.
    struct DissInfo { juce::String text; juce::Colour colour; };
    void setHoverDissProvider (std::function<DissInfo(int,int)> fn) { getHoverDiss = std::move (fn); }

    // Scale editing: right-click a key -> onEditKey(note) (Scala mode: type a new
    // cents/ratio value); Ctrl+left-click -> onAddKey(note, fracWithinKey) adds a
    // note at that spot (folded into every period repetition).
    std::function<void(int)>        onEditKey;
    std::function<void(int,double)> onAddKey;

    //==========================================================================
    // PERIOD NUMBERS, and why octave names had to go.
    //
    // "C2", "C4" are 12-EDO's own invention: they name a pitch class that only exists
    // because the octave is cut into twelve. In 31-EDO, in Bohlen-Pierce (period 3/1),
    // in any .scl with its own period, they name nothing - and the adaptation's register
    // range was expressed in them, so the one control that decides WHICH REGISTERS a
    // spectrum is optimised for spoke a language most of this instrument's tunings do
    // not have.
    //
    // What replaces them is the PERIOD number, counted exactly the way octaves are
    // counted in 12-EDO: the reference key (60, degree 0) is period 3 - which is what
    // the rest of the product already calls C3 - and every degree-0 key carries its own
    // number, drawn small to the right of its "0". Those numbers ARE the units the
    // register range is typed in, so the keyboard is where you read them off.
    // The FIXED key width (see numCells), and therefore the SMALLEST key the keyboard ever
    // draws: how many keys fit follows the window and nothing else, so `keysWidth() /
    // numCells()` is never below this. Public because it is the width every legibility
    // bound has to be measured at, and a test reconstructing it from a rect would be
    // measuring the window it happened to open at rather than the worst case.
    static constexpr int kCellW  = 32;

    static constexpr int kRefPeriodNumber = 3;
    static int periodNumberForNote (int note, int degrees)
    {
        const int D = juce::jmax (1, degrees);
        const int rel = note - kRefKey;
        // FLOOR division, not C's truncation: -1/12 is 0 in C++, which would put the whole
        // period below the reference into the reference's own number and give two adjacent
        // periods the same name.
        const int p = rel >= 0 ? rel / D : -(((-rel) + D - 1) / D);
        return p + kRefPeriodNumber;
    }
    // The MIDI key that STARTS period `p` under a tuning with `degrees` per period - the
    // inverse of the above, and what the register fields resolve their typed number with.
    // -1 when that period has no key on a 128-note keyboard.
    static int noteForPeriodNumber (int p, int degrees)
    {
        const int D = juce::jmax (1, degrees);
        const long long k = (long long) kRefKey + (long long) (p - kRefPeriodNumber) * D;
        return (k >= 0 && k <= 127) ? (int) k : -1;
    }

    // The geometry of a degree-0 key's two numbers. ONE place: paint() draws from it and
    // periodTagProbe() measures it, so a check can never be measuring a layout the screen
    // has stopped using.
    struct ZeroLayout
    {
        juce::Rectangle<float> zero, tag;
        // The band the PAIR was laid out in: the degree band every other key uses, plus
        // whatever it had to borrow from the interval label below it (see layOutZeroStack).
        juce::Rectangle<float> band;
        float tagFont = 0.0f, zeroFont = 0.0f;
    };
    // The width a string really takes in the bold font both numbers are drawn in. MEASURED,
    // not estimated at "0.62 em a character": the old estimate was what forced the strip to
    // be a fat RESERVATION with the "0" floating in whatever was left, and that reservation
    // is exactly the ~7 px of dead air the two numbers used to sit apart.
    static float boldTextWidth (const juce::String& s, float fontHeight)
    {
        return labelTextWidth (s, fontHeight, {});
    }
    // The font a degree label is drawn in. A null face means the default bold face (the
    // numbers); a face means the notation face (the names). ONE function, so a width that
    // was measured can never have been measured in a different font from the one drawn.
    static juce::Font labelFont (float fontHeight, juce::Typeface::Ptr face)
    {
        return face != nullptr ? juce::Font (juce::FontOptions (face).withHeight (fontHeight))
                               : juce::Font (juce::FontOptions (fontHeight).withStyle ("Bold"));
    }
    static float labelTextWidth (const juce::String& s, float fontHeight, juce::Typeface::Ptr face)
    {
        return (float) juce::GlyphArrangement::getStringWidthInt (labelFont (fontHeight, face), s);
    }
    // The width a label really has to live in inside its band: the band less the border
    // clearance the stack keeps on both sides, less the 1 px either side every label rect
    // carries so drawFittedText never has to scale the glyphs to place them.
    static float maxLabelWidth (const juce::Rectangle<float>& band)
    {
        return juce::jmax (4.0f, band.getWidth() - 2.0f * kEdge - 2.0f);
    }
    // `startH` brought down until `text` fits `maxW` in `face`. Iterative, not one
    // division: glyph advances are quantised, so a single scale by maxW/w lands a hair
    // over the line often enough that the first name of every wide EDO ellipsised.
    static float shrinkToWidth (float startH, const juce::String& text,
                                float maxW, juce::Typeface::Ptr face)
    {
        if (text.isEmpty() || startH <= 0.0f) return startH;
        float h = startH;
        const float w = labelTextWidth (text, h, face);
        if (w > maxW && w > 0.0f) h = startH * maxW / w;
        for (int i = 0; i < 24 && h > 1.0f && labelTextWidth (text, h, face) > maxW; ++i)
            h *= 0.96f;
        return h;
    }
    // THE "0" AND ITS PERIOD NUMBER, STACKED.
    //
    // They used to sit SIDE BY SIDE: the tag took its width first and the "0" was centred
    // in whatever was left. That is the bug the user hit - the number to the right of the
    // "0" DISPLACED it to the left, so on every period boundary the one degree the whole
    // keyboard is coloured and numbered for was the only number not standing in the same
    // column as the "1", "2", "3"... on the keys either side of it.
    //
    // Now the period number rides ABOVE the "0" and BOTH are centred on the key's own
    // centre line, which is exactly where degreeArea centres every other degree number.
    // Three things this has to get right, and none of them is visible to an overlap check:
    //
    //  * IT MUST FIT. drawFittedText with maximumLines = 1 does NOT reduce the font when
    //    the text is too wide - it squeezes the glyphs horizontally down to the minimum
    //    scale it was passed and then ellipsises, which looks like a design choice rather
    //    than a bug. So every rect here is sized from the string's MEASURED width in its
    //    own font, and when the room is not there the FONT comes down, never the text.
    //    The widest period string is three characters ("-10", which a one-degree-per-period
    //    scale really reaches) and it is the one nobody is looking at.
    //  * IT MUST NOT TOUCH THE KEY BORDER. The degree band starts at the key's top edge, so
    //    stacking a second number into it puts ink exactly where the key ends. kEdge is held
    //    clear on all four sides, measured on the INTEGER rects paint() really draws into
    //    (getSmallestIntegerContainer rounds outwards and can eat a pixel of margin).
    //  * IT MUST NOT COST THE "0" ITS SIZE. Stacked, the pair needs ~37 px and the degree
    //    band is ~34 at the smallest key the keyboard ever draws, so the band BORROWS the
    //    difference from the interval label underneath - capped at a quarter of it, because
    //    a stacked fraction has far less slack than these two numbers do. Only if that is
    //    still not enough does the "0"'s own font come down.
    //
    // `cell` is trimmed in place to exactly the room the interval label may use - the same
    // contract degreeArea has, so ONE call decides both and a change to either can never
    // orphan the other.
    // `zeroText` is what the degree-0 key WEARS - "0" when the keyboard is numbering its
    // degrees, that degree's NAME ("C") when it is naming them - and `widestText` is the
    // widest label anywhere on the keyboard, because the stack has to line up with its
    // neighbours and they all share one font height. `face` is the typeface both are
    // drawn in; null means the default bold face. The period number above is ALWAYS a
    // number in the bold face, whatever the degrees wear: it is a coordinate, not a
    // degree, and drawing "3" in the notation face would turn it into an accidental.
    static ZeroLayout layOutZeroStack (juce::Rectangle<float>& cell,
                                       const juce::String& label,
                                       float cellW, const juce::String& tagIn,
                                       const juce::String& zeroText = "0",
                                       const juce::String& widestText = "0",
                                       juce::Typeface::Ptr face = {})
    {
        ZeroLayout z;
        const juce::String t  = tagIn.isEmpty() ? juce::String ("0") : tagIn;
        const juce::String z0 = zeroText.isEmpty() ? juce::String ("0") : zeroText;

        // The band, and the font a NON-zero degree would get on this key: the stack has to
        // line up with its neighbours, so it starts from their geometry rather than its own.
        float zeroFont = 0.0f;
        auto band = degreeArea (cell, label, cellW, zeroFont);
        // Widest glyph either number may have. The rects are the glyph plus 1 px either
        // side, so subtracting that 2 px here is what keeps kEdge clear of the key edge.
        const float maxW = maxLabelWidth (band);
        // ...and the height the WHOLE ROW settled on, which the "0" (or the "C") must
        // share. Sizing it from its own width alone would give a one-character degree-0
        // key a bigger glyph than the "vCc" beside it.
        zeroFont = shrinkToWidth (zeroFont, widestText, maxW, face);

        // 1. The period number's font: 0.34 of the key width, clamped 8..12, brought down
        //    (7 pt floor) only if even that overruns the key.
        z.tagFont  = juce::jlimit (8.0f, 12.0f, cellW * 0.34f);
        float tagW = boldTextWidth (t, z.tagFont);
        if (tagW > maxW)
        {
            z.tagFont = juce::jmax (7.0f, z.tagFont * maxW / juce::jmax (1.0f, tagW));
            tagW      = boldTextWidth (t, z.tagFont);
        }

        // 2. Make room for the pair by borrowing downwards, NOT by shrinking the degree.
        const float need   = 2.0f * kEdge + z.tagFont + kVGap + zeroFont;
        const float borrow = juce::jlimit (0.0f, juce::jmax (0.0f, cell.getHeight() * 0.25f),
                                           need - band.getHeight());
        if (borrow > 0.0f)
        {
            band = band.withHeight (band.getHeight() + borrow);
            cell.removeFromTop (borrow);   // the label keeps the rest - and only the rest
        }

        // 3. Only now, if the pair still does not fit, does the "0" get smaller.
        const float room = band.getHeight() - 2.0f * kEdge - z.tagFont - kVGap;
        z.zeroFont  = juce::jmax (7.0f, juce::jmin (zeroFont, room));
        float zeroW = labelTextWidth (z0, z.zeroFont, face);
        if (zeroW > maxW)
        {
            z.zeroFont = juce::jmax (7.0f, z.zeroFont * maxW / juce::jmax (1.0f, zeroW));
            zeroW      = labelTextWidth (z0, z.zeroFont, face);
        }

        // The stack, centred in the band on the key's own centre line. Each rect is its
        // glyph plus 1 px either side so drawFittedText has room to place the text without
        // ever scaling it, and each is exactly one line tall so the glyph cannot spill out
        // of the rect the margin was measured on.
        const float stackH = z.tagFont + kVGap + z.zeroFont;
        const float top    = band.getY() + juce::jmax (kEdge, (band.getHeight() - stackH) * 0.5f);
        const float cx     = band.getCentreX();
        z.tag  = { cx - (tagW  + 2.0f) * 0.5f, top,                     tagW  + 2.0f, z.tagFont  };
        z.zero = { cx - (zeroW + 2.0f) * 0.5f, top + z.tagFont + kVGap, zeroW + 2.0f, z.zeroFont };
        z.band = band;
        return z;
    }

    // The two numbers as they would be drawn on a key `cellW` x `cellH` px, through the very
    // call paint() makes. Exists so a check can measure the SMALLEST key the keyboard ever
    // draws - kCellW - instead of whichever cell the test window happened to produce. Keys
    // have a FIXED width, so the tight case is by construction never the one on screen, and
    // it is exactly where a period number would be squeezed unnoticed.
    static ZeroLayout layOutPeriodTagForCell (float cellW, float cellH,
                                              const juce::String& degreeLabel,
                                              const juce::String& tag,
                                              const juce::String& zeroText = "0",
                                              const juce::String& widestText = "0",
                                              juce::Typeface::Ptr face = {})
    {
        auto cell = juce::Rectangle<float> (0.0f, 0.0f, cellW, cellH).reduced (0.6f);
        return layOutZeroStack (cell, degreeLabel, cellW, tag, zeroText, widestText, face);
    }
    // The key rect layOutPeriodTagForCell lays out in - the border nothing may touch. Same
    // construction, one place, so a margin check cannot be measuring a different key.
    static juce::Rectangle<float> cellRectForTests (float cellW, float cellH)
    {
        return juce::Rectangle<float> (0.0f, 0.0f, cellW, cellH).reduced (0.6f);
    }
    // Where a NON-zero degree number is centred on that same key. The whole point of the
    // stack is that the "0" shares this column; measured through the very call paint()
    // makes for the other degrees, so the two cannot drift apart silently.
    static float degreeCentreXForTests (float cellW, float cellH, const juce::String& degreeLabel)
    {
        auto cell = cellRectForTests (cellW, cellH);
        float f = 0.0f;
        return degreeArea (cell, degreeLabel, cellW, f).reduced (1.0f, 0.0f).getCentreX();
    }
    // Clear space between a label and the key's border, and between the period number and
    // the degree it annotates. Class-level because the width every label is fitted into
    // (maxLabelWidth) is derived from the first one, and the two must not drift apart.
    static constexpr float kEdge = 2.0f;
    static constexpr float kVGap = 1.0f;

    // Test hook (RhythmVerify): every degree-0 key on screen, with the exact rects and
    // fonts paint() uses and the width the period number really needs in that font. A
    // number drawn into too small a rect is shrunk by drawFittedText - legible-looking,
    // wrong size, invisible to every bounds check - so the width is measured, not eyeballed.
    struct PeriodTag
    {
        int note = 0, number = 0;
        juce::Rectangle<float> zero, tag;
        float zeroFont = 0.0f, tagFont = 0.0f;
        float zeroTextWidth = 0.0f, tagTextWidth = 0.0f;
        // The KEY this was laid out on, and the degree-0 label under the numbers. Carried
        // so a check can re-run the whole layout (layOutPeriodTagForCell) at kCellW for the
        // period strings this tuning does not happen to be showing: the band the numbers
        // share depends on both, and reconstructing either from a rect would be measuring
        // the window the test opened at rather than the smallest key the keyboard draws.
        float cellW = 0.0f, cellH = 0.0f;
        juce::String label;
        juce::Rectangle<float> area;
        // What degree 0 is actually WEARING ("0", or "C" when names are on), the widest
        // label anywhere on this keyboard, and whether names are showing at all. Carried
        // because "zeroTextWidth" alone cannot say WHICH text was measured, and a check
        // that assumes "0" would keep passing after the keyboard started drawing "C".
        juce::String zeroText, widestText;
        bool names = false;
        // The KEY as painted. The border the stack must not touch - and the only rect a
        // "does it fit" check may be measured against.
        juce::Rectangle<float> cell;
        // The VERTICAL distance from the period number down to the "0" it annotates, and
        // the SMALLEST clearance from either number to any of the key's four edges. Both
        // are invisible to an overlap check: two rects that do not overlap can still be a
        // mile apart, and a rect entirely inside another can still have ink hard against
        // its edge. `edge` is measured on the INTEGER rects paint() really draws into,
        // because getSmallestIntegerContainer rounds outwards and eats up to a pixel.
        float gap = 0.0f, edge = 0.0f;
        // Where the "0" sits ACROSS the key, against where a non-zero degree number is
        // drawn on that same key. The bug was exactly this: a number beside the "0" pushed
        // it off the column every other degree stands in.
        float zeroCentreX = 0.0f, degreeCentreX = 0.0f;
    };
    std::vector<PeriodTag> periodTagProbe() const
    {
        std::vector<PeriodTag> out;
        const int D = juce::jmax (1, getDegrees ? getDegrees() : 12);
        const int cells = numCells();
        const float w = (float) keysWidth() / (float) juce::jmax (1, cells);
        // Exactly what paint() resolves, once, before its loop.
        const bool         names       = showingNames();
        const juce::String widestLabel = names ? widestNameText() : widestNumberText();
        const juce::Typeface::Ptr face = names ? nameFace : juce::Typeface::Ptr();
        for (int i = 0; i < cells; ++i)
        {
            const int note = baseKey + i;
            if (note > 127 || note >= rangeLo + rangeCount) break;
            if (((note - kRefKey) % D + D) % D != 0) continue;
            PeriodTag t;
            t.note   = note;
            t.number = periodNumberForNote (note, D);
            const juce::String tag (t.number);
            const auto cell = cellRect (i);
            t.cell  = cell;
            t.label = getLabel ? getLabel (0) : juce::String();
            t.names = names; t.widestText = widestLabel;
            t.zeroText = names ? getDegreeName (0) : juce::String (0);
            auto rest = cell;                                  // trimmed to the label's room
            const auto z = layOutZeroStack (rest, t.label, w, tag, t.zeroText, widestLabel, face);
            t.zero = z.zero; t.tag = z.tag; t.tagFont = z.tagFont; t.zeroFont = z.zeroFont;
            t.cellW = w; t.cellH = keysHeight(); t.area = z.band;
            t.zeroTextWidth = labelTextWidth (t.zeroText, z.zeroFont, face);
            t.tagTextWidth  = boldTextWidth (tag, z.tagFont);
            t.gap = z.zero.getY() - z.tag.getBottom();
            // The rects paint() actually hands drawFittedText, not the float ones.
            const auto ti = z.tag.getSmallestIntegerContainer().toFloat();
            const auto zi = z.zero.getSmallestIntegerContainer().toFloat();
            t.edge = juce::jmin (juce::jmin (ti.getY() - cell.getY(), cell.getBottom() - zi.getBottom()),
                                 juce::jmin (juce::jmin (ti.getX() - cell.getX(), cell.getRight() - ti.getRight()),
                                             juce::jmin (zi.getX() - cell.getX(), cell.getRight() - zi.getRight())));
            t.zeroCentreX   = z.zero.getCentreX();
            t.degreeCentreX = degreeCentreXForTests (w, t.cellH, t.label)
                            + cell.getX() - cellRectForTests (w, t.cellH).getX();
            out.push_back (t);
        }
        return out;
    }

    juce::String getTooltip() override
    {
        if (hoverArmed && hoverCell >= 0 && getHoverText != nullptr)
        {
            const int note = baseKey + hoverCell;
            if (note <= 127)
            {
                const int D = juce::jmax (1, getDegrees ? getDegrees() : 12);
                return getHoverText (((note - kRefKey) % D + D) % D);
            }
        }
        return {};
    }

    void paint (juce::Graphics& g) override
    {
        const int D = juce::jmax (1, getDegrees ? getDegrees() : 12);
        const int cells = numCells();
        const float aw = (float) arrowW();                       // scroll-arrow zones (0 if all keys fit)
        const float w = (float) keysWidth() / (float) cells;
        const float keysY = (float) stripH;                      // strip on TOP, keys BELOW it (at screen bottom)
        const float keysH = (float) getHeight() - (float) stripH;

        // NAMES OR NUMBERS, decided ONCE for the whole keyboard and never per key: a row
        // where some keys carry a name and others a number is not a scale, it is a bug.
        // Resolved here rather than inside the loop because showingNames() walks every
        // degree, and asking it once per key turned a repaint into O(keys x degrees).
        const bool         names       = showingNames();
        const juce::String widestLabel = names ? widestNameText() : widestNumberText();
        const juce::Typeface::Ptr face = names ? nameFace : juce::Typeface::Ptr();

        // Info strip ABOVE the keys: dark; shows period-key Hz normally, hover deltas on hover.
        g.setColour (juce::Colour (0xff0c0c12));
        g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) stripH));
        const int hoveredNote = (hoverArmed && hoverCell >= 0) ? baseKey + hoverCell : -1;
        bool heldOffL = false, heldOffR = false; // a sounding note is left/right of the view

        for (int i = 0; i < cells; ++i)
        {
            const int note = baseKey + i;
            if (note > 127) break;
            if (note >= rangeLo + rangeCount) break; // past the tuning's playable range

            const int degree = ((note - kRefKey) % D + D) % D;
            const float hue = 0.82f * (float) degree / (float) D;
            const bool held = state.isNoteOnForChannels (0xffff, note);

            auto cell = cellRect (i);
            // The hovered key stretches a little into the strip - a clear "this one".
            if (note == hoveredNote && degree != 0)
                cell = cell.withTop (cell.getY() - 7.0f);
            auto col = juce::Colour::fromHSV (hue, 0.62f, held ? 1.0f : 0.86f, 1.0f);
            g.setColour (held ? col.brighter (0.35f) : col);
            g.fillRect (cell);
            if (held) { g.setColour (juce::Colours::white); g.drawRect (cell, 2.0f); }

            // Degree number (wide, but a touch smaller so it clears the key edges in
            // dense scales) + the just-interval label below it. On a degree-0 key the
            // PERIOD NUMBER rides beside it - see periodNumberForNote.
            const juce::String label = getLabel ? getLabel (degree) : juce::String();
            const auto ink = col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white;
            g.setColour (ink);
            {
                const juce::String dText = names ? getDegreeName (degree) : juce::String (degree);
                auto rest = cell;
                if (degree == 0)
                {
                    const juce::String tag (periodNumberForNote (note, D));
                    // ONE call decides the pair AND what is left for the interval label -
                    // the stack borrows height downwards, so a label laid out from an
                    // untrimmed cell would print straight through the "0".
                    const auto z = layOutZeroStack (rest, label, w, tag, dText, widestLabel, face);
                    // The period number goes ON TOP, smaller AND quieter: it is a
                    // coordinate, not a degree, and at full ink it read as a second scale
                    // degree sitting on the same key.
                    g.setColour (ink.withMultipliedAlpha (0.72f));
                    g.setFont (juce::Font (juce::FontOptions (z.tagFont).withStyle ("Bold")));
                    g.drawFittedText (tag, z.tag.getSmallestIntegerContainer(),
                                      juce::Justification::centred, 1, 0.5f);
                    g.setColour (ink);
                    // Both rects are already sized to their own measured glyph, so they are
                    // drawn WHOLE (getSmallestIntegerContainer, never a rounded-down
                    // toNearestInt) - rounding a rect one pixel narrower than the text is
                    // precisely what makes drawFittedText squeeze a number nobody can see.
                    g.setFont (labelFont (z.zeroFont, face));
                    g.drawFittedText (dText, z.zero.getSmallestIntegerContainer(),
                                      juce::Justification::centred, 1, 0.5f);
                }
                else
                {
                    float fontH = 0.0f;
                    const auto area = degreeArea (rest, label, w, fontH);
                    // The WHOLE ROW's height, from the widest label on the keyboard, not
                    // this key's own text: sized per key, "C" would tower over the "vCc"
                    // beside it. With numbers this is the height it always was, because a
                    // one- or two-digit degree is never the binding constraint.
                    fontH = shrinkToWidth (fontH, widestLabel, maxLabelWidth (area), face);
                    g.setFont (labelFont (fontH, face));
                    g.drawFittedText (dText, area.reduced (1.0f, 0.0f).toNearestInt(),
                                      juce::Justification::centred, 1, 0.5f);
                }
                if (label.isNotEmpty())
                    drawIntervalLabel (g, label, rest, w);
            }

            // Strip cell (above the key): hover deltas; the hovered key itself gets an
            // EYE whose pupil takes the key's colour (a "you are looking here" marker
            // that covers nothing - the strip is empty space). No hover: period Hz.
            const bool infoActive  = hoveredNote >= 0;
            auto sc = juce::Rectangle<float> (keysOriginX() + (float) i * w, 0.0f, w, (float) stripH).reduced (1.5f, 3.0f);
            if (hoveredNote >= 0 && note == hoveredNote)
            {
                auto eyeArea = sc.withTrimmedBottom (8.0f); // clear of the stretched key top
                auto eye = eyeArea.withSizeKeepingCentre (juce::jmin (eyeArea.getWidth() - 2.0f, 26.0f),
                                                          juce::jmin (eyeArea.getHeight(), 16.0f));
                g.setColour (juce::Colour (0xffe8ecf4));           // sclera outline
                g.drawEllipse (eye, 1.4f);
                auto iris = eye.withSizeKeepingCentre (eye.getHeight() * 0.78f, eye.getHeight() * 0.78f);
                g.setColour (col);                                  // iris = the key's colour
                g.fillEllipse (iris);
                g.setColour (juce::Colours::black);                 // pupil
                g.fillEllipse (iris.withSizeKeepingCentre (iris.getWidth() * 0.42f, iris.getHeight() * 0.42f));
            }
            else if (infoActive)
            {
                // EACH LINE IN ITS OWN BAND, from stripBands() - the same function the
                // caption gutter lays itself out from, so a caption can never drift off
                // the row it names. It used to be one drawFittedText of the whole 2- or
                // 3-line string, vertically centred in the 36 px above the dissonance
                // band: a hovered pair with a just ratio pushed its first two lines up
                // and a pair without one let them float back down, so there was no fixed
                // row for a title to sit beside.
                const auto rows = stripBands (sc);
                if (getHoverDiss)
                {
                    const auto info = getHoverDiss (hoveredNote, note);
                    if (info.text.isNotEmpty())
                    {
                        g.setColour (info.colour);
                        g.setFont (juce::Font (juce::FontOptions (juce::jmin (11.0f, w * 0.34f)).withStyle ("Bold")));
                        g.drawFittedText (info.text, rows.beat.toNearestInt(), juce::Justification::centred, 1, 0.4f);
                    }
                }
                const juce::String d = getDeltaLabel ? getDeltaLabel (hoveredNote, note) : juce::String();
                if (d.isNotEmpty())
                {
                    juce::StringArray lines;
                    lines.addLines (d);
                    g.setColour (juce::Colour (0xffc4ccda));
                    g.setFont (juce::Font (juce::FontOptions (juce::jmin (10.5f, w * 0.32f))));
                    for (int L2 = 0; L2 < juce::jmin (3, lines.size()); ++L2)
                        g.drawFittedText (lines[L2], rows.line[L2].toNearestInt(),
                                          juce::Justification::centred, 1, 0.5f);
                }
            }
            else if ((hoveredNote < 0 || ! infoActive) && degree == 0 && getNoteHz) // period keys: their frequency (khz over 1000)
            {
                const double hz = getNoteHz (note);
                if (hz > 0.0)
                {
                    // MIDI C3 = the tuning's C3 reference itself: Essentials green +
                    // bold, with REFERENCE spelled out above the number.
                    const bool isRef = note == kRefKey;
                    // THE WORD'S BAND IS RESERVED ON EVERY PERIOD KEY, DRAWN ON ONE.
                    //
                    // This band used to be taken off `sc` only when the key WAS the
                    // reference, so the three frequency markers above the keyboard sat on
                    // two different baselines: the two ordinary ones centred in the whole
                    // strip (y 544-551) and the middle one, pushed down by REFERENCE's 11 px
                    // minus the 3 px pull-up, centred in what was left (y 547-555). Four
                    // pixels of stagger across three numbers that are the same read-out - and
                    // the cause was that the caption above ONE of them was allowed to move
                    // the number under it.
                    //
                    // Reserving the band unconditionally costs the two ordinary markers 8 px
                    // of vertical room they were only using to sit higher than their
                    // neighbour, and it puts all three on one baseline for good: the word can
                    // now appear or disappear (a tuning change moves kRefKey) without any
                    // number moving at all.
                    auto hzArea = sc;
                    {
                        // Sits DIRECTLY above its Hz number (tight band, no gap) and is
                        // allowed to spill over the neighbouring key widths instead of
                        // being squeezed horizontally into unreadable letters.
                        auto band = hzArea.removeFromTop (11.0f);
                        hzArea.setTop (hzArea.getY() - 3.0f); // pull the number up to the word
                        if (isRef)
                        {
                            band = band.withSizeKeepingCentre (juce::jmax (band.getWidth(), kReferenceBandMinWidth), band.getHeight());
                            g.setColour (juce::Colour (0xff4fe09a));
                            g.setFont (juce::Font (juce::FontOptions (kReferenceFontHeight).withStyle ("Bold")));
                            g.drawText (referenceWord, band.toNearestInt(), juce::Justification::centred, false);
                        }
                    }
                    g.setColour (isRef ? juce::Colour (0xff4fe09a) : juce::Colour (0xffc4ccda));
                    g.setFont (isRef
                        ? juce::Font (juce::FontOptions (juce::jmin (12.0f, w * 0.36f)).withStyle ("Bold"))
                        : juce::Font (juce::FontOptions (juce::jmin (11.0f, w * 0.34f))));
                    // Hz and kHz with the capital the SI unit actually has: it is named
                    // after Heinrich Hertz, so the H is a person's initial and "hz" is
                    // simply wrong. The adapt panel across the window has always written
                    // it correctly ("131..523 Hz"), so until now one window spelled one
                    // unit two ways. The space before it is the SI rule too.
                    const juce::String t = juce::String::fromUTF8 ("\xe2\x89\x88 ")
                        + (hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz"
                                        : juce::String ((int) std::lround (hz)) + " Hz");
                    g.drawFittedText (t, hzArea.toNearestInt(), juce::Justification::centred, 1, 0.4f);
                }
            }
        }

        // THE ROW CAPTIONS, in the gutter kCaptionW reserves on the left.
        //
        // Only while a key is hovered: with nothing hovered these same three bands hold
        // the period keys' frequencies and the REFERENCE word, which "steps", "cents" and
        // "beating" would then be labelling falsely. Laid out from the SAME stripBands()
        // the per-key text is drawn from, so the two cannot drift apart.
        //
        // BOLD AND UNDERLINED, as asked, and the rule is DRAWN because there is no font
        // underline anywhere in this build - the precedent is the fraction bar under a
        // ratio in drawIntervalLabel, a fillRect at the MEASURED width of the glyphs. A
        // rule as wide as the box would be a border; a rule as wide as the word is an
        // underline, and it is what separates these from the numbers over the keys.
        if (hoveredNote >= 0)
        {
            const auto gutter = juce::Rectangle<float> ((float) arrowW(), 0.0f,
                                                        (float) kCaptionW, (float) stripH)
                                    .reduced (3.0f, 3.0f);
            const auto rows = stripBands (gutter);
            const juce::Rectangle<float> where[3] = { rows.line[0], rows.line[1], rows.beat };
            const juce::String           what[3]  = { capSteps, capCents, capBeating };
            g.setColour (juce::Colour (0xffe8ecf4));
            g.setFont (juce::Font (juce::FontOptions (kCaptionFontHeight).withStyle ("Bold")));
            for (int r = 0; r < 3; ++r)
            {
                if (what[r].isEmpty()) continue;
                g.drawText (what[r], where[r].toNearestInt(), juce::Justification::centredRight, false);
                const float tw = juce::jmin (boldTextWidth (what[r], kCaptionFontHeight),
                                             where[r].getWidth());
                const float by = juce::jmin (where[r].getCentreY() + kCaptionFontHeight * 0.55f,
                                             where[r].getBottom() - 1.0f);
                g.fillRect (juce::Rectangle<float> (where[r].getRight() - tw, by, tw, 1.0f));
            }
        }

        // Sounding notes outside the visible window: flag their direction so the
        // scroll arrows can point you back to what you're playing.
        for (int n = 0; n <= 127; ++n)
            if (state.isNoteOnForChannels (0xffff, n))
            {
                if (n < baseKey) heldOffL = true;
                else if (n >= baseKey + cells) heldOffR = true;
            }

        // Scroll arrows for scales too wide to show at the readable min key width.
        // They glow GREEN when a note you're playing sits off-view in that direction.
        if (isScrollable())
        {
            const bool canL = baseKey > rangeLo;
            const bool canR = baseKey + numCells() < rangeLo + rangeCount;
            auto drawArrow = [&] (juce::Rectangle<float> zone, bool left, bool enabled, bool playing)
            {
                g.setColour (playing ? juce::Colour (0xff1e5a2e) : juce::Colour (0xff1a1a24));
                g.fillRect (zone);
                g.setColour (playing ? juce::Colour (0xff54e678)
                                     : (enabled ? juce::Colours::white : juce::Colour (0xff44444e)));
                const float s = playing ? 0.24f : 0.16f; // bigger when it's pointing at your notes
                auto t = zone.withSizeKeepingCentre (zone.getWidth() * 0.5f, zone.getHeight() * s);
                juce::Path p;
                if (left) p.addTriangle (t.getRight(), t.getY(), t.getRight(), t.getBottom(), t.getX(), t.getCentreY());
                else      p.addTriangle (t.getX(), t.getY(), t.getX(), t.getBottom(), t.getRight(), t.getCentreY());
                g.fillPath (p);
            };
            drawArrow (juce::Rectangle<float> (0.0f, keysY, aw, keysH), true,  canL, heldOffL);
            drawArrow (juce::Rectangle<float> ((float) getWidth() - aw, keysY, aw, keysH), false, canR, heldOffR);
        }
    }

    // Draw a just-interval label inside `area`. A fraction "p/q" is drawn STACKED
    // (numerator over a bar over denominator) so it fits even very narrow keys and
    // never gets truncated to "..."; anything else (a cents value "317c") is drawn
    // on one line, auto-shrunk to fit.
    void drawIntervalLabel (juce::Graphics& g, const juce::String& label,
                            juce::Rectangle<float> area, float w)
    {
        const int slash = label.indexOfChar ('/');
        if (slash > 0 && label.indexOfChar ('c') < 0)
        {
            const auto num = label.substring (0, slash);
            const auto den = label.substring (slash + 1);
            auto numArea = area.removeFromTop (area.getHeight() * 0.5f);
            auto denArea = area;
            const float fs = juce::jmin (13.0f, numArea.getHeight() * 0.95f, w * 0.42f);
            g.setFont (juce::Font (juce::FontOptions (fs).withStyle ("Bold")));
            g.drawFittedText (num, numArea.toNearestInt(), juce::Justification::centredBottom, 1, 0.5f);
            g.drawFittedText (den, denArea.toNearestInt(), juce::Justification::centredTop,    1, 0.5f);
            // fraction bar
            const float bw = juce::jmin (numArea.getWidth() * 0.6f, fs * 1.4f);
            g.fillRect (juce::Rectangle<float> (numArea.getCentreX() - bw * 0.5f,
                                                numArea.getBottom() - 0.5f, bw, 1.2f));
        }
        else
        {
            // NOT CONDENSED, AND NOT TOUCHING ITS BORDERS.
            //
            // The old call passed minimumHorizontalScale = 0.4, which is a licence for
            // drawFittedText to SQUEEZE THE GLYPHS to 40% of their width rather than pick
            // a smaller font. Measured at the SHIPPED 1200x650 with 53-EDO: the cell is
            // 32 px, the box after the old 1 px inset is 30, and "1042" plus the cent sign
            // plus the "*" measures 38.2 px at 12 px Bold - so every cents label on that
            // keyboard was drawn at 79% of its own typeface. Invisible to every gate in
            // this build, because nothing overflowed: the text fits, it is just the wrong
            // shape. This project refuses condensed text everywhere else and this was the
            // one place still doing it.
            //
            // Three px of inset, not one. Measured on screen at 1200x650: the "800c"
            // cell's drawn borders are at x 747 and x 779 and the label's first ink pixel
            // is at x 749 - one pixel - which reads as text spilling out of its key rather
            // than sitting in it.
            //
            // The font comes DOWN to fit and the horizontal scale is pinned at 1.0, which
            // is the trade the degree number above it already makes. Measured in the BOLD
            // face this label is really drawn in: the shared shrink helper measures in the
            // degree number's face, and borrowing it here would under-measure by about 7%
            // and put the squeeze straight back.
            //
            // THE CENT SIGN GOES when the cell cannot hold it, and that is a decision
            // rather than a squeeze: "1042" at a readable size says more than "1042c" at
            // 79% of its width. 34 px is measured, not chosen - it is the narrowest cell
            // in which the widest label this keyboard produces still fits at 12 px Bold
            // WITH the sign and the 3 px insets. The unit is not lost: the key's hover
            // card spells "cents" out in full, and the "*" - the part carrying meaning the
            // number does not - is always kept.
            //
            // REJECTED: shrinking the font alone and keeping the sign. At a 32 px cell
            // that lands the six-glyph labels at 8.4 px, under the 9 px the rest of this
            // keyboard treats as the smallest readable size.
            constexpr float kCentSignMinCellW = 34.0f;
            juce::String shown = label;
            if (w < kCentSignMinCellW)
                shown = shown.replace (juce::String::fromUTF8 ("\xc2\xa2"), juce::String());
            auto box = area.reduced (3.0f, 0.0f);
            const float maxW = juce::jmax (4.0f, box.getWidth());
            const auto boldW = [] (const juce::String& t, float h)
            { return juce::GlyphArrangement::getStringWidth (
                         juce::Font (juce::FontOptions (h).withStyle ("Bold")), t); };
            float fh = juce::jmin (12.0f, w * 0.4f);
            // One division then 0.96 steps, the same shape the shrink helper uses and for
            // the same reason: glyph advances are quantised, so a single scale by
            // maxW / measured lands a hair over the line often enough to matter. One or
            // two iterations in practice, on the message thread, once per key per repaint.
            {
                const float w0 = boldW (shown, fh);
                if (w0 > maxW && w0 > 0.0f) fh = fh * maxW / w0;
                for (int i = 0; i < 24 && fh > 1.0f && boldW (shown, fh) > maxW; ++i) fh *= 0.96f;
            }
            g.setFont (juce::Font (juce::FontOptions (fh).withStyle ("Bold")));
            g.drawFittedText (shown, box.getSmallestIntegerContainer(),
                              juce::Justification::centred, 1, 1.0f);
        }
    }

    // Only the keys (below the top info strip) play; the side arrows scroll.
    void mouseDown (const juce::MouseEvent& e) override
    {
        hoverArmed = false;
        if (e.y < stripH) return; // top info strip
        const int aw = arrowW();
        if (aw > 0 && e.x < aw)                { scrollBy (-scrollStep()); return; }
        if (aw > 0 && e.x >= getWidth() - aw)  { scrollBy ( scrollStep()); return; }
        if (e.mods.isRightButtonDown())        // edit this degree (Scala mode)
        {
            if (onEditKey) { const int n = noteAt (e.x); if (n >= 0) onEditKey (n); }
            return;
        }
        if (e.mods.isCtrlDown())               // add a note at this exact spot
        {
            if (onAddKey)
            {
                const int n = noteAt (e.x);
                if (n >= 0)
                {
                    // Where inside the key the click landed - measured from where the
                    // keys really start (keysOriginX), not from the arrow zone.
                    const double cw  = (double) keysWidth() / (double) juce::jmax (1, numCells());
                    const double pos = ((double) e.x - (double) keysOriginX()) / juce::jmax (1.0, cw);
                    onAddKey (n, juce::jlimit (0.0, 0.999, pos - std::floor (pos)));
                }
            }
            return;
        }
        playAt (e.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        hoverArmed = false;
        const int aw = arrowW();
        if (e.y >= stripH && e.x >= aw && e.x < getWidth() - aw) playAt (e.x);
        else releaseHeld();
    }
    int scrollStep() const { return juce::jmax (1, numCells() - 1); } // near-page step
    void mouseUp   (const juce::MouseEvent&)   override
    {
        releaseHeld();
        hoverArmed = false;
        hoverStartMs = juce::Time::getMillisecondCounter(); // re-arm from scratch
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        // Derive the hovered CELL from noteAt so the arrow zones are accounted for
        // (the old width-proportional math drifted one key off next to the arrows).
        const int n    = noteAt (e.x);
        const int cell = n >= 0 ? n - baseKey : -1;
        if (cell != hoverCell)
        {
            hoverCell    = cell;
            hoverArmed   = false;
            hoverStartMs = juce::Time::getMillisecondCounter();
        }
    }
    void mouseExit (const juce::MouseEvent&) override { hoverCell = -1; hoverArmed = false; }

private:
    static constexpr int kRefKey = 60;

    static constexpr int kArrowW = 22; // scroll-arrow zone, ALWAYS reserved

    // Keys have a CONSTANT width. How many are on screen therefore follows the
    // window size and nothing else - never the tuning's degree count. Switching to
    // a denser or sparser scale, or reaching a note outside the current view, now
    // SCROLLS; it can no longer resize every key, which made the whole keyboard
    // visibly jump under your fingers. The arrow zones are always reserved for the
    // same reason: their appearing/disappearing used to shift the geometry too.
    int numCells() const
    {
        const int usable = juce::jmax (1, getWidth() - 2 * sideW());
        return juce::jmax (1, usable / kCellW);
    }
    // How many of those cells actually CARRY A KEY.
    //
    // "el teclado ahora mismo no aparece centrado en el sinte, haz que asi sea"
    //
    // paint()'s loop stops at the tuning's last playable note (and at 127), so under a
    // tuning whose period is wide - a Bohlen-Pierce, a stretched table, anything where
    // 40 Hz .. 4186 Hz spans only a handful of keys - the window has 35 cells and the
    // scale has 8. Those 8 were drawn from the left edge and the rest of the keyboard
    // was empty page: the keyboard sat in the left third of a full-width component,
    // which is exactly "not centred". Stated ONCE, here, because the drawn keys, the
    // hover anchor and the note-under-the-mouse all have to agree about where key 0
    // starts - an offset applied to the painter alone would move the keys away from
    // the notes they play.
    int liveCells() const
    {
        const int last = juce::jmin (128, rangeLo + rangeCount); // one past the last playable note
        return juce::jlimit (0, numCells(), last - baseKey);
    }
    // The x the FIRST key starts at: the arrow zone, plus half of whatever the live keys
    // left over. When they fill the window (the ordinary case) this is arrowW() exactly.
    float keysOriginX() const
    {
        const int cells = juce::jmax (1, numCells());
        const float w = (float) keysWidth() / (float) cells;
        return (float) sideW() + (float) (cells - liveCells()) * w * 0.5f;
    }
    // The resting key cell for view index `i`, exactly as paint() builds it (the hover
    // stretch is left out on purpose: it is a transient, and the probe measures the layout
    // the keyboard sits in). Shared so paint() and periodTagProbe() cannot disagree.
    juce::Rectangle<float> cellRect (int i) const
    {
        const float w = (float) keysWidth() / (float) juce::jmax (1, numCells());
        return juce::Rectangle<float> (keysOriginX() + (float) i * w, (float) stripH,
                                       w, (float) getHeight() - (float) stripH).reduced (0.6f);
    }
    // The band the DEGREE NUMBER is drawn in, and the font height it gets. `cell` is
    // trimmed in place so the caller is left holding exactly the room the interval label
    // may use - one call decides both, so a change to either can never orphan the other.
    static juce::Rectangle<float> degreeArea (juce::Rectangle<float>& cell,
                                              const juce::String& label, float w, float& fontOut)
    {
        if (label.isNotEmpty())
        {
            auto top = cell.removeFromTop (cell.getHeight() * 0.52f);
            fontOut = juce::jmin (top.getHeight() * 0.76f, w * 0.66f);
            return top;
        }
        fontOut = juce::jmin (cell.getHeight() * 0.66f, w * 0.66f);
        return cell;
    }
    // The height a KEY has (the component less the info strip on top), which is what every
    // degree-label measurement is laid out in. Named because "getHeight() - stripH" was
    // being written out at each call site, and one of them forgetting the strip would have
    // measured the fit in a band the keys do not have.
    float keysHeight() const { return juce::jmax (1.0f, (float) getHeight() - (float) stripH); }
    // The widest NAME and the widest NUMBER this tuning puts on a key. Both walk every
    // degree: the widest is very often not degree 0 and not the last one either (in
    // 22-EDO it is "vCc", degree 2), and guessing it from the count is how a keyboard
    // ships with one squeezed key nobody notices until they play in that register.
    juce::String widestNameText()   const { refreshWidestCache(); return cachedWidestName; }
    juce::String widestNumberText() const { refreshWidestCache(); return cachedWidestNumber; }

    // ...MEASURED ONCE PER TUNING, not once per repaint. One paint asks for the widest
    // label three times over (showingNames -> namesFitTheKeys -> nameFontAtMinKey, then
    // again for the row's font height), and each ask laid out every degree's text: 3 x D
    // text measurements on the message thread per frame, which a 512-degree scale turns
    // into fifteen hundred of them. The answer cannot change without one of the two
    // things below changing, so it is computed when they do.
    //
    // WHAT INVALIDATES IT. The degree COUNT, and a `namerGen` the editor bumps every time
    // it installs a namer - which it does on every change of the spelling, including
    // installing none (see refreshDegreeNames). A new tuning is always one or the other:
    // a scale with a different number of degrees, or the same number spelled differently,
    // which arrives as a fresh setDegreeNamer. Cache a name past either and the keyboard
    // would size this tuning's keys to the LAST tuning's widest word.
    void refreshWidestCache() const
    {
        const int D = juce::jmax (1, getDegrees ? getDegrees() : 12);
        if (cacheGen == namerGen && cacheDegrees == D) return;
        cacheGen = namerGen; cacheDegrees = D;

        cachedWidestName   = juce::String();
        // A tuning half-named would put "C" on one key and "7" on the next, so names are
        // all or nothing: ONE empty degree and the whole keyboard keeps its numbers.
        cachedNamerActive  = getDegreeName != nullptr && nameFace != nullptr;
        if (getDegreeName != nullptr)
        {
            float best = -1.0f;
            for (int d = 0; d < D; ++d)
            {
                const auto s = getDegreeName (d);
                if (s.isEmpty()) cachedNamerActive = false;
                // Measured at a fixed reference height, not by character count: "wwC" is
                // three characters and narrower than the two-character "Aa" in some faces.
                const float w = labelTextWidth (s, 20.0f, nameFace);
                if (w > best) { best = w; cachedWidestName = s; }
            }
        }
        cachedWidestNumber = juce::String();
        {
            float best = -1.0f;
            for (int d = 0; d < D; ++d)
            {
                const juce::String s (d);
                const float w = boldTextWidth (s, 20.0f);
                if (w > best) { best = w; cachedWidestNumber = s; }
            }
        }
    }
    // Bumped by setDegreeNamer; compared by refreshWidestCache. `mutable` on the cache
    // because measuring is what the const observers (paint's helpers, the probes) do.
    int                  namerGen = 0;
    mutable int          cacheGen = -1, cacheDegrees = -1;
    mutable bool         cachedNamerActive = false;
    mutable juce::String cachedWidestName, cachedWidestNumber;
    bool isScrollable() const { return rangeCount > numCells(); }
    int  arrowW()      const { return kArrowW; }
    // ARROWS PLUS THE CAPTION GUTTER, on both sides. Every place that decides how wide
    // the keys are, or where they start, asks THIS - it used to be two separate copies of
    // "getWidth() - 2 * kArrowW" in numCells() and keysWidth(), and a change to one of
    // them silently drops every key below the 32 px the layout is built on.
    int  sideW()       const { return kArrowW + kCaptionW; }
    int  keysWidth()   const { return juce::jmax (1, getWidth() - 2 * sideW()); }
    // Highest baseKey that still shows keys (never negative when the range is
    // smaller than the window - there is simply nothing to scroll).
    int  maxBaseKey()  const { return juce::jmax (rangeLo, rangeLo + rangeCount - numCells()); }
    void clampView() { baseKey = juce::jlimit (rangeLo, maxBaseKey(), baseKey); }
    void scrollBy (int d) { baseKey = juce::jlimit (rangeLo, maxBaseKey(), baseKey + d); repaint(); }

    // Screen x -> MIDI note (only within the keys area; the arrow zones return -1).
    int noteAt (int x) const
    {
        const int cells = numCells();
        const int aw = arrowW();
        if (cells <= 0 || x < aw || x >= getWidth() - aw) return -1;
        // Off keysOriginX, not off the arrow zone: when the live keys are centred in a
        // wider window (see liveCells) the two differ, and a hand-rolled x -> index that
        // ignored the offset would play the key several cells to the left of the one
        // under the pointer.
        const float w = (float) keysWidth() / (float) cells;
        const int i = (int) std::floor (((float) x - keysOriginX()) / juce::jmax (1.0f, w));
        if (i < 0 || i >= liveCells()) return -1;   // the blank margin plays nothing
        const int note = baseKey + i;
        // Past the tuning's range the cells are blank - they must not play either.
        return (note <= 127 && note < rangeLo + rangeCount) ? note : -1;
    }

    void playAt (int x)
    {
        const int note = noteAt (x);
        if (note == mouseNote) return;
        releaseHeld();
        if (note >= 0)
        {
            state.noteOn (1, note, 0.85f);
            mouseNote = note;
        }
    }

    void releaseHeld()
    {
        if (mouseNote >= 0)
        {
            state.noteOff (1, mouseNote, 0.0f);
            mouseNote = -1;
        }
    }

    void timerCallback() override
    {
        // Arm the hover tooltip only after resting on the same key with no button
        // held (the global TooltipWindow then displays getTooltip()). Slightly
        // quicker than a standard tooltip - these read-outs are core UI.
        if (! hoverArmed && hoverCell >= 0 && ! isMouseButtonDown()
            && juce::Time::getMillisecondCounter() - hoverStartMs >= 350)
            hoverArmed = true;
        repaint();
    }

    juce::MidiKeyboardState&        state;
    std::function<int()>            getDegrees;
    std::function<juce::String(int)> getLabel;
    std::function<juce::String(int)> getHoverText;
    std::function<juce::String(int,int)> getDeltaLabel;  // (hoveredNote, otherNote) -> multi-line delta
    std::function<juce::String(int)>     getSourceLabel; // note -> "from n/1" (segment scales)
    std::function<double(int)>           getNoteHz;      // note -> frequency (period keys show it)
    std::function<DissInfo(int,int)>     getHoverDiss;   // (hovered, other) -> dissonance line + colour
    std::function<juce::String(int)>     getDegreeName;  // degree -> ups-and-downs name ("" = none)
    juce::Typeface::Ptr                  nameFace;       // the face those names MUST be drawn in
    // The source label's colour, ALWAYS supplied by setDeltaProviders - the editor passes
    // the derived Spectrum-vs-Tuning colour, which is a blend of two palette entries and
    // therefore has no hex of its own to copy here.
    //
    // This used to read `{ 0xffff9a3c }` under the comment "Spectrum-to-Tuning colour".
    // That orange was retired when the palette became a measured, derived set; keeping it
    // here left a second, wrong answer to "what colour is Spectrum vs Tuning" sitting in
    // a file that never gets to paint it, which is precisely how GalacticPalette.h stops
    // being the single source of truth it says it is. White is what a caller who forgets
    // would get, and white is legible on every surface in this window - so the fallback
    // states nothing about the palette at all, which is the point.
    juce::Colour sourceColour { juce::Colours::white };
    juce::String referenceWord { "REFERENCE" };   // set by applyLanguage; see setReferenceWord
    // The three row captions, English by default so a build with no applyLanguage() call
    // still says something. setRowCaptions() writes them in the language on screen.
    juce::String capSteps { "steps" }, capCents { "cents" }, capBeating { "beating" };
    static constexpr int stripH = 56;                    // top info strip (3 delta lines + dissonance band)
    int baseKey   = 48; // leftmost VISIBLE key (the scroll position)
    int rangeLo   = 48; // full playable range start
    int rangeCount = 40; // full playable range length (keys); a window of it is shown
    int mouseNote = -1;
    int hoverCell = -1;
    bool hoverArmed = false;
    juce::uint32 hoverStartMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleKeyboard)
};
