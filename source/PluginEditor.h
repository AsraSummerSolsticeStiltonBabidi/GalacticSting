#pragma once

#include "PluginProcessor.h"
#include "ScaleKeyboard.h"
#include "PartialAmpEdit.h"
#include "IntervalTokens.h"
#include "dsp/Dissonance.h"
#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

//==============================================================================
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
                             essentials, tuning, engineChips,
                             specColumn; // the full-height Spectrum-vs-Tuning box on the right
        // The colour the WHOLE SPECTRUM panel is wearing right now (the selected
        // engine's). Probed because "it must not be the neutral steel again, and it
        // must not collide with the panels around it" is a measurable claim about a
        // hue, and nothing else in the build could check it.
        juce::Colour panelColour;
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
        };
        std::vector<PaintedLabel> paintedLabels;
        // The same measurement for every juce::Label COMPONENT laid out in those boxes,
        // in its own font. A Label does not clip - it squashes to 70% - so it fails by
        // going unreadably condensed instead of disappearing, which no bounds check can
        // see either. Same struct: cell = the label's bounds, textWidth = what it needs.
        std::vector<PaintedLabel> componentLabels;
        // Every combo whose ITEM TEXTS are rewritten by applyLanguage(), as
        // name -> selected id. Probed because re-wording a ComboBox is the one operation
        // that can silently DESELECT it (getSelectedId() compares the visible label
        // against the item's text), and a mode box that reads 0 gates its whole panel off.
        std::vector<std::pair<juce::String, int>> comboIds;
    };
    LayoutProbe layoutProbe() const;
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
    // Test hook (RhythmVerify): click an engine chip the way a hand does. Exists for
    // the one behaviour a probe of engineInForce cannot reach: with NO scale loaded,
    // clicking CUSTOM must still open that view (it is the only place the Load button
    // and the intervals editor live), while the sound honestly stays harmonic.
    void clickEngineChipForTests (int i) { if (engineChip[i & 1].onClick) engineChip[i & 1].onClick(); }

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
    void refreshSlopeFreeze();     // shows that SLOPE is inert while manual mode is on
    void openTimbreChooser();
    void openTuningBankChooser();
    void refreshTuningBank(); // sync the bank combo items + selection with the processor
    void loadRecent (int index);
    void applyLanguage();
    void refreshPresetList();
    void savePresetFlow();
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
        void showPopup() override { if (onBeforePopup) onBeforePopup(); juce::ComboBox::showPopup(); }
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
                        rl = " - not in this spectrum";
                    slotLbl[i].setColour (juce::Label::textColourId,
                                          missing ? juce::Colour (0xffe0453f) : juce::Colour (0xffc4ccda));
                    slotLbl[i].setText ("P" + juce::String (p + 1) + rl, juce::dontSendNotification);
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
            helpB.setBounds (top.removeFromRight (104).withSizeKeepingCentre (104, 22)); // "How to route"
            heading.setBounds (top); // big title, no toggle inside it
            r.removeFromTop (6);

            // Two big pill toggles, each with a caption: MIDI OUT (send to DAW) and
            // PLAY IN FIBLÓ (interpret the rhythm here; the held note stays silent).
            {
                auto tg = r.removeFromTop (46);
                auto p1 = tg.removeFromLeft (118);
                onT.setBounds (p1.removeFromTop (26).withSizeKeepingCentre (84, 24));
                outLbl.setBounds (p1);
                tg.removeFromLeft (14);
                auto p2 = tg.removeFromLeft (140);
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
                feedbackLbl.setBounds (fr.removeFromLeft (54));
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
                divLbl.setBounds (g1.removeFromLeft (50));
                g1.removeFromLeft (4);
                nDn.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                nVal.setBounds (g1.removeFromLeft (26));
                nUp.setBounds (g1.removeFromLeft (20).reduced (0, 2));
                timesLbl.setBounds (g1.removeFromLeft (66));
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
                tempoT.setBounds (g3.removeFromLeft (150));
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
                qTempoT.setBounds (g4.removeFromLeft (218));
                g4.removeFromLeft (6);
                monoT.setBounds (g4.removeFromLeft (76));
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
                head.removeFromRight (4 + 30 + 3 + 30 + 6); // over the -12/+12 pair + gaps
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
                s12P[i].setBounds (row.removeFromRight (30));
                row.removeFromRight (3);
                s12M[i].setBounds (row.removeFromRight (30));
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
        juce::Colour col { 0xff9a7bff };
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
            g.setColour (juce::Colour (0xff9a7bff));
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
            g.setColour (juce::Colour (0xff9a7bff));
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
            grid.onGestureStart = [this] { gestureBefore = proc->getPartialGains(); };
            grid.onEdited       = [this] { pushGains(); };
            grid.onGestureEnd   = [this]
            {
                // ONE undo step per gesture: pushing per frame filled the stack with
                // hundreds of micro-moves and made Ctrl+Z useless on a hand-drawn curve.
                proc->commitPartialGains (gestureBefore, proc->getPartialGains());
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
                w << trF ("MANUAL IS OFF - these knobs are stored but not sounding. Switch it on here or under the pencil.",
                          "EL MODE MANUAL \xc3\x89S APAGAT - aquests controls es desen per\xc3\xb2 no sonen. Activa'l aqu\xc3\xad o sota el llapis.");
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
            onT.setTooltip (trF ("Manual per-partial amplitudes. The SAME switch as the one under the pencil - one control in two places. While it is on, SLOPE is frozen.",
                                 "Amplituds manuals per parcial. \xc3\x89s EL MATEIX interruptor que el de sota el llapis - un control en dos llocs. Mentre estigui actiu, el PENDENT queda congelat."));
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
            resetAllB.setBounds (head.removeFromRight (96).reduced (0, 2));
            head.removeFromRight (8);
            onT.setBounds (head.removeFromRight (58).reduced (0, 2));
            head.removeFromRight (8);
            headLbl.setBounds (head);
            r.removeFromTop (2);
            statLbl.setBounds (r.removeFromTop (16));
            factsLbl.setBounds (r.removeFromTop (52));
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
    juce::TextButton loadSclButton, clearSclButton, loadTimbreButton, clearTimbreButton,
                     langButton, savePresetButton, dissBtn, dissSaveBtn, recentBtn[3], tuningBankLoadBtn,
                     tuningBankClearBtn;
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
    //== THE REGISTER RANGE the adaptation optimises over, as note names ("C2".."C4").
    //   Sethares' critical band is a width in Hz, so the answer genuinely differs per
    //   octave - which registers a spectrum was optimised for therefore has to be on
    //   screen, not implied. `adaptAggBox` picks mean-over-the-range vs the worst register.
    juce::TextEditor adaptRegLoEdit, adaptRegHiEdit;
    juce::ComboBox   adaptAggBox;
    juce::Label      adaptRegLbl, adaptRegInfoLbl;
    // AUTO until the user types: the fields follow an octave either side of the lowest
    // note being played. Typing in either one latches it off for good - a range the user
    // set is never moved under them.
    bool             adaptRegAuto = true;
    int              adaptRegAutoLo = 48, adaptRegAutoHi = 72;
    void             syncAdaptRegisterAuto();
    void             commitAdaptRegisters();
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
    //== REALTIME: one button per direction, and the one shared place that tells the
    //   processor what they should be doing (see syncRealtimeLink).
    juce::TextButton chordRtBtn, adaptRtBtn;
    bool             chordRtOn = false, adaptRtOn = false;
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
    // The CUSTOM view's two sub-cells, so it has the same shared divider every other box
    // in the window has: which SCALE is loaded (Load / On-Off / Temper) above, the
    // spectrum itself (typed intervals, what they built, Reps) below. Empty in the
    // harmonic view. Without them this view was the one undivided box in the plugin.
    juce::Rectangle<int> ccScale, ccBody;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessorEditor)
};
