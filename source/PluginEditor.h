#pragma once

#include "PluginProcessor.h"
#include "ScaleKeyboard.h"
#include "PartialAmpEdit.h"
#include "IntervalTokens.h"
#include "GalacticPalette.h"
#include "dsp/Dissonance.h"
#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

//==============================================================================
// HOW MUCH ROOM A CONTROL LEAVES FOR ITS OWN WORD, AND HOW WIDE IT MUST BE TO HOLD IT.
//
// "haz un repaso de todos los textos que quedan cortados como 'not in this spectrum' y
//  arreglalos"
//
// Three different places need these numbers and they have to agree: the LookAndFeel that
// PAINTS the control, resized() which SIZES it, and the text sweep which CHECKS it. They
// did not agree - the tuning buttons were sized with a plain 12 pt font while the
// LookAndFeel drew them in ~15 pt bold, and every caption was sized as if a juce::Label
// drew into its whole width when JUCE reserves 5 px a side - so "Afegeix al banc" was
// 24 px too narrow and drawFittedText silently squashed it. This is a bilingual product:
// Catalan runs 20-40% longer than English, so a width that is a hard-coded number is a
// truncation waiting for a language switch.
//
// Each function mirrors exactly the painter named beside it. Nothing here guesses.
namespace fgfit
{
    // GalacticLnF::getTextButtonFont.
    inline juce::Font textButtonFont (int h)
    { return juce::Font (juce::FontOptions (juce::jmin (16.0f, (float) h * 0.62f)).withStyle ("Bold")); }
    // The drawn tuning-fork glyph plus its gap (GalacticLnF::drawButtonText).
    inline float textButtonForkWidth (int h)
    {
        const float fh = juce::jmin (13.0f, (float) h - 4.0f);
        return (float) ((int) std::ceil (fh * 0.62f) + 3) + 2.0f;
    }
    // GalacticLnF::drawButtonText reduces the bounds by 4 px each side.
    inline float textButtonRoom (int w, int h, bool fork)
    { return (float) w - 8.0f - (fork ? textButtonForkWidth (h) : 0.0f); }
    inline int textButtonWidth (const juce::String& text, int h, bool fork)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (textButtonFont (h), text))
             + 8 + (fork ? (int) std::ceil (textButtonForkWidth (h)) : 0) + 2; // +2: rounding
    }
    inline int textButtonWidth (const juce::Button& b)
    { return textButtonWidth (b.getButtonText(), b.getHeight(), b.getProperties().contains ("fork")); }

    // LookAndFeel_V4::drawToggleButton: a tick box of fontSize*1.1 at x=4, the text
    // starting 10 px after it, 2 px spare at the right.
    inline juce::Font toggleFont (int h)
    { return juce::Font (juce::FontOptions (juce::jmin (15.0f, (float) h * 0.75f))); }
    inline float toggleRoom (int w, int h)
    { return (float) w - (float) juce::roundToInt (juce::jmin (15.0f, (float) h * 0.75f) * 1.1f) - 12.0f; }
    inline int toggleWidth (const juce::String& text, int h)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (toggleFont (h), text))
             + juce::roundToInt (juce::jmin (15.0f, (float) h * 0.75f) * 1.1f) + 12 + 2;
    }
    inline int toggleWidth (const juce::ToggleButton& b)
    { return toggleWidth (b.getButtonText(), b.getHeight()); }

    // GalacticLnF::drawToggleButton's "onled" pill: the word ON / OFF drawn centred in
    // the WHOLE button, bold, at h * 0.62. The pill has no juce::Button text at all
    // (the painted sub-label above it is its name), so every other measurement in this
    // file reads it as an empty string and would happily size it to nothing.
    inline juce::Font onLedFont (int h)
    { return juce::Font (juce::FontOptions ((float) h * 0.62f).withStyle ("Bold")); }
    inline int onLedWidth (int h)  // "OFF" is the longer of the two words
    { return (int) std::ceil (juce::GlyphArrangement::getStringWidth (onLedFont (h), "OFF")) + 2; }

    // GalacticLnF::positionComboBoxText: the label is inset by height/3 on the left and
    // has to stop clear of the chevron, which drawComboBox puts height/2 in from the
    // right end. Three places need this number and they have to agree: the painter, the
    // resized() that sizes a box to the words it holds, and the probe that checks it. A
    // ComboBox neither clips visibly nor resizes - it draws as much of the item as the
    // cell happens to hold - so a formula copied out by hand let "Sync Project Tempo"
    // appear on screen as "Sync Project Te" with every check in the build still green.
    inline int comboTextWidth (int boxWidth, int boxHeight) noexcept
    { return boxWidth - boxHeight / 3 - boxHeight / 2; }
    // ...and the inverse: the box width a word of `textWidth` needs.
    inline int comboWidthForText (int textWidth, int boxHeight) noexcept
    { return textWidth + boxHeight / 3 + boxHeight / 2 + 2; } // +2: the label's own rounding

    // ...and two more things stand between that room and the ITEM the user reads, both
    // of which a width check has to know about or it certifies boxes that truncate:
    //   - juce::Label keeps its own {1,5,1,5} border off the bounds it is given, so ten
    //     more pixels never hold text;
    //   - the item is drawn in GalacticLnF::getLabelFont's face, which is BOLD with a
    //     12.5 px floor - not in the lighter face LookAndFeel_V4::getComboBoxFont sizes
    //     the label with.
    inline constexpr int kComboLabelBorder = 10;      // juce::Label's default 5 px a side
    inline juce::Font comboItemFont (int h)
    {
        return juce::Font (juce::FontOptions (juce::jmax (12.5f, juce::jmin (16.0f, (float) h * 0.85f)))
                               .withStyle ("Bold"));
    }
    inline int comboItemRoom (int boxWidth, int boxHeight) noexcept
    { return comboTextWidth (boxWidth, boxHeight) - kComboLabelBorder; }
    inline int comboWidthForItem (int itemWidth, int boxHeight) noexcept
    { return comboWidthForText (itemWidth + kComboLabelBorder, boxHeight); }

    // THE ONE PADDING RULE for a control sized to the words it actually holds.
    //
    // "hay varios botones y desplegables ahora mismo que son absurdamente anchos,
    //  reduce su anchura". Six controls were taking a share of their row - a half, a
    //  third, whatever was left - so a pill with the word "OFF" on it was 275 px wide
    //  and "Recent..." was 300. Each is now sized to its own text, and they all add the
    //  SAME breathing room on top of the painter's bare minimum so the panel reads as
    //  one system rather than six separate decisions about what looks about right.
    //
    // The minimum each function above returns is the painter's own: exactly enough for
    // the glyphs plus the insets that painter reserves. kSnugPad is the air around them.
    inline constexpr int kSnugPad = 12;
    inline int snugTextButtonWidth (const juce::TextButton& b, int h)
    { return textButtonWidth (b.getButtonText(), h, b.getProperties().contains ("fork")) + kSnugPad; }
    inline int snugToggleWidth (const juce::ToggleButton& b, int h)
    { return (b.getName() == "onled" ? onLedWidth (h) : toggleWidth (b.getButtonText(), h)) + kSnugPad; }
    // A TAB CHIP.
    //
    // "haz que los botones harmonic/custom tengan el mismo tamano que los de los
    //  sub-menus dentro del menu modulator (pitch, filter, custom, etc)"
    //
    // The engine chips (HARMONIC / CUSTOM) and the modulator sub-tabs (AMP / PITCH /
    // FILTER / CUSTOM) are the same control doing the same job - a row of exclusive
    // chips that titles the box under it and recolours it - so they are one size, one
    // band height, one inset, one font. They were 104 x 16 and 58 x 19: the engine pair
    // was wider AND shorter, and its text a whole point smaller, because each row
    // carried its own literals.
    //
    // The corner radius needs no number here: GalacticLnF::drawButtonBackground fills
    // every button that is not named "round" as a square rect, so both rows already
    // share it and a radius stated here would be a second, drifting copy of nothing.
    namespace chip
    {
        inline constexpr int kHeight = 19;  // the band a chip row occupies, chips filling it
        inline constexpr int kInsetX = 1;   // air each chip keeps either side of itself
        // A chip's own width: its word in the font that height gives it, the painter's
        // own 4 px a side, and the same kSnugPad every other control in this window is
        // padded with. Never a fixed constant: "HARMÒNIC" is not "HARMONIC".
        inline int width (const juce::TextButton& b)
        { return textButtonWidth (b.getButtonText(), kHeight, false) + kSnugPad + 2 * kInsetX; }
    }

    // The WIDEST item a combo can show, never the selected one: the box does not resize
    // when the user picks another item, so a slot sized to today's choice draws
    // tomorrow's cut off - and nothing on screen says it was cut.
    inline int snugComboWidth (const juce::ComboBox& c, int h)
    {
        const auto f = comboItemFont (h);   // from the TARGET height, not the current one
        int w = 0;
        for (int i = 0; i < c.getNumItems(); ++i)
            w = juce::jmax (w, juce::GlyphArrangement::getStringWidthInt (f, c.getItemText (i)));
        return comboWidthForItem (w, h) + kSnugPad;
    }

    // juce::Label: drawLabel works inside getBorderSize(), in getLabelFont()'s face.
    inline juce::Font labelFont (const juce::Label& l)
    { return l.getLookAndFeel().getLabelFont (const_cast<juce::Label&> (l)); }
    inline int labelWidth (const juce::Label& l)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (labelFont (l), l.getText()))
             + l.getBorderSize().getLeftAndRight() + 3;
    }
    // ...and, for the ones that are meant to WRAP, the height that text takes at a
    // given box width. A fixed height on a bilingual paragraph is a cut last line.
    inline int labelHeight (const juce::Label& l, int boxWidth)
    {
        juce::AttributedString as;
        as.setText (l.getText());
        as.setFont (labelFont (l));
        as.setJustification (l.getJustificationType());
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, (float) juce::jmax (1, boxWidth - l.getBorderSize().getLeftAndRight()));
        return (int) std::ceil (tl.getHeight()) + l.getBorderSize().getTopAndBottom() + 2;
    }
}

class FractalAdditiveAudioProcessorEditor : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit FractalAdditiveAudioProcessorEditor (FractalAdditiveAudioProcessor&);
    ~FractalAdditiveAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override; // reclaim keyboard focus so play works after a window switch
    void broughtToFront() override;                      // ditto when the host re-activates our window
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    // Test hook (RhythmVerify): the macro boxes and every control laid out inside the
    // SPECTRUM panel and Essentials, as name -> bounds. The verifiers build the processor
    // only, so until this existed a layout change could not be checked by ANYTHING except
    // opening the plugin and looking - and a box whose content overflows it, or two boxes
    // that overlap, are exactly what a fixed 1200x650 rearrangement gets wrong.
    // Nothing in the plugin itself calls this.
    struct LayoutProbe
    {
        // engineBox is the ONE shared box both engines live in (the HARMONIC / CUSTOM
        // chips are its title; the selected chip decides whose controls are inside).
        juce::Rectangle<int> spectrum, engineBox, bothEngines,
                             essentials, envelopes, tuning, engineChips,
                             specColumn, // the full-height Spectrum-vs-Tuning box on the right
                             titleBar;   // the strip the preset cluster + playground button live in
        // THE TOP-RIGHT CORNER of every macro panel, as how far INSIDE its own corner the
        // drawn border passes: 0.5 px when that corner is square, 2.5 px for the 9 px
        // radius. Sampled off the very path paint() strokes (see macroFramePath).
        //
        // Measured this way because nothing else in the probe can see it. The wall lines
        // already agree (an arc leaves the border's x exactly where it was, it just stops
        // short of the top), the rects already touch, and every bounds check passes - while
        // on the glass two arcs pinch the shared wall at its top end and it reads as a seam
        // instead of one line.
        std::vector<std::pair<juce::String, float>> panelCorners;
        // The colour the WHOLE SPECTRUM panel is wearing right now (the selected
        // engine's). Probed because "it must not be the neutral steel again, and it
        // must not collide with the panels around it" is a measurable claim about a
        // hue, and nothing else in the build could check it.
        juce::Colour panelColour;
        // The SLOPE (AMP DECAY) fader's thumb colour, and whether it is frozen right now.
        // Probed because that fader is the ONE control in the panel allowed to leave the
        // panel colour, and only in one direction: the inert grey while manual per-partial
        // amplitudes are on. It went MINT after a pencil on/off cycle - a colour this
        // panel stopped using - and no geometric check in the build could see it.
        juce::Colour slopeColour;
        bool         slopeFrozen = false;
        std::vector<std::pair<juce::String, juce::Rectangle<int>>> controls; // "harm.baseN" etc.
        // The SUB-CELLS the boxes are divided into. They are rectangles only - the
        // dividers between them and their inline sub-labels are drawn in paint() from
        // exactly these - so a cell that escaped its box, or two cells that grew into
        // each other, draws a divider across a neighbour's controls and nothing in the
        // build could see it. Named "eng.base", "both.slope", "ess.ref", ...
        std::vector<std::pair<juce::String, juce::Rectangle<int>>> cells;
        // Every PAINTED label: the exact rect paint() draws it into, and how wide the
        // string actually is in the font paint() uses. A painted label has no component
        // to squash, so it fails by CLIPPING - silently, in one language only ("FROZEN -
        // DRAWN BY HAND" clipped at "DRA..."). Probing the measured width is the only way
        // a build can tell.
        struct PaintedLabel
        {
            juce::String name, text;
            juce::Rectangle<int> cell;  // the rect the text is drawn into
            float textWidth = 0.0f;     // measured in the very font paint() sets
            // The INK. "The whole panel is one colour" is a claim about exactly this,
            // and it is invisible to every geometric check in the build: the old
            // mint/magenta scheme of the Fractal-Morph-Range compartment laid out
            // perfectly and simply looked like a different box.
            juce::Colour colour;
        };
        std::vector<PaintedLabel> paintedLabels;
        // The same measurement for every juce::Label COMPONENT laid out in those boxes,
        // in its own font. A Label does not clip - it squashes to 70% - so it fails by
        // going unreadably condensed instead of disappearing, which no bounds check can
        // see either. Same struct: cell = the label's bounds, textWidth = what it needs.
        std::vector<PaintedLabel> componentLabels;
        // The read-outs that WRAP. A juce::Label lays its text out in as many lines as its
        // height allows, and modern JUCE will SHRINK THE FONT to make more of them fit - so
        // a sentence that "fits" can be rendering two points smaller than the one beside
        // it, and a single-line width check would reject every wrapping label out of hand.
        // Measured the only way that answers the real question: word-wrap this text, in
        // this font, at this width, and see how tall it comes out.
        struct WrappedLabel
        {
            juce::String name, text;
            juce::Rectangle<int> cell;   // the TEXT area - the Label's own border removed
            float needWidth = 0.0f, needHeight = 0.0f;
        };
        std::vector<WrappedLabel> wrappedLabels;
        // ...and every OTHER sentence the adaptation's read-out can print, measured against
        // that same label's text area with the widest numbers each can hold. The label only
        // ever shows one state; probing the live one would measure whichever state the
        // verifier happened to reach, and the state that clips is always the other one.
        std::vector<WrappedLabel> readoutVariants;
        // Every combo whose ITEM TEXTS are rewritten by applyLanguage(), as
        // name -> selected id. Probed because re-wording a ComboBox is the one operation
        // that can silently DESELECT it (getSelectedId() compares the visible label
        // against the item's text), and a mode box that reads 0 gates its whole panel off.
        std::vector<std::pair<juce::String, int>> comboIds;
        // ...and, for the combos whose WORDS are the feature, whether the selected item
        // really FITS the box it was given. A ComboBox does not clip visibly and does not
        // resize: it just draws as much of the word as the cell holds, so every geometric
        // check in the build passes while the screen reads "Sync Project Te". `avail` is
        // the text area LookAndFeel_V4 leaves after the arrow; `need` is what the word
        // measures in the box's own font. This is the check that bites when a label is
        // renamed to something longer than its slot - the Shepard rate box was.
        struct ComboText
        {
            juce::String name, text;
            float need = 0.0f;
            int   avail = 0;
        };
        std::vector<ComboText> comboTexts;
        // ...and the same measurement for every BUTTON that is sized to its own word.
        // A juce::TextButton does not condense and does not resize: drawFittedText
        // squashes the string to 70% and then clips it, so a button narrowed to fit
        // English shows a Catalan word cut in half with every bounds check still green.
        // Narrowing controls is exactly how that appears, so the six controls of this
        // pass - and every other button in the window - are measured here. `avail` is
        // the room the painter really leaves (fgfit::textButtonRoom / toggleRoom, the
        // fork glyph and the tick box already taken off); `need` is the string's width
        // in the painter's own font.
        std::vector<ComboText> buttonFits;
        // The BUTTONS whose word is the whole feature: the custom view's one button that
        // is "Load" or "Remove" depending on what is loaded, and the tuning bank's add
        // button. A wrong word on the first of those means a green Load that deletes the
        // user's spectrum, and no bounds check in the build could ever see it.
        std::vector<std::pair<juce::String, juce::String>> buttonTexts;
        // THE SHARED WALL, as the x of the LINE each of the three boxes actually draws
        // there - not the x of its rectangle. Every border is a stroke whose centre sits
        // a fixed distance inside the box, so two rects that merely touch draw TWO lines
        // back to back; comparing the rects would call that a shared wall while the
        // screen shows a doubled border. These are what paint() puts on the glass.
        float wallSpectrumRight = 0.0f, wallTuningRight = 0.0f, wallColumnLeft = 0.0f;
        // The Steps field's HOVER, in the language this probe was taken in. The average /
        // worst-case selector and the sentence under it are gone (minimax could not change
        // the result - see adaptRegStepsEdit), and the one thing that must not go with them
        // is the answer to "so how ARE those registers combined?". It lives in this tooltip
        // now, and a tooltip is invisible to every other check in this build - including to
        // the bilingual sweep, which restores the language before it returns, so it has to
        // be read HERE or not at all.
        juce::String regStepsTip;
        // EVERY DIVIDER LINE the SPECTRUM panel strokes, named, exactly as
        // spectrumDividers() returns them to paint(). A divider is a bare drawLine with
        // no component behind it, so a wall that stops short of the panel floor is
        // invisible to every rect check in the build - which is how the prime column's
        // left wall shipped with a 23 px hole under it.
        std::vector<std::pair<juce::String, juce::Line<float>>> dividers;
        // THE KEYBOARD, as two rectangles that are not the same claim.
        //
        // "el teclado ahora mismo no aparece centrado en el sinte, haz que asi sea"
        //
        // `keyboardBox` is the component's own bounds in the editor. `keysDrawn` is the
        // span the KEYS really occupy inside it, taken from ScaleKeyboard::cellRect -
        // the function paint() fills from. The two differ whenever the tuning has fewer
        // playable keys than the window has cells (a wide period puts only a handful
        // between 40 Hz and 4186 Hz), and that is the case that was off centre: the
        // component filled the editor while its keys hugged the left edge. Checking the
        // component alone would have called that centred.
        juce::Rectangle<int> keyboardBox, keysDrawn;
        int keysInWindow = 0, keysPlayable = 0;   // cells available vs cells with a key
    };
    LayoutProbe layoutProbe() const;
    // THE DIVIDER LINES INSIDE THE SPECTRUM PANEL, from the cell rects. paint() strokes
    // exactly what this returns and layoutProbe() reports exactly what this returns;
    // there is no second place deciding where a wall starts or ends. Same rule as
    // macroFramePath, for the same reason: a probe that rebuilt the lines from its own
    // copy would keep passing after paint() changed them.
    std::vector<std::pair<juce::String, juce::Line<float>>> spectrumDividers() const;
    // THE OUTLINE A MACRO PANEL IS DRAWN WITH, from its box rect - fill and stroke both.
    // paint() strokes exactly what this returns and layoutProbe() measures exactly what
    // this returns; there is no second place deciding which corners are round.
    //
    // That is not tidiness, it is what makes the corner check bite. The probe used to
    // rebuild the path from its own copy of the rule, so sabotaging paint() into rounding
    // the shared corner again left every check passing - the probe was measuring its own
    // assumption. With one function, a hand that squares or rounds the wrong end changes
    // the number the probe reads.
    //
    // The SHARED-WALL panels are SPECTRUM and TUNING: their right border is the same drawn
    // line as the Spectrum-vs-Tuning column's left border (see the wall block in resized()),
    // so that end is square and every corner facing open page keeps its radius.
    juce::Path macroFramePath (const juce::Rectangle<int>& box) const;
    // Test hook (RhythmVerify): probe the layout in the OTHER language. Every painted
    // sub-label is bilingual and Catalan is the longer language nearly everywhere, so a
    // label that fits in English can still clip in Catalan - which is precisely how the
    // "FROZEN - DRAWN BY HAND" clip survived review. Restores the language it found.
    LayoutProbe layoutProbeInLanguage (bool useCatalan);
    // Test hook (RhythmVerify): re-read engineInForce() and redress the panel for it,
    // exactly as the 30 Hz timer does. The verifier has no message loop, so without this
    // the panel would still be wearing the colour it was constructed with and "selecting
    // an engine recolours the whole panel" could not be measured at all.
    void refreshEngineDressing() { applyEngineState(); }
    // Test hook (RhythmVerify): run ONE tick of the panel timer. The verifier has no
    // message loop, so anything that only appears because the timer noticed a change -
    // the Shepard's rise-interval dropdown when the tuning's period stops being an
    // octave, say - is otherwise unreachable, and "it appears when the tuning changes"
    // could not be measured at all. It is the timer's own layout signature that is
    // under test, so calling resized() directly here would test nothing.
    void tickTimerForTests() { timerCallback(); }
    // Test hook (RhythmVerify): re-read "partialManual" and redress the SLOPE fader,
    // exactly as the 30 Hz timer does. The verifier has no message loop, so a pencil
    // on/off cycle is otherwise unreachable - and that cycle is the bug.
    void refreshSlopeFreezeForTests() { refreshSlopeFreeze(); }
    // Test hook (RhythmVerify): click an engine chip the way a hand does. Exists for
    // the one behaviour a probe of engineInForce cannot reach: with NO scale loaded,
    // clicking CUSTOM must still open that view (it is the only place the Load button
    // and the intervals editor live), while the sound honestly stays harmonic.
    void clickEngineChipForTests (int i) { if (engineChip[i & 1].onClick) engineChip[i & 1].onClick(); }
    // Test hooks (RhythmVerify): the custom view's two buttons, through their own
    // callbacks. The Load/Remove one is a SINGLE button with two opposite jobs, and
    // which job it has depends on state the verifier can set - so "the green Load never
    // deletes anything and the red Remove always does" is only checkable from here.
    // INSTRUMENTATION (RhythmVerify): drag a fader the way a hand does - press, move,
    // release - and report what happened around it. Written for "mover controles con
    // notas apretadas sigue siendo buggy, a veces no deja mover el control": the claim
    // is about a DRAG, and a drag is the one thing no probe in this build could see.
    // Everything that could plausibly refuse one is recorded next to the result, so a
    // failure says WHICH of them was true at the time instead of just "it did not move".
    struct DragProbe
    {
        bool   found = false, enabled = false, visible = false;
        double before = 0.0, afterDrag = 0.0, afterSettle = 0.0;
        // The state the drag happened in.
        int          heldNotes = 0;       // computer-keyboard keys this editor is sounding
        juce::String focusOwner;          // the component holding keyboard focus, by class
        bool         editorHasFocus = false;
        int          msSinceFocused = 0;  // the pollComputerKeyboard debounce clock
    };
    // `name`: "slope", "baseN", "fractal", "morphTime", "harmonics".  `dy`: pixels to
    // drag downwards.  `settleTicks`: timer ticks to run afterwards, to catch a value
    // that moves and is then written back.
    DragProbe dragFaderForTests (const juce::String& name, int dy, int settleTicks);
    void clickCustomLoadRemoveForTests() { if (loadTimbreButton.onClick) loadTimbreButton.onClick(); }
    void clickRandomSpectrumForTests()   { if (randomSpectrumBtn.onClick) randomSpectrumBtn.onClick(); }
    // Test hook (RhythmVerify): double-click a row of the chord-search results, which is
    // the ONLY way into the "Tuning info" view - it loads the chord as the scale and
    // flips the list over to that scale's own degrees. Through the real callback, so the
    // view flip and the resized() that follows it are the ones that ship.
    void loadChordResultForTests (int row) { if (chordList.onLoad) chordList.onLoad (row); }
    // TESTS: what the chord-recommendation list ("Tuning info") is SHOWING right now -
    // read off the rows it will paint, not off the worker's output. Every chip is a
    // degree INDEX, so `maxDegree` must always be inside 0..degreesPerPeriod-1 of the
    // scale that is loaded: "los resultados de tuning info aparecen con numeros que no
    // corresponden a los numeros de tunings que no son 12edo, tipo en una pentatonica me
    // dice 7 y 11". Nothing else in the build can see a stale row.
    struct NadShownProbe
    {
        int  rows = 0;
        int  maxDegree = -1;          // the biggest chip number on screen
        int  degreesPerPeriod = 0;    // what the LOADED scale actually has
        bool equalStep = false;       // rows shown as "any +n" offsets rather than degrees
    };
    NadShownProbe nadShownProbeForTests() const;

    // Test hooks (RhythmVerify): drive the Spectrum-vs-Tuning panel the way a hand does -
    // pick a mode, pick the adaptation's target, press REALTIME - and read back what the
    // panel then says. The wiring these check ("REALTIME follows the notes you PLAY,
    // whatever the target box was left on" and "each mode keeps its own Period") lives
    // entirely inside these callbacks, and the verifier has neither a mouse nor a message
    // loop to reach them with.
    struct PanelProbe
    {
        int  mode = 0;                 // specModeBox
        int  adaptTarget = 0;          // adaptTargetBox (1 = loaded tuning, 2 = held chord)
        bool adaptTargetEnabled = true;
        juce::String period;           // the shared Period field, as typed
    };
    PanelProbe panelProbeForTests() const;
    // Test hook (RhythmVerify): the keyboard's degree-0 keys, with the period number each
    // one prints and the rects and fonts paint() really uses (see ScaleKeyboard). Those
    // numbers are how the register range's ANCHOR is named back to the user ("3:0"), so
    // they have to be legible at the key width the keyboard actually uses, in every tuning -
    // and there is no other way in this build to see a number that drawFittedText silently
    // shrank to fit.
    std::vector<ScaleKeyboard::PeriodTag> keyboardPeriodTagsForTests() const
    { return keyboard.periodTagProbe(); }
    // Test hooks (RhythmVerify): the adaptation's register range - the step count the field
    // shows, the anchor it is counted from, the keys it resolves to and the Hz the search
    // is really given. A range that reads one width and searches another is the bug this
    // probe exists to catch, and it now has one more way to happen than it did: the anchor
    // moves with the hands.
    struct RegisterProbe { int steps = 0, fitting = 0, anchorKey = 0, keyLo = 0, keyHi = 0;
                           double loHz = 0.0, hiHz = 0.0; juce::String stepsText, spanText; };
    RegisterProbe registerProbeForTests() const;
    void setAdaptRegisterStepsForTests (const juce::String& n);
    // Test hook (RhythmVerify): what the panel would print on the Adapt button, and what
    // one run / one realtime update is predicted to cost with the settings as they stand.
    juce::String adaptButtonTextForTests() const { return adaptGoBtn.getButtonText(); }
    double       adaptCostMsForTests (bool realtime) const { return adaptCostMs (realtime); }
    juce::String adaptReadoutForTests() const { return adaptReadoutText(); }
    void setSpecModeForTests (int mode);
    void setAdaptTargetForTests (int id);
    // The two selectors that ADD rows to the right column (the free chord search's
    // Find/REALTIME pair, and the adaptation's "between lo hi" row). Both exist only in
    // one branch of resized(), so a layout sweep cannot reach them any other way.
    void setTuneSourceForTests (int id);
    void setAdaptGoalForTests (int id);
    void setPeriodTextForTests (const juce::String& t);
    void pressAdaptRealtimeForTests();

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;

    // Every inline sub-label the editor PAINTS (BASE / HARMONICS / AMP DECAY / FROZEN /
    // PHASE / ...). Built in ONE place and used by both paint() (which draws them) and
    // layoutProbe() (which measures them), because a painted label has no component to
    // squash and fails only by clipping - so the drawn rect and the measured rect must
    // be the SAME rect or the check is measuring a label that is not on screen.
    struct PaintedSub
    {
        juce::String name, text;
        juce::Rectangle<int> cell;
        juce::Colour col;
        float fontHeight = 11.5f;
        bool  fitted = false;  // drawFittedText (shrinks to fit) vs drawText (clips)
    };
    std::vector<PaintedSub> paintedSubLabels() const;

    void configGroup (juce::GroupComponent&, juce::Colour);
    void configFader (juce::Slider&, const juce::String& paramID,
                      std::unique_ptr<SliderAttachment>&, juce::Colour, const juce::String& tooltip);
    // Every fader configFader touches is watched: whenever an envelope/LFO/host
    // automation is actually driving its parameter right now, the LookAndFeel draws
    // a second marker at that LIVE position (see GalacticLnF's "modLive" property
    // read in drawLinearSlider/drawRotarySlider) alongside the thumb's own base
    // value - so picking e.g. the right sustain point under modulation is visual,
    // not guesswork. Polled on the existing 30 Hz timer; see timerCallback().
    std::vector<juce::Slider*> modWatchSliders;
    void refreshModWatch();
    void configCombo (juce::ComboBox&, const juce::String& paramID,
                      std::unique_ptr<ComboBoxAttachment>&, juce::Colour, const juce::String& tooltip);
    void makeLabel (juce::Label&, juce::Colour);
    void refocus();
    void reclaimFocusUnlessTyping(); // regrab keyboard focus after a window switch (not while typing)
    // Is any mouse button down anywhere right now? Desktop's own answer, and it is
    // already false again by the time a mouseUp is dispatched - see refocus().
    static bool mouseButtonHeld();
    // THE RULE for taking the keyboard back, as data, so the drag tests can ask it
    // directly instead of inferring it from where the focus happened to land.
public:
    struct FocusReclaimProbe
    {
        bool typing = false;      // a text field owns the letters
        bool mouseHeld = false;   // a press is in progress: a focus change could end it
        bool alreadyOurs = false;
        bool tooSoon = false;     // another attempt inside the debounce window
        bool wouldGrab = false;
    };
    FocusReclaimProbe focusReclaimProbeForTests() const { return focusReclaimState(); }
private:
    FocusReclaimProbe focusReclaimState() const;
    void pollComputerKeyboard(); // reconcile held keys against the real key state
    bool playKeysLive() const;   // may the QWERTY mapping ring notes right now?
    // Repaint the Custom Engine's interval list with one colour per token (see
    // IntervalTokens.h). Called on every edit and after every programmatic setText.
    void recolourIntervalTokens();
    void setIntervalText (const juce::String& text); // setText + recolour, never one without the other
    // The text the box was last painted from. It is the re-entry guard, and it has to be
    // the TEXT and not a bool: TextEditor::textChanged() POSTS the onTextChange callback
    // instead of calling it, so a scoped flag is already down by the time our own inserts
    // come back as messages - each of which would rebuild the text and post more, which
    // multiplies rather than settles. Comparing the text ends it after one pass.
    juce::String lastColouredIntervals;
    void openScalaChooser();
    void openPartialAmpWindow();   // the pencil in the SLOPE cell
    void openPlaygroundWindow();   // "Open Preset Playground", under the preset selector
    // Build-and-wire WITHOUT showing, split out of the two open functions so the
    // ownership probe below exercises the exact construction a real click runs.
    void ensurePartialWindow();
    void ensurePlaygroundWindow();
    // THE one mechanism both floating windows use to stay above the editor: OS-level
    // ownership (see FloatingWindowOwner.h). Called on every open, not just the first,
    // because a host may re-dock the editor under a different top-level window.
    void attachFloatingWindow (juce::TopLevelWindow&);

public:
    // Test hooks (RhythmVerify): the playground panel is in a SEPARATE desktop window,
    // so nothing the main layout probe measures ever touches it - and the verifier has
    // no mouse to open it with. These lay it out at a given size, run its paint(), and
    // drive the pad the way a drag does, so the component's own code (its layout, its
    // pixel-to-parameter mapping and the gesture pair the host records from) is exercised
    // by the build rather than only by someone opening the plugin and looking.
    struct PlaygroundProbe
    {
        juce::Rectangle<int> panel, pad, padArea, box[4], clear[4], lbl[4];
        // The per-corner EDIT button, its real (localised) label, and whether it is
        // enabled - an Edit button on an empty corner has nothing to edit and must not
        // be clickable. Bounds so the verifier can prove the new button did not shove
        // the corner's name box under its neighbours.
        juce::Rectangle<int> edit[4];
        juce::String editText[4];
        bool editEnabled[4] = { false, false, false, false };
        // The corner names AS DRAWN, so a check can see the "*" a modified corner must
        // carry (and, just as importantly, see it disappear when the corner is saved).
        juce::String cornerName[4];
        // The MORPH/MIX mode pair: bounds and the REAL (localised) label on each, so
        // the verifier can assert the Catalan "MESCLA" fits the button it is drawn in.
        juce::Rectangle<int> mode[2];
        juce::String modeText[2];
        // The MIX voice-budget row (label + control) and the label's real text.
        juce::Rectangle<int> voicesL, voicesC;
        juce::String voicesText;
        juce::String status;
    };
    // Probes in the given language and RESTORES the one it found, exactly as
    // layoutProbeInLanguage does - a probe that leaves the editor in Catalan would make
    // every later check in the run measure the wrong strings.
    PlaygroundProbe playgroundProbeForTests (int w, int h, bool useCatalan);
    // Press, move to the normalised point, release - the whole gesture, through the very
    // handlers a mouse reaches.
    void playgroundDragForTests (float nx, float ny);
    // The floating windows' OS ownership, read BACK from the OS. The Z-order behaviour
    // itself (clicking the editor no longer buries the floating window) is enforced by
    // Windows from this one recorded relationship, so the honest automated check is
    // "the OS has the editor's top-level root on record as the owner" - the raise
    // behaviour can only be confirmed by eye. The caller must have put the editor on
    // the desktop first (hidden is fine); peers are created hidden, nothing flashes.
    struct FloatingOwnershipProbe { bool windowOnDesktop = false, ownerIsEditorRoot = false; };
    FloatingOwnershipProbe floatingOwnershipProbeForTests (bool playground);

    // TESTS: what the preset drop-down's list REALLY shows for a typed query. Builds the
    // actual panel and types into its actual field, so a check can never pass against a
    // second copy of the filter that the drop-down itself has stopped using.
    static juce::StringArray presetSearchProbeForTests (const juce::StringArray& names,
                                                        const juce::String& typed);
    // TESTS: how many times a RefreshingComboBox whose drop-down is TAKEN OVER (the
    // presets box) can actually be opened. The user's report was "it opens once, I load
    // a preset, and it never opens again" - ComboBox's private menuActive flag was left
    // set forever because no PopupMenu ever finished to clear it. This drives the same
    // public entry point a click does (showPopupIfNotActive) `attempts` times and
    // returns how many of those reached onShowPopup. Anything less than `attempts` is
    // the bug. (showPopup() is dispatched asynchronously; the probe delivers that one
    // callback by hand, since the verifier has no dispatch loop - see the body.)
    static int comboReopenProbeForTests (int attempts);
    // TESTS: how much clear space the CUSTOM RATIOS box really leaves around the numbers
    // typed into it. "adapta la caja de custom ratios para que los numeros no toquen su
    // borde." Two separate ways that went wrong, neither visible to any rect check:
    //   - a TextEditor's clearances are asymmetric (setIndents has no bottom or right
    //     twin, and the default border is {1,1,1,3}), so the bottom row sat 1 px from
    //     the outline;
    //   - this LookAndFeel strokes a TextEditor's outline as a PILL of radius height/2,
    //     which on the 70 px tall ratios box is a 35 px arc biting deep into the first
    //     and last rows - the corners of the text area were OUTSIDE the drawn shape.
    // `clip` is the border inset, i.e. the only region the editor can put ink in at all
    // (the viewport clips there), read back out of JUCE rather than restated here.
    struct RatiosInkProbe
    {
        juce::Rectangle<int> box;   // the editor itself
        juce::Rectangle<int> clip;  // where any glyph can land: nothing outside reaches the glass
        juce::Rectangle<int> ink;   // bounding box of the glyphs currently laid out
        float radius = 0.0f;        // the corner radius the outline is stroked with
        float minGap = 0.0f;        // clear px between the outline's inner edge and `clip`
        bool  clipInsideOutline = true; // all four corners of `clip` inside the drawn shape
        bool  inkInsideClip = true;
    };
    RatiosInkProbe ratiosInkProbeForTests (const juce::String& typed);

    //==========================================================================
    // EVERY DRAWN STRING IN THE WHOLE EDITOR, MEASURED AGAINST THE RECT IT IS DRAWN INTO.
    //
    // "haz un repaso de todos los textos que quedan cortados como 'not in this spectrum'
    //  y arreglalos"
    //
    // The existing checks name their controls one by one, so they can only ever see the
    // text somebody remembered to register - and a truncation is precisely what nobody
    // remembered. This one WALKS THE TREE instead: every visible Label, Button and
    // ComboBox under the editor, whatever panel it lives in, named by its path so the
    // report says where to go. Nothing to keep in step, so a control added tomorrow is
    // measured tomorrow.
    //
    // `need` is what the string measures in the very font that control draws it in;
    // `avail` is the width that control really leaves for text (a Label's border, a
    // TextButton's corner indents, a ToggleButton's tick box, a ComboBox's arrow), each
    // taken from the LookAndFeel that actually paints it. Multi-line labels are
    // word-wrapped instead and report `needHeight` against the box.
    struct TextFit
    {
        juce::String path;      // "rhythmPanel/slot0Lbl", ...
        juce::String kind;      // Label / TextButton / ToggleButton / ComboBox
        juce::String text;
        juce::Rectangle<int> bounds;   // the control, in editor coordinates
        float need = 0.0f,  avail = 0.0f;   // single-line width
        float needH = 0.0f, availH = 0.0f;  // wrapped height, for the ones that wrap
        bool  wraps = false;
        float over() const { return wraps ? needH - availH : need - avail; }
    };
    // Lays the editor out in the given language, walks it, and puts the language back.
    std::vector<TextFit> textFitProbeForTests (bool useCatalan);
private:
    // Recursive half of textFitProbeForTests: every visible Label / Button / ComboBox
    // under `parent`, measured against the room its own LookAndFeel leaves it.
    void walkTextFits (const juce::Component& parent, const juce::String& path,
                       std::vector<TextFit>& out) const;
public:
    // TESTS: what the MAIN window says about whose patch its controls are. The preset
    // field is frozen (and disabled) while a playground corner is being edited; nothing
    // else in the build can see that, and getting it wrong is how a user loads a preset
    // onto somebody's corner.
    struct CornerEditUiProbe
    {
        int  corner = -1;
        juce::String presetText, saveText;
        bool presetEnabled = true, dirtyMarkVisible = false;
    };
    CornerEditUiProbe cornerEditUiProbeForTests()
    {
        refreshCornerEditUi();
        CornerEditUiProbe pr;
        pr.corner           = processorRef.getPlaygroundEditCorner();
        pr.presetText       = presetBox.getText();
        pr.saveText         = savePresetButton.getButtonText();
        pr.presetEnabled    = presetBox.isEnabled();
        pr.dirtyMarkVisible = presetDirtyMark.isVisible();
        return pr;
    }

private:
    void refreshSlopeFreeze();     // shows that SLOPE is inert while manual mode is on
    void openTimbreChooser();
    void openTuningBankChooser();
    void refreshTuningBank(); // sync the bank combo items + selection with the processor
    void loadRecent (int index);
    void applyLanguage();
    void refreshPresetList();
    void savePresetFlow();
    // The presets box's own drop-down: a search field over a live-filtered list (see
    // PresetSearchPanel). Returns false when there is nothing to show, so the ordinary
    // ComboBox popup still gets its turn and an empty folder is not an empty mystery box.
    bool openPresetSearch();
    // Ask for a name and save the playground corner currently in edit mode under it.
    void saveCornerFlow (int corner);
    // Keep the preset field / Save button honest about WHOSE patch the controls are.
    // Called whenever edit mode may have changed, and once per UI tick (automation and
    // the processor itself can end it - a corner cleared, the playground disengaging).
    void refreshCornerEditUi();
    void updateWarning (int templatePartials);
    void runDissonanceTune();
    void saveDissonanceScl();
    juce::String tr (const char* en, const char* ca) const { return catalan ? juce::String::fromUTF8 (ca) : juce::String (en); }
    void timerCallback() override;

    // ComboBox that refreshes its item list just before its popup opens (so the
    // preset list always reflects the folder contents).
    struct RefreshingComboBox : juce::ComboBox
    {
        std::function<void()> onBeforePopup;
        // Optional: take the drop-down over entirely (the presets box does - it opens a
        // SEARCHABLE list instead of a plain menu). Returning true means "handled".
        std::function<bool()> onShowPopup;
        void showPopup() override
        {
            if (onBeforePopup) onBeforePopup();
            if (onShowPopup && onShowPopup())
            {
                // THE BOX OPENED EXACTLY ONCE PER EDITOR, THEN DIED FOR GOOD.
                //
                // ComboBox::mouseDown -> showPopupIfNotActive() sets the private
                // menuActive flag and THEN calls showPopup() asynchronously. The only
                // thing in JUCE that ever clears menuActive again is hidePopup(), and
                // the only caller of that is comboBoxPopupMenuFinishedCallback - the
                // modal callback of the PopupMenu that juce::ComboBox::showPopup()
                // launches. When we take the drop-down over there IS no PopupMenu, so
                // returning here left menuActive stuck true: every later mouseDown,
                // mouseUp, Return key and scroll-wheel tick was swallowed by the
                // `if (! menuActive)` guard and the presets box never opened again.
                //
                // hidePopup() is public and does exactly the one thing needed: clear
                // the flag (plus PopupMenu::dismissAllActiveMenus(), which walks only
                // PopupMenu's own MenuWindow list and so cannot touch the CallOutBox
                // onShowPopup just launched).
                hidePopup();
                return;
            }
            juce::ComboBox::showPopup();
        }
    };

    //==========================================================================
    // CASE- AND DIACRITIC-INSENSITIVE FOLDING, for the preset search box.
    //
    // Preset names are the user's own words in the user's own language, and this is a
    // bilingual product: "Can\xc3\xa7\xc3\xb3 llunyana", "Melod\xc3\xada", "R\xc3\xa0dio"
    // are exactly the names it invites. A search that only matched byte-for-byte would
    // be a trap for those names specifically - you would have to know which accent you
    // typed a year ago to find your own preset. juce::String has no folding of its own,
    // so the Latin letters this project's two languages (and their neighbours) use are
    // mapped here BY CODE POINT - never by a literal accented character, which would
    // depend on how this file happens to be encoded.
    //
    // Anything not in the table falls through unchanged, so a Cyrillic or CJK name still
    // matches itself exactly. U+00B7 (Catalan's l\xc2\xb7l) folds to NOTHING, so
    // "col\xc2\xb7legi" is found by typing "collegi".
    // ORDER MATTERS: the accent fold happens FIRST, on the raw code point, and covers
    // BOTH cases of every letter. Lowercasing first would not work - JUCE's toLowerCase
    // goes through towlower, which is locale-dependent and in the "C" locale a plugin
    // starts in leaves U+00C7 ('\xc3\x87') exactly where it was. Folding after that would
    // silently miss every CAPITALISED accented name, which is most of the names people
    // actually give presets. Only once nothing matched does toLowerCase get its turn, on
    // ASCII, where it is reliable.
    static juce::juce_wchar foldSearchChar (juce::juce_wchar cIn)
    {
        const int  c  = (int) cIn;
        const auto in = [c] (int lo, int hi) { return c >= lo && c <= hi; };
        if (c == 0xb7) return 0;   // U+00B7, Catalan's l-geminada: punctuation, not a letter
        if (in (0xc0, 0xc5) || in (0xe0, 0xe5) || in (0x100, 0x105))            return 'a';
        if (c == 0xc7 || c == 0xe7 || in (0x106, 0x10d))                        return 'c';
        if (in (0x10e, 0x111))                                                  return 'd';
        if (in (0xc8, 0xcb) || in (0xe8, 0xeb) || in (0x112, 0x11b))            return 'e';
        if (in (0x11c, 0x123))                                                  return 'g';
        if (in (0x124, 0x127))                                                  return 'h';
        if (in (0xcc, 0xcf) || in (0xec, 0xef) || in (0x128, 0x131))            return 'i';
        // 0x13F/0x140 are Catalan's PRECOMPOSED L-with-middle-dot, so both spellings of
        // "col-legi" - the two-character one and the one-character one - fold to "collegi".
        if (in (0x139, 0x142))                                                  return 'l';
        if (c == 0xd1 || c == 0xf1 || in (0x143, 0x148))                        return 'n';
        if (in (0xd2, 0xd6) || c == 0xd8 || in (0xf2, 0xf6) || c == 0xf8
            || in (0x14c, 0x151))                                               return 'o';
        if (in (0x154, 0x159))                                                  return 'r';
        if (c == 0xdf || in (0x15a, 0x161))                                     return 's';
        if (in (0x162, 0x167))                                                  return 't';
        if (in (0xd9, 0xdc) || in (0xf9, 0xfc) || in (0x168, 0x173))            return 'u';
        if (c == 0xdd || c == 0xfd || c == 0xff || in (0x176, 0x178))           return 'y';
        if (in (0x179, 0x17e))                                                  return 'z';
        return juce::CharacterFunctions::toLowerCase (cIn);
    }
    static juce::String foldForSearch (const juce::String& s)
    {
        juce::String out;
        out.preallocateBytes ((size_t) s.getNumBytesAsUTF8() + 1);
        for (auto p = s.getCharPointer(); ! p.isEmpty();)
            if (const auto f = foldSearchChar (p.getAndAdvance()); f != 0)
                out += juce::String::charToString (f);
        return out;
    }

    //==========================================================================
    // THE PRESET DROP-DOWN, WITH A SEARCH BOX ("haz que el desplegable de presets tenga
    // un buscador"). A plain ComboBox popup is a PopupMenu, and a PopupMenu owns the
    // keyboard for its own navigation - a text field inside one cannot reliably be typed
    // into. So the presets box opens THIS instead: a search field over a live-filtered
    // list, in a CallOutBox (which handles click-outside and Escape by itself).
    //
    // The one thing it must not break is PLAYING. The editor's computer-keyboard mapping
    // is disabled whenever a juce::TextEditor holds focus (see pollComputerKeyboard), so
    // letters typed here can never fire notes - and the instant this closes, focus goes
    // back to the editor and the same letters play again. That is why the search field is
    // a real TextEditor and not a hand-rolled key handler.
    struct PresetSearchPanel : juce::Component,
                               juce::ListBoxModel
    {
        juce::TextEditor search;
        juce::ListBox    list { "presets", this };
        juce::StringArray all, shown;
        juce::String      current;
        std::function<void (const juce::String&)> onPick;
        std::function<void()> onDismissed;

        PresetSearchPanel (juce::StringArray items, juce::String cur, juce::String placeholder)
            : all (std::move (items)), current (std::move (cur))
        {
            search.setTextToShowWhenEmpty (placeholder, juce::Colour (0xff6a7285));
            search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff141a24));
            search.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff55557a));
            search.setColour (juce::TextEditor::textColourId,       juce::Colours::white);
            search.setFont (juce::Font (juce::FontOptions (14.0f)));
            search.onTextChange = [this] { refilter(); };
            // Enter takes the highlighted row (the first match when nothing is selected),
            // so a name can be found and loaded without the mouse ever moving.
            search.onReturnKey  = [this] { pick (juce::jmax (0, list.getSelectedRow())); };
            search.onEscapeKey  = [this] { dismiss(); };
            addAndMakeVisible (search);

            list.setRowHeight (20);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff0e1118));
            list.setOutlineThickness (0);
            addAndMakeVisible (list);
            refilter();
        }
        ~PresetSearchPanel() override { if (onDismissed) onDismissed(); }

        void refilter()
        {
            const auto q = foldForSearch (search.getText().trim());
            shown.clear();
            for (const auto& n : all)
                if (q.isEmpty() || foldForSearch (n).contains (q))
                    shown.add (n);
            list.updateContent();
            // Select the loaded preset when it is still on screen, otherwise the first
            // match - so Enter always has an obvious meaning and never nothing.
            const int cur = shown.indexOf (current);
            list.selectRow (cur >= 0 ? cur : (shown.isEmpty() ? -1 : 0), true, true);
            list.repaint();
        }
        void pick (int row)
        {
            if (! juce::isPositiveAndBelow (row, shown.size())) return;
            const auto name = shown[row];
            dismiss();
            if (onPick) onPick (name);
        }
        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            else                                                            setVisible (false);
        }

        int getNumRows() override { return shown.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (! juce::isPositiveAndBelow (row, shown.size())) return;
            if (selected) { g.setColour (juce::Colour (0xff2d6da8)); g.fillRect (0, 0, w, h); }
            g.setColour (shown[row] == current ? juce::Colour (0xffffc95c) : juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText (shown[row], 6, 0, w - 10, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override { pick (row); }
        void returnKeyPressed (int row) override { pick (row); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (4);
            search.setBounds (r.removeFromTop (22));
            r.removeFromTop (4);
            list.setBounds (r);
        }
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0e1118)); }
    };

    // TextEditor with a focus hook. It used to stretch to the window edge while
    // typing (the intervals box was one 24 px line); now that box is a multi-line
    // area filling most of the shared engine box, so the hook only resets the view
    // to the text's start on blur - but the hook itself stays: without it a long
    // list was left scrolled to its tail and read as if it began mid-way.
    struct ExpandingEditor : juce::TextEditor
    {
        std::function<void (bool)> onFocusChanged;
        void focusGained (FocusChangeType t) override { juce::TextEditor::focusGained (t); if (onFocusChanged) onFocusChanged (true); }
        void focusLost   (FocusChangeType t) override { juce::TextEditor::focusLost (t);   if (onFocusChanged) onFocusChanged (false); }
    };

    // Integer-stepped fader. JUCE forces absolute (snap-to-mouse) dragging when
    // one pixel covers less than one step, so Ctrl velocity mode never engages on
    // integer parameters. We disable snapping (see ctor) and stretch the drag
    // distance instead - x4 while Ctrl is held - so these faders get the same
    // fine control as the continuous ones.
    struct SteppedSlider : juce::Slider
    {
        int normalSensitivity = 300;
        void mouseDown (const juce::MouseEvent& e) override
        {
            setMouseDragSensitivity (e.mods.isCtrlDown() ? normalSensitivity * 4 : normalSensitivity);
            juce::Slider::mouseDown (e);
        }
    };

    // Continuous fader with magnetic integer detents (Ctrl = free fine drag) and
    // tick marks painted at each whole level. Used by the fractal-level fader.
    struct MagneticSlider : SteppedSlider
    {
        // How close to a whole value the drag must come before it snaps onto it.
        // Per-slider because the axes differ in how much room they have: a 0..3
        // axis (Harmonics) needs a NARROWER window than a 1..4 one, or most of the
        // blend between the named points is simply unreachable.
        double snapWindow = 0.12;
        double snapValue (double v, juce::Slider::DragMode dm) override
        {
            if (dm == juce::Slider::notDragging) return v;
            if (juce::ModifierKeys::currentModifiers.isCtrlDown()) return v;
            const double n = std::round (v);
            return (std::abs (v - n) < snapWindow && n >= getMinimum() && n <= getMaximum()) ? n : v;
        }
        void paint (juce::Graphics& g) override
        {
            juce::Slider::paint (g);
            const auto r = getLookAndFeel().getSliderLayout (*this).sliderBounds.toFloat();
            if (r.isEmpty()) return;
            g.setColour (findColour (juce::Slider::trackColourId).withAlpha (0.9f));
            const int lo = (int) std::ceil (getMinimum()), hi = (int) std::floor (getMaximum());
            const bool vertical = isVertical();
            for (int i = lo; i <= hi; ++i)
            {
                const float p = (float) valueToProportionOfLength ((double) i);
                if (vertical) // proportion 0 = bottom of the track
                {
                    const float y = r.getBottom() - p * r.getHeight();
                    g.fillRect (r.getX(), y - 0.5f, 4.0f, 1.0f);
                    g.fillRect (r.getRight() - 4.0f, y - 0.5f, 4.0f, 1.0f);
                }
                else
                {
                    const float x = r.getX() + p * r.getWidth();
                    g.fillRect (x - 0.5f, r.getY(), 1.0f, 4.0f);
                    g.fillRect (x - 0.5f, r.getBottom() - 4.0f, 1.0f, 4.0f);
                }
            }
        }
    };

    // Live list of the ACTIVE scale's degrees: colour chip + interval label +
    // Sethares dissonance (green->yellow->red). Fixed row height so it can live in
    // a Viewport and scroll when a scale has more steps than fit. Rows carry their
    // sort keys (step, ratio, height) so the header can re-order them.
    struct DissDegreeList : juce::Component, juce::TooltipClient
    {
        struct Row { int step; double ratio, height; juce::String num, label, value;
                     juce::Colour chip, valueCol; bool isPeriod; };
        std::vector<Row> rows;
        juce::String tip;
        static constexpr int kRowH = 19;
        // Hovering a row SOUNDS that dyad (0 + this degree); only one sounds at a time.
        std::function<void (int)> onHover;
        int hoverRow = -1;
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int r0 = e.y / kRowH;
            const int r = juce::isPositiveAndBelow (r0, (int) rows.size()) ? r0 : -1;
            if (r != hoverRow) { hoverRow = r; if (onHover) onHover (r); repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override
        { if (hoverRow != -1) { hoverRow = -1; if (onHover) onHover (-1); repaint(); } }
        juce::String getTooltip() override { return tip; }
        int contentHeight() const { return (int) rows.size() * kRowH; }
        void paint (juce::Graphics& g) override
        {
            // Every row is the DYAD "0 + that degree": a red "0" chip (the root key,
            // drawn like the keyboard's period boundary) always precedes the degree
            // chip, so the pairing is explicit at a glance.
            const int rowH   = kRowH;
            const int W      = getWidth();
            const int chipW  = rowH - 2;
            const int labelX = 2 * chipW + 6, labelW = 50;
            const int valueX = labelX + labelW + 2;
            int y = 0;
            for (const auto& r : rows)
            {
                auto zeroR = juce::Rectangle<int> (0, y + 1, chipW, chipW);
                auto chipR = juce::Rectangle<int> (chipW + 2, y + 1, chipW, chipW);
                if (r.isPeriod)
                {
                    // The period: one red box covering the "0" AND the interval, white text.
                    g.setColour (juce::Colour (0xffe0453f));
                    g.fillRoundedRectangle (juce::Rectangle<int> (0, y + 1, valueX - 3, rowH - 2).toFloat(), 3.0f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions ((float) rowH * 0.6f).withStyle ("Bold")));
                    g.drawText (r.num, zeroR, juce::Justification::centred, false);
                    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                    g.drawText (r.label, labelX, y, labelW, rowH, juce::Justification::centredLeft, false);
                }
                else
                {
                    g.setColour (juce::Colour (0xffe0453f)); // red root chip "0"
                    g.fillRoundedRectangle (zeroR.toFloat(), 3.0f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions ((float) rowH * 0.6f).withStyle ("Bold")));
                    g.drawText ("0", zeroR, juce::Justification::centred, false);
                    g.setColour (r.chip);
                    g.fillRoundedRectangle (chipR.toFloat(), 3.0f);
                    g.setColour (r.chip.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.drawText (r.num, chipR, juce::Justification::centred, false);
                    g.setColour (juce::Colour (0xffb9c0d0));
                    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                    g.drawText (r.label, labelX, y, labelW, rowH, juce::Justification::centredLeft, false);
                }
                g.setColour (r.valueCol);
                g.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
                g.drawText (r.value, valueX, y, W - valueX, rowH, juce::Justification::centredLeft, false);
                y += rowH;
            }
        }
    };

    // n-AD list: rows of degree chips (keyboard hues) + total-beating value.
    // The ADAPTED SPECTRUM, one row per partial: the interval it landed on and the
    // amplitude the search gave it, as a bar. Read-only - it is a read-out of what was
    // written into the Custom Engine, and the pencil/amplitude window still owns editing.
    struct AdaptList : juce::Component, juce::TooltipClient
    {
        struct Row { juce::String name; float level = 1.0f; }; // level = amp / loudest amp
        std::vector<Row> rows;
        juce::String tip, emptyText;
        static constexpr int kRowH = 18;
        int contentHeight() const { return (int) rows.size() * kRowH + (emptyText.isNotEmpty() ? 2 * kRowH : 0); }
        juce::String getTooltip() override { return tip; }
        void paint (juce::Graphics& g) override
        {
            int y = 0, r = 0;
            for (const auto& row : rows)
            {
                if (r % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                g.setColour (juce::Colour (0xffd8dbe6));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                g.drawText (row.name, 4, y, 62, kRowH, juce::Justification::centredLeft, false);
                const int bx = 70, bw = juce::jmax (10, getWidth() - bx - 6);
                g.setColour (juce::Colour (0x22ffffff));
                g.fillRect (bx, y + 5, bw, kRowH - 10);
                g.setColour (juce::Colour (0xff8f7ad6));
                g.fillRect (bx, y + 5, (int) std::round ((float) bw * juce::jlimit (0.0f, 1.0f, row.level)),
                            kRowH - 10);
                y += kRowH; ++r;
            }
            if (emptyText.isNotEmpty())
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
                g.drawFittedText (emptyText, 4, y, getWidth() - 8, 2 * kRowH,
                                  juce::Justification::topLeft, 3);
            }
        }
    };

    struct NadList : juce::Component, juce::TooltipClient
    {
        struct Row { std::vector<int> degs; juce::String value; juce::Colour valCol; };
        std::vector<Row> rows;
        int degreesPerPeriod = 12;
        // Set when every degree of the tuning is one equal step from the last. Then a
        // shape found on degree 0 has the same intervals started from ANY degree, so
        // the row is shown as "any +10 +18" in neutral grey rather than as coloured
        // key numbers - the colours would claim specific keys the result is not tied to.
        bool equalStep = false;
        juce::String tip, emptyText; // status/coverage line (computing / over budget / partial)
        static constexpr int kRowH = 21;
        // Hovering a row SOUNDS that combination; only one sounds at a time.
        std::function<void (int)> onHover;
        int hoverRow = -1;
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int r0 = e.y / kRowH;
            const int r = juce::isPositiveAndBelow (r0, (int) rows.size()) ? r0 : -1;
            if (r != hoverRow) { hoverRow = r; if (onHover) onHover (r); repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override
        { if (hoverRow != -1) { hoverRow = -1; if (onHover) onHover (-1); repaint(); } }
        int contentHeight() const { return (int) rows.size() * kRowH + (emptyText.isNotEmpty() ? kRowH : 0); }
        juce::String getTooltip() override { return tip; }
        void paint (juce::Graphics& g) override
        {
            int y = 0, r = 0;
            const int D = juce::jmax (1, degreesPerPeriod);
            for (const auto& row : rows)
            {
                if (r % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                if (r == hoverRow) { g.setColour (juce::Colour (0x332fb155)); g.fillRect (0, y, getWidth(), kRowH); }
                int x = 4;   // no rank number: the beating value already orders them
                const juce::Font chipFont (juce::FontOptions (11.0f).withStyle ("Bold"));
                for (size_t k = 0; k < row.degs.size(); ++k)
                {
                    const int d = row.degs[k];
                    // Equal-step tuning: the shape is rootless, so the first chip says
                    // "any" and the rest are OFFSETS from it, all in neutral grey.
                    // Otherwise each chip is the actual degree, in its keyboard colour.
                    const bool grey = equalStep;
                    const juce::String txt = grey
                        ? (k == 0 ? juce::String ("any")
                                  : "+" + juce::String (d - row.degs[0]))
                        : juce::String (d);
                    const auto col = grey
                        ? juce::Colour (0xff6b6b78)
                        : juce::Colour::fromHSV (0.82f * (float) (d % D) / (float) D, 0.62f, 0.9f, 1.0f);
                    const int w = juce::jmax (18, (int) std::ceil (
                        juce::GlyphArrangement::getStringWidth (chipFont, txt)) + 8);
                    auto chip = juce::Rectangle<int> (x, y + 2, w, kRowH - 4);
                    g.setColour (col);
                    g.fillRoundedRectangle (chip.toFloat(), 3.0f);
                    g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.setFont (chipFont);
                    g.drawText (txt, chip, juce::Justification::centred, false);
                    x += w + 2;
                }
                g.setColour (row.valCol);
                g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
                g.drawText (row.value, x + 4, y, getWidth() - x - 6, kRowH, juce::Justification::centredLeft, false);
                y += kRowH; ++r;
            }
            if (emptyText.isNotEmpty()) // status line: computing / over budget / coverage
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
                g.drawText (emptyText, 4, y, getWidth() - 8, kRowH, juce::Justification::centredLeft, false);
            }
        }
    };

    // Two-column header over the dissonance list: "Step" and "Sethares dissonance"
    // (two lines). The list is always ordered by dissonance (most consonant first);
    // hovering explains that the dissonance tracks the synth's timbre live.
    struct DissHeader : juce::Component, juce::TooltipClient
    {
        juce::String tip;
        int  splitX = 92;
        juce::String getTooltip() override { return tip; }
        void paint (juce::Graphics& g) override
        {
            const juce::Colour c (0xff8f96a8);
            g.setColour (c);
            g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            g.drawText ("Step", juce::Rectangle<int> (0, 0, splitX, getHeight()),
                        juce::Justification::centredLeft, false);
            auto rr = juce::Rectangle<int> (splitX, 0, getWidth() - splitX, getHeight());
            g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            g.drawText ("beating", rr, juce::Justification::centredLeft, false);
        }
    };

    // Chord-search results: "#rank  [play]  note note note..." - one colour PER NOTE
    // INDEX; the play glyph previews (all together, then an ascending strum);
    // double-clicking a row loads the chord as the active .scl scale.
    struct ChordList : juce::Component, juce::TooltipClient
    {
        struct Row { std::vector<juce::String> notes; double value = 0.0; };
        std::vector<Row> rows;
        juce::String tip;
        juce::String emptyText { "Press Find to search.\nhover a row to hear it\ndouble-click loads it as the scale" };
        std::function<void (int)> onPlay, onLoad, onHover; // onHover(-1) = pointer left
        // No play column any more (hover sounds it), so the notes get that space.
        static constexpr int kRowH = 22, kRankW = 2, kGapW = 7;
        int contentHeight() const { return (int) rows.size() * kRowH; }
        static juce::Font rowFont() { return juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")); }
        // Columns are only as wide as the widest text they actually hold, so the
        // intervals start hard left and sit as close together as they can while the
        // columns still line up down the list. Nothing is ever squeezed or hidden:
        // if the widest row still overflows, the Viewport scrolls sideways.
        std::vector<int> columnWidths() const
        {
            size_t maxN = 0;
            for (const auto& r : rows) maxN = std::max (maxN, r.notes.size());
            std::vector<int> w (maxN, 0);
            const auto f = rowFont();
            for (const auto& r : rows)
                for (size_t k = 0; k < r.notes.size(); ++k)
                    w[k] = juce::jmax (w[k], (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, r.notes[k])));
            return w;
        }
        int contentWidth() const
        {
            int total = kRankW;
            for (const int w : columnWidths()) total += w + kGapW;
            return total + 2;
        }
        juce::String getTooltip() override { return tip; }
        static juce::Colour noteColour (int k)
        {
            static const juce::uint32 pal[] = { 0xffe0453f, 0xffffc95c, 0xff4fe09a, 0xff4fe6ff,
                                                0xff9a7bff, 0xffff77c8, 0xffff9a3c, 0xffc4ccda };
            return juce::Colour (pal[(size_t) (k % 8)]);
        }
        void paint (juce::Graphics& g) override
        {
            if (rows.empty())
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (12.5f)));
                g.drawFittedText (emptyText,
                                  getLocalBounds().reduced (8), juce::Justification::centred, 4, 0.8f);
                return;
            }
            const auto cols = columnWidths();
            int y = 0, row = 0;
            for (const auto& r : rows)
            {
                if (row % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                if (row == hoverRow) // the row you are hearing
                { g.setColour (juce::Colour (0x332fb155)); g.fillRect (0, y, getWidth(), kRowH); }
                // the chord's notes, one colour per note index, packed left with just
                // enough room for the widest entry of each column
                int x = kRankW;
                g.setFont (rowFont());
                for (int k = 0; k < (int) r.notes.size(); ++k)
                {
                    const int cw = k < (int) cols.size() ? cols[(size_t) k] : 0;
                    g.setColour (noteColour (k));
                    g.drawFittedText (r.notes[(size_t) k], juce::Rectangle<int> (x, y, cw, kRowH),
                                      juce::Justification::centredLeft, 1, 0.5f);
                    x += cw + kGapW;
                }
                y += kRowH; ++row;
            }
        }
        // HOVER plays it - there is no play button any more, and only one chord ever
        // sounds: moving to another row (or off the list) stops the previous one.
        int hoverRow = -1;
        void mouseMove (const juce::MouseEvent& e) override
        {
            const int row = e.y / kRowH;
            const int r = juce::isPositiveAndBelow (row, (int) rows.size()) ? row : -1;
            if (r != hoverRow) { hoverRow = r; if (onHover) onHover (r); repaint(); }
        }
        void mouseExit (const juce::MouseEvent&) override
        { if (hoverRow != -1) { hoverRow = -1; if (onHover) onHover (-1); repaint(); } }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int row = e.y / kRowH;
            if (juce::isPositiveAndBelow (row, (int) rows.size()) && onLoad) onLoad (row);
        }
    };

    // "From spectrum's segment" partial picker: one row per partial of the current
    // spectrum. Click toggles selection; Shift-click selects the span to the anchor;
    // all selected rows share one colour; the tonic (lowest selected = C3) is starred.
    struct SegmentList : juce::Component, juce::TooltipClient
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<void()> onChange;
        std::function<juce::String (double)> ratioName;
        juce::String tip;
        int numRows = 0;
        static constexpr int kRowH = 18;
        juce::String getTooltip() override { return tip; }
        int contentHeight() const { return numRows * kRowH; }
        void paint (juce::Graphics& g) override
        {
            if (proc == nullptr) return;
            const auto* t = proc->getLiveTemplate();
            const int n = t != nullptr ? (int) t->partials.size() : 0;
            const int tonic = proc->segmentTonicIndex();
            for (int i = 0; i < n; ++i)
            {
                const int y = i * kRowH;
                const bool sel = proc->segmentSelected (i);
                if (sel)
                {
                    g.setColour (juce::Colour (0xff2fb0c8)); // one shared colour for all selected
                    g.fillRoundedRectangle (juce::Rectangle<int> (0, y + 1, getWidth(), kRowH - 2).toFloat(), 3.0f);
                }
                const bool isTonic = (i == tonic);
                // Tonic (lowest selected = C3) stands out in gold; others white/grey.
                g.setColour (isTonic ? juce::Colour (0xffffc95c) : (sel ? juce::Colours::white : juce::Colour (0xffb9c0d0)));
                g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
                g.drawText ("P" + juce::String (i + 1) + (isTonic ? " C3" : juce::String()),
                            4, y, 64, kRowH, juce::Justification::centredLeft, false);
                if (ratioName)
                    g.drawText (ratioName ((double) t->partials[(size_t) i].ratio),
                                70, y, getWidth() - 72, kRowH, juce::Justification::centredLeft, false);
            }
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (proc == nullptr) return;
            const auto* t = proc->getLiveTemplate();
            const int n = t != nullptr ? (int) t->partials.size() : 0;
            const int i = e.y / kRowH;
            if (i >= 0 && i < n) { proc->segmentToggle (i, e.mods.isShiftDown()); if (onChange) onChange(); }
        }
    };

    // Closable floating notice (e.g. "only N minima exist"). Click X to dismiss.
    // Floating "Recent" window: every scale file loaded in this plugin, newest first,
    // scrollable, click one to load it.
    struct RecentOverlay : juce::Component
    {
        struct List : juce::Component
        {
            juce::StringArray paths;
            std::function<void (int)> onPick;
            static constexpr int kRowH = 22;
            int contentHeight() const { return juce::jmax (kRowH, paths.size() * kRowH); }
            void paint (juce::Graphics& g) override
            {
                int y = 0;
                for (int i = 0; i < paths.size(); ++i)
                {
                    if (i % 2 == 1) { g.setColour (juce::Colour (0x14ffffff)); g.fillRect (0, y, getWidth(), kRowH); }
                    if (i == hoverRow) { g.setColour (juce::Colour (0x33b27bff)); g.fillRect (0, y, getWidth(), kRowH); }
                    const juce::File f (paths[i]);
                    g.setColour (juce::Colour (0xff8f96a8));
                    g.setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Bold")));
                    g.drawText ("#" + juce::String (i + 1), 4, y, 26, kRowH, juce::Justification::centredLeft, false);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                    g.drawText (f.getFileNameWithoutExtension(), 32, y, getWidth() - 200, kRowH,
                                juce::Justification::centredLeft, false);
                    g.setColour (juce::Colour (0xff6c7488));
                    g.setFont (juce::Font (juce::FontOptions (11.0f)));
                    g.drawText (f.getParentDirectory().getFileName(), getWidth() - 166, y, 160, kRowH,
                                juce::Justification::centredRight, false);
                    y += kRowH;
                }
                if (paths.isEmpty())
                {
                    g.setColour (juce::Colour (0xff8f96a8));
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText ("nothing loaded yet", getLocalBounds(), juce::Justification::centred, false);
                }
            }
            int hoverRow = -1;
            void mouseMove (const juce::MouseEvent& e) override
            { const int r = e.y / kRowH; if (r != hoverRow) { hoverRow = r; repaint(); } }
            void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }
            void mouseDown (const juce::MouseEvent& e) override
            {
                const int r = e.y / kRowH;
                if (juce::isPositiveAndBelow (r, paths.size()) && onPick) onPick (r);
            }
        };

        juce::String title { "Recent" };
        juce::TextButton closeBtn { "X" };
        juce::Viewport view;
        List list;
        RecentOverlay()
        {
            closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            closeBtn.onClick = [this] { setVisible (false); };
            addAndMakeVisible (closeBtn);
            view.setViewedComponent (&list, false);
            view.setScrollBarsShown (true, false);
            addAndMakeVisible (view);
        }
        void setPaths (const juce::StringArray& p)
        {
            list.paths = p;
            list.setSize (juce::jmax (10, view.getWidth() - 10), list.contentHeight());
            list.repaint();
        }
        void resized() override
        {
            closeBtn.setBounds (getWidth() - 28, 6, 22, 22);
            auto r = getLocalBounds().reduced (14, 12);
            r.removeFromTop (24);
            view.setBounds (r);
            list.setSize (juce::jmax (10, r.getWidth() - 10), list.contentHeight());
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2141a24));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xffb27bff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            g.setColour (juce::Colour (0xffb27bff));
            g.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
            g.drawText (title, getLocalBounds().reduced (16, 10).removeFromTop (22),
                        juce::Justification::centredLeft, false);
        }
    };

    struct NoticeOverlay : juce::Component
    {
        juce::String title, body;
        juce::TextButton closeBtn { "X" };
        NoticeOverlay()
        {
            closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            closeBtn.onClick = [this] { setVisible (false); };
            addAndMakeVisible (closeBtn);
        }
        void resized() override { closeBtn.setBounds (getWidth() - 28, 6, 22, 22); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2141a24));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            auto in = getLocalBounds().reduced (22, 14);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.setFont (juce::Font (juce::FontOptions (17.0f).withStyle ("Bold")));
            g.drawText (title, in.removeFromTop (24), juce::Justification::centredLeft);
            in.removeFromTop (6);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (13.5f)));
            g.drawFittedText (body, in, juce::Justification::topLeft, 8, 1.0f);
        }
    };

    // Darkens everything behind the Spectrum-to-Rhythm panel so it does not
    // distract; clicking it (= clicking outside the panel) closes the panel.
    struct Scrim : juce::Component
    {
        std::function<void()> onClick;
        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xa8000008)); }
    };

    // Timbre -> rhythm (MIDI out) panel: pick partials of the CURRENT patch (shown
    // with their just-intonation ratio to the fundamental, or cents when inexact /
    // .scl-derived), route each to a MIDI note + gate length, and set the global
    // divisor x^n. The pulse trains leave on the plugin's MIDI OUTPUT in exact
    // polyrhythm with the sounding timbre (see RhythmEngine.h for the maths).
    struct RhythmPanel : juce::Component, juce::ListBoxModel
    {
        // The -12 / +12 steppers on every slot row. 30 px left 22 for the word, and
        // "+12" measures 26 in the font a 22 px button draws in, so both were squashed
        // on every row of the panel at once.
        static constexpr int kStepBtnW = 36;
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (double)> ratioName;                       // 3/2 or 702c
        std::function<juce::String (const char*, const char*)> trF;           // EN/CA
        std::function<void()> onEdited;                                       // refocus etc.
        std::function<void()> onClose;                                        // hide panel + scrim

        juce::ListBox      list { "partials", this };
        juce::ToggleButton onT, selfT, mpeT, tempoT; // onT = MIDI out; selfT = play in Fibló; tempoT = DAW-tempo sync (CC20)
        juce::ToggleButton droneT;                    // also sound the held note under the pulses
        juce::ToggleButton qTempoT, monoT, qPulseT;   // note-start gate: beat-quantize / mono / pattern-quantize
        juce::Label        secGate;                   // "NOTE START" group header
        juce::ToggleButton feedbackT;                 // internal recursive feedback loop
        juce::Slider       feedbackS;                 // its volume slope (decay per loop)
        juce::Slider       velSlopeS;                 // velocity tilt across the spectrum
        juce::Label        feedbackLbl;               // "Feedback" caption
        juce::Label        outLbl, selfLbl;           // captions under the two big pill toggles
        juce::ComboBox     modeBox;
        juce::Label        heading, listHead, info, noteHead, durHead, velHead, bpmLbl, pickHint;
        juce::Rectangle<int> rateBox;                 // framed box around the PULSE RATE group
        juce::Label        secRate, secOut, secTempo;         // coloured group headers
        juce::Label        divLbl, timesLbl, nVal, xVal;      // "divide [n] times by [x]"
        juce::TextButton   nUp, nDn, xUp, xDn;                // tiny round steppers
        juce::TextButton   closeB { "X" }, helpB, jiB, tempoX2, tempoD2;
        // "JI switched MPE out on, so JI off should switch it back" - a PARAMETER, not
        // an editor member: as a member it was lost on reopen and the restore never fired.
        bool jiForcedMpe() const
        {
            return proc != nullptr
                && proc->apvts.getRawParameterValue ("rhythmJiForcedMpe")->load() > 0.5f;
        }
        void setJiForcedMpe (bool b)
        {
            if (proc == nullptr) return;
            if (auto* p = proc->apvts.getParameter ("rhythmJiForcedMpe"))
                p->setValueNotifyingHost (b ? 1.0f : 0.0f);
        }
        juce::String       helpText;                          // shown by the Help button
        // Slot rows live in a scrollable container: kSlots is large ("unlimited"
        // pulses), so the panel shows a window onto however many are routed.
        juce::Viewport     slotView;
        juce::Component    slotRows;
        juce::Label        slotLbl[RhythmEngine::kSlots];
        juce::TextEditor   pitchE[RhythmEngine::kSlots];
        juce::TextButton   s12M[RhythmEngine::kSlots], s12P[RhythmEngine::kSlots]; // -12/+12 (steps only)
        juce::Slider       durS[RhythmEngine::kSlots];
        juce::Slider       velS[RhythmEngine::kSlots]; // per-slot MIDI velocity of each pulse
        juce::TextButton   delB[RhythmEngine::kSlots];
        juce::Label        octLbl, octAllVal;          // "Octave (all pulses)" stepper
        juce::Label        stepsAllLbl;                // "Steps (all pulses)" caption
        juce::Slider       stepsAllS;                  // global scale-step offset (modulatable)
        juce::TextButton   octAllUp, octAllDn;
        int numRows = 0;

        // Clicking any empty spot of the panel gives keyboard focus back to the
        // editor (text boxes commit + collapse; the play keys work again).
        void mouseDown (const juce::MouseEvent&) override { if (onEdited) onEdited(); }

        // Scale steps FROM the played note: "7step" = 7 steps up, "7step-" (or
        // "-7step") = 7 down. Accepts "step"/"steps"/"st". INT_MIN if not a steps spec.
        static int parseSteps (juce::String t)
        {
            t = t.trim().toLowerCase().removeCharacters (" ");
            bool down = false;
            if (t.endsWith ("-"))   { down = true; t = t.dropLastCharacters (1); }
            if (t.startsWith ("-")) { down = true; t = t.substring (1); }
            if      (t.endsWith ("steps")) t = t.dropLastCharacters (5);
            else if (t.endsWith ("step"))  t = t.dropLastCharacters (4);
            else if (t.endsWith ("st"))    t = t.dropLastCharacters (2);
            else return std::numeric_limits<int>::min();
            if (t.isEmpty() || ! t.containsOnly ("0123456789")) return std::numeric_limits<int>::min();
            const int v = juce::jlimit (0, 127, t.getIntValue());
            return down ? -v : v;
        }
        static juce::String stepsText (int steps)
        {
            return steps < 0 ? juce::String (-steps) + "step-" : juce::String (steps) + "step";
        }

        // Best small fraction p/q ~ v (continued fractions, den <= 16) for the
        // integer polyrhythm chain display.
        static void approxFraction (double v, long long& p, long long& q)
        {
            long long bestP = 1, bestQ = 1; double bestErr = 1.0e18;
            for (long long d = 1; d <= 16; ++d)
            {
                const long long n = (long long) std::llround (v * (double) d);
                if (n <= 0) continue;
                const double err = std::abs (v - (double) n / (double) d);
                if (err < bestErr - 1.0e-12) { bestErr = err; bestP = n; bestQ = d; }
            }
            p = bestP; q = bestQ;
        }

        // MPE pitch spec: "1987.4c" (cents) or a ratio "3" / "3/2" -> linear ratio.
        static double parseRatioSpec (juce::String t)
        {
            t = t.trim();
            // Accept BOTH "700c" and the "700\xc2\xa2" the rest of the UI prints: without
            // the cent sign a pasted-back value was read as a linear RATIO of 700.
            if (t.endsWith (juce::String::fromUTF8 ("\xc2\xa2")))
                t = t.dropLastCharacters (1).trim() + "c";
            if (t.endsWithIgnoreCase ("c"))
                return std::exp2 (t.dropLastCharacters (1).getDoubleValue() / 1200.0);
            if (t.containsChar ('/'))
            {
                const double p = t.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
                const double q = t.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
                return (p > 0.0 && q > 0.0) ? p / q : 0.0;
            }
            const double v = t.getDoubleValue();
            return v > 0.0 ? v : 0.0;
        }

        void init()
        {
            heading.setJustificationType (juce::Justification::centredLeft);
            heading.setColour (juce::Label::textColourId, juce::Colour (0xff5cb8ff));
            heading.setFont (juce::Font (juce::FontOptions (21.0f).withStyle ("Bold")));
            addAndMakeVisible (heading);
            listHead.setJustificationType (juce::Justification::centredLeft);
            listHead.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            listHead.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            addAndMakeVisible (listHead);
            // Points at the partial list when On is on but nothing is routed yet.
            pickHint.setJustificationType (juce::Justification::centredLeft);
            pickHint.setColour (juce::Label::textColourId, juce::Colour (0xff2fb155)); // green (matches ON)
            pickHint.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            addChildComponent (pickHint);
            list.setRowHeight (20);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff141420));
            addAndMakeVisible (list);
            onT.setName ("onled"); // big green(on)/red(off) pill, drawn by GalacticLnF
            addAndMakeVisible (onT);
            selfT.setName ("onled"); // second big pill: "play in Fibló" (interpret internally)
            addAndMakeVisible (selfT);
            // Captions under the two big pill toggles.
            for (auto* l : { &outLbl, &selfLbl })
            {
                l->setJustificationType (juce::Justification::centred);
                l->setColour (juce::Label::textColourId, juce::Colour (0xffb9c0d0));
                l->setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
                addAndMakeVisible (*l);
            }
            droneT.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffb9c0d0));
            addAndMakeVisible (droneT);
            // Internal feedback loop + its volume slope (only meaningful with self-play).
            feedbackT.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffb9c0d0));
            addAndMakeVisible (feedbackT);
            feedbackLbl.setJustificationType (juce::Justification::centredRight);
            feedbackLbl.setColour (juce::Label::textColourId, juce::Colour (0xffb9c0d0));
            feedbackLbl.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            addAndMakeVisible (feedbackLbl);
            feedbackS.setSliderStyle (juce::Slider::LinearHorizontal);
            feedbackS.setRange (0.05, 0.95, 0.01);
            feedbackS.setDoubleClickReturnValue (true, 0.5); // double-click = default
            feedbackS.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 16);
            feedbackS.textFromValueFunction = [] (double v) { return juce::String ((int) std::lround (v * 100.0)) + " %"; };
            feedbackS.setColour (juce::Slider::trackColourId, juce::Colour (0xffff77c8));
            addAndMakeVisible (feedbackS);
            addAndMakeVisible (mpeT);
            modeBox.addItem ("Lowest note", 1); // idx-mapped to the rhythmMode parameter
            modeBox.addItem ("All notes",   2);
            addAndMakeVisible (modeBox);

            // Coloured group headers so each zone reads at a glance.
            auto sec = [this] (juce::Label& l, juce::Colour c)
            {
                l.setJustificationType (juce::Justification::centredLeft);
                l.setColour (juce::Label::textColourId, c);
                l.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                addAndMakeVisible (l);
            };
            sec (secRate,  juce::Colour (0xff4fe6ff)); // cyan   - PULSE RATE
            sec (secOut,   juce::Colour (0xffff77c8)); // pink   - OUTPUT
            sec (secTempo, juce::Colour (0xffffc95c)); // yellow - DAW TEMPO
            sec (secGate,  juce::Colour (0xff7ff59a)); // green  - NOTE START (quantize/mono)
            for (auto* t : { &qTempoT, &monoT, &qPulseT })
            {
                t->setColour (juce::ToggleButton::textColourId, juce::Colour (0xffb9c0d0));
                addAndMakeVisible (*t);
            }

            // "divide [n] times by [x]": two tiny +/- steppers driving the params.
            for (auto* l : { &divLbl, &timesLbl })
            {
                l->setColour (juce::Label::textColourId, juce::Colour (0xffb9c0d0));
                l->setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                addAndMakeVisible (*l);
            }
            divLbl.setJustificationType (juce::Justification::centredLeft);  // flush to the left border
            timesLbl.setJustificationType (juce::Justification::centredRight);
            for (auto* v : { &nVal, &xVal })
            {
                v->setJustificationType (juce::Justification::centred);
                v->setColour (juce::Label::textColourId, juce::Colours::white);
                v->setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
                addAndMakeVisible (*v);
            }
            auto miniStep = [this] (juce::TextButton& b, const char* txt, const char* id, int delta)
            {
                b.setButtonText (txt);
                b.setName ("round");
                b.setColour (juce::TextButton::buttonColourId,
                             juce::Colour (txt[0] == '+' ? 0xff2fb155 : 0xffe0453f).withAlpha (0.85f));
                b.onClick = [this, id, delta]
                {
                    if (proc == nullptr) return;
                    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc->apvts.getParameter (id)))
                    {
                        const float cur = proc->apvts.getRawParameterValue (id)->load();
                        p->setValueNotifyingHost (p->convertTo0to1 (cur + (float) delta));
                    }
                    refreshLive();
                };
                addAndMakeVisible (b);
            };
            miniStep (nUp, "+", "rhythmDivN",  1);
            miniStep (nDn, "-", "rhythmDivN", -1);
            miniStep (xUp, "+", "rhythmDivX",  1);
            miniStep (xDn, "-", "rhythmDivX", -1);

            helpB.setButtonText ("How to route");
            helpB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffffc95c));
            helpB.onClick = [this]
            {
                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                    "Spectrum vs Rhythm", helpText, this);
            };
            addAndMakeVisible (helpB);

            // JI pitches TOGGLE: when ON, every routed pulse (present and future) plays
            // the JUST ratio of ITS OWN partial (P3 -> 3/1...), sent retuned via MPE, so
            // the receiver plays the partials' exact pitches. Green when on, red when off.
            // Turning it on auto-enables MPE out (JI needs per-note bend); off reverts
            // the slots to scale steps.
            jiB.setClickingTogglesState (true);
            jiB.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xffe0453f)); // off = red
            jiB.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2fb155)); // on  = green
            jiB.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
            jiB.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
            jiB.onClick = [this]
            {
                if (proc == nullptr) return;
                const bool on = jiB.getToggleState();
                if (on)
                {
                    // JI needs per-note bend -> MPE out goes on with it. Remember
                    // whether WE were the ones who switched it on, so turning JI back
                    // off leaves the panel exactly as it found it. Without this the
                    // toggle was not an involution: MPE stayed on for ever after, and
                    // its channel spreading kept detuning notes that shared a channel.
                    setJiForcedMpe (! mpeOn());
                    if (jiForcedMpe())
                        if (auto* p = proc->apvts.getParameter ("rhythmMpe"))
                            p->setValueNotifyingHost (1.0f);
                }
                else if (jiForcedMpe())
                {
                    setJiForcedMpe (false);
                    if (auto* p = proc->apvts.getParameter ("rhythmMpe"))
                        p->setValueNotifyingHost (0.0f);
                }
                if (auto* p = proc->apvts.getParameter ("rhythmJi")) // persist the switch itself
                    p->setValueNotifyingHost (on ? 1.0f : 0.0f);
                applyJiToSlots();
                updateJiText();
                refreshSlots();
            };
            addAndMakeVisible (jiB);

            // DAW-tempo sync: while the TOGGLE is on, the editor timer keeps sending
            // CC20 whenever the target BPM changes (see the editor's timerCallback) -
            // a MIDI-mapped host tempo then FOLLOWS pulse 1 continuously.
            bpmLbl.setJustificationType (juce::Justification::centredLeft);
            bpmLbl.setColour (juce::Label::textColourId, juce::Colour (0xffffc95c));
            bpmLbl.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
            addAndMakeVisible (bpmLbl);
            addAndMakeVisible (tempoT);
            for (auto* b : { &tempoX2, &tempoD2 })
            {
                b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
                addAndMakeVisible (*b);
            }
            tempoX2.setButtonText ("x2");
            tempoD2.setButtonText ("/2");
            // The octave scaling is a PARAMETER now (the processor drives the tempo,
            // so it has to be reachable from there - and it survives a reload).
            tempoX2.onClick = [this] { nudgeTempoOct (+1); };
            tempoD2.onClick = [this] { nudgeTempoOct (-1); };
            info.setJustificationType (juce::Justification::topLeft);
            info.setColour (juce::Label::textColourId, juce::Colour (0xff7ff59a));
            info.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            addAndMakeVisible (info);
            for (auto* h : { &noteHead, &durHead, &velHead })
            {
                h->setJustificationType (juce::Justification::centredLeft);
                h->setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
                h->setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                addAndMakeVisible (*h);
            }
            // Velocity SLOPE: a tilt over the whole column of per-pulse velocities.
            // 0 = flat; up = the high partials fade out first, down = the low ones;
            // at the top of its travel the ramp is vertical and nothing is left.
            velSlopeS.setSliderStyle (juce::Slider::LinearHorizontal);
            velSlopeS.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            velSlopeS.setColour (juce::Slider::trackColourId, juce::Colour (0xffff77c8));
            velSlopeS.setColour (juce::Slider::thumbColourId, juce::Colour (0xffff77c8));
            velSlopeS.setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (velSlopeS);
            // Global STEPS offset for step-pitched pulses (ratio/JI slots ignore it).
            stepsAllS.setSliderStyle (juce::Slider::LinearHorizontal);
            stepsAllS.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 15);
            stepsAllS.setColour (juce::Slider::trackColourId, juce::Colour (0xff4fe6ff));
            stepsAllS.setColour (juce::Slider::thumbColourId, juce::Colour (0xff4fe6ff));
            stepsAllS.setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (stepsAllS);
            stepsAllLbl.setJustificationType (juce::Justification::centredRight);
            stepsAllLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            stepsAllLbl.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            addAndMakeVisible (stepsAllLbl);

            closeB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeB.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (closeB);

            // Scrollable slot-rows container ("unlimited" pulses).
            slotView.setViewedComponent (&slotRows, false);
            slotView.setScrollBarsShown (true, false);
            addAndMakeVisible (slotView);

            // Global octave stepper: shifts ALL pulses together (+-4 octaves).
            octLbl.setJustificationType (juce::Justification::centredRight);
            octLbl.setColour (juce::Label::textColourId, juce::Colour (0xffb9c0d0));
            octLbl.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            addAndMakeVisible (octLbl);
            octAllVal.setJustificationType (juce::Justification::centred);
            octAllVal.setColour (juce::Label::textColourId, juce::Colours::white);
            octAllVal.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            addAndMakeVisible (octAllVal);
            miniStep (octAllUp, "+", "rhythmOctAll",  1);
            miniStep (octAllDn, "-", "rhythmOctAll", -1);

            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                slotLbl[i].setJustificationType (juce::Justification::centredLeft);
                slotLbl[i].setColour (juce::Label::textColourId, juce::Colours::white);
                slotLbl[i].setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                slotRows.addChildComponent (slotLbl[i]);

                pitchE[i].setJustification (juce::Justification::centred);
                pitchE[i].setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
                pitchE[i].setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff5cb8ff).withAlpha (0.6f));
                pitchE[i].setColour (juce::TextEditor::textColourId, juce::Colours::white);
                pitchE[i].setSelectAllWhenFocused (true);
                pitchE[i].onReturnKey = [this, i] { commitPitch (i); if (onEdited) onEdited(); };
                pitchE[i].onFocusLost = [this, i] { commitPitch (i); };
                slotRows.addChildComponent (pitchE[i]);

                // -12 / +12 step an OCTAVE of scale steps; they only apply to steps
                // values (hidden when the slot holds a ratio/cents pitch).
                for (auto* b : { &s12M[i], &s12P[i] })
                {
                    b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
                    slotRows.addChildComponent (*b);
                }
                s12M[i].setButtonText ("-12");
                s12P[i].setButtonText ("+12");
                auto step12 = [this, i] (int delta)
                {
                    if (proc == nullptr) return;
                    auto& s = proc->getRhythm().slots[i];
                    if (s.useRatio.load()) return; // steps only
                    s.steps = juce::jlimit (-127, 127, s.steps.load() + delta);
                    refreshSlots();
                };
                s12M[i].onClick = [step12] { step12 (-12); };
                s12P[i].onClick = [step12] { step12 ( 12); };

                durS[i].setSliderStyle (juce::Slider::LinearHorizontal);
                durS[i].setRange (5.0, 500.0, 1.0);
                durS[i].setDoubleClickReturnValue (true, 60.0); // double-click = default
                durS[i].setSkewFactorFromMidPoint (80.0);
                durS[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 16);
                durS[i].textFromValueFunction = [] (double v) { return juce::String ((int) v) + " ms"; };
                durS[i].onValueChange = [this, i]
                { if (proc) proc->getRhythm().slots[i].durMs = (float) durS[i].getValue(); };
                slotRows.addChildComponent (durS[i]);

                // Per-slot velocity: the MIDI velocity of every pulse this train sends.
                velS[i].setSliderStyle (juce::Slider::LinearHorizontal);
                velS[i].setRange (1.0, 127.0, 1.0);
                velS[i].setDoubleClickReturnValue (true, 100.0); // double-click = default
                velS[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 32, 16);
                velS[i].setColour (juce::Slider::trackColourId, juce::Colour (0xffff77c8)); // pink = output
                velS[i].onValueChange = [this, i]
                { if (proc) proc->getRhythm().slots[i].vel = (int) velS[i].getValue(); };
                slotRows.addChildComponent (velS[i]);

                delB[i].setButtonText ("X");
                delB[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
                delB[i].onClick = [this, i]
                {
                    auto v = readSlots();
                    if (i < (int) v.size()) v.erase (v.begin() + i);
                    writeSlots (std::move (v)); // stays compact, no gaps
                    refreshSlots();
                    list.repaint();
                };
                slotRows.addChildComponent (delB[i]);
            }
        }

        //-- ListBoxModel: the current template's partials --------------------------
        int getNumRows() override { return numRows; }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            if (t == nullptr || row >= (int) t->partials.size()) return;
            const bool assigned = slotOfPartial (row) >= 0;
            if (assigned) { g.setColour (juce::Colour (0xff2a4a6a)); g.fillRect (0, 0, w, h); }
            g.setColour (juce::Colour (0xff8f96a8));
            g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            g.drawText ("P" + juce::String (row + 1), 6, 0, 40, h, juce::Justification::centredLeft);
            g.setColour (assigned ? juce::Colours::white : juce::Colour (0xffb9c0d0));
            g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
            g.drawText (ratioName ? ratioName ((double) t->partials[(size_t) row].ratio) : juce::String(),
                        50, 0, w - 56, h, juce::Justification::centredLeft);
        }
        // The routed trains are kept COMPACT and in LIST ORDER (sorted by partial
        // index): no empty rows between them, no reordering surprises when a
        // partial is added or removed.
        struct SlotData { int partial, steps; bool useRatio; double ratio; float durMs; int vel; };
        std::vector<SlotData> readSlots() const
        {
            std::vector<SlotData> v;
            if (proc == nullptr) return v;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const auto& s = proc->getRhythm().slots[i];
                if (s.partial.load() >= 0)
                    v.push_back ({ s.partial.load(), s.steps.load(), s.useRatio.load(),
                                   s.ratio.load(), s.durMs.load(), s.vel.load() });
            }
            return v;
        }
        void writeSlots (std::vector<SlotData> v)
        {
            if (proc == nullptr) return;
            std::sort (v.begin(), v.end(), [] (const SlotData& a, const SlotData& b) { return a.partial < b.partial; });
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto& s = proc->getRhythm().slots[i];
                if (i < (int) v.size())
                {
                    s.steps = v[(size_t) i].steps;  s.useRatio = v[(size_t) i].useRatio;
                    s.ratio = v[(size_t) i].ratio;  s.durMs    = v[(size_t) i].durMs;
                    s.vel   = v[(size_t) i].vel;
                    s.partial = v[(size_t) i].partial;
                    s.enabled = true;
                }
                else
                {
                    s.enabled = false; s.partial = -1;
                    s.steps = 0; s.useRatio = false; s.ratio = 1.0; s.durMs = 60.0f; s.vel = 100;
                }
            }
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override
        {
            if (proc == nullptr) return;
            auto v = readSlots();
            const auto it = std::find_if (v.begin(), v.end(), [row] (const SlotData& d) { return d.partial == row; });
            if (it != v.end())
                v.erase (it);                            // click an assigned partial -> unassign
            else if ((int) v.size() < RhythmEngine::kSlots)
            {
                v.push_back ({ row, 0, false, 1.0, 60.0f, 100 }); // assign (writeSlots sorts + compacts)
                // Routing a partial means the user WANTS pulses -> make sure SOME output
                // is on. Self-play is the default sink (plays here); don't force MIDI-out.
                const bool anyOut = proc->apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f
                                 || proc->apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f;
                if (! anyOut)
                    if (auto* p = proc->apvts.getParameter ("rhythmSelfPlay"))
                        p->setValueNotifyingHost (1.0f);
            }
            writeSlots (std::move (v));
            // Only the NEWLY routed partial adopts JI - existing slots keep any
            // hand-edited ratio (routing more partials must never reset them).
            if (jiB.getToggleState()) applyJiToSlots (row);
            refreshSlots();
            list.repaint();
        }
        int slotOfPartial (int p) const
        {
            if (proc == nullptr) return -1;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
                if (proc->getRhythm().slots[i].partial.load() == p) return i;
            return -1;
        }

        //-- Pitch spec of a slot ---------------------------------------------------
        // Plain: a note name ("A4"). MPE out on: also cents ("1987.4c") or ratios
        // ("3", "3/2") -> the exact pitch f0*ratio is sent via per-note bend.
        bool mpeOn() const
        {
            return proc != nullptr && proc->apvts.getRawParameterValue ("rhythmMpe")->load() > 0.5f;
        }
        // JI toggle: ON -> routed slots use the JUST ratio of their own partial;
        // OFF -> slots revert to scale steps. `onlyPartial >= 0` applies it to that
        // newly-routed partial ONLY, so routing another partial never resets a ratio
        // the user has hand-edited on an existing slot.
        void applyJiToSlots (int onlyPartial = -1)
        {
            if (proc == nullptr) return;
            const bool on = jiB.getToggleState();
            const auto* t = proc->getLiveTemplate();
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto& s = proc->getRhythm().slots[i];
                const int p = s.partial.load();
                if (p < 0 || (onlyPartial >= 0 && p != onlyPartial)) continue;
                if (on && t != nullptr && p < (int) t->partials.size())
                { s.ratio = (double) t->partials[(size_t) p].ratio; s.useRatio = true; }
                else if (! on)
                    s.useRatio = false;
            }
        }
        void updateJiText() { jiB.setButtonText (jiB.getToggleState() ? "JI: ON" : "JI: OFF"); }
        void commitPitch (int i)
        {
            if (proc == nullptr) return;
            auto& s = proc->getRhythm().slots[i];
            const auto t = pitchE[i].getText();
            if (const int st = parseSteps (t); st != std::numeric_limits<int>::min())
            { s.steps = st; s.useRatio = false; }
            else if (const double r = parseRatioSpec (t); r > 0.0)
            {
                if (mpeOn())
                { s.ratio = r; s.useRatio = true; }
                else
                {
                    // Ratios/cents need per-note pitch bend -> MPE out must be on.
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "MPE out required",
                        trF ? trF ("Ratio and cents pitches (\"3/2\", \"702c\") need MPE out enabled - they are sent retuned with per-note pitch bend. Enable \"MPE out\" or type scale steps (\"7st\").",
                                   "Els tons en ratio o cents (\"3/2\", \"702c\") necessiten MPE out actiu - s'envien reafinats amb pitch bend per nota. Activa \"MPE out\" o escriu passos d'escala (\"7st\").")
                            : juce::String ("Enable MPE out to use ratio/cents pitches."), this);
                }
            }
            refreshSlots(); // reverts the shown text to the stored value when rejected
        }
        int tempoOctValue() const
        {
            return proc != nullptr
                 ? (int) proc->apvts.getRawParameterValue ("rhythmTempoOct")->load() : 0;
        }
        void nudgeTempoOct (int d)
        {
            if (proc == nullptr) return;
            if (auto* p = proc->apvts.getParameter ("rhythmTempoOct"))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (-6, 6, tempoOctValue() + d)));
            refreshLive();
        }
        double tempoBpm() const // pulse 1 (first enabled slot) in BPM, times 2^octave
        {
            if (proc == nullptr) return 0.0;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
                if (proc->getRhythm().slots[i].enabled.load())
                {
                    const double r = (double) proc->getRhythm().getRateHz (i);
                    if (r > 0.0) return r * 60.0 * std::exp2 ((double) tempoOctValue());
                }
            return 0.0;
        }

        //-- Refresh ----------------------------------------------------------------
        void refreshSlots()
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const int p = proc != nullptr ? proc->getRhythm().slots[i].partial.load() : -1;
                const bool used = p >= 0;
                const bool stepsPitch = used && ! proc->getRhythm().slots[i].useRatio.load();
                slotLbl[i].setVisible (used); pitchE[i].setVisible (used);
                durS[i].setVisible (used);    delB[i].setVisible (used);
                // +-12 steps only make sense for steps pitches - hidden for ratio/cents.
                s12M[i].setVisible (stepsPitch); s12P[i].setVisible (stepsPitch);
                velS[i].setVisible (used);
                if (used)
                {
                    auto& s = proc->getRhythm().slots[i];
                    juce::String rl;
                    // A routed partial that the CURRENT spectrum no longer has cannot
                    // pulse: say so on the row instead of leaving it looking active.
                    const bool missing = t != nullptr && p >= (int) t->partials.size();
                    if (t != nullptr && ! missing && ratioName)
                        rl = juce::String::fromUTF8 (" \xc2\xb7 ") + ratioName ((double) t->partials[(size_t) p].ratio);
                    else if (missing)
                        // The row is 96 px wide. " - not in this spectrum" measured 170
                        // in the font it is drawn in, so it was cut off mid-word - the
                        // user's own example - AND it was raw English in a bilingual
                        // product. One red word says the same thing and fits; the
                        // sentence is in the row's hover.
                        rl = trF ? " - " + trF ("missing", "no hi \xc3\xa9s") : juce::String (" - missing");
                    slotLbl[i].setColour (juce::Label::textColourId,
                                          missing ? juce::Colour (0xffe0453f) : juce::Colour (0xffc4ccda));
                    slotLbl[i].setText ("P" + juce::String (p + 1) + rl, juce::dontSendNotification);
                    slotLbl[i].setTooltip (missing && trF
                        ? trF ("This train is routed to a partial the CURRENT spectrum does not have, so it "
                               "cannot pulse. Route it to a partial that exists, or load the spectrum it came from.",
                               "Aquest tren est\xc3\xa0 encaminat a un parcial que l'espectre ACTUAL no t\xc3\xa9, "
                               "aix\xc3\xad que no pot polsar. Encamina'l a un parcial que existeixi, o carrega "
                               "l'espectre d'on venia.")
                        : juce::String());
                    if (! pitchE[i].hasKeyboardFocus (false))
                        pitchE[i].setText (s.useRatio.load() && ratioName
                                               ? ratioName (s.ratio.load())
                                               : stepsText (s.steps.load()),
                                           juce::dontSendNotification);
                    durS[i].setValue ((double) s.durMs.load(), juce::dontSendNotification);
                    velS[i].setValue ((double) s.vel.load(), juce::dontSendNotification);
                }
            }
            // Guide the user the moment they switch On with nothing routed yet.
            const bool on = proc != nullptr
                && (proc->apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f
                 || proc->apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f);
            const bool none = readSlots().empty();
            pickHint.setVisible (on && none);
            layoutSlotRows(); // the used-row count may have changed
        }

        void refreshLive() // pulse rates + polyrhythm chain, from the audio thread's read-outs
        {
            if (proc == nullptr) return;
            nVal.setText (juce::String ((int) proc->apvts.getRawParameterValue ("rhythmDivN")->load()),
                          juce::dontSendNotification);
            xVal.setText (juce::String ((int) proc->apvts.getRawParameterValue ("rhythmDivX")->load()),
                          juce::dontSendNotification);
            octAllVal.setText (juce::String ((int) proc->apvts.getRawParameterValue ("rhythmOctAll")->load()),
                               juce::dontSendNotification);
            juce::String s;
            const float f0 = proc->getRhythm().getDriverF0();
            s << (f0 > 0.0f ? "f0 = " + juce::String (f0, 2) + " Hz\n"
                            : juce::String (trF ? trF ("(hold a note to pulse)", "(mant\xc3\xa9n una nota per polsar)") : ""))
              ;
            float rates[RhythmEngine::kSlots]; int idx[RhythmEngine::kSlots]; int m = 0;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const float r = proc->getRhythm().getRateHz (i);
                if (r > 0.0f) { rates[m] = r; idx[m] = i; ++m; }
            }
            for (int k = 0; k < m && k < 4; ++k) // cap the listing; the polyrhythm line summarises the rest
                s << "P" << juce::String (proc->getRhythm().slots[idx[k]].partial.load() + 1)
                  << ": " << juce::String (rates[k], 3) << " Hz ("
                  << juce::String (1000.0f / rates[k], 0) << " ms)\n";
            if (m > 4)
                s << "+" << juce::String (m - 4) << (trF ? trF (" more trains\n", " trens m\xc3\xa9s\n") : juce::String ("\n"));
            if (m >= 2)
            {
                // ONE integer chain "1:2:3" over all active trains (not pairwise):
                // rationalise every rate against the slowest, then scale to the
                // least common denominator and reduce.
                double base = rates[0];
                for (int k = 1; k < m; ++k) base = juce::jmin (base, (double) rates[k]);
                long long num[RhythmEngine::kSlots], den[RhythmEngine::kSlots], L = 1;
                bool ok = base > 0.0;
                if (ok)
                    for (int k = 0; k < m; ++k)
                    {
                        approxFraction ((double) rates[k] / base, num[k], den[k]);
                        if (den[k] <= 0) { ok = false; break; }
                        L = std::lcm (L, den[k]);
                    }
                if (ok)
                {
                    long long chain[RhythmEngine::kSlots], g = 0;
                    for (int k = 0; k < m; ++k) { chain[k] = num[k] * (L / den[k]); g = std::gcd (g, chain[k]); }
                    std::sort (chain, chain + m);
                    s << (trF ? trF ("polyrhythm ", "poliritme ") : juce::String ("polyrhythm "));
                    for (int k = 0; k < m; ++k)
                        s << juce::String (g > 0 ? chain[k] / g : chain[k]) << (k + 1 < m ? ":" : "");
                }
            }
            info.setText (s, juce::dontSendNotification);

            // Guide the user the moment they switch On with nothing routed yet
            // (updated here, every tick, so it reacts to the On toggle too).
            const bool onNow = proc->apvts.getRawParameterValue ("rhythmOn")->load()       > 0.5f
                            || proc->apvts.getRawParameterValue ("rhythmSelfPlay")->load() > 0.5f;
            pickHint.setVisible (onNow && readSlots().empty());

            const double bpm = tempoBpm();
            bpmLbl.setText (bpm > 0.0 ? "= " + juce::String (bpm, 1) + " BPM"
                                      : juce::String ("= ... BPM"),
                            juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14, 10);
            auto top = r.removeFromTop (30);
            closeB.setBounds (top.removeFromRight (24).withSizeKeepingCentre (22, 22));
            top.removeFromRight (5);
            {   // "How to route" / "Com encaminar" - the Catalan is 6 px longer.
                const int hw = juce::jmax (104, fgfit::textButtonWidth (helpB.getButtonText(), 22, false));
                helpB.setBounds (top.removeFromRight (hw).withSizeKeepingCentre (hw, 22));
            }
            heading.setBounds (top); // big title, no toggle inside it
            r.removeFromTop (6);

            // Two big pill toggles, each with a caption: MIDI OUT (send to DAW) and
            // PLAY IN FIBLÓ (interpret the rhythm here; the held note stays silent).
            {
                // Widths from the CAPTIONS, not from numbers: "MIDI out (to DAW)" is
                // "Sortida MIDI (al DAW)" in Catalan and needed 23 px more than the
                // column it was given, so it shipped condensed.
                auto tg = r.removeFromTop (46);
                auto p1 = tg.removeFromLeft (juce::jmax (118, fgfit::labelWidth (outLbl)));
                onT.setBounds (p1.removeFromTop (26).withSizeKeepingCentre (84, 24));
                outLbl.setBounds (p1);
                tg.removeFromLeft (14);
                auto p2 = tg.removeFromLeft (juce::jmax (140, fgfit::labelWidth (selfLbl)));
                selfT.setBounds (p2.removeFromTop (26).withSizeKeepingCentre (96, 24));
                selfLbl.setBounds (p2);
                tg.removeFromLeft (16);
                droneT.setBounds (tg.withSizeKeepingCentre (tg.getWidth(), 24)); // "note under pulses"
            }
            r.removeFromTop (6);
            // Internal feedback loop + its volume slope.
            {
                auto fr = r.removeFromTop (24);
                feedbackT.setBounds (fr.removeFromLeft (160));
                fr.removeFromLeft (8);
                feedbackLbl.setBounds (fr.removeFromLeft (juce::jmax (54, fgfit::labelWidth (feedbackLbl))));
                fr.removeFromLeft (6);
                feedbackS.setBounds (fr.removeFromLeft (juce::jmin (240, fr.getWidth())).reduced (0, 1));
            }
            r.removeFromTop (8);

            // PULSE RATE group, wrapped in its own framed box (header + divide row).
            {
                auto box = r.removeFromTop (13 + 3 + 24 + 8);
                rateBox = box; // stored for paint()'s frame
                auto ra = box.reduced (8, 4);
                secRate.setBounds (ra.removeFromTop (13));
                ra.removeFromTop (3);
                auto g1 = ra.removeFromTop (24);
                divLbl.setBounds (g1.removeFromLeft (juce::jmax (50, fgfit::labelWidth (divLbl))));
                g1.removeFromLeft (4);
                nDn.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                nVal.setBounds (g1.removeFromLeft (26));
                nUp.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                timesLbl.setBounds (g1.removeFromLeft (juce::jmax (66, fgfit::labelWidth (timesLbl))));
                g1.removeFromLeft (4);
                xDn.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                xVal.setBounds (g1.removeFromLeft (26));
                xUp.setBounds (g1.removeFromLeft (20).reduced (0, 2));
            }
            r.removeFromTop (6);

            // OUTPUT and DAW TEMPO groups.
            auto grpRow = [&r] (juce::Label& sec) { sec.setBounds (r.removeFromTop (13)); return r.removeFromTop (24); };
            {
                auto g2 = grpRow (secOut);
                modeBox.setBounds (g2.removeFromLeft (128).reduced (0, 1));
                g2.removeFromLeft (8);
                mpeT.setBounds (g2.removeFromLeft (86));
                g2.removeFromLeft (8);
                jiB.setBounds (g2.removeFromLeft (100).reduced (0, 1));
                g2.removeFromLeft (10);
                octLbl.setBounds (g2.removeFromLeft (96));       // "Octave (all)"
                g2.removeFromLeft (4);
                octAllDn.setBounds (g2.removeFromLeft (20).reduced (0, 2));
                octAllVal.setBounds (g2.removeFromLeft (26));
                octAllUp.setBounds (g2.removeFromLeft (20).reduced (0, 2));
                g2.removeFromLeft (10);
                stepsAllLbl.setBounds (g2.removeFromLeft (44));   // "Steps"
                stepsAllS.setBounds (g2.removeFromLeft (juce::jmin (120, g2.getWidth())).reduced (0, 3));
            }
            r.removeFromTop (4);
            {
                auto g3 = grpRow (secTempo);
                tempoT.setBounds (g3.removeFromLeft (juce::jmax (150, fgfit::toggleWidth (tempoT))));
                g3.removeFromLeft (4);
                tempoX2.setBounds (g3.removeFromLeft (32).reduced (0, 1));
                g3.removeFromLeft (4);
                tempoD2.setBounds (g3.removeFromLeft (32).reduced (0, 1));
                g3.removeFromLeft (6);
                bpmLbl.setBounds (g3);
            }
            r.removeFromTop (4);
            {
                auto g4 = grpRow (secGate); // NOTE START: quantize/mono toggles
                qTempoT.setBounds (g4.removeFromLeft (juce::jmax (218, fgfit::toggleWidth (qTempoT))));
                g4.removeFromLeft (6);
                monoT.setBounds (g4.removeFromLeft (juce::jmax (76, fgfit::toggleWidth (monoT))));
                g4.removeFromLeft (6);
                qPulseT.setBounds (g4);
            }
            r.removeFromTop (8);

            auto left = r.removeFromLeft (236);
            listHead.setBounds (left.removeFromTop (16));
            list.setBounds (left);
            r.removeFromLeft (10);
            pickHint.setBounds (r.withTrimmedTop (16).withHeight (34)); // over the empty slot rows, points at the list
            {
                auto head = r.removeFromTop (14);
                head.removeFromLeft (96);
                head.removeFromRight (28);        // over delB (24 + gap)
                durHead.setBounds (head.removeFromRight (104));
                head.removeFromRight (4);
                // The velocity SLOPE knob sits right above the VEL column it tilts.
                auto velCol = head.removeFromRight (76);
                velSlopeS.setBounds (velCol.withY (velCol.getY() - 1).withHeight (15));
                velHead.setBounds (velCol.withY (velCol.getY() + 14).withHeight (13));
                head.removeFromRight (4 + kStepBtnW + 3 + kStepBtnW + 6); // over the -12/+12 pair + gaps
                noteHead.setBounds (head);        // over the pitch box
            }
            // Scrollable slot rows: the viewport takes all space above the info
            // read-out ("unlimited" pulses - the panel shows a window onto them).
            auto infoArea = r.removeFromBottom (76);
            slotView.setBounds (r.reduced (0, 2));
            layoutSlotRows();
            infoArea.removeFromTop (4);
            info.setBounds (infoArea);
        }

        // Lay the slot rows out inside the scrollable container (called from
        // resized() AND refreshSlots(), since the used count changes the height).
        void layoutSlotRows()
        {
            int used = 0;
            if (proc != nullptr)
                for (int i = 0; i < RhythmEngine::kSlots; ++i)
                    if (proc->getRhythm().slots[i].partial.load() >= 0) ++used;
            const int rowH = 26;
            const bool scroll = used * rowH > slotView.getHeight();
            slotRows.setSize (juce::jmax (60, slotView.getWidth() - (scroll ? 10 : 0)),
                              juce::jmax (1, used * rowH));
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto row = juce::Rectangle<int> (0, i * rowH, slotRows.getWidth(), rowH).reduced (0, 2);
                slotLbl[i].setBounds (row.removeFromLeft (96));
                delB[i].setBounds (row.removeFromRight (24));
                row.removeFromRight (4);
                durS[i].setBounds (row.removeFromRight (104));
                row.removeFromRight (4);
                velS[i].setBounds (row.removeFromRight (76));
                row.removeFromRight (4);
                s12P[i].setBounds (row.removeFromRight (kStepBtnW));
                row.removeFromRight (3);
                s12M[i].setBounds (row.removeFromRight (kStepBtnW));
                row.removeFromRight (6);
                pitchE[i].setBounds (row.reduced (0, 1));
            }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2101018));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            // Frame around the PULSE RATE group (drawn behind its controls).
            if (! rateBox.isEmpty())
            {
                auto rb = rateBox.toFloat();
                g.setColour (juce::Colour (0x224fe6ff)); // faint cyan wash (matches secRate)
                g.fillRoundedRectangle (rb, 7.0f);
                g.setColour (juce::Colour (0x664fe6ff));
                g.drawRoundedRectangle (rb.reduced (0.5f), 7.0f, 1.2f);
            }
        }
    };

    // The selected envelope drawn as a LINE WITH NODES. Every node is draggable and
    // every leg between nodes can be bent by dragging it; both write straight into
    // the real parameters, so the curve on screen IS the signal the modulator sends
    // (ModEngine::shape uses the identical formula).
    struct EnvEditor : juce::Component, juce::TooltipClient
    {
        // Live values (times in seconds, level 0..1, curves -1..1). refresh() pulls
        // them from the parameters; a drag updates them immediately so the node
        // follows the mouse without waiting for a repaint from elsewhere.
        float a = 0.1f, d = 0.3f, s = 0.7f, r = 0.4f, cA = 0.0f, cD = 0.55f, cR = 0.55f;
        juce::Colour col { 0xff000000u | galactic::palette::kCustomMod }; // the CUSTOM modulator identity
        juce::String tip;
        std::function<void (const juce::String&, float)> onEdit;    // suffix, plain value
        std::function<void (const juce::String&, bool)>  onGesture; // suffix, starting
        std::function<float (const juce::String&)>       getParam;  // suffix -> plain value
        juce::String getTooltip() override { return tip; }

        void refresh()
        {
            if (! getParam) return;
            const float na = getParam ("A"),  nd = getParam ("D"),  ns = getParam ("S"),  nr = getParam ("R");
            const float ka = getParam ("CurveA"), kd = getParam ("CurveD"), kr = getParam ("CurveR");
            if (na == a && nd == d && ns == s && nr == r && ka == cA && kd == cD && kr == cR) return;
            a = na; d = nd; s = ns; r = nr; cA = ka; cD = kd; cR = kr;
            repaint();
        }

        static float shape (float t, float c)
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            return std::pow (t, std::pow (2.0f, -3.0f * juce::jlimit (-1.0f, 1.0f, c)));
        }

        // Geometry. Times use a square-root scale so short times still get real estate.
        static constexpr float kMaxT = 5.0f, kPad = 8.0f;
        float sustainW() const { return juce::jmax (12.0f, (float) getWidth() * 0.10f); }
        float legW() const     { return juce::jmax (10.0f, ((float) getWidth() - 2.0f * kPad - sustainW()) / 3.0f); }
        float tToW (float secs) const { return legW() * std::sqrt (juce::jlimit (0.0f, kMaxT, secs) / kMaxT); }
        float wToT (float w) const
        { const float u = juce::jlimit (0.0f, 1.0f, w / juce::jmax (1.0f, legW())); return u * u * kMaxT; }
        float lToY (float lvl) const { return (float) getHeight() - kPad - juce::jlimit (0.0f, 1.0f, lvl) * ((float) getHeight() - 2.0f * kPad); }
        float yToL (float y) const   { return juce::jlimit (0.0f, 1.0f, ((float) getHeight() - kPad - y) / juce::jmax (1.0f, (float) getHeight() - 2.0f * kPad)); }

        // 0 start | 1 attack end (peak) | 2 decay end = SUSTAIN LEVEL | 3 sustain end | 4 release end
        juce::Point<float> node (int i) const
        {
            const float x0 = kPad;
            const float x1 = x0 + tToW (a);
            const float x2 = x1 + tToW (d);
            const float x3 = x2 + sustainW();
            const float x4 = x3 + tToW (r);
            switch (i)
            {
                case 0:  return { x0, lToY (0.0f) };
                case 1:  return { x1, lToY (1.0f) };
                case 2:  return { x2, lToY (s) };
                case 3:  return { x3, lToY (s) };
                default: return { x4, lToY (0.0f) };
            }
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff101018));
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (col.withAlpha (0.18f));
            g.drawRoundedRectangle (b.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (col.withAlpha (0.10f));
            g.drawHorizontalLine ((int) lToY (1.0f), b.getX() + 2.0f, b.getRight() - 2.0f);
            g.drawHorizontalLine ((int) lToY (0.0f), b.getX() + 2.0f, b.getRight() - 2.0f);

            juce::Path p;
            p.startNewSubPath (node (0));
            const auto leg = [&] (juce::Point<float> from, juce::Point<float> to, float curve)
            {
                constexpr int steps = 28;
                for (int i = 1; i <= steps; ++i)
                {
                    const float t = (float) i / (float) steps;
                    p.lineTo (from.x + (to.x - from.x) * t,
                              from.y + (to.y - from.y) * shape (t, curve));
                }
            };
            leg (node (0), node (1), cA);
            leg (node (1), node (2), cD);
            p.lineTo (node (3));
            leg (node (3), node (4), cR);

            auto fill = p;
            fill.lineTo (node (4).x, lToY (0.0f));
            fill.lineTo (node (0).x, lToY (0.0f));
            fill.closeSubPath();
            g.setColour (col.withAlpha (0.13f));
            g.fillPath (fill);
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (2.0f));

            // Each draggable node IS its letter - bold, a size up from the old dots,
            // with a thin black outline so it reads over the curve and the fill.
            static const char* letters[5] = { "", "A", "D", "S", "R" };
            for (int i : { 1, 2, 3, 4 })
            {
                const auto n = node (i);
                const bool hot = i == hover || i == dragNode;
                juce::GlyphArrangement ga;
                ga.addLineOfText (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")), letters[i], 0.0f, 0.0f);
                juce::Path gp;
                ga.createPath (gp);
                const auto gb = gp.getBounds();
                gp.applyTransform (juce::AffineTransform::translation (n.x - gb.getCentreX(), n.y - gb.getCentreY()));
                g.setColour (juce::Colours::black.withAlpha (0.9f));
                g.strokePath (gp, juce::PathStrokeType (2.4f));
                g.setColour (hot ? juce::Colours::white : col);
                g.fillPath (gp);
            }
        }

        //== interaction ======================================================
        int   dragNode = -1, dragSeg = -1, hover = -1;
        float dragStartCurve = 0.0f, dragStartY = 0.0f;

        int nodeAt (juce::Point<float> pos) const
        {
            for (int i : { 1, 2, 3, 4 })
                if (pos.getDistanceFrom (node (i)) < 12.0f) return i; // letter-sized grab area
            return -1;
        }
        int segAt (juce::Point<float> pos) const   // 0 = attack leg, 1 = decay leg, 2 = release leg
        {
            if (pos.x < node (1).x) return 0;
            if (pos.x < node (2).x) return 1;
            if (pos.x > node (3).x) return 2;
            return -1;                              // the flat sustain leg has no curve
        }
        static const char* segParam (int seg)
        { return seg == 0 ? "CurveA" : (seg == 1 ? "CurveD" : "CurveR"); }
        float segCurve (int seg) const { return seg == 0 ? cA : (seg == 1 ? cD : cR); }

        juce::String draggedParam() const
        {
            switch (dragNode)
            {
                case 1:  return "A";
                case 2:  return "D";
                case 3:  return "S";
                case 4:  return "R";
                default: return dragSeg >= 0 ? juce::String (segParam (dragSeg)) : juce::String();
            }
        }

        // Writes the parameter AND the local value, so the node follows the mouse now.
        void put (const juce::String& suffix, float v)
        {
            if      (suffix == "A") a = v;
            else if (suffix == "D") d = v;
            else if (suffix == "S") s = v;
            else if (suffix == "R") r = v;
            else if (suffix == "CurveA") cA = v;
            else if (suffix == "CurveD") cD = v;
            else if (suffix == "CurveR") cR = v;
            if (onEdit) onEdit (suffix, v);
            repaint();
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = nodeAt (e.position);
            if (h != hover) { hover = h; repaint(); }
            setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor
                                   : (segAt (e.position) >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                                              : juce::MouseCursor::NormalCursor));
        }
        void mouseExit (const juce::MouseEvent&) override { if (hover >= 0) { hover = -1; repaint(); } }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragNode = nodeAt (e.position);
            dragSeg  = dragNode >= 0 ? -1 : segAt (e.position);
            if (dragSeg >= 0) { dragStartCurve = segCurve (dragSeg); dragStartY = e.position.y; }
            const auto pid = draggedParam();
            if (pid.isNotEmpty() && onGesture) onGesture (pid, true);
            repaint();
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            const auto pos = e.position;
            switch (dragNode)
            {
                case 1: put ("A", juce::jlimit (0.0f, kMaxT, wToT (pos.x - node (0).x))); break;
                case 2: // the SUSTAIN node: decay time across, sustain level up/down
                    put ("D", juce::jlimit (0.0f, kMaxT, wToT (pos.x - node (1).x)));
                    put ("S", yToL (pos.y));
                    break;
                case 3: put ("S", yToL (pos.y)); break;   // sustain level only
                case 4: put ("R", juce::jlimit (0.0f, kMaxT, wToT (pos.x - node (3).x))); break;
                default:
                    if (dragSeg >= 0)
                    {
                        // Drag the LINE: up bends it convex, down concave.
                        const float dy = (dragStartY - pos.y) / juce::jmax (24.0f, (float) getHeight() * 0.6f);
                        put (segParam (dragSeg), juce::jlimit (-1.0f, 1.0f, dragStartCurve + dy * 2.0f));
                    }
                    break;
            }
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            const auto pid = draggedParam();
            if (pid.isNotEmpty() && onGesture) onGesture (pid, false);
            dragNode = -1; dragSeg = -1;
            repaint();
        }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int seg = segAt (e.position);      // straighten that leg again
            if (seg < 0) return;
            const juce::String pid = segParam (seg);
            if (onGesture) onGesture (pid, true);
            put (pid, 0.0f);
            if (onGesture) onGesture (pid, false);
        }
    };

    // Drawn-LFO wave editor (Ableton-automation style): drag points; double-click
    // adds a point; right-click deletes one; dragging a segment's MIDDLE bends it
    // (power curve), like curving an automation line. One cycle, x 0..1, y 0..1.
    struct WaveEditor : juce::Component
    {
        struct Pt { float x = 0, y = 0, curve = 0; }; // curve bends the segment AFTER this point
        std::vector<Pt> pts { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
        std::function<void()> onChange, onClose;
        juce::TextButton closeB { "X" };
        int dragIdx = -1, curveIdx = -1;
        float curveStartVal = 0.0f, curveStartY = 0.0f;

        WaveEditor()
        {
            closeB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeB.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (closeB);
        }
        void resized() override { closeB.setBounds (getWidth() - 26, 4, 22, 22); }

        static float shaped (float t, float curve) // Ableton-style segment bend
        { return std::pow (juce::jlimit (0.0f, 1.0f, t), std::exp2 (curve * 3.0f)); }
        float valueAt (float x) const
        {
            if (pts.empty()) return 0.0f;
            if (x <= pts.front().x) return pts.front().y;
            for (size_t i = 1; i < pts.size(); ++i)
                if (x <= pts[i].x)
                {
                    const auto& a = pts[i - 1]; const auto& b = pts[i];
                    const float t = b.x > a.x ? (x - a.x) / (b.x - a.x) : 1.0f;
                    return a.y + (b.y - a.y) * shaped (t, a.curve);
                }
            return pts.back().y;
        }
        void fillTable (float* out, int n) const
        { for (int i = 0; i < n; ++i) out[i] = valueAt ((float) i / (float) (n - 1)); }

        juce::String toText() const
        {
            juce::String s;
            for (const auto& p : pts)
                s += juce::String (p.x, 4) + ":" + juce::String (p.y, 4) + ":" + juce::String (p.curve, 3) + ";";
            return s;
        }
        void fromText (const juce::String& s)
        {
            std::vector<Pt> v;
            for (const auto& tok : juce::StringArray::fromTokens (s, ";", ""))
            {
                auto parts = juce::StringArray::fromTokens (tok, ":", "");
                if (parts.size() == 3)
                    v.push_back ({ parts[0].getFloatValue(), parts[1].getFloatValue(), parts[2].getFloatValue() });
            }
            if (v.size() >= 2) pts = std::move (v);
        }

        juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (14.0f, 30.0f); }
        juce::Point<float> toScreen (const Pt& p) const
        { auto r = plot(); return { r.getX() + p.x * r.getWidth(), r.getBottom() - p.y * r.getHeight() }; }

        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colour (0xf2101018));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
            g.setColour (juce::Colour (0xff000000u | galactic::palette::kCustomMod));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 10.0f, 3.0f);
            g.setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));
            g.drawText ("LFO wave  -  drag points, double-click adds, right-click deletes, drag a line's middle to curve it",
                        14, 6, getWidth() - 48, 20, juce::Justification::centredLeft);
            auto r = plot();
            g.setColour (juce::Colour (0xff20202b));
            g.fillRect (r);
            g.setColour (juce::Colour (0x33ffffff));
            for (int i = 1; i < 4; ++i)
            {
                g.drawHorizontalLine ((int) (r.getY() + r.getHeight() * i / 4.0f), r.getX(), r.getRight());
                g.drawVerticalLine   ((int) (r.getX() + r.getWidth()  * i / 4.0f), r.getY(), r.getBottom());
            }
            juce::Path curve;
            for (int i = 0; i <= 256; ++i)
            {
                const float x = (float) i / 256.0f;
                const auto  p = juce::Point<float> (r.getX() + x * r.getWidth(),
                                                    r.getBottom() - valueAt (x) * r.getHeight());
                i == 0 ? curve.startNewSubPath (p) : curve.lineTo (p);
            }
            g.setColour (juce::Colour (0xff000000u | galactic::palette::kCustomMod));
            g.strokePath (curve, juce::PathStrokeType (2.2f));
            for (const auto& p : pts)
            {
                g.setColour (juce::Colours::white);
                g.fillEllipse (juce::Rectangle<float> (11, 11).withCentre (toScreen (p)));
            }
        }
        int hitPoint (juce::Point<float> m) const
        {
            for (size_t i = 0; i < pts.size(); ++i)
                if (toScreen (pts[i]).getDistanceFrom (m) < 9.0f) return (int) i;
            return -1;
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto m = e.position;
            const int hp = hitPoint (m);
            if (e.mods.isRightButtonDown())
            {
                if (hp > 0 && hp < (int) pts.size() - 1) { pts.erase (pts.begin() + hp); changed(); }
                return;
            }
            if (hp >= 0) { dragIdx = hp; return; }
            // grab the SEGMENT under the mouse to bend it
            auto r = plot();
            const float x = juce::jlimit (0.0f, 1.0f, (m.x - r.getX()) / r.getWidth());
            for (size_t i = 1; i < pts.size(); ++i)
                if (x <= pts[i].x) { curveIdx = (int) i - 1; break; }
            if (curveIdx >= 0) { curveStartVal = pts[(size_t) curveIdx].curve; curveStartY = m.y; }
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            auto r = plot();
            if (dragIdx >= 0)
            {
                auto& p = pts[(size_t) dragIdx];
                p.y = juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight());
                if (dragIdx > 0 && dragIdx < (int) pts.size() - 1) // endpoints keep x = 0 / 1
                    p.x = juce::jlimit (pts[(size_t) dragIdx - 1].x + 0.01f,
                                        pts[(size_t) dragIdx + 1].x - 0.01f,
                                        (e.position.x - r.getX()) / r.getWidth());
                changed();
            }
            else if (curveIdx >= 0)
            {
                const bool down = pts[(size_t) curveIdx + 1].y < pts[(size_t) curveIdx].y;
                const float d = (curveStartY - e.position.y) / 120.0f;
                pts[(size_t) curveIdx].curve = juce::jlimit (-1.0f, 1.0f, curveStartVal + (down ? -d : d));
                changed();
            }
        }
        void mouseUp (const juce::MouseEvent&) override { dragIdx = -1; curveIdx = -1; }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            auto r = plot();
            Pt np { juce::jlimit (0.01f, 0.99f, (e.position.x - r.getX()) / r.getWidth()),
                    juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight()), 0.0f };
            for (size_t i = 1; i < pts.size(); ++i)
                if (np.x < pts[i].x) { pts.insert (pts.begin() + (long) i, np); break; }
            changed();
        }
        void changed() { repaint(); if (onChange) onChange(); }
    };

    //==========================================================================
    // Per-partial amplitude editor (the pencil next to the SLOPE fader).
    //==========================================================================

    // The pencil itself, drawn as a path: at the 24 px this cell can spare, a letter
    // or the word "Edit" was unreadable, and the icon says "draw the spectrum" in a
    // way no abbreviation did.
    struct PencilButton : juce::Button
    {
        juce::Colour col { 0xff5cb8ff };
        PencilButton() : juce::Button ("pencil") { setMouseClickGrabsKeyboardFocus (false); }
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (juce::Colour (down ? 0xff2c3648 : (over ? 0xff232c3c : 0xff171d29)));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (col.withAlpha (over || down ? 1.0f : 0.8f));
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

            // Tip at the bottom-left, blunt end at the top-right; the nib is a triangle
            // so the icon still reads as a pencil and not as a slash.
            const float pad = juce::jmax (3.0f, r.getWidth() * 0.24f);
            const juce::Point<float> tip { r.getX() + pad, r.getBottom() - pad };
            const juce::Point<float> end { r.getRight() - pad, r.getY() + pad };
            const float len = tip.getDistanceFrom (end);
            if (len < 2.0f) return;
            const juce::Point<float> dir { (end.x - tip.x) / len, (end.y - tip.y) / len };
            const juce::Point<float> nrm { -dir.y, dir.x };
            const float w = juce::jmax (1.2f, r.getWidth() * 0.11f);
            const auto shoulder = tip + dir * (len * 0.26f);
            juce::Path body;
            body.startNewSubPath (shoulder + nrm * w);
            body.lineTo (end + nrm * w);
            body.lineTo (end - nrm * w);
            body.lineTo (shoulder - nrm * w);
            body.closeSubPath();
            g.fillPath (body);
            juce::Path nib;
            nib.addTriangle (tip, shoulder + nrm * w, shoulder - nrm * w);
            g.fillPath (nib);
        }
    };

    // The knob grid.
    //
    // ONE Component draws every partial. A juce::Slider per partial is not an option:
    // kMaxPartials is 16384 and a thousand partials is an ordinary patch, and JUCE's
    // per-child hit-testing, focus and repaint bookkeeping collapses long before that
    // (the window took seconds to open and then dropped frames on every mouse move).
    // So this paints only the rows the clip region touches and does its own hit
    // testing, exactly the way ScaleKeyboard draws and dispatches its keys.
    //
    // EVERY partial of the template is listed. There is no "first N" window and no
    // cap: a partial the user cannot see is a partial they cannot un-mute.
    struct PartialAmpGrid : juce::Component, juce::TooltipClient
    {
        partialamp::Model model;
        std::function<juce::String (const char*, const char*)> trF;
        std::function<void()> onGestureStart, onEdited, onGestureEnd;
        bool manualOn = false;   // dims the whole grid when the edits are not sounding

        static constexpr int kCellW = 62, kCellH = 80;

        int   columns()  const { return juce::jmax (1, getWidth() / kCellW); }
        float colWidth() const { return (float) getWidth() / (float) columns(); }
        int   rows()     const { return (model.count() + columns() - 1) / columns(); }
        int   contentHeight() const { return juce::jmax (kCellH, rows() * kCellH); }

        juce::Rectangle<float> cellBounds (int i) const
        {
            const int c = columns();
            const float cw = colWidth();
            return { (float) (i % c) * cw, (float) (i / c) * (float) kCellH, cw, (float) kCellH };
        }
        int cellAt (juce::Point<float> p) const
        {
            const int c = columns();
            const int col = juce::jlimit (0, c - 1, (int) (p.x / colWidth()));
            const int row = (int) (p.y / (float) kCellH);
            if (row < 0) return -1;
            const int i = row * c + col;
            return model.valid (i) ? i : -1;
        }

        //-- painting ----------------------------------------------------------
        void paint (juce::Graphics& g) override
        {
            const int n = model.count();
            if (n == 0)
            {
                g.setColour (juce::Colour (0xff8f96a8));
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (trF ? trF ("No partials in the current spectrum.",
                                       "Cap parcial a l'espectre actual.")
                                : juce::String(),
                            getLocalBounds(), juce::Justification::centred, false);
                return;
            }
            const auto clip = g.getClipBounds();
            const int c = columns();
            const int r0 = juce::jmax (0, clip.getY() / kCellH);
            const int r1 = juce::jmin (rows() - 1, clip.getBottom() / kCellH);
            for (int r = r0; r <= r1; ++r)
                for (int k = 0; k < c; ++k)
                {
                    const int i = r * c + k;
                    if (i >= n) return;
                    drawCell (g, i, cellBounds (i));
                }
        }

        void drawCell (juce::Graphics& g, int i, juce::Rectangle<float> cell)
        {
            const auto& cd = model.cells[(size_t) i];
            const bool sel = model.isSelected (i);
            const float dim = (manualOn ? 1.0f : 0.55f) * (cd.locked ? 0.45f : 1.0f);

            if (sel)
            {
                g.setColour (juce::Colour (0xff2fb0c8).withAlpha (0.28f));
                g.fillRoundedRectangle (cell.reduced (1.5f), 4.0f);
                g.setColour (juce::Colour (0xff2fb0c8).withAlpha (i == model.anchor ? 0.95f : 0.5f));
                g.drawRoundedRectangle (cell.reduced (1.5f), 4.0f, i == model.anchor ? 1.6f : 1.0f);
            }
            else if (i == hover)
            {
                g.setColour (juce::Colour (0x18ffffff));
                g.fillRoundedRectangle (cell.reduced (1.5f), 4.0f);
            }

            auto body = cell.reduced (2.0f, 3.0f);
            auto top  = body.removeFromTop (13.0f);
            auto bot  = body.removeFromBottom (12.0f);

            // Caption = the PARTIAL NUMBER: "3", never "3/1" (see partialNumberName).
            g.setColour (juce::Colours::white.withAlpha (0.92f * dim));
            g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            g.drawText (juce::String::fromUTF8 (dissonance::partialNumberName (cd.ratio).c_str()),
                        top.toNearestInt(), juce::Justification::centred, false);

            // Knob: the arc runs the engine's whole range, and the coloured sweep starts
            // at the ZERO tick, so the eye reads "how far from the compiled amplitude"
            // rather than an absolute position that means nothing on its own.
            const float rad = juce::jmin (body.getWidth(), body.getHeight()) * 0.44f;
            const juce::Point<float> ctr { body.getCentreX(), body.getCentreY() };
            constexpr float a0 = -2.356f, a1 = 2.356f;   // -135 deg .. +135 deg
            const auto angleOf = [&] (float dB)
            {
                const float t = (dB - partialamp::kMinDb) / (partialamp::kMaxDb - partialamp::kMinDb);
                return a0 + juce::jlimit (0.0f, 1.0f, t) * (a1 - a0);
            };
            const float aZero = angleOf (0.0f);
            const float aVal  = angleOf (cd.dB);

            juce::Path back;
            back.addCentredArc (ctr.x, ctr.y, rad, rad, 0.0f, a0, a1, true);
            g.setColour (juce::Colour (0xff39414f).withAlpha (dim));
            g.strokePath (back, juce::PathStrokeType (2.5f));

            const bool muted = partialamp::isMuted (cd.dB);
            const juce::Colour vc = muted        ? juce::Colour (0xffe0453f)
                                  : cd.dB > 0.0f ? juce::Colour (0xffffb020)
                                  : cd.dB < 0.0f ? juce::Colour (0xff5cb8ff)
                                                 : juce::Colour (0xff6c7488);
            if (cd.dB != 0.0f)
            {
                juce::Path val;
                val.addCentredArc (ctr.x, ctr.y, rad, rad, 0.0f,
                                   juce::jmin (aZero, aVal), juce::jmax (aZero, aVal), true);
                g.setColour (vc.withAlpha (dim));
                g.strokePath (val, juce::PathStrokeType (2.8f));
            }
            // Zero tick: where this partial's own compiled amplitude sits.
            g.setColour (juce::Colours::white.withAlpha (0.45f * dim));
            {
                const juce::Point<float> d { std::sin (aZero), -std::cos (aZero) };
                g.drawLine (ctr.x + d.x * (rad - 3.0f), ctr.y + d.y * (rad - 3.0f),
                            ctr.x + d.x * (rad + 3.0f), ctr.y + d.y * (rad + 3.0f), 1.0f);
            }
            // Pointer.
            {
                const juce::Point<float> d { std::sin (aVal), -std::cos (aVal) };
                g.setColour (vc.withAlpha (dim));
                g.drawLine (ctr.x + d.x * rad * 0.25f, ctr.y + d.y * rad * 0.25f,
                            ctr.x + d.x * (rad - 1.0f), ctr.y + d.y * (rad - 1.0f), 2.0f);
            }

            // Read-out. MUTE is spelled out: "-120.0" would read as a number the user
            // could nudge, when it is the point at which the partial stops sounding.
            juce::String vt;
            if (cd.locked)     vt = juce::String::fromUTF8 ("\xe2\x9c\x95");  // locked (see Cell::locked)
            else if (muted)    vt = "MUTE";
            else if (cd.dB == 0.0f) vt = "0.0";
            else               vt = (cd.dB > 0.0f ? "+" : "") + juce::String (cd.dB, 1);
            g.setColour ((cd.dB == 0.0f ? juce::Colour (0xff8f96a8) : vc).withAlpha (dim));
            g.setFont (juce::Font (juce::FontOptions (10.5f).withStyle (cd.dB == 0.0f ? "Regular" : "Bold")));
            g.drawText (vt, bot.toNearestInt(), juce::Justification::centred, false);
        }

        //-- interaction -------------------------------------------------------
        int   hover = -1;
        float dragStartY = 0.0f;

        juce::String getTooltip() override
        {
            if (! model.valid (hover) || trF == nullptr) return {};
            const auto& cd = model.cells[(size_t) hover];
            const double natDb = cd.natural > 0.0f ? 20.0 * std::log10 ((double) cd.natural) : -144.0;
            juce::String s;
            s << trF ("Partial ", "Parcial ") << (hover + 1) << " / " << model.count()
              << "   *" << juce::String::fromUTF8 (dissonance::partialNumberName (cd.ratio).c_str()) << "*\n"
              << trF ("As compiled: `", "Tal com es compila: `") << juce::String (natDb, 1) << " dB`\n";
            if (cd.locked)
                s << trF ("!Locked!: this partial sits under 0.01 cent from the one below it, so an edit cannot be aimed at it separately.",
                          "!Blocat!: aquest parcial \xc3\xa9s a menys de 0,01 cent del de sota, i una edici\xc3\xb3 no es pot dirigir nom\xc3\xa9s a ell.");
            else if (partialamp::isMuted (cd.dB))
                s << trF ("Your edit: *MUTE*. The partial is !still in the template! - it comes back on undo, or when manual mode goes off.",
                          "La teva edici\xc3\xb3: *MUT*. El parcial !segueix a la plantilla! - torna amb un desfer, o quan apagues el mode manual.");
            else
                s << trF ("Your edit: `", "La teva edici\xc3\xb3: `") << (cd.dB > 0.0f ? "+" : "")
                  << juce::String (cd.dB, 1) << trF (" dB`\nDrag up/down; Ctrl = fine; double-click = back to compiled.",
                                                     " dB`\nArrossega amunt/avall; Ctrl = fi; doble clic = torna al compilat.");
            return s;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = cellAt (e.position);
            if (h != hover)
            {
                if (model.valid (hover)) repaint (cellBounds (hover).getSmallestIntegerContainer());
                hover = h;
                if (model.valid (hover)) repaint (cellBounds (hover).getSmallestIntegerContainer());
            }
        }
        void mouseExit (const juce::MouseEvent&) override
        {
            if (model.valid (hover)) repaint (cellBounds (hover).getSmallestIntegerContainer());
            hover = -1;
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = cellAt (e.position);
            if (i < 0) { model.clearSelection(); repaint(); return; }
            if (e.mods.isShiftDown())
            {
                model.shiftClick (i);   // arm, then stretch (see Model::shiftClick)
                repaint();
                return;
            }
            if (! model.isSelected (i)) { model.clearSelection(); repaint(); }
            dragStartY = e.position.y;
            model.beginDrag (i);
            if (onGestureStart) onGestureStart();
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! model.dragging()) return;
            const float perPx = e.mods.isCtrlDown() ? partialamp::kDbPerPixelFine
                                                    : partialamp::kDbPerPixel;
            if (model.applyDragDelta ((dragStartY - e.position.y) * perPx))
            {
                repaintDragSet();
                if (onEdited) onEdited();
            }
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (! model.dragging()) return;
            model.endDrag();
            if (onGestureEnd) onGestureEnd();
        }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int i = cellAt (e.position);
            if (i < 0) return;
            const auto set = model.dragSetFor (i);
            if (onGestureStart) onGestureStart();
            if (model.resetSet (set)) { repaint(); if (onEdited) onEdited(); }
            if (onGestureEnd) onGestureEnd();
        }
        void repaintDragSet()
        {
            // A selection can be thousands of cells; repainting each rect separately is
            // slower than one pass over the visible rows.
            if (model.dragIdx.size() > 24) repaint();
            else for (int k : model.dragIdx) repaint (cellBounds (k).getSmallestIntegerContainer());
        }
    };

    // The window's contents: the linked ON/OFF, the two facts that surprise everyone,
    // the honest counters, and the scrolling grid.
    struct PartialAmpPanel : juce::Component
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (const char*, const char*)> trF;
        std::function<void()> onEdited;                     // editor: refresh the slope cell
        // Fired the instant a pencil edit arms manual mode by itself. The editor uses it
        // to grey SLOPE and pop its FROZEN explanation THERE AND THEN, rather than
        // letting the 30 Hz poll do it silently behind a window the user is not looking at.
        std::function<void()> onManualArmed;
        bool armedManualThisGesture = false;                // this drag turned manual on

        juce::ToggleButton onT;                             // twin of the pill under the pencil
        std::unique_ptr<ButtonAttachment> onAtt;
        juce::TextButton   resetAllB;
        juce::Label        headLbl, factsLbl, statLbl, warnLbl;
        juce::Viewport     view;
        PartialAmpGrid     grid;
        // Its own TooltipWindow, PARENTED to this panel. The editor's is parented to the
        // editor, and a tooltip window only serves its own parent's subtree - so in a
        // separate top-level window every hover read-out here was simply invisible.
        juce::TooltipWindow tooltips { this, 350 };

        // Edits that bound to no partial of the CURRENT spectrum. Kept verbatim and
        // merged back on every write - see partialamp::mergedGains.
        std::vector<spectrum::PartialGain> homeless;
        std::vector<spectrum::PartialGain> gestureBefore;   // for one undo step per drag
        juce::uint32 lastGen = 0;
        unsigned     lastRev = 0;
        bool         haveSynced = false;

        PartialAmpPanel()
        {
            onT.setName ("onled");
            addAndMakeVisible (onT);
            resetAllB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
            resetAllB.setMouseClickGrabsKeyboardFocus (false);
            resetAllB.onClick = [this] { resetEverything(); };
            addAndMakeVisible (resetAllB);
            for (auto* l : { &headLbl, &factsLbl, &statLbl, &warnLbl })
            {
                l->setInterceptsMouseClicks (false, false);
                addAndMakeVisible (*l);
            }
            headLbl.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
            headLbl.setColour (juce::Label::textColourId, juce::Colour (0xff5cb8ff));
            factsLbl.setFont (juce::Font (juce::FontOptions (11.5f)));
            factsLbl.setColour (juce::Label::textColourId, juce::Colour (0xffc4ccda));
            statLbl.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            statLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            warnLbl.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            warnLbl.setColour (juce::Label::textColourId, juce::Colour (0xffffb020));
            view.setViewedComponent (&grid, false);
            view.setScrollBarsShown (true, false);
            // The knob list takes the keyboard focus inside its window, so Up/Down/Page
            // scroll it. That also puts it FIRST in the key-event chain, ahead of the
            // window's forward-to-the-editor hook: a key this list uses is used here and
            // never also rings a note.
            view.setWantsKeyboardFocus (true);
            addAndMakeVisible (view);
        }

        void init (FractalAdditiveAudioProcessor& p)
        {
            proc = &p;
            grid.onGestureStart = [this]
            {
                // ARM MANUAL MODE FIRST, and before gestureBefore is captured.
                // With "partialManual" off - and it ships off - the engine throws these
                // edits away, so the drag below moved the sound by exactly 0.00 dB while
                // the knob, the read-out and the undo stack all said it had worked. See
                // armManualPartialAmpsForEdit for the measurement and for why arming is
                // the right answer rather than refusing the drag.
                // FIRST, because it freezes the slope at its PRE-drag value and puts the
                // edits in force before pushGains() runs on the drag's first frame.
                armedManualThisGesture = proc->armManualPartialAmpsForEdit();
                gestureBefore = proc->getPartialGains();
                if (armedManualThisGesture)
                {
                    refreshText();                     // the panel's banner changes meaning
                    if (onManualArmed) onManualArmed(); // ...and SLOPE goes inert in the main window
                }
            };
            grid.onEdited       = [this] { pushGains(); };
            grid.onGestureEnd   = [this]
            {
                // ONE undo step per gesture: pushing per frame filled the stack with
                // hundreds of micro-moves and made Ctrl+Z useless on a hand-drawn curve.
                // ...and when this gesture also armed manual mode, that flip is already
                // inside the transaction armManualPartialAmpsForEdit opened, so ONE
                // Ctrl+Z takes back the whole action instead of leaving manual on.
                proc->commitPartialGains (gestureBefore, proc->getPartialGains(),
                                          /*startNewTransaction*/ ! armedManualThisGesture);
                armedManualThisGesture = false;
                lastRev = proc->getPartialGainsRevision();
                refreshText();
            };
            onAtt = std::make_unique<ButtonAttachment> (proc->apvts, "partialManual", onT);
        }

        bool manualOn() const
        {
            return proc != nullptr && proc->apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
        }

        void pushGains()
        {
            if (proc == nullptr) return;
            proc->setPartialGains (partialamp::mergedGains (grid.model, homeless), manualOn());
            lastRev = proc->getPartialGainsRevision();
            refreshText();
            if (onEdited) onEdited();
        }

        void resetEverything()
        {
            if (proc == nullptr) return;
            // Clears the visible knobs AND the homeless edits: this button says "no
            // hand-drawn amplitudes at all", so leaving invisible ones behind to
            // reappear later would be a lie. One undo step brings the lot back.
            const auto before = proc->getPartialGains();
            for (auto& c : grid.model.cells) c.dB = 0.0f;
            homeless.clear();
            proc->setPartialGains ({}, manualOn());
            proc->commitPartialGains (before, {});
            lastRev = proc->getPartialGainsRevision();
            grid.repaint();
            refreshText();
            if (onEdited) onEdited();
        }

        //-- keeping up with the spectrum --------------------------------------
        // Called on the editor's 30 Hz timer. The template is rebuilt by a background
        // thread on every Base/Fractal/prime/tuning change, so the knob list has to
        // follow it; and the stored edits can change without any template change at all
        // (an undo, a preset, a reload), which is what the revision counter catches.
        void syncIfNeeded()
        {
            if (proc == nullptr || grid.model.dragging()) return;   // never mid-gesture
            const auto gen = proc->getTemplateGeneration();
            const auto rev = proc->getPartialGainsRevision();
            if (haveSynced && gen == lastGen && rev == lastRev) return;
            lastGen = gen; lastRev = rev; haveSynced = true;
            resync();
        }

        void resync()
        {
            if (proc == nullptr) return;
            const auto* t = proc->getLiveTemplate();

            std::vector<partialamp::Cell> cells;
            if (t != nullptr)
            {
                cells.reserve (t->partials.size());
                for (const auto& p : t->partials)
                    cells.push_back ({ (double) p.ratio, p.amp, spectrum::gainKey ((double) p.ratio), 0.0f, false });
            }

            // Which stored edit belongs to which knob is decided by the SAME pass the
            // audio uses (bindGains), never by a second rule invented here - a knob must
            // not show an edit that some other partial is actually getting.
            const auto& stored = proc->getPartialGains();
            homeless.clear();
            if (t != nullptr && ! stored.empty())
            {
                std::vector<int> where;
                spectrum::bindGains (t->partials, stored, where);
                for (size_t j = 0; j < stored.size(); ++j)
                {
                    if (where[j] >= 0) cells[(size_t) where[j]].dB = stored[j].dB;
                    else               homeless.push_back (stored[j]);
                }
            }
            else
            {
                homeless = stored;
            }

            // Keep the selection across a rebuild that did not actually move the
            // partials (the spectrum is republished on every override write, and losing
            // a 200-knob selection to that would make range editing impossible).
            const bool same = grid.model.cells.size() == cells.size()
                && std::equal (cells.begin(), cells.end(), grid.model.cells.begin(),
                               [] (const partialamp::Cell& a, const partialamp::Cell& b)
                               { return a.key == b.key; });
            std::vector<char> keepSel;
            int keepAnchor = -1;
            if (same) { keepSel = grid.model.sel; keepAnchor = grid.model.anchor; }
            grid.model.setCells (std::move (cells));
            if (same) { grid.model.sel = std::move (keepSel); grid.model.anchor = keepAnchor; }

            layoutGrid();
            refreshText();
            grid.repaint();
        }

        void refreshText()
        {
            if (proc == nullptr || trF == nullptr) return;
            grid.manualOn = manualOn();

            const int n       = grid.model.count();
            const int edited  = grid.model.editedCount();
            const int lockedN = grid.model.lockedCount();
            juce::String s;
            s << n << trF (" partials", " parcials") << "   \xc2\xb7   "
              << edited << trF (" edited", " editats");
            if (! homeless.empty())
                s << "   \xc2\xb7   " << (int) homeless.size()
                  << trF (" waiting for their partial (kept)", " esperant el seu parcial (desats)");
            if (lockedN > 0)
                s << "   \xc2\xb7   " << lockedN << trF (" locked (under 0.01\xc2\xa2 apart)",
                                                          " blocats (a menys de 0,01\xc2\xa2)");
            statLbl.setText (s, juce::dontSendNotification);

            juce::String w;
            if (! manualOn())
                w << trF ("MANUAL IS OFF - these knobs are stored but not sounding. Your first drag switches it on for you (one Ctrl+Z takes both back).",
                          "EL MODE MANUAL \xc3\x89S APAGAT - aquests controls es desen per\xc3\xb2 no sonen. El primer arrossegament l'activa per tu (un sol Ctrl+Z ho desf\xc3\xa0 tot).");
            else
                // THE AMBUSH, ON SCREEN. Manual mode freezes the compile slope, so
                // somebody who drags one partial here and then reaches for AMP DECAY in
                // the main window finds a fader that does nothing and no reason given.
                // It has to be said where the user is looking - which, mid-drag, is this
                // window and not the fader three hundred pixels away in another one.
                w << trF ("MANUAL IS ON - these knobs are the amplitudes now, so AMP DECAY (slope) in the main window is FROZEN where it stood. Switch manual off and it applies again.",
                          "EL MODE MANUAL \xc3\x89S ACTIU - ara les amplituds venen d'aquests controls, i la CAIGUDA AMP (pendent) de la finestra principal queda CONGELADA tal com estava. Apaga el manual i torna a aplicar-se.");
            if (proc->isSpectrumTruncated())
            {
                if (w.isNotEmpty()) w << "\n";
                w << trF ("TRUNCATED: the spectrum hit the ", "TRUNCAT: l'espectre ha topat amb el sostre de ")
                  << spectrum::kMaxPartials
                  << trF ("-partial ceiling (", " parcials (") << proc->getRawPartialCount()
                  << trF (" wanted). The partials the cap removed are NOT in this list.",
                          " en volia). Els parcials que el sostre ha tret NO s\xc3\xb3n en aquesta llista.");
            }
            const bool hadBanner = warnLbl.getText().isNotEmpty();
            warnLbl.setText (w, juce::dontSendNotification);
            warnLbl.setColour (juce::Label::textColourId,
                               proc->isSpectrumTruncated() ? juce::Colour (0xffe0453f)
                                                           : juce::Colour (0xffffb020));
            // The banner's row is zero-height when there is nothing to say, so its
            // appearing has to re-run the layout or the text would land in a 0 px strip
            // and the truncation warning - the one message that must never be missed -
            // would be invisible.
            if (hadBanner != w.isNotEmpty()) resized();
        }

        void applyLanguage()
        {
            if (trF == nullptr) return;
            headLbl.setText (trF ("PARTIAL AMPLITUDES", "AMPLITUDS DELS PARCIALS"), juce::dontSendNotification);
            onT.setButtonText ({});
            resetAllB.setButtonText (trF ("Reset all", "Reinicia-ho tot"));
            resetAllB.setTooltip (trF ("Drop every per-partial edit, including the ones whose partial is not in this spectrum right now. One Ctrl+Z brings them all back.",
                                       "Descarta totes les edicions per parcial, incloses les dels parcials que ara no hi s\xc3\xb3n. Un sol Ctrl+Z les torna totes."));
            onT.setTooltip (trF ("Manual per-partial amplitudes. The SAME switch as the one under the pencil - one control in two places. Dragging a knob turns it on for you if it is off, because otherwise the drag would be stored and never sound. While it is on, SLOPE is frozen.",
                                 "Amplituds manuals per parcial. \xc3\x89s EL MATEIX interruptor que el de sota el llapis - un control en dos llocs. Arrossegar un control l'activa si est\xc3\xa0 apagat, perqu\xc3\xa8 si no l'arrossegament es desaria i no sonaria mai. Mentre estigui actiu, el PENDENT queda congelat."));
            // The two things that surprise everyone, on screen rather than in a tooltip
            // nobody opens: they are not bugs and they are not reversible by guessing.
            factsLbl.setText (trF (
                "Pulling one partial DOWN lifts the others: the voice rebuilds its normalisation from these targets every sample, so this redistributes the sound, it does not attenuate it.\n"
                "A partial at MUTE is still in the template - it comes straight back with Ctrl+Z, or when manual mode goes off. Nothing here removes a partial.\n"
                "SHIFT+click a knob, then SHIFT+click another: both and everything between are selected, and a drag moves the whole selection.",
                "Abaixar un parcial PUJA els altres: la veu reconstrueix la normalitzaci\xc3\xb3 des d'aquests objectius a cada mostra, aix\xc3\xad que aix\xc3\xb2 redistribueix el so, no l'atenua.\n"
                "Un parcial en MUT segueix a la plantilla - torna de seguida amb Ctrl+Z, o quan apagues el mode manual. Res d'aqu\xc3\xad no treu cap parcial.\n"
                "SHIFT+clic en un control i SHIFT+clic en un altre: se seleccionen tots dos i tot el que hi ha entremig, i arrossegar mou tota la selecci\xc3\xb3."),
                juce::dontSendNotification);
            refreshText();
        }

        void layoutGrid()
        {
            grid.setSize (juce::jmax (60, view.getWidth() - (view.isVerticalScrollBarShown() ? 10 : 0)),
                          grid.contentHeight());
            // The height depends on the column count, which depends on the width the
            // scrollbar leaves: settle it in a second pass or a spectrum that only just
            // needs scrolling ends one row short of its last partials.
            grid.setSize (juce::jmax (60, view.getWidth() - (view.isVerticalScrollBarShown() ? 10 : 0)),
                          grid.contentHeight());
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 8);
            auto head = r.removeFromTop (26);
            resetAllB.setBounds (head.removeFromRight (juce::jmax (96, fgfit::textButtonWidth (
                                     resetAllB.getButtonText(), 22, false))).reduced (0, 2));
            head.removeFromRight (8);
            onT.setBounds (head.removeFromRight (58).reduced (0, 2));
            head.removeFromRight (8);
            headLbl.setBounds (head);
            r.removeFromTop (2);
            statLbl.setBounds (r.removeFromTop (16));
            // Three paragraphs, in two languages, at whatever width the window is: a
            // fixed 52 px was 23 short of both, so the last line was cut off inside the
            // panel with nothing to say it had been.
            factsLbl.setBounds (r.removeFromTop (juce::jmax (52, fgfit::labelHeight (factsLbl, r.getWidth()))));
            warnLbl.setBounds (r.removeFromTop (warnLbl.getText().isEmpty() ? 0 : 32));
            r.removeFromTop (4);
            view.setBounds (r);
            layoutGrid();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff0e1118));
            g.setColour (juce::Colour (0xff5cb8ff).withAlpha (0.25f));
            g.drawHorizontalLine (view.getY() - 3, (float) view.getX(), (float) view.getRight());
        }

        // Clicking anywhere empty drops the selection - the same "click outside to
        // deselect" every list in this plugin has.
        void mouseDown (const juce::MouseEvent&) override
        { grid.model.clearSelection(); grid.repaint(); }
    };

    //==========================================================================
    // THE PRESET PLAYGROUND.
    //
    // Four presets on the corners of a rectangle and a draggable point inside it. The
    // point is nothing but a view onto the pgX / pgY PARAMETERS: dragging writes them
    // through beginChangeGesture / setValueNotifyingHost / endChangeGesture so the host
    // records the move, and automation played back moves the point here - or, with this
    // window shut, moves everything it drives anyway, because the blending itself lives
    // in the processor's 60 Hz poll and not in this component.
    //
    // The COLOURS are the feature's vocabulary: each corner owns one, the point is
    // tinted by the weights it is currently made of, and the corner that owns every
    // DISCRETE setting (engine, phase mode, manual amps, realtime arming - things that
    // cannot be averaged) wears a ring. Without that ring "why did the engine change
    // when I crossed the middle?" has no visible answer at all.
    static juce::Colour playgroundCornerColour (int i)
    {
        static const juce::Colour c[4] { juce::Colour (0xff5cb8ff), juce::Colour (0xffffb020),
                                         juce::Colour (0xff4ed08a), juce::Colour (0xffe07ad0) };
        return c[i & 3];
    }

    struct PlaygroundPad : juce::Component, juce::SettableTooltipClient
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (const char*, const char*)> trF;
        std::function<void()> onMoved;
        bool dragging = false;

        // The square the point actually travels in. Inset so a corner's dot and its
        // percentage are drawn INSIDE the component and never clipped by its edge.
        juce::Rectangle<float> padArea() const
        { return getLocalBounds().toFloat().reduced (26.0f); }

        void writePoint (juce::Point<float> p)
        {
            const auto a = padArea();
            const float x = a.getWidth()  > 1.0f ? juce::jlimit (0.0f, 1.0f, (p.x - a.getX()) / a.getWidth())  : 0.0f;
            const float y = a.getHeight() > 1.0f ? juce::jlimit (0.0f, 1.0f, (p.y - a.getY()) / a.getHeight()) : 0.0f;
            if (proc == nullptr) return;
            for (auto* id : { "pgX", "pgY" })
                if (auto* q = proc->apvts.getParameter (id))
                    q->setValueNotifyingHost (juce::String (id) == "pgX" ? x : y);
            repaint();
            if (onMoved) onMoved();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (proc == nullptr) return;
            // ONE gesture per drag, opened before the first value goes out: a host in
            // touch/latch mode only starts writing automation when it sees the gesture,
            // and only stops when it sees the end - so without this pair a recorded drag
            // is either nothing at all or never stops recording.
            dragging = true;
            for (auto* id : { "pgX", "pgY" })
                if (auto* q = proc->apvts.getParameter (id)) q->beginChangeGesture();
            writePoint (e.position);
        }
        void mouseDrag (const juce::MouseEvent& e) override { if (dragging) writePoint (e.position); }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (! dragging || proc == nullptr) return;
            dragging = false;
            for (auto* id : { "pgX", "pgY" })
                if (auto* q = proc->apvts.getParameter (id)) q->endChangeGesture();
        }

        juce::String getTooltip() override
        {
            if (trF == nullptr) return {};
            return trF ("Drag the point. Each corner is a preset and the point is the blend of them - "
                        "the SOUND only, never the tuning. Its position is the automatable pgX / pgY "
                        "parameters, so a move recorded in the DAW plays back with this window shut.",
                        "Arrossega el punt. Cada cantonada \xc3\xa9s un preset i el punt \xc3\xa9s la seva barreja - "
                        "nom\xc3\xa9s el SO, mai l'afinaci\xc3\xb3. La seva posici\xc3\xb3 s\xc3\xb3n els par\xc3\xa0metres "
                        "automatitzables pgX / pgY, o sigui que un moviment gravat al DAW es reprodueix amb "
                        "aquesta finestra tancada.");
        }

        void paint (juce::Graphics& g) override
        {
            const auto a = padArea();
            g.setColour (juce::Colour (0xff141a24));
            g.fillRoundedRectangle (a, 6.0f);
            g.setColour (juce::Colour (0xff55557a));
            g.drawRoundedRectangle (a.reduced (0.5f), 6.0f, 1.0f);
            // Quarter lines: the middle of each edge and the centre, so the eye can see
            // where "half way" is without measuring.
            g.setColour (juce::Colour (0xff2a3140));
            g.drawLine (a.getCentreX(), a.getY(), a.getCentreX(), a.getBottom(), 1.0f);
            g.drawLine (a.getX(), a.getCentreY(), a.getRight(), a.getCentreY(), 1.0f);

            if (proc == nullptr) return;

            double w[FractalAdditiveAudioProcessor::kPlaygroundCorners];
            proc->getPlaygroundWeights (w);
            const int winner = proc->getPlaygroundNearestCorner();
            const bool active = proc->isPlaygroundActive();

            const int editing = proc->getPlaygroundEditCorner();
            for (int i = 0; i < 4; ++i)
            {
                // The DISPLAY name: a corner carrying unsaved edits reads "name*" here,
                // which is the one place the user is looking while dragging the point.
                const auto name = proc->getPlaygroundCornerDisplayName (i);
                const juce::Point<float> c { (i & 1) ? a.getRight() : a.getX(),
                                             (i & 2) ? a.getBottom() : a.getY() };
                const auto col = playgroundCornerColour (i);
                const float ww = (float) (active ? w[i] : (name.isNotEmpty() ? 0.0 : 0.0));
                // Radius grows with the weight - the size of a corner's dot IS how much
                // of it you are hearing. Empty corners stay a hollow outline.
                const float rad = name.isEmpty() ? 7.0f : 7.0f + 13.0f * ww;
                if (name.isEmpty())
                {
                    g.setColour (juce::Colour (0xff3a4152));
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c), 1.5f);
                }
                else
                {
                    g.setColour (col.withAlpha (0.30f + 0.70f * ww));
                    g.fillEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c));
                }
                // The corner every DISCRETE setting is coming from - a MORPH-mode
                // concept only. In MIX mode every corner is its own engine and no
                // setting comes "from" anywhere, so the ring would be a lie there.
                if (i == winner && active && ! proc->isPlaygroundMixMode())
                {
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f + 8.0f, rad * 2.0f + 8.0f).withCentre (c), 2.0f);
                }
                // The corner the main window's controls are standing in for: an amber
                // ring, the same colour the Edit button turns, so "where am I typing?"
                // has an answer on the pad itself and not only in the status text.
                if (i == editing)
                {
                    g.setColour (juce::Colour (0xffffc95c));
                    g.drawEllipse (juce::Rectangle<float> (rad * 2.0f + 14.0f, rad * 2.0f + 14.0f).withCentre (c), 2.5f);
                }

                // Name + percentage, pushed OUTWARDS from the corner so it never sits
                // under the point.
                auto lab = juce::Rectangle<float> (150.0f, 15.0f)
                               .withCentre ({ c.x + ((i & 1) ? -58.0f : 58.0f),
                                              c.y + ((i & 2) ? 17.0f : -17.0f) });
                g.setColour (name.isEmpty() ? juce::Colour (0xff6a7285) : col);
                g.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
                juce::String t;
                t << (char) ('A' + i) << "  ";
                t << (name.isEmpty() ? (trF ? trF ("empty", "buida") : juce::String ("empty")) : name);
                if (active && ! name.isEmpty()) t << "  " << juce::String (w[i] * 100.0, 0) << "%";
                g.drawText (t, lab.toNearestInt(),
                            (i & 1) ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
            }

            // The point, tinted by what it is made of right now.
            const float px = a.getX() + a.getWidth()  * proc->apvts.getRawParameterValue ("pgX")->load();
            const float py = a.getY() + a.getHeight() * proc->apvts.getRawParameterValue ("pgY")->load();
            float r = 0.0f, gg = 0.0f, b = 0.0f, sum = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                const auto col = playgroundCornerColour (i);
                r  += (float) w[i] * col.getFloatRed();
                gg += (float) w[i] * col.getFloatGreen();
                b  += (float) w[i] * col.getFloatBlue();
                sum += (float) w[i];
            }
            const auto pc = sum > 0.001f ? juce::Colour::fromFloatRGBA (r / sum, gg / sum, b / sum, 1.0f)
                                         : juce::Colour (0xff8f96a8);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre ({ px, py }));
            g.setColour (pc);
            g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ px, py }));
            g.setColour (juce::Colours::white);
            g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ px, py }), 2.0f);
        }
    };

    struct PlaygroundPanel : juce::Component, private juce::Timer
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (const char*, const char*)> trF;

        juce::Label        headLbl, statLbl;
        juce::Label        cornerLbl[4];
        RefreshingComboBox cornerBox[4];
        juce::TextButton   cornerClear[4];
        // EDIT: hands the MAIN editor's controls to this corner (see
        // FractalAdditiveAudioProcessor::beginPlaygroundCornerEdit for the ownership
        // rules). One corner at a time; the button of the corner being edited reads
        // "OK" and lights up, because "how do I get my own patch back" must be visible
        // from the same place the trip started.
        juce::TextButton   cornerEdit[4];
        // Set by the editor: edit mode began or ended - the main window's preset field
        // and Save button have to change with it.
        std::function<void()> onEditModeChanged;
        // The playground's two modes, as a radio pair driving the pgMode PARAMETER
        // (host-automatable; the buttons are only a view onto it, like the pad onto
        // pgX/pgY): MORPH blends one patch, MIX sounds all four corners at once and
        // the point becomes their volume balance.
        juce::TextButton   modeBtn[2];
        // MIX's per-corner polyphony (the "pgVoices" parameter). VISIBLE by design:
        // five engines at 32 voices each measured ~3x a 48 kHz/512 block on dense
        // spectra, and the fidelity rule is that a CPU limit is a control the user
        // holds, never a cap the code hides.
        juce::Label        voicesLbl;
        juce::Slider       voicesSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> voicesAtt;
        PlaygroundPad      pad;

        // Called once the processor pointer is set (attachments need the APVTS).
        void attachVoices()
        {
            if (proc == nullptr || voicesAtt != nullptr) return;
            voicesAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                            proc->apvts, "pgVoices", voicesSlider);
        }
        // Its own TooltipWindow, parented HERE: the editor's serves only its own
        // subtree, so in a separate desktop window every hover read-out is invisible
        // without this (the same trap PartialAmpPanel documents).
        juce::TooltipWindow tooltips { this, 350 };
        float lastX = -1.0f, lastY = -1.0f;
        int   lastAssigned = -1, lastWinner = -2, lastMode = -1;
        // Adaptive-corner state the status line reports: count + whether a refusal is
        // showing. In the change detection because a corner can be SWAPPED without the
        // assigned count moving, and a stale line would claim a follower that is gone.
        int   lastAdaptSig = -1;
        // Which corner is being edited + which carry a "*", as one number (see refresh).
        int   lastEditSig = -1;

        PlaygroundPanel()
        {
            headLbl.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
            headLbl.setColour (juce::Label::textColourId, juce::Colour (0xff5cb8ff));
            headLbl.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (headLbl);

            statLbl.setFont (juce::Font (juce::FontOptions (12.0f)));
            statLbl.setColour (juce::Label::textColourId, juce::Colour (0xffb8c0d0));
            statLbl.setJustificationType (juce::Justification::topLeft);
            statLbl.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (statLbl);

            for (int i = 0; i < 4; ++i)
            {
                cornerLbl[i].setText (juce::String::charToString ((juce::juce_wchar) ('A' + i)),
                                      juce::dontSendNotification);
                cornerLbl[i].setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));
                cornerLbl[i].setColour (juce::Label::textColourId, playgroundCornerColour (i));
                cornerLbl[i].setJustificationType (juce::Justification::centred);
                // A single bold letter in a 16 px box: JUCE's default 5 px side border
                // left 6 px for a glyph that measures 8-10, so even "A" was condensed.
                cornerLbl[i].setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
                cornerLbl[i].setInterceptsMouseClicks (false, false);
                addAndMakeVisible (cornerLbl[i]);

                cornerBox[i].setColour (juce::ComboBox::outlineColourId, playgroundCornerColour (i).withAlpha (0.6f));
                cornerBox[i].setMouseClickGrabsKeyboardFocus (false);
                cornerBox[i].onBeforePopup = [this, i] { refillCorner (i); };
                cornerBox[i].onChange = [this, i]
                {
                    if (proc == nullptr) return;
                    juce::String err;
                    // Item 1 is "(empty)" - selecting it CLEARS the corner. Anything else
                    // is a preset name read straight off the folder listing.
                    const auto name = cornerBox[i].getSelectedId() <= 1 ? juce::String() : cornerBox[i].getText();
                    // Dropping a different preset here REPLACES the corner from disk, so
                    // it ends any edit of it (and throws the unsaved edits away - the one
                    // explicit act that does, see rule 4). Tell the main window.
                    const bool wasEditing = proc->getPlaygroundEditCorner() == i;
                    proc->assignPlaygroundCorner (i, name, err);
                    refresh (true);
                    if (wasEditing && onEditModeChanged) onEditModeChanged();
                };
                addAndMakeVisible (cornerBox[i]);

                cornerEdit[i].setButtonText ("E");
                cornerEdit[i].setMouseClickGrabsKeyboardFocus (false);
                cornerEdit[i].onClick = [this, i]
                {
                    if (proc == nullptr) return;
                    if (proc->getPlaygroundEditCorner() == i) proc->endPlaygroundCornerEdit();
                    else                                      proc->beginPlaygroundCornerEdit (i);
                    refresh (true);
                    if (onEditModeChanged) onEditModeChanged();
                };
                addAndMakeVisible (cornerEdit[i]);

                cornerClear[i].setButtonText ("X");
                cornerClear[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f).withAlpha (0.5f));
                cornerClear[i].setMouseClickGrabsKeyboardFocus (false);
                cornerClear[i].onClick = [this, i]
                {
                    if (proc == nullptr) return;
                    juce::String err;
                    // assignPlaygroundCorner ends edit mode on this corner by itself (it
                    // is what discards unsaved edits), but the MAIN window still has to
                    // be told, or it would keep showing a frozen preset field for a
                    // corner that no longer exists.
                    const bool wasEditing = proc->getPlaygroundEditCorner() == i;
                    proc->assignPlaygroundCorner (i, {}, err);
                    cornerBox[i].setSelectedId (1, juce::dontSendNotification);
                    refresh (true);
                    if (wasEditing && onEditModeChanged) onEditModeChanged();
                };
                addAndMakeVisible (cornerClear[i]);
            }

            for (int m = 0; m < 2; ++m)
            {
                modeBtn[m].setMouseClickGrabsKeyboardFocus (false);
                modeBtn[m].onClick = [this, m]
                {
                    if (proc == nullptr) return;
                    // Through the parameter, with a gesture, so the host records the
                    // switch and Ctrl+Z steps back over it like any other control.
                    if (auto* p = proc->apvts.getParameter ("pgMode"))
                    {
                        p->beginChangeGesture();
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) m));
                        p->endChangeGesture();
                    }
                    refresh (true);
                };
                addAndMakeVisible (modeBtn[m]);
            }

            voicesLbl.setFont (juce::Font (juce::FontOptions (12.0f)));
            voicesLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            voicesLbl.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (voicesLbl);
            voicesSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 18);
            voicesSlider.setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (voicesSlider);

            pad.onMoved = [this] { refresh (false); };
            addAndMakeVisible (pad);
            startTimerHz (30);
        }

        void refillCorner (int i)
        {
            if (proc == nullptr) return;
            // The DISPLAY name, so a corner carrying unsaved edits shows its "*" here
            // too. The box's ITEMS stay the plain folder listing - the "*" is a state of
            // this corner, not the name of a preset anyone could pick.
            const auto cur = proc->getPlaygroundCornerDisplayName (i);
            cornerBox[i].clear (juce::dontSendNotification);
            cornerBox[i].addItem (trF ? trF ("(empty)", "(buida)") : juce::String ("(empty)"), 1);
            int id = 2;
            for (const auto& n : proc->getPresetNames())
                cornerBox[i].addItem (n, id++);
            if (cur.isNotEmpty()) cornerBox[i].setText (cur, juce::dontSendNotification);
            else                  cornerBox[i].setSelectedId (1, juce::dontSendNotification);
        }

        void applyLanguage()
        {
            headLbl.setText (trF ? trF ("PRESET PLAYGROUND", "LABORATORI DE PRESETS")
                                 : juce::String ("PRESET PLAYGROUND"), juce::dontSendNotification);
            modeBtn[0].setButtonText (trF ? trF ("MORPH", "MORPH") : juce::String ("MORPH"));
            modeBtn[1].setButtonText (trF ? trF ("MIX", "MESCLA") : juce::String ("MIX"));
            modeBtn[0].setTooltip (trF ? trF ("Morph mode: the corners blend into ONE patch - every control and "
                                              "the spectrum move to the point.",
                                              "Mode morph: les cantonades es fonen en UN sol patch - cada control "
                                              "i l'espectre segueixen el punt.")
                                       : juce::String());
            modeBtn[1].setTooltip (trF ? trF ("Mix mode: the four presets SOUND AT ONCE, each as its own full "
                                              "engine, and the point balances their volumes (equal power). Your "
                                              "own patch is untouched and comes back when you leave.",
                                              "Mode mescla: els quatre presets SONEN ALHORA, cadascun amb el seu "
                                              "propi motor complet, i el punt equilibra els seus volums (pot\xc3\xa8ncia "
                                              "constant). El teu patch no es toca i torna quan en surts.")
                                       : juce::String());
            voicesLbl.setText (trF ? trF ("Mix voices per corner", "Veus de mescla per cantonada")
                                   : juce::String ("Mix voices per corner"), juce::dontSendNotification);
            voicesSlider.setTooltip (trF ? trF ("Polyphony of EACH corner engine in mix mode. Mix runs up to five "
                                                "engines at once (four corners plus your patch tracking "
                                                "underneath), so this is the CPU trade-off - a visible one, never "
                                                "a hidden cap. 8 covers normal playing; raise it if your CPU can.",
                                                "Polifonia de CADA motor de cantonada en mode mescla. La mescla fa "
                                                "anar fins a cinc motors alhora (quatre cantonades m\xc3\xa9s el teu "
                                                "patch seguint per sota), o sigui que aix\xc3\xb2 \xc3\xa9s l'equilibri "
                                                "de CPU - visible, mai un l\xc3\xadmit amagat. 8 cobreix el joc "
                                                "normal; puja-ho si la teva CPU pot.")
                                         : juce::String());
            for (int i = 0; i < 4; ++i)
            {
                refillCorner (i);
                cornerBox[i].setTooltip (trF ? trF ("The preset on this corner. Drop up to four; two make a line, "
                                                    "three a triangle, four the whole rectangle. An empty corner "
                                                    "contributes nothing at all.",
                                                    "El preset d'aquesta cantonada. Posa'n fins a quatre; dos fan una "
                                                    "l\xc3\xadnia, tres un triangle, quatre tot el rectangle. Una "
                                                    "cantonada buida no hi aporta res.")
                                              : juce::String());
                cornerClear[i].setTooltip (trF ? trF ("Take the preset off this corner.",
                                                      "Treu el preset d'aquesta cantonada.")
                                               : juce::String());
                cornerEdit[i].setTooltip (trF ? trF ("Edit this corner. The MAIN window's controls become this "
                                                     "corner's, live: what you change is heard in the blend "
                                                     "straight away and the corner's name gets a * to say it no "
                                                     "longer matches the preset it came from. Your own patch is "
                                                     "kept aside and comes back untouched when you press OK. "
                                                     "Save (main window) stores the corner under a new name.",
                                                     "Edita aquesta cantonada. Els controls de la finestra "
                                                     "PRINCIPAL passen a ser els d'aquesta cantonada, en directe: "
                                                     "el que canvi\xc3\xafs se sent a la barreja de seguida i el nom "
                                                     "de la cantonada rep un * per dir que ja no coincideix amb el "
                                                     "preset d'on ve. El teu patch es guarda a part i torna intacte "
                                                     "quan prems OK. Desar (finestra principal) guarda la cantonada "
                                                     "amb un nom nou.")
                                              : juce::String());
            }
            lastWinner = -2;   // force the status line to be rebuilt in the new language
            refresh (true);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10);
            // The mode pair shares the header row, so the pad loses no height to it.
            auto head = r.removeFromTop (20);
            modeBtn[1].setBounds (head.removeFromRight (64).reduced (0, 1));
            head.removeFromRight (3);
            modeBtn[0].setBounds (head.removeFromRight (64).reduced (0, 1));
            head.removeFromRight (6);
            headLbl.setBounds (head);
            r.removeFromTop (4);
            statLbl.setBounds (r.removeFromBottom (58));
            r.removeFromBottom (4);
            // MIX's voice budget rides just above the status text; the pad keeps the rest.
            {
                auto vr = r.removeFromBottom (20);
                voicesSlider.setBounds (vr.removeFromRight (96).reduced (0, 1));
                vr.removeFromRight (4);
                voicesLbl.setBounds (vr);
                r.removeFromBottom (2);
            }

            const auto row = [this] (int lo, int hi, juce::Rectangle<int> band)
            {
                auto left  = band.removeFromLeft (band.getWidth() / 2).reduced (2, 0);
                auto right = band.reduced (2, 0);
                const auto lay = [this] (int i, juce::Rectangle<int> cell)
                {
                    cornerLbl[i].setBounds (cell.removeFromLeft (16));
                    // Right to left: X (take the preset off), then E (edit it), then the
                    // name box gets everything left. The two buttons are sized from the
                    // RIGHT so adding E could only ever cost the box width - never push a
                    // control off the end of the row.
                    cornerClear[i].setBounds (cell.removeFromRight (24).reduced (1, 1));
                    cell.removeFromRight (3);
                    cornerEdit[i].setBounds (cell.removeFromRight (26).reduced (1, 1));
                    cell.removeFromRight (3);
                    cornerBox[i].setBounds (cell);
                };
                lay (lo, left);
                lay (hi, right);
            };
            row (0, 1, r.removeFromTop (24));
            row (2, 3, r.removeFromBottom (24));
            pad.setBounds (r.reduced (0, 4));
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0e1118)); }

        // Automation moves the point with this window open, and a preset dropped on a
        // corner changes what the pad has to draw. Both arrive from OUTSIDE this
        // component, so it polls rather than waiting to be told.
        void timerCallback() override { refresh (false); }

        void refresh (bool force)
        {
            if (proc == nullptr) return;
            const float x = proc->apvts.getRawParameterValue ("pgX")->load();
            const float y = proc->apvts.getRawParameterValue ("pgY")->load();
            const int   n = proc->getPlaygroundAssignedCount();
            const int   wn = proc->getPlaygroundNearestCorner();
            const int   adSig = proc->getPlaygroundAdaptiveCount() * 2
                              + (proc->getPlaygroundAdaptRefusal().isNotEmpty() ? 1 : 0);
            const int   mode  = proc->isPlaygroundMixMode() ? 1 : 0;
            // WHICH corner is being edited, and which corners carry unsaved edits. In the
            // change detector because the "*" can appear with nothing else moving at all
            // (a knob turned in the main window is invisible to every other signal here),
            // and a status line that only redraws on a drag would deny the edit exists.
            int editSig = proc->getPlaygroundEditCorner() + 1;
            for (int i = 0; i < 4; ++i)
                if (proc->isPlaygroundCornerEdited (i)) editSig += 16 << i;
            if (! force && x == lastX && y == lastY && n == lastAssigned && wn == lastWinner
                && adSig == lastAdaptSig && mode == lastMode && editSig == lastEditSig)
                return;
            lastX = x; lastY = y; lastAssigned = n; lastWinner = wn; lastAdaptSig = adSig;
            lastMode = mode;
            const bool editChanged = editSig != lastEditSig;
            lastEditSig = editSig;

            // The corner rows: only an ASSIGNED corner has anything to edit, and only
            // while the playground is really driving (two corners) does editing one mean
            // anything - the blend is what you hear the edit in.
            {
                const int ed = proc->getPlaygroundEditCorner();
                for (int i = 0; i < 4; ++i)
                {
                    const bool can = proc->getPlaygroundCornerName (i).isNotEmpty() && n >= 2;
                    cornerEdit[i].setEnabled (can);
                    cornerEdit[i].setButtonText (i == ed ? juce::String ("OK") : juce::String ("E"));
                    cornerEdit[i].setColour (juce::TextButton::buttonColourId,
                                             i == ed ? juce::Colour (0xffffc95c) : juce::Colour (0xff2d6da8).withAlpha (0.55f));
                    cornerEdit[i].setColour (juce::TextButton::textColourOffId,
                                             i == ed ? juce::Colours::black : juce::Colours::white);
                    // The "*" lives on the name, so the box has to be re-read whenever the
                    // edit signature moved - not only when a corner is (re)assigned.
                    if (editChanged)
                    {
                        const auto disp = proc->getPlaygroundCornerDisplayName (i);
                        if (disp.isNotEmpty() && cornerBox[i].getText() != disp)
                            cornerBox[i].setText (disp, juce::dontSendNotification);
                    }
                }
            }

            // The mode pair reads like a radio group: the mode in force is lit.
            for (int m = 0; m < 2; ++m)
            {
                modeBtn[m].setColour (juce::TextButton::buttonColourId,
                                      m == mode ? juce::Colour (0xff2d6da8) : juce::Colour (0xff1a2130));
                modeBtn[m].setColour (juce::TextButton::textColourOffId,
                                      m == mode ? juce::Colours::white : juce::Colour (0xff8f96a8));
            }
            // The voice budget only means anything in mix mode; a control that quietly
            // does nothing is the trap this project keeps refusing, so it greys out.
            // MONOPHONIC GLIDE takes it away for the same reason and SAYS SO: while mono
            // is on every corner engine runs one voice, and a control still showing 8
            // would be claiming a polyphony that is not happening.
            // (The status line below says it in words - a label rewritten to carry the
            // reason would have had to grow, and this row has no width to give.)
            const bool monoNow = proc != nullptr
                              && proc->apvts.getRawParameterValue ("monoGlide")->load() > 0.5f;
            voicesSlider.setEnabled (mode == 1 && ! monoNow);
            voicesLbl.setAlpha (mode == 1 && ! monoNow ? 1.0f : 0.45f);

            juce::String s;
            if (n < 2)
            {
                s << (trF ? trF ("Drop a preset on at least TWO corners. With one there is nothing to blend "
                                 "with, so the playground stays out of the way and leaves your patch alone.",
                                 "Posa un preset a com a m\xc3\xadnim DUES cantonades. Amb una no hi ha res amb "
                                 "qu\xc3\xa8 barrejar, aix\xc3\xad que el laboratori no toca res i et deixa el patch "
                                 "tal com el tens.")
                          : juce::String());
            }
            else
            {
                double w[FractalAdditiveAudioProcessor::kPlaygroundCorners];
                proc->getPlaygroundWeights (w);
                s << (mode == 1 ? (trF ? trF ("Mix: ", "Mescla: ") : juce::String ("Mix: "))
                                : (trF ? trF ("Blend: ", "Barreja: ") : juce::String ("Blend: ")));
                bool first = true;
                for (int i = 0; i < 4; ++i)
                    if (proc->getPlaygroundCornerName (i).isNotEmpty())
                    {
                        if (! first) s << " + ";
                        first = false;
                        s << (char) ('A' + i) << " " << juce::String (w[i] * 100.0, 0) << "%";
                    }
                s << "\n";
                if (mode == 1)
                {
                    // MIX: no winner corner, no morphed controls - four whole engines
                    // and a balance. Say exactly that, and that the user's patch waits
                    // untouched underneath.
                    s << (trF ? trF ("All four corners SOUND AT ONCE (equal-power balance). Your own "
                                     "patch is silent but untouched - it comes back exactly as it was.",
                                     "Totes les cantonades SONEN ALHORA (balan\xc3\xa7 de pot\xc3\xa8ncia "
                                     "constant). El teu patch calla per\xc3\xb2 no es toca - torna "
                                     "exactament com era.")
                              : juce::String());
                    // MONOPHONIC GLIDE reaches every corner engine, because there is ONE
                    // note stream and they all hear it. Said here rather than left for
                    // the user to notice: the polyphony control they can see is disabled
                    // and a silent override is exactly what this panel refuses to do.
                    if (monoNow)
                        s << (trF ? trF (" Monophonic glide is ON, so EVERY corner engine runs one voice "
                                         "and glides the same way - \"Mix voices per corner\" waits.",
                                         " El glide monof\xc3\xb2nic \xc3\xa9s actiu, aix\xc3\xad que CADA motor de "
                                         "cantonada porta una sola veu i llisca igual - \"Veus de mescla "
                                         "per cantonada\" espera.")
                                  : juce::String());
                    s << "\n";
                }
                else if (wn >= 0)
                    s << (trF ? trF ("Settings that cannot be averaged (engine, phase, prime mode, manual amps, "
                                     "realtime) come from ", "Els ajustos que no es poden promitjar (motor, fase, mode "
                                     "primer, amplituds manuals, temps real) venen de ")
                              : juce::String ("From "))
                      << (char) ('A' + wn) << ".\n";
                s << (trF ? trF ("The tuning is never touched.", "L'afinaci\xc3\xb3 no es toca mai.")
                          : juce::String());
                if (proc->isPlaygroundSuspendingRealtime())
                    s << (trF ? trF ("  REALTIME is paused while the playground drives the spectrum; it comes "
                                     "back when a corner is cleared.",
                                     "  TEMPS REAL est\xc3\xa0 en pausa mentre el laboratori porta l'espectre; torna "
                                     "quan buidis una cantonada.")
                              : juce::String());
                // Corners saved with the follower armed keep following INSIDE the blend -
                // the one thing that would otherwise look broken ("my adaptive preset
                // stopped adapting") or invisible ("why is the pad moving by itself?").
                if (proc->getPlaygroundAdaptiveCount() > 0)
                    s << (trF ? trF ("  Corners saved with REALTIME keep adapting to what you play, "
                                     "inside the blend.",
                                     "  Les cantonades desades amb TEMPS REAL segueixen adaptant-se al que "
                                     "toques, dins la barreja.")
                              : juce::String());
                if (proc->getPlaygroundAdaptRefusal().isNotEmpty())
                    s << "\n" << proc->getPlaygroundAdaptRefusal();
                // EDIT MODE is the loudest thing this line can say: the main window's
                // controls are somebody else's patch right now, and a user who does not
                // know that will "fix" a corner thinking it is their own sound.
                if (const int ed = proc->getPlaygroundEditCorner(); ed >= 0)
                {
                    s << "\n";
                    s << (trF ? trF ("EDITING ", "EDITANT ") : juce::String ("EDITING "))
                      << (char) ('A' + ed) << ": "
                      << proc->getPlaygroundCornerDisplayName (ed) << ". ";
                    s << (trF ? trF ("The main window's controls are THIS CORNER now - your own patch is "
                                     "kept and comes back with OK. Save stores the corner under a new name; "
                                     "unsaved edits stay on the corner (the *) until you replace it.",
                                     "Els controls de la finestra principal s\xc3\xb3n ARA AQUESTA CANTONADA - "
                                     "el teu patch es guarda i torna amb OK. Desar guarda la cantonada amb un "
                                     "nom nou; els canvis no desats es queden a la cantonada (l'*) fins que "
                                     "la substitueixis.")
                              : juce::String());
                }
            }
            statLbl.setText (s, juce::dontSendNotification);
            pad.repaint();
        }
    };

    // Its own desktop window, like the partial-amplitude editor: the pad wants to be as
    // square and as large as the user cares to make it, which a panel clipped inside the
    // plugin's fixed 1200x650 could not be.
    struct PlaygroundWindow : juce::DocumentWindow
    {
        std::function<void()> onHide;
        std::function<bool (const juce::KeyPress&)> onKey;
        std::function<bool()>                       onKeyState;
        explicit PlaygroundWindow (const juce::String& name)
            : juce::DocumentWindow (name, juce::Colour (0xff0e1118), juce::DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (420, 380, 3000, 3000);
            setWantsKeyboardFocus (true);
        }
        void closeButtonPressed() override { if (onHide) onHide(); }
        // The computer keyboard has to go on playing while this window is in front -
        // the whole point of a morph pad is hearing it move (same forwarding, and the
        // same "one key map, not a second copy", as PartialAmpWindow).
        bool keyPressed (const juce::KeyPress& k) override
        { return (onKey != nullptr && onKey (k)) || juce::DocumentWindow::keyPressed (k); }
        bool keyStateChanged (bool isDown) override
        { return (onKeyState != nullptr && onKeyState()) || juce::DocumentWindow::keyStateChanged (isDown); }
    };

    // A REAL window, not an in-editor overlay: with up to 16384 knobs the useful size
    // is "as big as the screen", which a panel clipped to the plugin's own 1200x650
    // could never be. Resizable, and it keeps its size for the session.
    struct PartialAmpWindow : juce::DocumentWindow
    {
        std::function<void()> onHide;
        // The computer keyboard has to go on playing while this window is in front.
        // It is a separate DESKTOP window, so opening it moves the keyboard focus out
        // of the editor and the QWERTY-as-MIDI mapping simply stopped: you could not
        // hear the partial you were dragging. These two forward the key events to the
        // editor's OWN handlers - one mapping, not a second copy that could drift -
        // and returning true is also what stops a play key escaping to the host, where
        // the DAW's own computer-MIDI keyboard would fire a second, differently tuned
        // note (the "ghost note" the editor's keyPressed already guards against).
        std::function<bool (const juce::KeyPress&)> onKey;
        std::function<bool()>                       onKeyState;
        explicit PartialAmpWindow (const juce::String& name)
            : juce::DocumentWindow (name, juce::Colour (0xff0e1118), juce::DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (320, 240, 6000, 4000);
            // With nothing inside it wanting focus, key events reach this window itself
            // (ComponentPeer::getTargetForKeyPress falls back to the peer's component).
            setWantsKeyboardFocus (true);
        }
        void closeButtonPressed() override { if (onHide) onHide(); }
        bool keyPressed (const juce::KeyPress& k) override
        {
            return (onKey != nullptr && onKey (k)) || juce::DocumentWindow::keyPressed (k);
        }
        bool keyStateChanged (bool isDown) override
        {
            return (onKeyState != nullptr && onKeyState()) || juce::DocumentWindow::keyStateChanged (isDown);
        }
    };

    // Floating centre panel that explains WHY the current settings are silent.
    struct WarningOverlay : juce::Component
    {
        juce::String title, body;
        WarningOverlay() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf21a1420));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xffffb020));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            auto in = getLocalBounds().reduced (22, 14);
            g.setColour (juce::Colour (0xffffb020));
            g.setFont (juce::Font (juce::FontOptions (18.0f).withStyle ("Bold")));
            g.drawText (title, in.removeFromTop (26), juce::Justification::centred);
            in.removeFromTop (6);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawFittedText (body, in, juce::Justification::topLeft, 8, 1.0f);
        }
    };

    FractalAdditiveAudioProcessor& processorRef;

    std::unique_ptr<juce::LookAndFeel_V4> lnf;
    juce::TooltipWindow tooltipWindow { this, 350 }; // subtly quicker hover read-outs
    ScaleKeyboard keyboard;

    SteppedSlider baseNFader, timbreRepeatsFader;
    MagneticSlider fractalFader, harmonicsFader, primeLimitFader; // magnetic whole-value detents
    juce::Slider  slopeFader, gainFader, cutoffFader, resFader, fEnvAmtFader, pEnvAmtFader;
    juce::Slider  ampAdsr[4], filtAdsr[4], pitchAdsr[4];
    juce::ComboBox tuneSourceBox, tuneScopeBox; // beating mode: source + scope
    juce::ComboBox basePitchBox, phaseBox, primeModeBox, tuningBox, bendBox,
                   dissCountBox, dissModeBox, tuningBankBox;
    RefreshingComboBox presetBox;
    juce::Label presetDirtyMark; // "*" - some parameter drifted from the loaded preset
    void refreshPresetDirtyMark();
    // The corner the main window's controls were standing in for last time the UI was
    // refreshed, and the live patch's own preset name parked while that is true. The
    // preset field is FROZEN during a corner edit (it is not the loaded preset any more),
    // and this is what puts the real name back afterwards.
    int          lastEditCornerSeen = -1;
    bool         cornerEditUiActive = false;   // the TRANSITION guard, not a repeat of the above
    juce::String presetTextBeforeCornerEdit;
    juce::Rectangle<int> titleClickArea; // the title squares; clicking them plays the jingle
    juce::ToggleButton primeToggle[9], morphToggle;
    juce::ToggleButton temperToggle; // lives INSIDE the Custom Engine box (the scale it bypasses)
    std::unique_ptr<ButtonAttachment> temperAtt;
    // SPECTRUM panel: the two ENGINES as chips - the TITLE ROW of the one shared
    // engine box, built exactly like the modulator tab strip. Exactly one lit, the
    // selected one's colour worn by the whole panel, and the selected one's controls
    // the only ones inside the box (resized() gates visibility from engineInForce).
    // They READ engineInForce() and WRITE the parameters that decide it, so chip
    // selection and which engine SOUNDS are one thing - and switching away never
    // discards the loaded scale (only "timbreOn" goes down).
    // TEMPERED is NOT one of them: it is a mode of the harmonic engine, a tick box
    // laid out in BOTH views (see temperToggle), not a third engine.
    juce::TextButton engineChip[2];
    // Show the CUSTOM view while nothing sounds custom yet. The engines share ONE box,
    // so with no scale loaded the Load button and the intervals editor only exist
    // inside the custom view - if the CUSTOM chip refused the click until a scale
    // existed (as it used to), the only ways to CREATE one would be unreachable.
    // Cleared the moment any non-harmonic engine state is observed, and by the
    // HARMONIC chip: it may only ever hold the box open as a workspace, never
    // contradict a real engine change.
    bool customViewPinned = false;
    // ONE predicate for "which view is inside the shared engine box", used by
    // resized() (visibility + layout), paint() (dividers/sub-labels) and
    // applyEngineState() (chip lighting + panel colour) - three readers that must
    // never disagree about what is on screen.
    bool customViewShown() const
    {
        using E = FractalAdditiveAudioProcessor::EngineInForce;
        const auto st = processorRef.engineInForce();
        return st == E::CustomScale || (st == E::Harmonic && customViewPinned);
    }
    // What applyEngineState() dressed the panel in. Seeded with the harmonic engine's
    // blue - the state a fresh instance opens in - so the very first paint (which can
    // land before the first applyEngineState) is never a default black frame.
    juce::Colour         spectrumPanelColour { 0xff5cb8ff };
    void applyEngineState(); // dim/enable + chip look + repaint, from engineInForce()
    // SELECTING A VIEW IS SELECTING AN ENGINE. One function, used by both chips and by
    // every other route that changes which view is on screen (Remove, Random), so the
    // two can never be wired differently: showing the custom view puts the custom engine
    // in force and showing the harmonic view takes it out. Writes the PARAMETERS
    // engineInForce() reads (never a member), so the choice is automatable, undoable,
    // and identical whether it came from a click or from the host.
    void selectEngineView (bool custom);
    // The custom view's Load/Remove button wears whichever of its two jobs is live right
    // now - word, colour and tooltip together. In ONE place because a button that says
    // "Load" in green and DELETES the user's spectrum is the worst failure this panel
    // could have, and three call sites setting two of the three properties is how that
    // happens. Called by applyLanguage() and by the timer whenever the engine changes.
    void refreshCustomEngineButtons();
    // The "Load spectrum's tuning" button's word, enabled state and tooltip, in ONE
    // place for the same reason as refreshCustomEngineButtons: called by applyLanguage()
    // and the timer, it must never say "load" while the click would refuse (already
    // active), so the word and the enabled state have to move together.
    void refreshSpectrumTuningButton();
    // The Shepard beats box's honesty dress: while Sync is chosen but the host has no
    // BPM, the FREE ms value is what actually runs - the box turns amber and its
    // tooltip says so. Colour and tooltip move together, in ONE place, called by the
    // timer and by applyLanguage() (which resets lastShepNoBpm so the words re-render).
    void refreshShepardSyncState();
    // Repaint every control inside the SPECTRUM panel in the engine's colour. The panel
    // is ONE box now, so a fader still wearing its old per-cell hue would be the only
    // thing on screen still claiming the compartments are separate boxes.
    void recolourSpectrumControls (juce::Colour col);
    // The SLOPE (AMP DECAY) fader's colour, decided in ONE place - it is the only control
    // in the panel with a second, INERT state, and having two writers is what left it
    // mint green after a pencil on/off cycle. See the definition.
    void applySlopeFaderColour();
    std::unique_ptr<SliderAttachment> primeLimitFaderAtt;
    // SLOPE cell: the pencil that opens the per-partial amplitude window, and the ON/OFF
    // pill under it. The pill and the one INSIDE the window are two attachments to the
    // same "partialManual" parameter - that is what makes them one control in two
    // places, and it is why switching either one is automatable and undoable.
    PencilButton                      partialEditBtn;
    juce::ToggleButton                partialManualT;
    std::unique_ptr<ButtonAttachment> partialManualAtt;
    PartialAmpPanel                   partialPanel;
    // Declared AFTER the panel it shows, so it is torn down FIRST: the window holds the
    // panel as non-owned content and must not outlive it.
    std::unique_ptr<PartialAmpWindow> partialWindow;
    // The preset playground: the button that lives with the preset selector, its panel,
    // and its window. Same ordering rule - the window must be destroyed before the panel
    // it points at.
    juce::TextButton                 playgroundButton;
    PlaygroundPanel                  playgroundPanel;
    std::unique_ptr<PlaygroundWindow> playgroundWindow;
    // -1 = not applied yet, so the first timer tick (and every language switch, which
    // resets it) writes the slope fader's frozen/normal look and its bilingual tooltip.
    int   lastManualFrozenShown = -1;
    juce::Slider       morphTimeFader;
    juce::TextButton   menuButton; // top-bar menu: Init (reset all) / Panic - works in the VST too
    int                undoTxnTick = 0; // paces the tuning-bank combo refresh
    juce::String       lastSeenTimbreSource; // re-seeds intervalEdit when the timbre changes elsewhere
    // The envelopes share ONE compact box; these chips switch which one shows.
    juce::TextButton   envTabB[4];
    int                envTab = 0; // 0 = Amp, 1 = Pitch, 2 = Filter, 3 = Custom
    // CUSTOM tab = 6 modulators (3 envelopes + 3 LFOs), selected by modSelBox; the
    // faders/combos below re-attach to the selected modulator's parameters.
    juce::ComboBox     modSelBox;                   // Env 1..3 / LFO 1..3
    juce::Slider       custAdsr[4], custRangeFader; // env: A D S R + Range (LFO reuses Range)
    juce::Label        custAdsrLbl[4], lblCustRange;
    juce::ComboBox     lfoWaveBox, lfoRateModeBox, lfoPhaseModeBox;
    juce::Slider       lfoDivSlider, lfoHzSlider, lfoPhaseSlider;
    juce::Label        lblLfoDiv, lblLfoHz, lblLfoPhase;
    juce::ToggleButton modOnT;                      // per-modulator on/off
    juce::TextButton   modUnmapBtn;                 // unmap this modulator from everything
    juce::TextButton   lfoWaveEditBtn;              // opens the drawn-wave editor
    std::unique_ptr<SliderAttachment>   custAdsrAtt[4], custRangeAtt, lfoDivAtt, lfoHzAtt, lfoPhaseAtt;
    std::unique_ptr<ComboBoxAttachment> lfoWaveAtt, lfoRateModeAtt, lfoPhaseModeAtt;
    std::unique_ptr<ButtonAttachment>   modOnAtt;
    int  modSel = 0;                                // 0..2 env, 3..5 lfo
    WaveEditor waveEditor;                          // drawn-LFO wave overlay
    EnvEditor  envEditor;                           // the selected MODULATOR envelope, as a curve
    EnvEditor  voiceEnv;                            // the Amp / Pitch / Filter envelope, same editor
    int        voiceEnvTab = 0;                     // which one voiceEnv is showing
    int        lastDrawLfo = -1;                    // which LFO the docked wave editor is on
    void attachCustomTab();                         // rebind attachments to the selected modulator
    void showEnvMapMenu (const juce::String& paramID);
    juce::String currentModParamId (const juce::String& templateId) const; // modE1A -> modE2A...
    void addMapListener (juce::Component& c, const juce::String& paramID, bool dynamicMod = false);
    struct ParamMapListener : juce::MouseListener
    {
        FractalAdditiveAudioProcessorEditor* ed = nullptr;
        juce::String pid;
        // CUSTOM-tab controls are re-bound to whichever modulator is selected, so
        // their parameter id changes at runtime: those listeners resolve it live.
        bool dynamicMod = false;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown() && ed != nullptr)
                ed->showEnvMapMenu (dynamicMod ? ed->currentModParamId (pid) : pid);
        }
    };
    std::vector<std::unique_ptr<ParamMapListener>> mapListeners;
    juce::TextButton   rhythmBtn;  // top-bar: opens the spectrum->rhythm MIDI panel
    juce::TextButton   octDn, octUp, sprDn, sprUp; // OCTAVE cell: -1/+1 steppers
    juce::Label        octVal, sprVal;             // their value read-outs
    juce::TextButton saveTuningBtn;    // save the CURRENT tuning (any mode) as .scl to a chosen folder
    juce::TextButton tuningBankAddBtn;  // put the CURRENT tuning into the bank
    juce::TextButton recentOpenBtn;     // opens the floating Recent list
    juce::TextButton loadItsSpectrumBtn; // .scl written by this synth: restore its patch
    juce::TextButton loadPresetTuningBtn; // the loaded PRESET carries a different tuning
    // The current CUSTOM SPECTRUM was derived from a tuning (adapted to it, or loaded
    // from a scale file): this loads that tuning. Shown only while the provenance
    // exists; DISABLED (word changes to say so) when that tuning is already active,
    // because reloading what is active would only push a do-nothing undo step.
    juce::TextButton loadSpectrumTuningBtn;
    juce::String     lastSpectrumTuningSig; // skip the per-tick refresh when nothing moved
    // loadTimbreButton is ONE button with two jobs: "Load" while no custom spectrum
    // exists, "Remove" once one does. The Custom-Engine On/Off pill that used to sit
    // beside it is gone - with the engine chips deciding which engine is in force, an
    // Off pill was a second, quieter switch for the same thing, and the pair could
    // disagree on screen. Removing is the destructive half the pill never offered.
    juce::TextButton loadSclButton, clearSclButton, loadTimbreButton,
                     langButton, savePresetButton, dissBtn, dissSaveBtn, recentBtn[3], tuningBankLoadBtn,
                     tuningBankClearBtn;
    // RANDOM SPECTRUM: fires the automatable "specRandom" trigger (see the processor).
    // The button does not build anything itself - it writes the parameter, and the
    // processor's own poll builds - so a click and a host automation step take exactly
    // the same path and cannot drift apart.
    juce::TextButton randomSpectrumBtn;
    juce::Label          vibeLabel;   // "Vibe coded by" credit, left of the language button
    juce::HyperlinkButton vibeLink;   // "Asra3" -> Instagram
    juce::TextEditor periodEdit;
    // Absolute-table tunings only: the period is METADATA there (the 128 keys already fix
    // every pitch), so it is Fibló's estimate until the user types over it. Visible only
    // in table mode - a scale's period comes from the scale and must not look editable.
    juce::TextEditor tablePeriodEdit;
    juce::TextButton tablePeriodResetBtn;
    ExpandingEditor  intervalEdit; // Custom Engine: typed intervals -> partials (multi-line)
    juce::Rectangle<int> intervalEditHome; // its laid-out bounds (kept for the focus hook)
    juce::TextEditor refAEdit;     // Reference A: type the Hz directly ("hz" shown to its right)
    WarningOverlay warningOverlay;
    NoticeOverlay  noticeOverlay;
    RecentOverlay  recentOverlay;   // floating list of every scale file loaded
    DissDegreeList degreeList;   // active-scale dissonance list (lives inside dissView)
    DissHeader     dissHeader;   // Step / Sethares-dissonance column header
    juce::Viewport dissView;     // scrolls degreeList when a scale has many steps
    // Spectrum-to-Tuning mode + the "from spectrum's segment" partial picker.
    juce::ComboBox   specModeBox;                 // dyads Sethares / segment / chords Sethares
    // Chord-search mode (n-note Sethares consonance ranking).
    juce::ComboBox   chordNotesBox, chordCountBox, chordMinMaxBox;
    juce::Label      chordNotesLbl, chordPerLbl, chordProgLbl;
    juce::TextEditor chordPeriodEdit;
    // Minimum interval between generated notes (cents, default 33) - both creators.
    juce::TextEditor dissMinEdit, chordMinEdit;
    juce::Label      dissMinLbl, chordMinLbl, chordCountLbl;
    // Loaded-chord degree view: back (all chords) / forward (this chord's ranked degrees).
    juce::TextButton chordViewBack, chordViewFwd;
    bool             chordDegView = false;
    // n-AD selector over the degree list (Degrees / Dyads / Triads / ... up to the
    // scale's own note count, live) + its ranked-combination list.
    juce::ComboBox   nadBox;
    NadList          nadList;
    juce::Viewport   nadView;
    int              lastNadGen = 0, lastNadSel = 1, lastNadD = 0, lastNotesMax = 0;
    bool             lastNadRootOnly = true;
    juce::String     lastNadOpts;   // Min/Max + period + smallest interval, as typed
    double           lastNadSig = -1.0e30;
    juce::uint32     lastNadSpecMs = 0;      // rate limit for SPECTRUM-driven recomputes (see timer)
    double           lastNadSpecSig = -1.0;  // last ACCEPTED spectrum signature (held between refreshes)
    // ONE REMEMBERED PERIOD PER MODE. The search wants 2/1 and the adaptation wants 8/1 -
    // eight partials spread over three octaves is the spectrum that mode exists to build,
    // and 2/1 crammed them into one - so the field cannot have a single default. The rule:
    // the text belongs to the mode it was typed in. Leaving a mode parks whatever the field
    // says under that mode and the mode you arrive at shows its own value back; nothing a
    // user typed is ever thrown away, it just stays where it was typed.
    // [0] = "Find tunings based on beating", [1] = "Adapt spectrum to tuning based on beating".
    juce::String     periodTextByMode[2] { "2", "8" };
    int              lastSpecMode = 1;
    void             switchPeriodTextForMode (int newMode);
    juce::TextButton chordGoBtn;
    ChordList        chordList;
    juce::Viewport   chordView;
    int              lastChordGen = 0;
    //== "Adapt spectrum to tuning based on beating" - the INVERSE of the search above:
    //   the notes are given and the PARTIALS move (see Dissonance.h::adaptSpectrum).
    juce::ComboBox   adaptTargetBox;   // the loaded tuning / the chord held right now
    juce::ComboBox   adaptPartialsBox; // how many partials the adapted spectrum has
    juce::ComboBox   adaptGoalBox;     // minimum / maximum / inside a range
    juce::TextEditor adaptLoEdit, adaptHiEdit;   // the range's two ends (goal = range)
    juce::Label      adaptPartialsLbl, adaptRangeLbl, adaptInfoLbl;
    //== THE REGISTER RANGE the adaptation optimises over: ONE number, N, counted in STEPS
    //   of the loaded tuning, running UPWARD from the note being played (see
    //   FractalAdditiveAudioProcessor::registerAnchorKey for which note that is).
    //
    //   It was two absolute bounds before - "C2".."C4", then periods 2..4 - and both forms
    //   had the same defect underneath the notation: they were pinned to fixed pitches, so
    //   the adaptation handed back the same spectrum whichever register was actually being
    //   played, which is the complaint this whole thread started from. Anchor + count
    //   follows the hands, and a count of STEPS is the one unit every tuning has: 24 steps
    //   is two octaves in 12-EDO, 62 steps is two periods in 31-EDO, 26 is two in
    //   Bohlen-Pierce, and none of that needs an octave to be named.
    //
    //   Sethares' critical band is a width in Hz, so the answer genuinely differs per
    //   register - which registers a spectrum was optimised for therefore has to be on
    //   screen, not implied.
    //
    //   There is no aggregate selector beside it any more. `adaptAggBox` offered mean over
    //   the range vs minimax (the worst register), and minimax stopped being a choice the
    //   day the range was anchored at the played note and made to run UPWARD: the worst
    //   register of an upward range is the anchor itself, so minimax optimised the anchor
    //   alone and THIS field stopped changing the result - 10.742 at every width. The
    //   aggregate is the mean, always; the tooltip on this field says so.
    juce::TextEditor adaptRegStepsEdit;
    // `adaptRegSpanLbl` is what stops N being a magic number: it names the ANCHOR the range
    // hangs off and says what N amounts to in periods, so the width is readable without
    // arithmetic and the user can see WHICH note is being counted from.
    juce::Label      adaptRegLbl, adaptRegSpanLbl;
    // N, in the user's own unit. < 0 means "not typed yet": until then the field shows -
    // and the search uses - the default width for whatever scale is loaded, so swapping
    // 12-EDO for 31-EDO on a fresh instance still gives two periods rather than two thirds
    // of one. Once the user types, the NUMBER is theirs and is never reinterpreted: N is
    // their statement of how wide in the tuning's own units, and re-deriving it on a scale
    // change is precisely the silent rewrite this control is built to avoid.
    int              adaptRegSteps = -1;
    int              registerDegreesPerPeriod() const;
    int              registerStepsEffective() const;       // N as typed, or the default width
    int              registerStepsFitting() const;         // ...and how many of them the board has
    juce::String     registerPeriodText (int key) const;   // key -> the period it starts
    juce::String     registerKeyText (int key) const;      // key -> "period:degree", e.g. "3:0"
    juce::String     adaptRegSpanText() const;             // the sentence under the N field
    juce::String     adaptRegSpanLine (const juce::String& anchor, int steps, int fit,
                                       const juce::String& periods) const;
    void             syncAdaptRegisterSpan();
    void             commitAdaptRegisterSteps();
    // The key whose ACTIVE-TUNING pitch is nearest `hz` - how a register in Hz is named
    // back to the user. Through the tuning, so a shifted or table-anchored instrument
    // names its own keys and not 12-EDO's.
    int              nearestKeyForHz (double hz) const;
    juce::ToggleButton adaptAmpsToggle;          // shape AMPLITUDES as well as positions
    juce::TextButton adaptGoBtn;
    AdaptList        adaptList;
    juce::Viewport   adaptView;
    int              lastAdaptGen = 0;
    void runSpectrumAdapt();
    //== THE ONE READ-OUT under the two buttons. There used to be two lines of numbers
    //   there and neither said what it was for ("avg C2-C4: 769.9 -> 27.8 (96% less)" over
    //   "sounding now: 31.4 (Reps/Fractal)"), the second one usually clipped. This has a
    //   single job - WHAT IS HAPPENING, OR WHAT WILL HAPPEN IF YOU PRESS - and answers it
    //   in one sentence, in priority order (see adaptReadoutText). Every number that was
    //   on screen is still available, in the read-out's tooltip.
    juce::String adaptReadoutText() const;
    juce::String adaptReadoutTip() const;
    // Its five states, each a function of its own numbers so nothing is written twice and
    // the layout probe can measure every one of them at its widest (see readoutVariants).
    juce::String adaptLineWorking   (int pct) const;
    juce::String adaptLineLag       (double ms) const;
    juce::String adaptLineFollowing (int updates) const;
    juce::String adaptLineCost      (double ms) const;
    juce::String adaptLineResult    (int pct, bool less, const juce::String& span, bool drift) const;
    // What ONE Adapt with the panel's current settings will cost, in ms, and the same
    // figure for one REALTIME update (which uses fewer registers). Both come from the
    // processor's estimator, so the price on the button is the number the guard uses.
    double adaptCostMs (bool realtime) const;
    // The result the last finished run reported, kept so the read-out can be rebuilt on
    // any tick (a warning may replace it and then have to give it back). `adaptInfoTipBase`
    // is the language's own explanation of what this line is for, set by applyLanguage and
    // always the first paragraph of the tooltip.
    juce::String adaptResultLine, adaptResultTip, adaptInfoTipBase;
    juce::uint32 lastAdaptTipMs = 0;   // the tooltip is rebuilt at 4 Hz, not at timer rate
    void refreshAdaptButtonText();
    //== REALTIME: one button per direction, and the one shared place that tells the
    //   processor what they should be doing (see syncRealtimeLink).
    juce::TextButton chordRtBtn, adaptRtBtn;
    bool             chordRtOn = false, adaptRtOn = false;
    // ADAPTIVE LOOKAHEAD: the follower's own latency trade, on the follower's own row.
    // Not a parameter (see RtSettings::lookahead) - it travels in the realtime settings
    // like the rest of the link, and the panel adopts the live value when it opens.
    juce::TextButton lookaheadBtn;
    bool             lookaheadOn = false;
    // The target the user chose by hand, parked while REALTIME forces the box to the held
    // chord (the only target a follower can meaningfully have) and handed straight back
    // when realtime goes off. 0 = nothing parked.
    int              adaptTargetBeforeRt = 0;
    FractalAdditiveAudioProcessor::RtSettings realtimeSettingsFromPanel() const;
    void syncRealtimeLink();
    void syncAdaptTargetLock();
    void paintRealtimeButtons();
    // Chord preview scheduling (messages sent into the processor's collector when due).
    struct PrevEv { juce::uint32 dueMs; juce::MidiMessage msg; };
    std::vector<PrevEv> previewQueue;
    std::vector<std::pair<int,int>> previewNotes; // (ch,key) of the current preview - for explicit stops
    void stopChordPreview();                      // explicit note-offs + bend recentre (MPE-safe)
    void playChordPreview (int row);
    void hoverPreview (const std::vector<double>& ratios); // sounds while hovering a row
    void runChordSearch();
    // Hover-dissonance cache: Sethares dissonance of every key AGAINST the hovered
    // one (recomputed only when the hovered note / timbre / tuning changes).
    void updateHoverDissCache (int hoveredNote);
    int    hoverDissNote = -1;
    double hoverDissSig  = -1.0e30;
    std::array<float, 128> hoverDissVal {};
    float  hoverDissLo = 0.0f, hoverDissHi = 1.0f;
    SegmentList      segList;                     // partial picker (inside segView)
    juce::Viewport   segView;
    juce::TextEditor segPeriodEdit;               // "2" / "3/2" / "500c"
    juce::ToggleButton segTopToggle;              // period = highest selected partial
    juce::Label      segPeriodLbl, segHelpLbl;
    // Cached active-scale dissonance (recomputed only when timbre/tuning change) +
    // the sort mode toggled by the header.
    std::vector<FractalAdditiveAudioProcessor::DissDegreeInfo> activeDiss;
    double lastScaleSig   = -1.0e30; // change signature (timbre gen + tuning shape)
    juce::uint32 lastScaleDissMs = 0; // rate limit: a modulated spectrum changes the
                                      // signature every tick, and this analysis is advisory
    Scrim          rhythmScrim;
    RhythmPanel    rhythmPanel;
    std::unique_ptr<ButtonAttachment>   rhythmOnAtt, rhythmSelfAtt, rhythmDroneAtt, rhythmFbAtt, rhythmMpeAtt, rhythmTempoAtt,
                                        rhythmQTempoAtt, rhythmMonoAtt, rhythmQPulseAtt;
    std::unique_ptr<SliderAttachment>   rhythmFbDecayAtt, rhythmVelSlopeAtt, rhythmStepsAllAtt;
    std::unique_ptr<ComboBoxAttachment> rhythmModeAtt;
    juce::uint32 lastRhythmGen = 0; // refresh the partial list when the template changes
    juce::uint32 lastSegGen = 0;    // refresh the segment picker on template change

    // Only the boxes that really are GroupComponents. The SPECTRUM panel and everything
    // nested in it (both engines, the shared cell), plus ESSENTIALS and TUNING, are
    // PAINTED from the rects below - which is what keeps their controls clickable: an
    // opaque GroupComponent added after its children buries them (they keep their
    // tooltips, so the bug looks like "the control is there but dead").
    juce::GroupComponent gFilter, gAmpEnv, gPitchEnv, gTuning, gSpecScale, gScaleDiss;
    juce::Label lblBaseN, lblBasePitch, lblPrimeLimit, lblCutoff, lblRes, lblEnvAmt,
                lblTimbreRepeats, lblBend, lblPeriod, lblSteps, lblRefA, lblRefHz, lblPitchAmt,
                lblMorphTime, lblTuningBank, tuningStatusLabel, tuningNameLabel, timbreStatusLabel, recentLabel,
                tuningPeriodLbl;
    juce::Label ampAdsrLbl[4], filtAdsrLbl[4], pitchAdsrLbl[4], primeLbl[9];

    std::unique_ptr<SliderAttachment>   baseNAtt, slopeAtt, fractalAtt, gainAtt,
                                        cutoffAtt, resAtt, fEnvAmtAtt, timbreRepeatsAtt, pEnvAmtAtt,
                                        harmonicsAtt;
    std::unique_ptr<SliderAttachment>   ampAdsrAtt[4], filtAdsrAtt[4], pitchAdsrAtt[4];
    std::unique_ptr<ComboBoxAttachment> basePitchAtt, phaseAtt,
                                        primeModeAtt, tuningAtt, bendAtt, tuningBankAtt;
    std::unique_ptr<ButtonAttachment>   primeToggleAtt[9], morphAtt;
    std::unique_ptr<SliderAttachment>   morphTimeAtt;

    //== MONOPHONIC GLIDE (the PITCH tab of the shared envelope box) ============
    // The toggle sits on the PITCH tab because that is where the user asked for it, and
    // because that is where it belongs: it is a pitch behaviour, not an envelope. The
    // time fader stands beside the pitch-envelope amount as a second "extra" column;
    // both are visible only while that tab is showing.
    juce::ToggleButton                monoGlideT;
    juce::Slider                      glideFader;
    juce::Label                       lblGlide;
    std::unique_ptr<ButtonAttachment> monoGlideAtt;
    std::unique_ptr<SliderAttachment> glideAtt;

    // SHEPARD MODE (bottom row of the Tuning box): the mode toggle, the ONE rate
    // control in its two dresses (the ms fader in Free mode, the beats box in Sync -
    // resized() shows exactly one of them), and the rise-interval choice, which is on
    // screen ONLY while the active tuning's period is not an octave (nothing to
    // choose otherwise). The v1 LEVEL fader is gone: the Shepard is the voice now,
    // so its level was a second master volume, and the only other thing a depth
    // control could do - stop the window reaching zero at its edges - would break the
    // silent wrap that makes the whole illusion click-free.
    juce::ToggleButton shepOnT;
    juce::ComboBox     shepModeBox, shepDivBox, shepRiseBox;
    juce::Slider       shepTimeFader;
    std::unique_ptr<ButtonAttachment>   shepOnAtt;
    std::unique_ptr<ComboBoxAttachment> shepModeAtt, shepDivAtt, shepRiseAtt;
    std::unique_ptr<SliderAttachment>   shepTimeAtt;
    // (Whether the rise-interval box is on screen rides in the timer's `packed`
    //  layout signature - see the tuning-period bit there.)
    // Last "Sync chosen but the host offers no BPM" state shown (-1 = not yet):
    // the timer redresses the beats box amber and says in its tooltip that the
    // free ms value is in force, instead of letting it claim a sync that is not
    // happening. See shepardSyncHasNoBpm().
    int lastShepNoBpm = -1;

    std::unique_ptr<juce::FileChooser>  fileChooser;
    juce::Component::SafePointer<juce::AlertWindow> activePrompt; // open save-preset dialog, if any

    juce::Label meterLabel, helpLabel;

    bool catalan = false;
    int  baseKey = 60; // MIDI note of the leftmost computer key; Z/X shift it by one period
    int  lastPrimeMode = -1;
    // Packs engineInForce() with "is a custom scale loaded at all" - the second bit
    // can change without the first one moving (loading a scale while the engine is
    // off), and the custom view's Off button hangs on it. -1 = never applied yet.
    int  lastEngineShown = -1;
    int  lastResetOffState = -1; // packs Reset/Off visibility to detect changes for relayout
    int  lastDissGeneration = 0; // refresh the degree list / shortfall notice once per generation
    int  lastKbLo = -1, lastKbNum = -1; // last keyboard window applied (re-adapt on tuning change)
    // SPECTRUM macro-panel: the outer frame, its three nested boxes and their sub-cells.
    // Everything here is a rect only - the frames, the title chips, the dividers, the
    // sub-labels and the override badges are all drawn in paint() from these.
    juce::Rectangle<int> spectrumBounds;       // the whole SPECTRUM panel
    // ONE shared engine box: the HARMONIC / CUSTOM chips are its title row, and only
    // the selected engine's controls are laid out inside it (the other's are hidden).
    // Two separate framed boxes said "two independent things" about two engines that
    // are mutually exclusive, and left the Custom Engine squeezed into a fifth of the
    // panel while most of its box was air.
    juce::Rectangle<int> engineBoxBounds;
    juce::Rectangle<int> hcBase, hcHarm, hcPrime;  // harmonic view's sub-cells (empty in custom view)
    juce::Rectangle<int> bothEnginesBounds;    // nested: what applies to EITHER engine
    juce::Rectangle<int> beSlope, beFractal, beMorph, beRange; // its sub-cells
    juce::Rectangle<int> essentialsBounds;     // Essentials macro-group frame
    juce::Rectangle<int> ecPhase, ecGain, ecRef, ecOct; // its 4 sub-cells (dividers/labels in paint)
    juce::Rectangle<int> lBarBounds, lFootBounds; // the inverted-L Tuning box: dissonance BAR (right) + controls FOOT (bottom-left)
    void refreshDegreeList (bool showNotice = true); // generated-scale shortfall notice
    void updateScaleDissonance();  // recompute the active-scale dissonance when timbre/tuning change
    void rebuildScaleDissList();   // (re)build + sort degreeList.rows from the cached activeDiss
    void commitRefA();       // parse the Reference-A text box -> set the refA parameter
    void refreshRefAText();  // set the Reference-A text box from the current parameter
    std::map<int, int> heldKeys;
    juce::uint32 lastFocusedMs = 0; // debounce: keep playing through brief focus flickers
    juce::uint32 lastFocusReclaimMs = 0; // rate limit on the hover-driven focus grab (see refocus)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessorEditor)
};
