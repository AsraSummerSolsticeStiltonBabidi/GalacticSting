#include "PluginEditor.h"
#include "BinaryData.h"
#include "dsp/Dissonance.h"

namespace
{
    // Embedded title typeface (loaded once).
    const juce::Typeface::Ptr& titleTypeface()
    {
        static juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (
            BinaryData::Kablammo_ttf, (size_t) BinaryData::Kablammo_ttfSize);
        return tf;
    }

    // Section colours, deliberately spread around the hue wheel so that NO two
    // neighbouring panels (horizontally OR vertically) share a similar hue - in
    // particular no two near-identical blues sit side by side. Reading the two
    // rows left-to-right the hue hops far each step:
    //   Row 1: Harmonic(blue) | Range(yellow) | Fractal(vermilion) | Phase(green)
    //   Row 2: Filter(orange) | Volume(purple) | Pitch(teal) | Tuning(gold) | Custom(pink)
    // and every vertical pair contrasts too (blue over orange, yellow over purple,
    // vermilion over teal, green over gold...).
    // Layout (so the adjacency reasoning is legible):
    //   Row 1: Harmonic Engine | Range | Fractal | Essentials(Phase/Gain/Ref/Octave)
    //   Row 2: Custom | Volume | Pitch | Filter | Tuning
    //   Right: Spectrum-to-Scale (full height)
    // Colours are placed around the hue wheel so every ADJACENT pair (horizontal
    // AND vertical) is a clearly different colour - no two near-identical neighbours.
    const juce::Colour cBase      { 0xff5cb8ff }; // Harmonic Engine is one blue
    const juce::Colour cHarmonic  { 0xff5cb8ff }; // blue   210
    const juce::Colour cWhitelist { 0xff5ff2b0 };
    const juce::Colour cSlope     { 0xff8ef0c4 };
    const juce::Colour cRange     { 0xffffce3d }; // yellow 50
    const juce::Colour cFractal   { 0xfff062cf }; // magenta 318
    const juce::Colour cEssentials{ 0xff4fe09a }; // green  148  (Phase/Gain/Reference/Octave macro)
    const juce::Colour cPrime     { 0xffffd84d };
    const juce::Colour cFilter    { 0xff3fd0e6 }; // cyan   190
    const juce::Colour cEnv       { 0xff3ad9b0 }; // teal-green 165 (Volume envelope)
    const juce::Colour cPitch     { 0xffff5b52 }; // red    5
    const juce::Colour cTuning    { 0xffb27bff }; // purple 275
    const juce::Colour cTimbre    { 0xffff77c8 }; // pink   332  (Custom Engine)
    const juce::Colour cSpecScale { 0xffff9a3c }; // orange 32   (its own colour now, not a yellow twin of Range)
    const juce::Colour cScaleDiss { 0xff32c8be }; // teal        (live scale-dissonance list, under Tuning)
    // Phase/Gain/Reference now share the Essentials colour (one macro, one colour).
    const juce::Colour cPhase     = cEssentials;
    const juce::Colour cGain      = cEssentials;
    const juce::Colour cReference = cEssentials;
    const juce::Colour cLoad      { 0xff2fb155 }; // green = Load buttons
    const juce::Colour cReset     { 0xffe0453f }; // red   = Reset / Off buttons

    // UI wrapper over the (unit-tested, pure-C++) label logic in Dissonance.h.
    juce::String justName (double ratio)
    {
        return juce::String::fromUTF8 (dissonance::justIntervalName (ratio).c_str());
    }

    // Contiguous scale-degree layout (not a piano): the home row A..L is degrees
    // 0..8, then the top row Q..P continues higher, 9..18 - one unbroken run that
    // matches the colour keyboard. Z / X shift the whole thing by one period.
    struct KeyNote { int ch; int off; };
    const KeyNote kKeyMap[] = {
        {'a',0},{'s',1},{'d',2},{'f',3},{'g',4},{'h',5},{'j',6},{'k',7},{'l',8},
        {'q',9},{'w',10},{'e',11},{'r',12},{'t',13},{'y',14},{'u',15},{'i',16},{'o',17},{'p',18}
    };

    // Panel frame outline: ROUNDED top corners, sharp (angular) bottom corners -
    // matching the title chips. Used for both the fill and the stroke.
    juce::Path topRoundedFrame (juce::Rectangle<float> r, float rad)
    {
        rad = juce::jmin (rad, r.getWidth() * 0.5f, r.getHeight() * 0.5f);
        juce::Path p;
        p.startNewSubPath (r.getX(), r.getBottom());
        p.lineTo (r.getX(), r.getY() + rad);
        p.quadraticTo (r.getX(), r.getY(), r.getX() + rad, r.getY());
        p.lineTo (r.getRight() - rad, r.getY());
        p.quadraticTo (r.getRight(), r.getY(), r.getRight(), r.getY() + rad);
        p.lineTo (r.getRight(), r.getBottom());
        p.closeSubPath();
        return p;
    }

    // Inverted-L frame: a tall vertical BAR (right) plus a horizontal FOOT extending
    // left at the bottom (they share the bottom edge and the bar's left edge over the
    // foot's height). Rounded top-outer corners; angular bottom + inner notch.
    juce::Path lShapedFrame (juce::Rectangle<float> bar, juce::Rectangle<float> foot, float rad)
    {
        const float bx = bar.getX(), by = bar.getY(), br = bar.getRight(), bb = bar.getBottom();
        const float fx = foot.getX();
        // Guard: the bar must start above the foot top by at least the corner radius,
        // else the notch would self-cross - fall back to a plain top-rounded union rect.
        const float fy = juce::jmax (foot.getY(), by + rad + 1.0f);
        rad = juce::jmin (rad, bar.getWidth() * 0.5f);
        juce::Path p;
        p.startNewSubPath (bx + rad, by);
        p.lineTo (br - rad, by);
        p.quadraticTo (br, by, br, by + rad);   // bar top-right round
        p.lineTo (br, bb);                       // right edge down
        p.lineTo (fx, bb);                       // bottom edge to foot bottom-left
        p.lineTo (fx, fy + rad);                 // foot left edge up
        p.quadraticTo (fx, fy, fx + rad, fy);    // foot top-left round
        p.lineTo (bx, fy);                       // foot top edge to inner notch
        p.lineTo (bx, by + rad);                 // bar left edge up from notch
        p.quadraticTo (bx, by, bx + rad, by);    // bar top-left round
        p.closeSubPath();
        return p;
    }

    // Draws a menu title inside an ANGULAR (sharp-cornered) chip filled with the
    // section colour, bounded to the text, at the top-left of `box` - just like the
    // coloured squares of the main title. The letter colour flips black/white with
    // the chip's brightness (as the title letters and the keys do). The font shrinks
    // so long Catalan titles still fit inside the box.
    void drawTitleChip (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour col, const juce::String& text)
    {
        constexpr float stretch = 1.34f; // letters stretched taller (upward)
        constexpr float chipH   = 16.0f; // stays INSIDE the box (never clipped at the top)
        const juce::String upper = text.toUpperCase();
        const float maxW = box.getWidth() - 4.0f;

        // Measure + auto-shrink the font so even a long Catalan title fits the box.
        auto measure = [&] (const juce::Font& f, juce::GlyphArrangement& ga)
        {
            ga.clear();
            ga.addLineOfText (f, upper, 0.0f, 0.0f);
            return ga.getBoundingBox (0, -1, true);
        };
        float fontH = 13.0f;
        juce::Font tf (juce::FontOptions (fontH).withStyle ("Bold"));
        juce::GlyphArrangement ga;
        auto bb = measure (tf, ga);
        if (bb.getWidth() + 11.0f > maxW && bb.getWidth() > 1.0f)
        {
            fontH = juce::jmax (8.0f, fontH * (maxW - 11.0f) / bb.getWidth());
            tf = juce::Font (juce::FontOptions (fontH).withStyle ("Bold"));
            bb = measure (tf, ga);
        }

        // The chip's LEFT edge sits exactly on the frame's left border line (outer
        // edge of the 2.6px stroke), so the box border reads as a continuation of
        // the chip's own left edge. Top stays inside the box (never clipped).
        const float chipW = juce::jmin (maxW, bb.getWidth() + 11.0f);
        auto chip = juce::Rectangle<float> (box.getX(), box.getY() + 1.0f, chipW, chipH);

        // Rounded TOP corners, sharp (angular) BOTTOM corners.
        const float rad = juce::jmin (6.0f, chipW * 0.5f);
        juce::Path shape;
        shape.startNewSubPath (chip.getX(), chip.getBottom());
        shape.lineTo (chip.getX(), chip.getY() + rad);
        shape.quadraticTo (chip.getX(), chip.getY(), chip.getX() + rad, chip.getY());
        shape.lineTo (chip.getRight() - rad, chip.getY());
        shape.quadraticTo (chip.getRight(), chip.getY(), chip.getRight(), chip.getY() + rad);
        shape.lineTo (chip.getRight(), chip.getBottom());
        shape.closeSubPath();
        g.setColour (col);
        g.fillPath (shape);

        // Letters, stretched vertically (upward) and centred in the chip; black/white
        // by the chip's brightness (as the title letters and the keys do).
        juce::Path p;
        ga.createPath (p);
        p.applyTransform (juce::AffineTransform::scale (1.0f, stretch));
        const auto gb = p.getBounds();
        p.applyTransform (juce::AffineTransform::translation (chip.getCentreX() - gb.getCentreX(),
                                                              chip.getCentreY() - gb.getCentreY()));
        g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
        g.fillPath (p);
    }

    // Bold group titles in a filled chip (so nothing overlaps the title), a thicker
    // rounded frame, and bold labels -> more contrast + clarity.
    struct GalacticLnF : juce::LookAndFeel_V4
    {
        GalacticLnF()
        {
            setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff20202b));
            setColour (juce::ComboBox::textColourId, juce::Colours::white);
            setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff20202b));
            setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff3a3a52));
        }

        juce::Font getLabelFont (juce::Label& l) override
        {
            // A slider's value read-out is a child Label: keep it SMALL (and not bold)
            // so two decimals fit in the narrow box instead of clipping to "...".
            if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
                return juce::Font (juce::FontOptions (10.0f));
            if (l.getName() == "small") // opt-out of the 12.5px floor (e.g. the play-keys hint)
                return l.getFont();
            return juce::Font (juce::FontOptions (juce::jmax (12.5f, l.getFont().getHeight())).withStyle ("Bold"));
        }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return juce::Font (juce::FontOptions (juce::jmin (16.0f, (float) buttonHeight * 0.62f)).withStyle ("Bold"));
        }

        // Slider value read-outs: a small font + generous horizontal squish so the
        // value ALWAYS fits (e.g. two decimals) instead of being clipped to "...".
        juce::Label* createSliderTextBox (juce::Slider& s) override
        {
            auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
            l->setFont (juce::Font (juce::FontOptions (10.0f))); // small: fits the squeezed fader value boxes
            l->setMinimumHorizontalScale (0.35f); // shrink-to-fit rather than ellipsize
            l->setJustificationType (juce::Justification::centred);
            return l;
        }

        void drawGroupComponentOutline (juce::Graphics& g, int w, int h, const juce::String& text,
                                        const juce::Justification&, juce::GroupComponent& group) override
        {
            const auto col = group.findColour (juce::GroupComponent::outlineColourId);
            constexpr float titleH = 21.0f;
            auto frame = juce::Rectangle<float> (1.3f, titleH * 0.5f,
                                                 (float) w - 2.6f, (float) h - titleH * 0.5f - 1.3f);

            // Interior panel: lighter than the page (0xff0c0c12) and subtly tinted by the
            // section colour. Rounded TOP corners + angular bottom (like the chips).
            const auto shape = topRoundedFrame (frame, 9.0f);
            g.setColour (juce::Colour (0xff17171f).interpolatedWith (col, 0.05f));
            g.fillPath (shape);
            g.setColour (col.withAlpha (0.95f));
            g.strokePath (shape, juce::PathStrokeType (2.6f));

            // Title: an angular colour chip bounded to the text (see drawTitleChip).
            drawTitleChip (g, juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h), col, text);
        }

        // Square buttons (sharp corners) filled with the button's colour; buttons
        // named "round" (the OCTAVE +/- steppers) get a circular body instead.
        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                                   bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat();
            auto c = bg;
            if (down)      c = c.darker (0.28f);
            else if (over) c = c.brighter (0.10f);
            g.setColour (c);
            if (b.getName() == "round")
            {
                const float d = juce::jmin (r.getWidth(), r.getHeight());
                auto circle = r.withSizeKeepingCentre (d, d);
                g.fillEllipse (circle);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.drawEllipse (circle.reduced (0.5f), 1.0f);
                return;
            }
            g.fillRect (r);                                    // square, not rounded
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawRect (r, 1.0f);
        }

        // Button text follows the SAME rule as the title letters and the keys:
        // black on a light button, white on a dark one (translucent = reads dark).
        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
        {
            const auto bg = b.findColour (b.getToggleState() ? juce::TextButton::buttonOnColourId
                                                             : juce::TextButton::buttonColourId);
            const bool light = bg.getFloatAlpha() > 0.6f && bg.getPerceivedBrightness() > 0.52f;
            auto txt = light ? juce::Colours::black : juce::Colours::white;
            if (! b.isEnabled()) txt = txt.withAlpha (0.5f);
            g.setColour (txt);
            g.setFont (getTextButtonFont (b, b.getHeight()));
            g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0),
                              juce::Justification::centred, 2);
        }

        // ADSR-style faders carry their letter INSIDE the drag thumb (the slider's
        // name, e.g. "A"): bigger thumb, letter black/white by the thumb's colour -
        // the same rule as the keyboard keys and the title letters.
        static constexpr float kAdsrThumbR = 18.0f; // ADSR thumb circle radius
        int getSliderThumbRadius (juce::Slider& s) override
        {
            if (s.getName().isNotEmpty() && s.isVertical())
                return (int) kAdsrThumbR;
            return juce::LookAndFeel_V4::getSliderThumbRadius (s);
        }
        void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                               float sliderPos, float minPos, float maxPos,
                               juce::Slider::SliderStyle style, juce::Slider& s) override
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, minPos, maxPos, style, s);
            if (s.getName().isNotEmpty() && s.isVertical())
            {
                const auto col = s.findColour (juce::Slider::thumbColourId);
                g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                // Letter sized to sit INSIDE the circle (~0.8 of the diameter) so it
                // never spills over the edges.
                g.setFont (juce::Font (juce::FontOptions (kAdsrThumbR * 0.85f).withStyle ("Bold")));
                const float d = kAdsrThumbR * 2.0f;
                g.drawText (s.getName(),
                            juce::Rectangle<float> ((float) x + (float) w * 0.5f - kAdsrThumbR, sliderPos - kAdsrThumbR, d, d),
                            juce::Justification::centred, false);
            }
        }

        // Toggle buttons named "onled" render as a big filled ON/OFF pill: green
        // when on, red when off (used for the Spectrum-to-Rhythm master switch).
        void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                               bool over, bool down) override
        {
            if (b.getName() == "onled")
            {
                const bool on = b.getToggleState();
                auto r  = b.getLocalBounds().toFloat().reduced (1.0f);
                auto col = on ? juce::Colour (0xff2fb155) : juce::Colour (0xffe0453f);
                if (down)      col = col.darker (0.20f);
                else if (over) col = col.brighter (0.10f);
                g.setColour (col);
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (juce::Colours::white);
                g.setFont (juce::Font (juce::FontOptions ((float) b.getHeight() * 0.62f).withStyle ("Bold")));
                g.drawText (on ? "ON" : "OFF", b.getLocalBounds(), juce::Justification::centred, false);
                return;
            }
            juce::LookAndFeel_V4::drawToggleButton (g, b, over, down);
        }

        // Text FIELDS (combo boxes + text editors) are as rounded as possible - a
        // full pill (corner radius = half the height).
        void drawComboBox (juce::Graphics& g, int width, int height, bool,
                           int, int, int, int, juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
            const float rad = (float) height * 0.5f;
            g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
            g.fillRoundedRectangle (r.reduced (0.5f), rad);
            g.setColour (box.findColour (juce::ComboBox::outlineColourId));
            g.drawRoundedRectangle (r.reduced (0.75f), rad, 1.4f);

            // Chevron on the right.
            const float cx = (float) width - (float) height * 0.55f;
            const float cy = (float) height * 0.5f;
            const float s  = (float) height * 0.15f;
            juce::Path p;
            p.startNewSubPath (cx - s, cy - s * 0.5f);
            p.lineTo (cx,     cy + s * 0.6f);
            p.lineTo (cx + s, cy - s * 0.5f);
            g.setColour (box.findColour (juce::ComboBox::textColourId).withAlpha (box.isEnabled() ? 0.9f : 0.35f));
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Keep the selected-item text clear of the rounded left end.
        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            const int inset = box.getHeight() / 3;
            label.setBounds (inset, 1, box.getWidth() - inset - box.getHeight() / 2, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }

        void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& te) override
        {
            g.setColour (te.findColour (juce::TextEditor::backgroundColourId));
            g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f),
                                    (float) height * 0.5f);
        }
        void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& te) override
        {
            g.setColour (te.findColour (juce::TextEditor::outlineColourId));
            g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.9f),
                                    (float) height * 0.5f, 1.4f);
        }
    };
}

//==============================================================================
FractalAdditiveAudioProcessorEditor::FractalAdditiveAudioProcessorEditor (
        FractalAdditiveAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      keyboard (p.keyboardState, [this] { return processorRef.getScaleDegrees(); })
{
    lnf = std::make_unique<GalacticLnF>();
    setLookAndFeel (lnf.get());

    // Open in Catalan if the computer's language is Catalan (else English).
    {
        const auto lang = juce::SystemStats::getUserLanguage().toLowerCase();
        catalan = (lang == "ca" || lang.startsWith ("ca-") || lang.startsWith ("ca_"));
    }

    // Harmonic Engine macro-group: one framed box, one colour, with the sub-sections
    // (Base / Harmonics / Prime / Slope) separated by shared divider lines - the whole
    // box (frame/title/dividers/labels) is drawn in paint(), not as a GroupComponent.
    configFader (baseNFader,   "baseN",    baseNAtt,     cHarmonic, "Base harmonic N");
    configCombo (basePitchBox, "baseMode", basePitchAtt, cHarmonic, "Original = harmonics at natural positions.  Transposed down = lowest partial at the played pitch.");
    makeLabel (lblBaseN, cHarmonic); makeLabel (lblBasePitch, cHarmonic);
    lblBasePitch.setJustificationType (juce::Justification::centredLeft); // top-LEFT of its combo, like the others

    // Morphing: when ON, a spectrum change glides each partial to its new position
    // (a per-partial pitch bend) over the Morph time, instead of snapping.
    morphToggle.setColour (juce::ToggleButton::textColourId, cHarmonic);
    morphToggle.setColour (juce::ToggleButton::tickColourId, cHarmonic);
    morphToggle.setColour (juce::ToggleButton::tickDisabledColourId, cHarmonic.withAlpha (0.4f));
    morphToggle.setMouseClickGrabsKeyboardFocus (false);
    morphToggle.onClick = [this] { refocus(); };
    addAndMakeVisible (morphToggle);
    morphAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "morph", morphToggle);
    configFader (morphTimeFader, "morphTime", morphTimeAtt, cHarmonic, "Morph transition time in seconds: how long partials take to glide to the new spectrum.");
    morphTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    morphTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);
    makeLabel (lblMorphTime, cHarmonic);

    configCombo (whitelistBox, "selection", whitelistAtt, cHarmonic, "Which harmonics may sound: All / Even / Odd / Prime");
    configFader (slopeFader, "curveSlope", slopeAtt, cHarmonic, "Amplitude roll-off. 0 = flat/bright, 1 = saw, 2 = triangle.  (Also thins the highs in Timbre mode.)");
    // "Range" (spectrum hard-cut) removed from the UI - redundant with the Filter.
    // The parameter defaults to 10 (= no cut), so it has no effect.
    configGroup (gFractal, cFractal);
    configFader (fractalFader, "fractalLevel", fractalAtt, cFractal, "Fractal self-similarity level (1 = off)");
    makeLabel (lblFractal, cFractal);
    // Essentials macro (Phase / Gain / Reference / Octave) - one framed box drawn
    // in paint() like the Harmonic Engine, so no per-cell GroupComponents here.
    configCombo (phaseBox, "phaseMode", phaseAtt, cEssentials, "Aligned: tonal pulse, never clips.  Scattered: louder, noisier.");
    configFader (gainFader, "masterGain", gainAtt, cEssentials, "Master output gain");
    gainFader.setSliderStyle (juce::Slider::LinearHorizontal);
    gainFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 14);
    // OCTAVE cell, two stepper rows (+1/-1 buttons, no faders): whole-synth octave
    // transpose (+-5) and the spectrum's octave span (partials above 2^N x the note
    // are cut; 10 = full spectrum). The value labels are refreshed by the timer.
    makeLabel (lblOctave, cEssentials);
    makeLabel (lblRange,  cEssentials);
    auto stepParam = [this] (const char* id, int delta)
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (processorRef.apvts.getParameter (id)))
        {
            const float cur = processorRef.apvts.getRawParameterValue (id)->load();
            p->setValueNotifyingHost (p->convertTo0to1 (cur + (float) delta));
        }
        refocus();
    };
    auto configStep = [&] (juce::TextButton& b, const char* txt, const char* id, int delta, const juce::String& tip)
    {
        b.setButtonText (txt);
        b.setName ("round"); // circular body (see GalacticLnF::drawButtonBackground)
        b.setColour (juce::TextButton::buttonColourId, cEssentials.withAlpha (0.30f));
        b.setMouseClickGrabsKeyboardFocus (false);
        b.setTooltip (tip);
        b.onClick = [stepParam, id, delta] { stepParam (id, delta); };
        addAndMakeVisible (b);
    };
    const auto octTip = juce::String ("Octave transpose: shift the WHOLE synth by whole octaves (+-5).");
    const auto sprTip = juce::String ("Spectrum's octaves: keep only partials within this many octaves above the played note. 10 = no cut (full spectrum).");
    configStep (octDn, "-", "octaveShift", -1, octTip);
    configStep (octUp, "+", "octaveShift",  1, octTip);
    configStep (sprDn, "-", "spectrumRange", -1, sprTip);
    configStep (sprUp, "+", "spectrumRange",  1, sprTip);
    octDn.setColour (juce::TextButton::buttonColourId, cReset.withAlpha (0.85f)); // "-" red
    sprDn.setColour (juce::TextButton::buttonColourId, cReset.withAlpha (0.85f));
    octUp.setColour (juce::TextButton::buttonColourId, cLoad.withAlpha (0.95f));  // "+" green
    sprUp.setColour (juce::TextButton::buttonColourId, cLoad.withAlpha (0.95f));
    for (auto* v : { &octVal, &sprVal })
    {
        v->setJustificationType (juce::Justification::centred);
        v->setColour (juce::Label::textColourId, juce::Colours::white);
        v->setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
        addAndMakeVisible (*v);
    }
    octVal.setTooltip (octTip);
    sprVal.setTooltip (sprTip);

    configCombo (primeModeBox,  "primeMode",  primeModeAtt,  cHarmonic, "Limit = drop harmonics with a prime factor above the value.  Whitelist = only the ticked primes.");
    configCombo (primeLimitBox, "primeLimit", primeLimitAtt, cHarmonic, "Largest prime factor allowed (Limit mode).");
    makeLabel (lblPrimeLimit, cHarmonic);
    const char* pn[] = { "2","3","5","7","11","13","17","19","23" };
    for (int i = 0; i < 9; ++i)
    {
        primeToggle[i].setButtonText ({});
        primeToggle[i].setColour (juce::ToggleButton::tickColourId, cHarmonic);
        primeToggle[i].setColour (juce::ToggleButton::tickDisabledColourId, cHarmonic.withAlpha (0.4f));
        primeToggle[i].setMouseClickGrabsKeyboardFocus (false);
        primeToggle[i].setTooltip ("Allow prime " + juce::String (pn[i]));
        primeToggle[i].onClick = [this] { refocus(); };
        addAndMakeVisible (primeToggle[i]);
        primeToggleAtt[i] = std::make_unique<ButtonAttachment> (processorRef.apvts, "primeWL" + juce::String (i), primeToggle[i]);
        makeLabel (primeLbl[i], cHarmonic); primeLbl[i].setText (pn[i], juce::dontSendNotification);
    }

    configGroup (gFilter, cFilter);
    configFader (cutoffFader,  "filterCutoff",    cutoffAtt,  cFilter, "Low-pass cutoff");
    cutoffFader.setTextValueSuffix (" c"); // cents above the played note (note-tracking)
    configFader (resFader,     "filterResonance", resAtt,     cFilter, "Resonance");
    configFader (fEnvAmtFader, "filterEnvAmount", fEnvAmtAtt, cFilter, "How far the filter envelope moves the cutoff (octaves)");
    makeLabel (lblCutoff, cFilter); makeLabel (lblRes, cFilter); makeLabel (lblEnvAmt, cFilter);

    configGroup (gAmpEnv, cEnv);
    configGroup (gPitchEnv, cPitch);
    configFader (pEnvAmtFader, "pitchEnvAmount", pEnvAmtAtt, cPitch, "How far the pitch envelope bends the pitch (semitones, +/-).");
    makeLabel (lblPitchAmt, cPitch);
    const char* ampIds[]   = { "ampAttack","ampDecay","ampSustain","ampRelease" };
    const char* fltIds[]   = { "filterAttack","filterDecay","filterSustain","filterRelease" };
    const char* pitchIds[] = { "pitchAttack","pitchDecay","pitchSustain","pitchRelease" };
    const char* adsrTxt[]  = { "A","D","S","R" };
    for (int i = 0; i < 4; ++i)
    {
        configFader (ampAdsr[i],   ampIds[i],   ampAdsrAtt[i],   cEnv,    "Amplitude envelope");
        configFader (filtAdsr[i],  fltIds[i],   filtAdsrAtt[i],  cFilter, "Filter envelope");
        configFader (pitchAdsr[i], pitchIds[i], pitchAdsrAtt[i], cPitch,  "Pitch envelope");
        // The letter rides INSIDE the drag thumb (see GalacticLnF::drawLinearSlider);
        // the old labels above the faders stay hidden (layoutFaders skips them).
        ampAdsr[i]  .setName (adsrTxt[i]);
        filtAdsr[i] .setName (adsrTxt[i]);
        pitchAdsr[i].setName (adsrTxt[i]);
        makeLabel (ampAdsrLbl[i],   cEnv);    ampAdsrLbl[i].setText   (adsrTxt[i], juce::dontSendNotification);
        makeLabel (filtAdsrLbl[i],  cFilter); filtAdsrLbl[i].setText  (adsrTxt[i], juce::dontSendNotification);
        makeLabel (pitchAdsrLbl[i], cPitch);  pitchAdsrLbl[i].setText (adsrTxt[i], juce::dontSendNotification);
    }

    // Tuning (scale) + Recent .scl.
    configGroup (gTuning, cTuning);
    configCombo (tuningBox, "tuningMode", tuningAtt, cTuning,
                 "12-TET = standard.  MTS-ESP = follow a connected ODDSound master.  Scala = the loaded .scl scale.");
    loadSclButton.setColour (juce::TextButton::buttonColourId, cLoad);
    loadSclButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadSclButton.setMouseClickGrabsKeyboardFocus (false);
    loadSclButton.setTooltip ("Load a Scala scale file (retunes the notes).");
    loadSclButton.onClick = [this] { openScalaChooser(); };
    addAndMakeVisible (loadSclButton);
    clearSclButton.setColour (juce::TextButton::buttonColourId, cReset);
    clearSclButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearSclButton.setMouseClickGrabsKeyboardFocus (false);
    clearSclButton.setTooltip ("Back to 12-TET.");
    clearSclButton.onClick = [this] { processorRef.clearScala(); refocus(); };
    addAndMakeVisible (clearSclButton);
    makeLabel (recentLabel, cTuning); recentLabel.setJustificationType (juce::Justification::centredLeft);
    for (int i = 0; i < 3; ++i)
    {
        recentBtn[i].setColour (juce::TextButton::buttonColourId, cTuning.withAlpha (0.28f));
        recentBtn[i].setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        recentBtn[i].setMouseClickGrabsKeyboardFocus (false);
        recentBtn[i].onClick = [this, i] { loadRecent (i); };
        addChildComponent (recentBtn[i]);
    }
    tuningStatusLabel.setVisible (false); // "loaded:" prefix removed - name shows by the mode combo
    tuningNameLabel.setJustificationType (juce::Justification::centredLeft);
    tuningNameLabel.setColour (juce::Label::textColourId, cTuning); // the loaded scale's file name
    tuningNameLabel.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    // Period of the active scale, in RED, clearly labelled.
    tuningPeriodLbl.setJustificationType (juce::Justification::centredLeft);
    tuningPeriodLbl.setColour (juce::Label::textColourId, juce::Colour (0xffe0453f));
    tuningPeriodLbl.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
    addAndMakeVisible (tuningPeriodLbl);
    addAndMakeVisible (tuningNameLabel);

    // Tuning bank: a slot selector (automatable) + a button to load a .scl into it.
    makeLabel (lblTuningBank, cTuning);
    lblTuningBank.setJustificationType (juce::Justification::centredLeft);
    tuningBankBox.setColour (juce::ComboBox::outlineColourId, cTuning.withAlpha (0.7f));
    tuningBankBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    tuningBankBox.setMouseClickGrabsKeyboardFocus (false);
    tuningBankBox.setTooltip (tr (
        "Bank of scales: pick a slot to make it the active tuning. This selector is a PARAMETER, so a host can automate switching between the loaded scales.",
        "Banc d'escales: tria una ranura per fer-la l'afinaci\xc3\xb3 activa. Aquest selector \xc3\xa9s un PAR\xc3\x80METRE, aix\xc3\xad que un host pot automatitzar el canvi entre les escales carregades."));
    tuningBankBox.onChange = [this] { processorRef.setTuningBankSlotParam (tuningBankBox.getSelectedId() - 1); refocus(); };
    addAndMakeVisible (tuningBankBox);
    tuningBankLoadBtn.setColour (juce::TextButton::buttonColourId, cLoad);
    tuningBankLoadBtn.setMouseClickGrabsKeyboardFocus (false);
    tuningBankLoadBtn.setTooltip (tr ("Load a .scl into the selected bank slot.",
                                      "Carrega un .scl a la ranura seleccionada del banc."));
    tuningBankLoadBtn.onClick = [this] { openTuningBankChooser(); };
    addAndMakeVisible (tuningBankLoadBtn);
    refreshTuningBank();

    // Timbre (.scl as spectrum).
    configGroup (gTimbre, cTimbre);
    loadTimbreButton.setColour (juce::TextButton::buttonColourId, cLoad);
    loadTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    loadTimbreButton.setTooltip ("Use a Scala file's intervals AS the timbre (partials). Slope + fractal + repeats shape it; the harmonic controls turn off.");
    loadTimbreButton.onClick = [this] { openTimbreChooser(); };
    addAndMakeVisible (loadTimbreButton);
    clearTimbreButton.setColour (juce::TextButton::buttonColourId, cReset);
    clearTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    clearTimbreButton.setTooltip ("Back to the harmonic engine.");
    clearTimbreButton.onClick = [this] { processorRef.clearTimbre(); intervalEdit.clear(); refocus(); };
    addAndMakeVisible (clearTimbreButton);
    configFader (timbreRepeatsFader, "timbreRepeats", timbreRepeatsAtt, cTimbre,
                 "How many times the scale's period stacks into the timbre. More = reaches higher.");
    timbreRepeatsFader.setSliderStyle (juce::Slider::LinearHorizontal); // compact box
    timbreRepeatsFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 16);
    makeLabel (lblTimbreRepeats, cTimbre);
    timbreStatusLabel.setJustificationType (juce::Justification::centredLeft);
    timbreStatusLabel.setColour (juce::Label::textColourId, cTimbre);
    timbreStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    addAndMakeVisible (timbreStatusLabel);

    // Language toggle (top-right).
    langButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
    langButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    langButton.setMouseClickGrabsKeyboardFocus (false);
    langButton.onClick = [this] { catalan = ! catalan; applyLanguage(); refocus(); };
    addAndMakeVisible (langButton);

    // Top-bar menu (works in the VST too, where the standalone's Options bar is absent):
    // Init = reset every parameter to default; Panic = all notes off.
    menuButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
    menuButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    menuButton.setMouseClickGrabsKeyboardFocus (false);
    menuButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, tr ("Init - reset all parameters", "Init - reinicia tots els par\xc3\xa0metres"));
        m.addItem (2, catalan ? juce::String ("Language: English")
                              : juce::String::fromUTF8 ("Idioma: catal\xc3\xa0"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton),
            [this] (int r)
            {
                if (r == 1)
                {
                    for (auto* p : processorRef.getParameters())
                        p->setValueNotifyingHost (p->getDefaultValue());
                    processorRef.clearScala();
                    processorRef.clearTimbre();
                    intervalEdit.clear();
                }
                else if (r == 2)
                {
                    catalan = ! catalan;
                    applyLanguage();
                }
                refocus();
            });
    };
    addAndMakeVisible (menuButton);

    // Spectrum -> rhythm MIDI panel (top-bar button toggles it). WHITE: maximum
    // contrast against the dark bar and the rainbow title squares beside it.
    rhythmBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xfff2f2f2));
    rhythmBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    rhythmBtn.setMouseClickGrabsKeyboardFocus (false);
    rhythmBtn.setTooltip (tr (
        "SPECTRUM TO RHYTHM: turns partials of the current patch into MIDI pulse trains - exact polyrhythms derived from the spectrum's frequency ratios, sample-locked to the sound - sent out of this plugin's MIDI OUTPUT to any other track/instrument. Click to open the panel.",
        "ESPECTRE A RITME: converteix parcials del patch actual en trens de polsos MIDI - poliritmes exactes derivats de les relacions de freq\xc3\xbc\xc3\xa8ncia de l'espectre, clavats al so mostra a mostra - que surten per la SORTIDA MIDI del plugin cap a qualsevol altra pista/instrument. Clica per obrir el panell."));
    rhythmBtn.onClick = [this]
    {
        const bool show = ! rhythmPanel.isVisible();
        if (show)
        {
            rhythmPanel.numRows = processorRef.getTemplatePartialCount();
            rhythmPanel.list.updateContent();
            rhythmPanel.refreshSlots();
            rhythmPanel.refreshLive();
        }
        rhythmScrim.setVisible (show); // dims + click-blocks everything behind the panel
        rhythmPanel.setVisible (show);
        if (show) { rhythmScrim.toFront (false); rhythmPanel.toFront (false); }
        else refocus();
    };
    addAndMakeVisible (rhythmBtn);

    rhythmPanel.proc      = &processorRef;
    rhythmPanel.ratioName = [] (double r) { return justName (r); };
    rhythmPanel.trF       = [this] (const char* en, const char* ca) { return tr (en, ca); };
    rhythmPanel.onEdited  = [this] { refocus(); };
    rhythmPanel.onClose   = [this]
    { rhythmPanel.setVisible (false); rhythmScrim.setVisible (false); refocus(); };
    rhythmScrim.onClick   = rhythmPanel.onClose; // click OUTSIDE the panel = close
    addChildComponent (rhythmScrim);
    rhythmPanel.init();
    addChildComponent (rhythmPanel);
    rhythmOnAtt   = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmOn",   rhythmPanel.onT);
    rhythmMpeAtt  = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmMpe",  rhythmPanel.mpeT);
    rhythmTempoAtt = std::make_unique<ButtonAttachment>  (processorRef.apvts, "rhythmTempoSync", rhythmPanel.tempoT);
    rhythmModeAtt = std::make_unique<ComboBoxAttachment> (processorRef.apvts, "rhythmMode", rhythmPanel.modeBox);

    // Credit: "Vibe coded by Asra3" (Asra3 is a link to Instagram), left of the language button.
    vibeLabel.setText ("Vibe coded by", juce::dontSendNotification);
    vibeLabel.setJustificationType (juce::Justification::centredRight);
    vibeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
    vibeLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    vibeLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (vibeLabel);

    vibeLink.setButtonText ("Asra3");
    vibeLink.setURL (juce::URL ("https://www.instagram.com/asra3333333/"));
    vibeLink.setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff5cb8ff));
    vibeLink.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")),
                      false, juce::Justification::centredLeft);
    vibeLink.setMouseClickGrabsKeyboardFocus (false);
    vibeLink.setTooltip ("https://www.instagram.com/asra3333333/");
    addAndMakeVisible (vibeLink);

    // Preset browser (top-left): a combo that lists Documents/Galactic Sting/Presets
    // and a Save button. The list refreshes every time the popup opens.
    presetBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff55557a));
    presetBox.setMouseClickGrabsKeyboardFocus (false);
    presetBox.onBeforePopup = [this] { refreshPresetList(); };
    presetBox.onChange = [this]
    {
        const auto name = presetBox.getText();
        if (name.isNotEmpty() && processorRef.loadPreset (name))
            applyTimbreEnablement (processorRef.isTimbreActive());
        refocus();
    };
    addAndMakeVisible (presetBox);
    refreshPresetList();

    savePresetButton.setColour (juce::TextButton::buttonColourId, cLoad);
    savePresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    savePresetButton.setMouseClickGrabsKeyboardFocus (false);
    savePresetButton.onClick = [this] { savePresetFlow(); };
    addAndMakeVisible (savePresetButton);

    // Pitch-bend range (inside the Tuning box).
    configCombo (bendBox, "bendRange", bendAtt, cTuning, {});
    makeLabel (lblBend, cTuning);

    // "Spectrum to Scale" (Sethares): generate + count + reverse + period + save,
    // plus the sorted degree/height list.
    configGroup (gSpecScale, cSpecScale);
    dissBtn.setColour (juce::TextButton::buttonColourId, cSpecScale.withAlpha (0.35f));
    dissBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    dissBtn.setMouseClickGrabsKeyboardFocus (false);
    dissBtn.onClick = [this] { runDissonanceTune(); };
    addAndMakeVisible (dissBtn);
    for (int i = 1; i <= 24; ++i)
        dissCountBox.addItem (juce::String (i), i);
    dissCountBox.setSelectedId (5, juce::dontSendNotification);
    makeLabel (lblSteps, cSpecScale);
    dissCountBox.setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
    dissCountBox.setMouseClickGrabsKeyboardFocus (false);
    dissCountBox.onChange = [this] { refocus(); };
    addAndMakeVisible (dissCountBox);
    // Min / Max consonance target (replaces the old "Max" checkbox): default Min.
    dissModeBox.addItem ("Min", 1);
    dissModeBox.addItem ("Max", 2);
    dissModeBox.setSelectedId (1, juce::dontSendNotification);
    dissModeBox.setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
    dissModeBox.setMouseClickGrabsKeyboardFocus (false);
    dissModeBox.onChange = [this] { refocus(); };
    addAndMakeVisible (dissModeBox);
    dissSaveBtn.setColour (juce::TextButton::buttonColourId, cLoad);
    dissSaveBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    dissSaveBtn.setMouseClickGrabsKeyboardFocus (false);
    dissSaveBtn.onClick = [this] { saveDissonanceScl(); };
    addAndMakeVisible (dissSaveBtn);
    periodEdit.setText ("2", juce::dontSendNotification);
    periodEdit.setJustification (juce::Justification::centred);
    periodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    periodEdit.setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
    periodEdit.setIndents (10, 2); // clear the rounded (pill) ends
    periodEdit.setSelectAllWhenFocused (true);
    periodEdit.onReturnKey = [this] { refocus(); }; // give the keys back to note-playing
    periodEdit.onEscapeKey = [this] { refocus(); };
    addAndMakeVisible (periodEdit);
    makeLabel (lblPeriod, cSpecScale);

    // Scale-dissonance box (its own group, right of Tuning): the live per-degree
    // Sethares dissonance of the ACTIVE scale, in a scrollable, header-sortable list.
    configGroup (gScaleDiss, cScaleDiss);
    gTuning.setVisible (false);   // the Tuning + dissonance L-frame is painted in paint()
    gScaleDiss.setVisible (false);
    addAndMakeVisible (dissHeader);
    dissView.setViewedComponent (&degreeList, false); // Viewport owns display, not deletion
    dissView.setScrollBarsShown (true, false);
    addAndMakeVisible (dissView); // tooltip set in applyLanguage()

    // Spectrum-to-Tuning MODE selector: Sethares dissonance (Transmute) vs the new
    // "from spectrum's segment" partial picker. Relayout + reflect the tuning on change.
    specModeBox.addItem ("Based on Sethares dissonance", 1);
    specModeBox.addItem ("From spectrum's segment",       2);
    specModeBox.setSelectedId (processorRef.isSegmentModeActive() ? 2 : 1, juce::dontSendNotification); // restored state
    specModeBox.setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
    specModeBox.setMouseClickGrabsKeyboardFocus (false);
    specModeBox.onChange = [this]
    {
        const bool seg = specModeBox.getSelectedId() == 2;
        processorRef.setSegmentModeActive (seg);
        if (seg) processorRef.rebuildSegmentScale();
        resized(); refocus();
    };
    addAndMakeVisible (specModeBox);

    segList.proc      = &processorRef;
    segList.ratioName = [] (double r) { return justName (r); };
    segList.onChange  = [this] { segList.repaint(); updateScaleDissonance(); refocus(); };
    segView.setViewedComponent (&segList, false);
    segView.setScrollBarsShown (true, false);
    addAndMakeVisible (segView);
    makeLabel (segPeriodLbl, cSpecScale);
    segPeriodEdit.setText (processorRef.getSegmentPeriodText(), juce::dontSendNotification); // restored
    segPeriodEdit.setJustification (juce::Justification::centred);
    segPeriodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    segPeriodEdit.setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
    segPeriodEdit.setSelectAllWhenFocused (true);
    segPeriodEdit.onReturnKey = [this] { processorRef.setSegmentPeriodText (segPeriodEdit.getText()); refocus(); };
    segPeriodEdit.onFocusLost = [this] { processorRef.setSegmentPeriodText (segPeriodEdit.getText()); };
    addAndMakeVisible (segPeriodEdit);
    segTopToggle.setToggleState (processorRef.getSegmentUseTopPeriod(), juce::dontSendNotification); // restored
    segPeriodEdit.setEnabled (! processorRef.getSegmentUseTopPeriod());
    segTopToggle.onClick = [this]
    {
        processorRef.setSegmentUseTopPeriod (segTopToggle.getToggleState());
        segPeriodEdit.setEnabled (! segTopToggle.getToggleState());
        refocus();
    };
    addAndMakeVisible (segTopToggle);
    segHelpLbl.setJustificationType (juce::Justification::topLeft);
    segHelpLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
    segHelpLbl.setFont (juce::Font (juce::FontOptions (10.5f)));
    addAndMakeVisible (segHelpLbl);

    // Spectrum Builder: typed intervals -> partials (Enter applies them).
    intervalEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    intervalEdit.setColour (juce::TextEditor::outlineColourId, cTimbre.withAlpha (0.7f));
    intervalEdit.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    intervalEdit.setIndents (12, 2); // clear the rounded (pill) ends
    intervalEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
    intervalEdit.onReturnKey = [this]
    {
        juce::String err;
        if (intervalEdit.getText().trim().isNotEmpty())
        {
            if (processorRef.loadTimbreText (intervalEdit.getText(), err))
            {
                // Same as loading a .scl: switch the engine off, grey the harmonic
                // controls and show the Off button - immediately, not on the next tick.
                applyTimbreEnablement (true);
                resized();
            }
            else
                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                    tr ("Spectrum Builder", "Constructor d'espectre"), err, this);
        }
        refocus();
    };
    intervalEdit.onEscapeKey = [this] { refocus(); };
    // While typing, the box grows to the window's right edge so long interval
    // lists stay readable; on blur it collapses back and shows the text's start.
    intervalEdit.onFocusChanged = [this] (bool focused)
    {
        if (intervalEditHome.isEmpty()) return;
        if (focused)
        {
            intervalEdit.setBounds (intervalEditHome.withRight (getWidth() - 10));
            intervalEdit.toFront (false);
        }
        else
        {
            intervalEdit.setBounds (intervalEditHome);
            intervalEdit.setCaretPosition (0); // show the beginning when collapsed
        }
    };
    addAndMakeVisible (intervalEdit);

    // Master reference A = <hz> (transposes everything, all tuning modes). A text
    // box you type the Hz into (with "hz" shown to its right), not a fader.
    refAEdit.setJustification (juce::Justification::centred);
    refAEdit.setFont (juce::Font (juce::FontOptions (13.0f))); // fits the slim cell, no clipped descenders
    refAEdit.setInputRestrictions (7, "0123456789.");
    refAEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    refAEdit.setColour (juce::TextEditor::outlineColourId, cEssentials.withAlpha (0.7f));
    refAEdit.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    refAEdit.setIndents (8, 2); // clear the rounded (pill) ends
    refAEdit.setSelectAllWhenFocused (true);
    refAEdit.setText (juce::String (processorRef.apvts.getRawParameterValue ("refA")->load(), 2), juce::dontSendNotification);
    refAEdit.onReturnKey  = [this] { commitRefA(); refocus(); };
    refAEdit.onFocusLost  = [this] { commitRefA(); };
    refAEdit.onEscapeKey  = [this] { refreshRefAText(); refocus(); };
    addAndMakeVisible (refAEdit);
    makeLabel (lblRefA, cEssentials);
    lblRefHz.setText ("hz", juce::dontSendNotification);
    lblRefHz.setJustificationType (juce::Justification::centredLeft);
    lblRefHz.setColour (juce::Label::textColourId, cEssentials.brighter (0.15f));
    lblRefHz.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
    addAndMakeVisible (lblRefHz);

    addChildComponent (noticeOverlay);

    // A generation may already exist (editor reopened): repopulate the degree
    // list WITHOUT resurrecting an already-dismissed shortfall notice.
    lastDissGeneration = processorRef.getDissGeneration();
    refreshDegreeList (false);

    // Floating "why is this silent?" explainer (shown by the timer when the
    // template is empty).
    addChildComponent (warningOverlay);

    // Integer faders: relative drag (no snap-to-click) so the drag distance,
    // not the track length, sets the resolution; Ctrl stretches it x4 (see
    // SteppedSlider). Base N (1..64) gets the longest travel = least sensitive.
    for (auto* s : { &baseNFader, &fractalFader, &timbreRepeatsFader })
        s->setSliderSnapsToMousePosition (false);
    baseNFader.normalSensitivity         = 520;
    fractalFader.normalSensitivity       = 260;
    timbreRepeatsFader.normalSensitivity = 280;

    meterLabel.setJustificationType (juce::Justification::centred);
    meterLabel.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    addAndMakeVisible (meterLabel);
    helpLabel.setJustificationType (juce::Justification::centred);
    helpLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0b0));
    helpLabel.setName ("small"); // opt-out of the LnF's 12.5px label floor
    helpLabel.setFont (juce::Font (juce::FontOptions (8.5f))); // smaller: it is a hint, not a control
    addAndMakeVisible (helpLabel);

    keyboard.setDegreeLabeller ([this] (int d) { return justName (processorRef.getDegreeRatio (d)); });
    keyboard.setHoverTextProvider ([this] (int d) -> juce::String
    {
        // LITERAL interval of this key vs the tonic - no octave folding, so a
        // 5/2 degree in a tritave scale reads as 5/2 / 1586 c, never "5/4".
        const double ratio = processorRef.getDegreeRatio (d);
        if (! (ratio > 0.0) || ! std::isfinite (ratio))
            return {};
        const double cents = 1200.0 * std::log2 (ratio);
        long long p = 1, q = 1;
        dissonance::bestFraction (ratio, p, q);
        const double fracCents = 1200.0 * std::log2 ((double) p / (double) q);
        const auto frac = juce::String (p) + "/" + juce::String (q);

        if (std::abs (cents - fracCents) < 0.5) // the key IS this ratio -> show it in cents
            return frac + " = " + juce::String (cents, 2) + " c";

        // Cents key -> nearest simple just interval + signed error.
        const int err = juce::roundToInt (cents - fracCents);
        return juce::String (cents, 2) + " c   (" + frac + (err >= 0 ? " +" : " -")
             + juce::String (std::abs (err)) + "c)";
    });
    addAndMakeVisible (keyboard);

    applyLanguage();
    applyTimbreEnablement (processorRef.isTimbreActive());
    lastTimbreActive = processorRef.isTimbreActive();

    setWantsKeyboardFocus (true);
    setSize (1200, 620); // a touch taller so the Essentials cells breathe
    startTimerHz (30); // also the reconcile rate for stuck/ghost computer-keyboard notes
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });
}

FractalAdditiveAudioProcessorEditor::~FractalAdditiveAudioProcessorEditor()
{
    stopTimer();
    // Release any computer-keyboard notes still held when the UI closes.
    for (const auto& kv : heldKeys)
        processorRef.keyboardState.noteOff (1, kv.second, 0.0f);
    heldKeys.clear();
    // Dismiss a still-open save-preset dialog so no orphaned modal window
    // survives the editor (its callback is already SafePointer-guarded).
    if (activePrompt != nullptr)
        activePrompt->exitModalState (0);
    setLookAndFeel (nullptr);
}

//==============================================================================
void FractalAdditiveAudioProcessorEditor::refocus() { grabKeyboardFocus(); }

void FractalAdditiveAudioProcessorEditor::forceDefault (const juce::String& paramID)
{
    if (auto* prm = processorRef.apvts.getParameter (paramID))
        prm->setValueNotifyingHost (prm->getDefaultValue());
}

void FractalAdditiveAudioProcessorEditor::loadRecent (int index)
{
    const auto paths = processorRef.getRecentSclPaths();
    if (index < paths.size())
    {
        juce::String err;
        if (! processorRef.loadScalaFile (juce::File (paths[index]), err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load scale", err, this);
    }
    refocus();
}

void FractalAdditiveAudioProcessorEditor::applyLanguage()
{
    using juce::dontSendNotification;
    langButton.setButtonText (catalan ? "ENG" : "CAT");
    menuButton.setButtonText (tr ("Menu", "Men\xc3\xba"));
    rhythmBtn.setButtonText (tr ("Spectrum to Rhythm", "Espectre a ritme"));
    rhythmPanel.heading.setText (tr ("SPECTRUM TO RHYTHM (MIDI OUT)", "ESPECTRE A RITME (SORTIDA MIDI)"), dontSendNotification);
    rhythmPanel.listHead.setText (tr ("Partials of the current patch (click to route)",
                                      "Parcials del patch actual (clica per encaminar)"), dontSendNotification);
    rhythmPanel.onT.setButtonText (tr ("On", "Actiu"));
    rhythmPanel.secRate.setText  (tr ("PULSE RATE", "VELOCITAT DELS POLSOS"), dontSendNotification);
    rhythmPanel.secOut.setText   (tr ("OUTPUT", "SORTIDA"), dontSendNotification);
    rhythmPanel.secTempo.setText (tr ("DAW TEMPO", "TEMPO DEL DAW"), dontSendNotification);
    rhythmPanel.divLbl.setText   (tr ("divide", "divideix"), dontSendNotification);
    rhythmPanel.timesLbl.setText (tr ("times by", "vegades per"), dontSendNotification);
    for (auto* b : { &rhythmPanel.nUp, &rhythmPanel.nDn })
        b->setTooltip (tr ("How many times the frequency is divided (n in x^n). The ratios between trains never change - only the absolute speed.",
                           "Quantes vegades es divideix la freq\xc3\xbc\xc3\xa8ncia (la n de x^n). Les relacions entre trens no canvien - nom\xc3\xa9s la velocitat absoluta."));
    for (auto* b : { &rhythmPanel.xUp, &rhythmPanel.xDn })
        b->setTooltip (tr ("The divisor base (x in x^n). 2 = halve per division (octaves of rhythm).",
                           "La base del divisor (la x de x^n). 2 = meitat per divisi\xc3\xb3 (octaves de ritme)."));
    rhythmPanel.updateJiText(); // "JI: ON" / "JI: OFF" (green/red toggle)
    rhythmPanel.jiB.setTooltip (tr (
        "Needs MPE out. Sets every routed pulse's pitch to the JUST ratio of ITS OWN partial (P3 -> 3/1...), so the receiving synth plays the partials' exact pitches.",
        "Necessita MPE out. Posa el to de cada pols al ratio JUST del SEU parcial (P3 -> 3/1...), perqu\xc3\xa8 el sinte receptor toqui els tons exactes dels parcials."));
    rhythmPanel.helpB.setTooltip (tr ("Routing & setup help", "Ajuda d'encaminament i configuraci\xc3\xb3"));
    rhythmPanel.noteHead.setText (tr ("Steps from the played note", "Passos des de la nota tocada"),
                                  dontSendNotification);
    rhythmPanel.noteHead.setTooltip (tr (
        "Each train sends the PLAYED note shifted by this many scale steps: \"7st\" = 7 steps up, \"7st-\" = 7 steps down. Never a fixed note. With MPE out, ratios (\"3/2\") and cents (\"702c\") are also FROM the played note.",
        "Cada tren envia la nota TOCADA despla\xc3\xa7""ada aquests passos d'escala: \"7st\" = 7 amunt, \"7st-\" = 7 avall. Mai una nota fixa. Amb MPE out, els ratios (\"3/2\") i cents (\"702c\") tamb\xc3\xa9 s\xc3\xb3n DES DE la nota tocada."));
    rhythmPanel.durHead.setText ("GATE", dontSendNotification);
    rhythmPanel.durHead.setTooltip (tr (
        "GATE: how long each pulse's MIDI note lasts (5-500 ms). It is NOT latency - pulse timing stays sample-locked; this only sets where the note-off lands.",
        "GATE: quant dura la nota MIDI de cada pols (5-500 ms). NO \xc3\xa9s lat\xc3\xa8ncia - els polsos queden clavats a la mostra; nom\xc3\xa9s fixa on cau el note-off."));
    // Help popup text (the "?" button). fromUTF8 directly: tr()'s English branch
    // does not decode UTF-8 accents.
    rhythmPanel.helpText = juce::String::fromUTF8 (catalan
        ? "ENCAMINAMENT (qualsevol DAW):\nEn una altra pista d'instrument, tria com a ENTRADA MIDI la SORTIDA MIDI d'aquest plugin (no l'entrada de la pista del Fibl\xc3\xb3!) i activa el monitoratge d'entrada.\nA Ableton: MIDI From -> pista del Fibl\xc3\xb3 -> 2n selector \"Fibl\xc3\xb3 Gal\xc3\xa0" "ctic\" -> Monitor In.\n\nTEMPO DEL DAW (CC20):\n. Ableton (f\xc3\xa0" "cil): arrossega el dispositiu Max for Live \"Fibl\xc3\xb3 Tempo Sync\" (a User Library > Presets > MIDI Effects) a una pista MIDI que rebi la sortida del Fibl\xc3\xb3, i activa Sync de tempo. Sense mapejar res: el tempo segueix el pols 1.\n. Altres DAW (Reaper, FL...): instal\xc2\xb7la loopMIDI (gratu\xc3\xaft), crea un port; envia la sortida MIDI del Fibl\xc3\xb3 a aquest port; al DAW, mapeja el tempo a l'entrada loopMIDI (rang 20-147) i activa Sync de tempo.\n\nLAT\xc3\x88NCIA: els polsos surten clavats a la mostra; el DAW pot afegir un buffer al MIDI entre pistes - fes servir un buffer petit."
        : "ROUTING (any DAW):\nOn another instrument track, choose this plugin's MIDI OUTPUT as the track's MIDI INPUT (not the Fibl\xc3\xb3 track's own input!) and enable input monitoring.\nIn Ableton: MIDI From -> the Fibl\xc3\xb3 track -> 2nd selector \"Fibl\xc3\xb3 Gal\xc3\xa0" "ctic\" -> Monitor In.\n\nDAW TEMPO (CC20):\n. Ableton (easy): drag the Max for Live device \"Fibl\xc3\xb3 Tempo Sync\" (in User Library > Presets > MIDI Effects) onto a MIDI track that receives the Fibl\xc3\xb3's MIDI output, and turn Tempo sync on. No mapping needed - the tempo follows pulse 1.\n. Other DAWs (Reaper, FL...): install loopMIDI (free), create a port; send the Fibl\xc3\xb3's MIDI output to that port; in the DAW, MIDI-map the tempo to the loopMIDI input (range 20-147) and turn Tempo sync on.\n\nLATENCY: pulses leave sample-locked; the DAW may add one buffer to inter-track MIDI - use a small buffer size.");
    rhythmPanel.mpeT.setButtonText ("MPE out");
    rhythmPanel.mpeT.setTooltip (tr (
        "MPE output: each pulse train gets its own MIDI channel with per-note pitch bend, and the pitch boxes also accept cents (\"1987.4c\") or ratios (\"3\", \"3/2\") - sent RETUNED so an MPE-capable synth plays the exact pitch (assumes the receiver's bend range is +-48, the MPE default).",
        "Sortida MPE: cada tren de polsos t\xc3\xa9 el seu canal MIDI amb pitch bend per nota, i les caselles de to tamb\xc3\xa9 accepten cents (\"1987.4c\") o ratios (\"3\", \"3/2\") - s'envien REAFINATS perqu\xc3\xa8 un sinte amb MPE toqui el to exacte (assumeix rang de bend +-48 al receptor, el defecte MPE)."));
    rhythmPanel.modeBox.setTooltip (tr (
        "Lowest note: only the lowest held note drives the pulses. All notes: every held note drives its own copy of the trains, transposed by its interval from the lowest.",
        "Nota m\xc3\xa9s greu: nom\xc3\xa9s la nota m\xc3\xa9s greu mant\xc3\xa9 els polsos. Totes les notes: cada nota mantinguda porta la seva c\xc3\xb2pia dels trens, transposada pel seu interval respecte a la m\xc3\xa9s greu."));
    rhythmPanel.tempoT.setButtonText (tr ("Tempo sync (CC20)", "Sync de tempo (CC20)"));
    rhythmPanel.tempoT.setTooltip (tr (
        "While ON, the DAW tempo FOLLOWS pulse 1 (x2 / /2 to scale): CC20 is re-sent whenever the BPM changes. One-time setup: MIDI-map the DAW's tempo to this plugin's CC20 with a 20-147 BPM range - the sent value is then EXACTLY the shown BPM. Works in DAWs whose tempo is MIDI-mappable (Ableton, Reaper, FL...).",
        "Mentre \xc3\xa9s actiu, el tempo del DAW SEGUEIX el pols 1 (x2 / /2 per escalar): el CC20 es reenvia quan canvia el BPM. Configuraci\xc3\xb3 \xc3\xbanica: mapeja el tempo del DAW al CC20 d'aquest plugin amb rang 20-147 BPM - el valor enviat \xc3\x89S exactament el BPM mostrat. Funciona als DAWs amb tempo mapejable per MIDI (Ableton, Reaper, FL...)."));
    for (int i = 0; i < RhythmEngine::kSlots; ++i)
    {
        rhythmPanel.pitchE[i].setTooltip (tr (
            "Scale steps FROM the played note: \"7st\" = 7 up, \"7st-\" = 7 down (always relative, never a fixed note). With MPE out on, also cents (\"1987.4c\") or a ratio (\"3\", \"3/2\") above the played note, sent retuned via per-note bend.",
            "Passos d'escala DES DE la nota tocada: \"7st\" = 7 amunt, \"7st-\" = 7 avall (sempre relatiu, mai una nota fixa). Amb MPE out actiu, tamb\xc3\xa9 cents (\"1987.4c\") o un ratio (\"3\", \"3/2\") sobre la nota tocada, enviat reafinat amb bend per nota."));
        rhythmPanel.durS[i].setTooltip (tr (
            "GATE: how long each pulse's MIDI note lasts (5-500 ms). It is NOT latency - pulse timing stays sample-locked; this only sets where the note-off lands.",
            "GATE: quant dura la nota MIDI de cada pols (5-500 ms). NO \xc3\xa9s lat\xc3\xa8ncia - els polsos queden clavats a la mostra; nom\xc3\xa9s fixa on cau el note-off."));
    }
    rhythmPanel.onT.setTooltip (tr (
        "Master switch for the rhythm MIDI output.",
        "Interruptor general de la sortida MIDI de ritme."));
    langButton.setTooltip (tr ("Switch the interface language to Catal\xc3\xa0.",
                               "Torna la interf\xc3\xad" "cie a l'angl\xc3\xa8s."));

    gFractal.setText       (tr ("Fractal", "Fractal"));
    gPrime.setText         (tr ("Prime Control", "Control primers"));
    gFilter.setText        (tr ("Filter", "Filtre"));
    gAmpEnv.setText        (tr ("Volume Envelope", "Envolupant volum"));
    gPitchEnv.setText      (tr ("Pitch Envelope", "Envolupant to"));
    gTuning.setText        (tr ("Tuning", "Afinaci\xc3\xb3"));
    gTimbre.setText        (tr ("Custom Engine", "Motor propi"));
    intervalEdit.setTextToShowWhenEmpty (tr ("intervals: 3/2 2 700c ...", "intervals: 3/2 2 700c ..."),
                                         juce::Colour (0xff8f96a8));
    gSpecScale.setText     (tr ("Spectrum to Tuning", "Espectre a afinaci\xc3\xb3"));
    gScaleDiss.setText     (tr ("Scale Dissonance", "Dissonancia de l'escala"));
    segPeriodLbl.setText   (tr ("Period", "Per\xc3\xado" "de"), juce::dontSendNotification);
    segTopToggle.setButtonText (tr ("= top", "= sup"));
    segTopToggle.setTooltip (tr ("Use the highest selected partial as the period (instead of the typed value).",
                                 "Fes servir el parcial seleccionat m\xc3\xa9s agut com a per\xc3\xado" "de (en lloc del valor escrit)."));
    segHelpLbl.setText (tr ("Click partials to add them to the scale; Shift-click for a range. Lowest selected = tonic (C3).",
                            "Clica parcials per afegir-los a l'escala; Shift-clic per un rang. El m\xc3\xa9s greu seleccionat = t\xc3\xb2nica (C3)."),
                        juce::dontSendNotification);
    segList.tip = tr ("Each partial of the current spectrum. Click to add/remove it as a scale degree (live). Shift-click selects the span to the last click. The lowest selected is the tonic (C3); all selected share one colour.",
                      "Cada parcial de l'espectre actual. Clica per afegir/treure'l com a grau de l'escala (en viu). Shift-clic selecciona el tram fins a l'\xc3\xbaltim clic. El m\xc3\xa9s greu seleccionat \xc3\xa9s la t\xc3\xb2nica (C3); tots els seleccionats comparteixen color.");

    lblBaseN.setText     ("N", dontSendNotification);
    lblBasePitch.setText (tr ("Pitch", "To"), dontSendNotification);
    lblFractal.setText   (tr ("Level", "Nivell"), dontSendNotification);
    lblOctave.setText    (tr ("Transpose", "Transposa"), dontSendNotification);
    lblRange.setText     (tr ("Spectrum", "Espectre"), dontSendNotification);
    lblPrimeLimit.setText(tr ("Limit", "L\xc3\xadmit"), dontSendNotification);
    lblCutoff.setText    (tr ("Cut", "Tall"), dontSendNotification);
    lblRes.setText       (tr ("Res", "Res"), dontSendNotification);
    lblEnvAmt.setText    (tr ("Env", "Env"), dontSendNotification);
    lblPitchAmt.setText  (tr ("St", "St"), dontSendNotification);
    lblMorphTime.setText (tr ("Time", "Temps"), dontSendNotification);
    morphToggle.setButtonText (tr ("Morph", "Morph"));
    lblTimbreRepeats.setText (tr ("Reps", "Reps"), dontSendNotification);
    lblBend.setText      (tr ("Pitch bend", "Pitch bend"), dontSendNotification);
    lblPeriod.setText    (tr ("Period", "Per\xc3\xado" "de"), dontSendNotification);
    lblSteps.setText     (tr ("Steps", "Passos"), dontSendNotification);
    lblRefA.setText      ("A =", dontSendNotification);
    lblTuningBank.setText (tr ("Tuning bank", "Banc d'afinacions"), dontSendNotification);
    degreeList.tip = tr (
        "Each row is a degree of the ACTIVE scale (any tuning): colour chip = its keyboard colour, then its interval, then its Sethares DISSONANCE against the current timbre (how rough it sounds) - lower = more consonant. Ordered most consonant first. The period is the red row.",
        "Cada fila \xc3\xa9s un grau de l'escala ACTIVA (qualsevol afinaci\xc3\xb3): xip de color = el seu color al teclat, despr\xc3\xa9s l'interval, i la DISSONANCIA de Sethares contra el timbre actual (com d'aspre sona) - m\xc3\xa9s baix = m\xc3\xa9s consonant. Ordenats de m\xc3\xa9s consonant primer. El per\xc3\xado""de \xc3\xa9s la fila vermella.");
    dissHeader.tip = tr (
        "Sethares dissonance: how ROUGH each scale step sounds with the CURRENT timbre. It is not fixed - it recomputes live from the synth's actual partials, so editing the timbre (N, fractal, slope, primes, .scl...) changes these values in real time. Lower = more consonant; the rows are ordered most consonant first.",
        "Dissonancia de Sethares: com d'ASPRE sona cada pas de l'escala amb el timbre ACTUAL. No \xc3\xa9s fixa - es recalcula en viu a partir dels parcials reals del sinte, aix\xc3\xad que editar el timbre (N, fractal, pendent, primers, .scl...) canvia aquests valors en temps real. M\xc3\xa9s baix = m\xc3\xa9s consonant; les files van de m\xc3\xa9s consonant primer.");
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);

    loadSclButton.setButtonText    (tr ("Load .scl", "Obrir .scl"));
    clearSclButton.setButtonText   (tr ("Reset", "Reinicia"));
    tuningBankLoadBtn.setButtonText (tr ("+ .scl", "+ .scl"));
    loadTimbreButton.setButtonText (tr ("Load .scl", "Obrir .scl"));
    clearTimbreButton.setButtonText(tr ("Off", "Apaga"));
    savePresetButton.setButtonText (tr ("Save", "Desa"));
    presetBox.setTextWhenNothingSelected (tr ("Presets...", "Presets..."));
    dissBtn.setButtonText          (tr ("Transmute", "Transmuta"));
    dissSaveBtn.setButtonText      (tr ("Save", "Desa"));

    helpLabel.setText (tr ("Play: A S D F G H J K L = the scale in order, then Q W E R T Y U I O P continue higher.   Z / X = down / up one scale period.",
                           "Toca: A S D F G H J K L = l'escala en ordre, i Q W E R T Y U I O P segueixen m\xc3\xa9s agudes.   Z / X = avall / amunt un per\xc3\xado" "de."),
                       dontSendNotification);

    //-- Tooltips: one idea per sentence, no loose ends. ------------------------
    baseNFader.setTooltip (tr (
        "Which harmonic of a virtual fundamental the played key becomes. The spectrum is the harmonic series from N upward (N, N+1, N+2...), so raising N packs in more, closer partials. 1 = normal harmonic series. Hold Ctrl while dragging for fine control.",
        "Quin harm\xc3\xb2nic d'una fonamental virtual passa a ser la tecla tocada. L'espectre \xc3\xa9s la s\xc3\xa8rie harm\xc3\xb2nica des de N cap amunt (N, N+1, N+2...), aix\xc3\xad que pujar N afegeix parcials m\xc3\xa9s densos. 1 = s\xc3\xa8rie harm\xc3\xb2nica normal. Mant\xc3\xa9n Ctrl mentre arrossegues per a ajust fi."));
    basePitchBox.setTooltip (tr (
        "Where that series sits. 'Transposed down': the lowest surviving partial is moved onto the played pitch; with high N and Aligned phase this creates an audible slow pulse (the difference tone) - that is the intended effect, not extra notes. 'Original': harmonics stay at their natural frequencies, so raising N just makes the sound brighter and thinner.",
        "On queda la s\xc3\xa8rie. 'Transposada avall': el parcial m\xc3\xa9s greu es mou al to tocat; amb N alta i fase Alineada apareix un pols greu audible (el to diferencial) - \xc3\xa9s l'efecte buscat, no notes extra. 'Original': els harm\xc3\xb2nics queden a la seva freq\xc3\xbc\xc3\xa8ncia natural, i pujar N nom\xc3\xa9s fa el so m\xc3\xa9s brillant i prim."));
    whitelistBox.setTooltip (tr (
        "Keeps only these harmonic numbers: All, Even (2,4,6...), Odd (1,3,5...) or Prime (2,3,5,7...). A harmonic must ALSO pass Prime Control - both filters apply at once, and if together they exclude everything the sound is silent.",
        "Nom\xc3\xa9s deixa aquests n\xc3\xbameros d'harm\xc3\xb2nic: Tots, Parells (2,4,6...), Senars (1,3,5...) o Primers (2,3,5,7...). L'harm\xc3\xb2nic TAMB\xc3\x89 ha de passar el Control de primers - els dos filtres s'apliquen alhora, i si entre tots dos ho exclouen tot, no sona res."));
    slopeFader.setTooltip (tr (
        "How fast higher partials lose volume: 0 = all equal (brightest), 1 = like a saw (1/n), 2 = like a triangle, higher = darker. Also shapes the stacked scale in Timbre mode.",
        "Com de r\xc3\xa0pid perden volum els parcials aguts: 0 = tots iguals (m\xc3\xa9s brillant), 1 = com una serra (1/n), 2 = com un triangle, m\xc3\xa9s = m\xc3\xa9s fosc. Tamb\xc3\xa9 d\xc3\xb3na forma a l'escala apilada en mode Timbre."));
    fractalFader.setTooltip (tr (
        "Replaces each partial with a scaled copy of the whole spectrum, this many levels deep. 1 = off. Each level multiplies density and adds self-similar, inharmonic detail.",
        "Substitueix cada parcial per una c\xc3\xb2pia escalada de tot l'espectre, tants nivells com indiquis. 1 = apagat. Cada nivell multiplica la densitat i afegeix detall autosemblant i inharm\xc3\xb2nic."));
    phaseBox.setTooltip (tr (
        "Starting phase of the partials. 'Aligned': all peak together, forming a periodic pulse - tonal, never clips, and with dense spectra the pulse itself is heard as a low difference tone. 'Scattered': random phases - smoother, louder, no pulse.",
        "Fase inicial dels parcials. 'Alineat': tots fan el pic alhora i formen un pols peri\xc3\xb2""dic - tonal, mai clipa, i amb espectres densos el pols s'escolta com un to diferencial greu. 'Dispers': fases aleat\xc3\xb2ries - m\xc3\xa9s suau, m\xc3\xa9s fort, sense pols."));
    gainFader.setTooltip (tr ("Final output volume, in dB.", "Volum final de sortida, en dB."));
    primeModeBox.setTooltip (tr (
        "How prime numbers filter the harmonics. 'Limit': a harmonic passes only if its largest prime factor is at most the chosen limit. 'Whitelist': it passes only if EVERY prime factor is ticked below.",
        "Com filtren els primers els harm\xc3\xb2nics. 'L\xc3\xadmit': un harm\xc3\xb2nic passa nom\xc3\xa9s si el seu factor primer m\xc3\xa9s gran \xc3\xa9s com a molt el l\xc3\xadmit triat. 'Llista': passa nom\xc3\xa9s si TOTS els seus factors primers estan marcats a sota."));
    primeLimitBox.setTooltip (tr (
        "Largest prime factor a harmonic may contain. Example: limit 7 keeps 14 (2*7) and 25 (5*5) but drops 11 and 22 (2*11). Off = no prime filtering.",
        "Factor primer m\xc3\xa9s gran que pot tenir un harm\xc3\xb2nic. Exemple: l\xc3\xadmit 7 mant\xc3\xa9 14 (2*7) i 25 (5*5) per\xc3\xb2 elimina 11 i 22 (2*11). No = sense filtre de primers."));
    const char* pn2[] = { "2","3","5","7","11","13","17","19","23" };
    for (int i = 0; i < 9; ++i)
        primeToggle[i].setTooltip (tr ("Allow harmonics whose prime factors include ",
                                       "Permet harm\xc3\xb2nics amb el factor primer ") + juce::String (pn2[i]));
    cutoffFader.setTooltip (tr (
        "Low-pass cutoff as an INTERVAL above the played note, in cents (1200 = one octave, 12000 = fully open). It TRACKS the note, so the same partials are cut at every pitch - a partial at ratio r sits at 1200*log2(r) cents (e.g. harmonic 2 = 1200c, harmonic 3 = 1902c). An absolute-Hz cutoff would eat different partials depending on the note.",
        "Tall del passa-baixos com a INTERVAL per sobre de la nota tocada, en cents (1200 = una octava, 12000 = tot obert). SEGUEIX la nota, aix\xc3\xad que talla els mateixos parcials a qualsevol al\xc3\xa7""ada - un parcial de ratio r \xc3\xa9s a 1200*log2(r) cents (p. ex. harm\xc3\xb2nic 2 = 1200c, harm\xc3\xb2nic 3 = 1902c). Un tall en Hz absoluts es menjaria parcials diferents segons la nota."));
    resFader.setTooltip (tr ("Resonance: a volume peak right at the cutoff frequency.",
                             "Res(son\xc3\xa0ncia): un pic de volum just a la freq\xc3\xbc\xc3\xa8ncia de tall."));
    fEnvAmtFader.setTooltip (tr (
        "How many octaves the filter envelope moves the cutoff. Positive opens it, negative closes it, 0 = envelope off.",
        "Quantes octaves mou el tall l'envolupant de filtre. Positiu l'obre, negatiu el tanca, 0 = envolupant sense efecte."));
    for (int i = 0; i < 4; ++i)
    {
        ampAdsr[i].setTooltip (tr ("Volume envelope: Attack, Decay, Sustain (level), Release. Times in seconds.",
                                   "Envolupant de volum: Atac, Decay, Sustain (nivell), Release. Temps en segons."));
        filtAdsr[i].setTooltip (tr ("Filter envelope: Attack, Decay, Sustain (level), Release. Times in seconds.",
                                    "Envolupant de filtre: Atac, Decay, Sustain (nivell), Release. Temps en segons."));
        pitchAdsr[i].setTooltip (tr ("Pitch envelope: Attack, Decay, Sustain (level), Release. Bends pitch by the Amount (semitones). Sustain 0 = a pitch blip that settles back to the played note.",
                                     "Envolupant de to: Atac, Decay, Sustain (nivell), Release. Desafina el to segons la Quantitat (semitons). Sustain 0 = un cop de to que torna a la nota tocada."));
    }
    tuningBox.setTooltip (tr (
        "Which tuning the keys follow. 12-TET: standard. MTS-ESP: follow an ODDSound MTS-ESP master running in the session. Scala: the .scl/.ascl file loaded below. Note: Ableton Live's own tuning arrives as MPE pitch bend and works in ANY of these modes (set Bend to +-48).",
        "Quina afinaci\xc3\xb3 segueixen les tecles. 12-TET: est\xc3\xa0ndard. MTS-ESP: segueix un mestre MTS-ESP d'ODDSound actiu a la sessi\xc3\xb3. Scala: el fitxer .scl/.ascl carregat a sota. Nota: l'afinaci\xc3\xb3 pr\xc3\xb2pia d'Ableton Live arriba com a pitch bend MPE i funciona en QUALSEVOL d'aquests modes (posa Bend a +-48)."));
    loadSclButton.setTooltip (tr (
        "Load a Scala .scl or Ableton .ascl file to retune the keys. The 1/1 sits on middle C unless the file sets its own reference (@ABL REFERENCE_PITCH).",
        "Carrega un fitxer Scala .scl o Ableton .ascl per reafinar les tecles. L'1/1 queda al Do central tret que el fitxer porti la seva refer\xc3\xa8ncia (@ABL REFERENCE_PITCH)."));
    clearSclButton.setTooltip (tr ("Unload the scale and return to standard 12-TET.",
                                   "Descarrega l'escala i torna al 12-TET est\xc3\xa0ndard."));
    for (int i = 0; i < 3; ++i)
        recentBtn[i].setTooltip (tr ("Reload this recently used file as the scale.",
                                     "Torna a carregar aquest fitxer recent com a escala."));
    bendBox.setTooltip (tr (
        "Per-note pitch-bend range. +-48 st is the MPE standard and is REQUIRED for Ableton Live 12's tuning systems and the Microtuner device (enable MPE on this plugin in Live). Use +-2 for a classic keyboard pitch wheel.",
        "Rang del pitch bend per nota. +-48 st \xc3\xa9s l'est\xc3\xa0ndard MPE i \xc3\x89S NECESSARI per als sistemes d'afinaci\xc3\xb3 d'Ableton Live 12 i el Microtuner (activa MPE al plugin dins de Live). Fes servir +-2 per a la roda d'un teclat cl\xc3\xa0ssic."));
    loadTimbreButton.setTooltip (tr (
        "Load a .scl/.ascl whose intervals become the PARTIALS of the sound, replacing the harmonic engine. Slope, Fractal, Range and Reps still shape it; the greyed-out controls do not apply.",
        "Carrega un .scl/.ascl els intervals del qual passen a ser els PARCIALS del so, substituint el motor harm\xc3\xb2nic. Pendent, Fractal, Rang i Reps encara hi actuen; els controls en gris no apliquen."));
    clearTimbreButton.setTooltip (tr ("Turn Timbre mode off and return to the harmonic engine.",
                                      "Apaga el mode Timbre i torna al motor harm\xc3\xb2nic."));
    timbreRepeatsFader.setTooltip (tr (
        "How many periods of the scale are stacked as partials. More repeats = the timbre reaches higher in the spectrum.",
        "Quants per\xc3\xado""des de l'escala s'apilen com a parcials. M\xc3\xa9s repeticions = el timbre arriba m\xc3\xa9s amunt de l'espectre."));
    presetBox.setTooltip (tr ("Load a saved preset.", "Carrega un preset desat."));
    savePresetButton.setTooltip (tr ("Save the current sound as a preset (stored in Documents/Galactic Sting/Presets).",
                                     "Desa el so actual com a preset (a Documents/Galactic Sting/Presets)."));
    dissBtn.setTooltip (tr (
        "Computes the Sethares sensory-dissonance curve of the CURRENT timbre and builds a scale from its most consonant points (the curve's minima), then loads it as the active tuning. With Max ticked it uses the roughest points (maxima) instead.",
        "Calcula la corba de dissonancia sensorial de Sethares del timbre ACTUAL i construeix una escala amb els seus punts m\xc3\xa9s consonants (els m\xc3\xadnims de la corba), i la carrega com a afinaci\xc3\xb3 activa. Amb Max marcat fa servir els punts m\xc3\xa9s aspres (m\xc3\xa0xims)."));
    dissCountBox.setTooltip (tr (
        "How many notes the generated scale gets (curve minima - or maxima in Max mode), plus the period as the final note.",
        "Quantes notes tindra l'escala generada (m\xc3\xadnims de la corba - o m\xc3\xa0xims en mode Max), m\xc3\xa9s el per\xc3\xado" "de com a nota final."));
    dissModeBox.setTooltip (tr (
        "Min = pick the curve's minima (the most consonant intervals).  Max = pick the maxima (the most dissonant intervals) instead.",
        "Min = tria els m\xc3\xadnims de la corba (els intervals m\xc3\xa9s consonants).  Max = tria els m\xc3\xa0xims (els m\xc3\xa9s dissonants) en lloc d'aix\xc3\xb2."));
    dissSaveBtn.setTooltip (tr (
        "Save the generated scale as a .scl file (it also joins the Recent list).",
        "Desa l'escala generada com a fitxer .scl (tamb\xc3\xa9 s'afegeix a Recents)."));
    periodEdit.setTooltip (tr (
        "Period of the generated scale: an integer (\"3\"), a fraction (\"3/2\"), or cents with a trailing c (\"701.955c\"). Default 2 = octave. Wider periods find new extremes inside them; narrower ones omit those outside.",
        "Per\xc3\xado" "de de l'escala generada: un enter (\"3\"), una fracci\xc3\xb3 (\"3/2\") o cents amb una c final (\"701.955c\"). Per defecte 2 = octava. Un per\xc3\xado" "de m\xc3\xa9s ample troba nous extrems a dins; un de m\xc3\xa9s estret omet els de fora."));
    refAEdit.setTooltip (tr (
        "Reference pitch: A = this many Hz (default 440). Transposes the WHOLE instrument in every tuning mode. Type a value and press Enter.",
        "To de refer\xc3\xa8ncia: La = aquests Hz (per defecte 440). Transposa TOT l'instrument en qualsevol mode d'afinaci\xc3\xb3. Escriu un valor i prem Enter."));
    octaveFader.setTooltip (tr (
        "Transpose the WHOLE synth by whole octaves (-5..+5), on top of the reference.",
        "Transposa TOT el sintetitzador per octaves senceres (-5..+5), a sobre de la refer\xc3\xa8ncia."));

    auto localise = [] (juce::ComboBox& b, const juce::StringArray& items)
    {
        for (int i = 0; i < items.size(); ++i) b.changeItemText (i + 1, items[i]);
        b.setSelectedId (b.getSelectedId(), juce::dontSendNotification); // refresh displayed text
    };
    localise (basePitchBox, { tr ("Transposed down", "Transposada avall"), tr ("Original", "Original") });
    localise (whitelistBox, { tr ("All", "Tots"), tr ("Even", "Parells"), tr ("Odd", "Senars"), tr ("Prime", "Primers") });
    localise (phaseBox,     { tr ("Aligned", "Alineat"), tr ("Scattered", "Dispers") });
    localise (primeModeBox, { tr ("Limit", "L\xc3\xadmit"), tr ("Whitelist", "Llista") });
    localise (primeLimitBox,{ tr ("Off", "No"), "2","3","5","7","11","13","17","19","23" });
    localise (bendBox,      { "\xc2\xb1""2 st", "\xc2\xb1""12 st", "\xc2\xb1""48 st (MPE)" });

    repaint(); // title uses the language
}

//==============================================================================
void FractalAdditiveAudioProcessorEditor::refreshPresetList()
{
    const auto names = processorRef.getPresetNames();
    const auto current = presetBox.getText();
    presetBox.clear (juce::dontSendNotification);
    int id = 1;
    for (const auto& n : names)
        presetBox.addItem (n, id++);
    // keep showing the loaded preset's name without re-triggering a load
    if (current.isNotEmpty() && names.contains (current))
        presetBox.setText (current, juce::dontSendNotification);
}

void FractalAdditiveAudioProcessorEditor::savePresetFlow()
{
    auto* aw = new juce::AlertWindow (tr ("Save preset", "Desar preset"),
                                      tr ("Name for this preset:", "Nom per a aquest preset:"),
                                      juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("name", presetBox.getText(), {});
    aw->addButton (tr ("Save", "Desa"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (tr ("Cancel", "Cancel\xc2\xb7la"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    activePrompt = aw; // so the destructor can dismiss an orphaned dialog

    // The AlertWindow is a top-level window that OUTLIVES this editor if the host
    // closes the plugin UI while the dialog is up -> capture a SafePointer, never
    // raw `this`, and bail out if the editor is gone.
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<FractalAdditiveAudioProcessorEditor> (this), aw] (int result)
        {
            if (safe == nullptr)
                return;

            const auto name = aw->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty())
            {
                const auto stored = safe->processorRef.savePreset (name); // sanitised name, or empty
                if (stored.isNotEmpty())
                {
                    safe->refreshPresetList();
                    safe->presetBox.setText (stored, juce::dontSendNotification);
                }
                else
                {
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        safe->tr ("Save preset", "Desar preset"),
                        safe->tr ("Could not save the preset. Try a different name.",
                                  "No s'ha pogut desar el preset. Prova un altre nom."),
                        safe.getComponent());
                }
            }
            safe->refocus();
        }), true);
}

void FractalAdditiveAudioProcessorEditor::runDissonanceTune()
{
    double periodRatio = 2.0;
    juce::String periodLine = "2/1";
    if (! TuningManager::parsePeriodSpec (periodEdit.getText(), periodRatio, periodLine))
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Spectrum to Scale", "Espectre a escala"),
            tr ("Invalid period. Use an integer (\"3\"), a fraction (\"3/2\"), or cents with a trailing c (\"701.955c\").",
                "Per\xc3\xado" "de no v\xc3\xa0lid. Fes servir un enter (\"3\"), una fracci\xc3\xb3 (\"3/2\") o cents amb una c final (\"701.955c\")."),
            this);
        refocus();
        return;
    }

    const int  n      = juce::jmax (1, dissCountBox.getSelectedId());
    const bool maxima = dissModeBox.getSelectedId() == 2;
    juce::String err;
    if (! processorRef.generateDissonanceScale (n, maxima, periodRatio, periodLine, err))
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Spectrum to Scale", "Espectre a escala"), err, this);
    refocus();
}

void FractalAdditiveAudioProcessorEditor::updateScaleDissonance()
{
    // Cheap change signature: recompute the per-degree dissonance only when the
    // timbre or the tuning actually changes. Combines the timbre template
    // generation, the tuning mode + degree count, the absolute pitch (octave shift
    // and reference-A both move roughness via Sethares' frequency term), and a few
    // degree ratios (captures a loaded/generated .scl or a tuning-bank switch).
    const int D = juce::jmax (1, processorRef.getScaleDegrees());
    double sig = (double) processorRef.getTemplateGeneration() * 101.0
               + (double) processorRef.getTuningMode() * 7.0
               + (double) D * 0.5
               + (double) processorRef.apvts.getRawParameterValue ("octaveShift")->load() * 13.0
               + (double) processorRef.apvts.getRawParameterValue ("refA")->load() * 0.01;
    for (int d = 1; d <= juce::jmin (D, 16); ++d) sig += processorRef.getDegreeRatio (d) * (double) (d + 3);
    if (std::abs (sig - lastScaleSig) < 1.0e-6) return;
    lastScaleSig = sig;

    activeDiss = processorRef.computeActiveScaleDissonance();
    rebuildScaleDissList();
}

void FractalAdditiveAudioProcessorEditor::rebuildScaleDissList()
{
    const int D = juce::jmax (1, processorRef.getScaleDegrees());

    // Green (most-consonant) -> yellow -> red (most-dissonant) anchored at the
    // actual min/max height of THIS scale's degrees.
    double lo = 1.0e30, hi = -1.0e30;
    for (const auto& d : activeDiss) { lo = juce::jmin (lo, d.height); hi = juce::jmax (hi, d.height); }

    // Always ordered by dissonance (most consonant first).
    auto sorted = activeDiss;
    std::sort (sorted.begin(), sorted.end(),
               [] (const auto& a, const auto& b) { return a.height < b.height; });

    degreeList.rows.clear();
    for (const auto& d : sorted)
    {
        DissDegreeList::Row row;
        row.step     = d.degree;
        row.ratio    = d.ratio;
        row.height   = d.height;
        row.isPeriod = (d.degree == D); // the octave/period row -> red box
        row.num      = juce::String (row.isPeriod ? 0 : d.degree); // period shows "0" like the keyboard boundary
        row.label    = juce::String::fromUTF8 (dissonance::justIntervalName (d.ratio).c_str());
        row.value    = juce::String (d.height, 2);
        row.chip     = juce::Colour::fromHSV (0.82f * (float) (d.degree % D) / (float) D, 0.62f, 0.9f, 1.0f);
        const double t = juce::jlimit (0.0, 1.0, (d.height - lo) / juce::jmax (1.0e-12, hi - lo));
        const juce::Colour green (0xff3ec659), yellow (0xffe8d44d), red (0xffe0453f);
        row.valueCol = t < 0.5 ? green.interpolatedWith (yellow, (float) (t * 2.0))
                               : yellow.interpolatedWith (red, (float) ((t - 0.5) * 2.0));
        degreeList.rows.push_back (std::move (row));
    }
    // Re-size for the Viewport (scrolls when the list is taller than the box).
    const int visH = dissView.getHeight();
    const int ch   = degreeList.contentHeight();
    const bool scroll = ch > visH && visH > 0;
    degreeList.setSize (scroll ? dissView.getWidth() - 10 : dissView.getWidth(),
                        juce::jmax (visH, ch));
    degreeList.repaint();
}

void FractalAdditiveAudioProcessorEditor::refreshDegreeList (bool showNotice)
{
    // The degree LIST is now the live active-scale list (updateScaleDissonance);
    // this only surfaces the "fewer degrees than requested" notice after a Transmute.
    // Fewer extremes than requested -> closable notice explaining why. A
    // generation that satisfies the request dismisses any stale notice.
    if (processorRef.getDissFound() >= processorRef.getDissRequested() || ! showNotice)
    {
        if (processorRef.getDissFound() >= processorRef.getDissRequested())
            noticeOverlay.setVisible (false);
    }
    else
    {
        noticeOverlay.title = tr ("Fewer degrees than requested", "Menys graus dels demanats");
        noticeOverlay.body  = tr ("You asked for ", "N'has demanat ")
            + juce::String (processorRef.getDissRequested())
            + tr (" but this timbre's dissonance curve only has ", " per\xc3\xb2 la corba de dissonancia d'aquest timbre nom\xc3\xa9s t\xc3\xa9 ")
            + juce::String (processorRef.getDissFound())
            + tr (" usable extremes in the chosen period. A curve only forms a notch where partials interact; sparse spectra (few partials, e.g. a near-sine) form few notches - a pure sine forms none. The scale was generated with all the extremes that exist. Add partials (higher N, Fractal, flatter Slope) or widen the period for more.",
                  " extrems usables dins del per\xc3\xado" "de triat. La corba nom\xc3\xa9s fa un clot on els parcials interactuen; els espectres escassos (pocs parcials) en fan pocs - un sinus pur no en fa cap. L'escala s'ha generat amb tots els extrems existents. Afegeix parcials (N m\xc3\xa9s alta, Fractal, Pendent m\xc3\xa9s pla) o eixampla el per\xc3\xado" "de per obtenir-ne m\xc3\xa9s.");
        noticeOverlay.setVisible (true);
        noticeOverlay.toFront (false);
        noticeOverlay.repaint();
    }
}

void FractalAdditiveAudioProcessorEditor::saveDissonanceScl()
{
    if (! processorRef.hasGeneratedScl())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            tr ("Save scale", "Desar escala"),
            tr ("Generate a dissonance scale first (the 'Diss. tune' button).",
                "Primer genera una escala de dissonancia (bot\xc3\xb3 'Afina diss.')."), this);
        return;
    }

    fileChooser = std::make_unique<juce::FileChooser> (
        tr ("Save the generated scale (.scl)", "Desa l'escala generada (.scl)"),
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile (processorRef.getGeneratedSclName() + ".scl"),
        "*.scl");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.saveGeneratedScl (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "Save scale", err, this);
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::updateWarning (int templatePartials)
{
    const bool silent = (templatePartials == 0) && ! processorRef.isTimbreActive();
    if (! silent)
    {
        warningOverlay.setVisible (false);
        return;
    }

    const auto& ap = processorRef.apvts;
    const int  sel       = (int) ap.getRawParameterValue ("selection")->load();
    const bool wl        = ap.getRawParameterValue ("primeMode")->load() > 0.5f;
    const int  limitIdx  = (int) ap.getRawParameterValue ("primeLimit")->load();
    const int  baseN     = (int) ap.getRawParameterValue ("baseN")->load();
    unsigned mask = 0;
    for (int i = 0; i < 9; ++i)
        if (ap.getRawParameterValue ("primeWL" + juce::String (i))->load() > 0.5f)
            mask |= (1u << i);

    // Plain-language cause + the single most direct fix. The synth has two
    // harmonic filters (Harmonics and Prime Control) that BOTH must let a
    // harmonic through; when they overlap to nothing, every partial is removed.
    juce::String why;
    if (sel == 3 && wl)
        why = tr ("Two filters are cancelling out.  'Harmonics = Prime' keeps only prime-numbered harmonics, and the Prime whitelist then keeps only harmonics built from the ticked primes.  A prime is only itself, so together they can keep nothing above 23.\n\nFix: set Harmonics to 'All', or set Prime Control to 'Limit / Off'.",
                  "Dos filtres s'anul\xc2\xb7len.  'Harm\xc3\xb2nics = Primers' nom\xc3\xa9s deixa els harm\xc3\xb2nics de n\xc3\xbamero primer, i la Llista de primers nom\xc3\xa9s deixa els fets amb els primers marcats.  Un primer nom\xc3\xa9s \xc3\xa9s ell mateix, aix\xc3\xad que junts no poden deixar res per sobre de 23.\n\nSoluci\xc3\xb3: posa Harm\xc3\xb2nics en 'Tots', o el Control de primers en 'L\xc3\xadmit / No'.");
    else if (wl && mask == 0)
        why = tr ("The Prime whitelist has no primes ticked, so no harmonic is allowed through.\n\nFix: tick at least one prime, or switch Prime Control to 'Limit'.",
                  "La Llista de primers no t\xc3\xa9 cap primer marcat, aix\xc3\xad que no passa cap harm\xc3\xb2nic.\n\nSoluci\xc3\xb3: marca almenys un primer, o posa el Control de primers en 'L\xc3\xadmit'.");
    else if (sel == 3 && ! wl && limitIdx > 0)
        why = tr ("'Harmonics = Prime' keeps only prime harmonics, but the Prime Limit then removes every prime bigger than the limit - and your Base N starts the series above it.\n\nFix: raise the Prime Limit (or set it to Off), or lower Base N.",
                  "'Harm\xc3\xb2nics = Primers' nom\xc3\xa9s deixa harm\xc3\xb2nics primers, per\xc3\xb2 el L\xc3\xadmit de primers treu tot primer m\xc3\xa9s gran que el l\xc3\xadmit - i la teva Base N comen\xc3\xa7""a la s\xc3\xa8rie per sobre.\n\nSoluci\xc3\xb3: puja el L\xc3\xadmit de primers (o posa'l en No), o abaixa la Base N.");
    else
        why = tr ("The Harmonics filter and the Prime Control filter, applied together, remove every harmonic of the current Base N.\n\nFix: relax either filter (Harmonics = All, or Prime Control = Off), or change Base N.",
                  "El filtre d'Harm\xc3\xb2nics i el de Control de primers, aplicats alhora, treuen tots els harm\xc3\xb2nics de la Base N actual.\n\nSoluci\xc3\xb3: relaxa qualsevol dels dos (Harm\xc3\xb2nics = Tots, o Control de primers = No), o canvia la Base N.");

    juce::ignoreUnused (baseN);
    warningOverlay.title = tr ("No sound: every partial is filtered out",
                               "Sense so: s'han filtrat tots els parcials");
    warningOverlay.body  = why;
    warningOverlay.setVisible (true);
    warningOverlay.repaint();
}

void FractalAdditiveAudioProcessorEditor::openScalaChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a scale (.scl / .ascl)", juce::File(), "*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.loadScalaFile (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load scale", err, this);
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::openTuningBankChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a .scl into the tuning bank", juce::File(), "*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                // Always ADD a new scale to the bank (first free slot) - there is no
                // practical limit; the bank just keeps growing.
                processorRef.setTuningBankFile (processorRef.firstEmptyBankSlot(), fc.getResult());
                refreshTuningBank();
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::refreshTuningBank()
{
    // List ONLY the loaded slots (the bank is effectively unlimited, so we never show
    // the empty ones), plus "- none -". Item id = slot value + 1, so id-1 is always the
    // automatable parameter value regardless of which slots are filled.
    const int cur = processorRef.getTuningBankSlotParam(); // 0 = none
    tuningBankBox.clear (juce::dontSendNotification);
    tuningBankBox.addItem (tr ("- none -", "- cap -"), 1); // id 1 -> slot value 0
    for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i)
    {
        const auto nm = processorRef.getTuningBankName (i);
        if (nm.isNotEmpty())
            tuningBankBox.addItem (juce::String (i + 1) + ": " + nm, i + 2); // id i+2 -> slot value i+1
    }
    tuningBankBox.setSelectedId (cur + 1, juce::dontSendNotification);
}

void FractalAdditiveAudioProcessorEditor::openTimbreChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a scale file as timbre (.scl / .ascl)", juce::File(), "*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.loadTimbreFile (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load timbre", err, this);
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::applyTimbreEnablement (bool active)
{
    const bool en = ! active;
    baseNFader.setEnabled (en);
    for (auto* c : { &basePitchBox, &whitelistBox, &phaseBox, &primeModeBox, &primeLimitBox })
        c->setEnabled (en);
    for (auto& t : primeToggle) t.setEnabled (en);

    const float a = active ? 0.4f : 1.0f;
    phaseBox.setAlpha (a);
    baseNFader.setAlpha (a); basePitchBox.setAlpha (a); whitelistBox.setAlpha (a);
    phaseBox.setAlpha (a); primeModeBox.setAlpha (a); primeLimitBox.setAlpha (a);
    for (auto& t : primeToggle) t.setAlpha (a);
    for (auto& l : primeLbl)    l.setAlpha (a);
    lblBaseN.setAlpha (a); lblBasePitch.setAlpha (a); lblPrimeLimit.setAlpha (a);

    // Reps only does anything once a custom spectrum is loaded/typed -> grey it off.
    timbreRepeatsFader.setEnabled (active);
    timbreRepeatsFader.setAlpha (active ? 1.0f : 0.4f);
    lblTimbreRepeats.setAlpha (active ? 1.0f : 0.4f);

    // Repaint so the Harmonic-Engine interior (drawn in paint(), not a child) dims
    // too when a custom engine takes over - not just the child controls.
    repaint();
}

void FractalAdditiveAudioProcessorEditor::commitRefA()
{
    if (auto* p = processorRef.apvts.getParameter ("refA"))
    {
        const float hz = refAEdit.getText().getFloatValue();
        if (hz > 0.0f)
        {
            const auto& range = processorRef.apvts.getParameterRange ("refA");
            p->setValueNotifyingHost (range.convertTo0to1 (juce::jlimit (range.start, range.end, hz)));
        }
    }
    refreshRefAText();
}

void FractalAdditiveAudioProcessorEditor::refreshRefAText()
{
    const float hz = processorRef.apvts.getRawParameterValue ("refA")->load();
    const auto txt = juce::String (hz, 2);
    if (refAEdit.getText() != txt && ! refAEdit.hasKeyboardFocus (false))
        refAEdit.setText (txt, juce::dontSendNotification);
}

void FractalAdditiveAudioProcessorEditor::makeLabel (juce::Label& l, juce::Colour c)
{
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, c.brighter (0.15f));
    l.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
    addAndMakeVisible (l);
}

void FractalAdditiveAudioProcessorEditor::configGroup (juce::GroupComponent& g, juce::Colour col)
{
    g.setColour (juce::GroupComponent::outlineColourId, col);
    g.setColour (juce::GroupComponent::textColourId, col);
    addAndMakeVisible (g);
}

void FractalAdditiveAudioProcessorEditor::configFader (juce::Slider& s, const juce::String& paramID,
                                                       std::unique_ptr<SliderAttachment>& att, juce::Colour col,
                                                       const juce::String& tooltip)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 46, 15);
    s.setColour (juce::Slider::thumbColourId, col);
    s.setColour (juce::Slider::trackColourId, col.withAlpha (0.55f));
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setMouseClickGrabsKeyboardFocus (false);
    s.setTooltip (tooltip);
    // Uniform fine-adjust: hold Ctrl while dragging ANY fader to switch to slow
    // velocity mode (some sliders had it by default, others not).
    s.setVelocityModeParameters (0.6, 1, 0.06, true, juce::ModifierKeys::ctrlModifier);
    s.onDragEnd = [this] { refocus(); };
    addAndMakeVisible (s);
    att = std::make_unique<SliderAttachment> (processorRef.apvts, paramID, s);
}

void FractalAdditiveAudioProcessorEditor::configCombo (juce::ComboBox& b, const juce::String& paramID,
                                                       std::unique_ptr<ComboBoxAttachment>& att, juce::Colour col,
                                                       const juce::String& tooltip)
{
    b.setColour (juce::ComboBox::outlineColourId, col.withAlpha (0.7f));
    b.setColour (juce::ComboBox::arrowColourId, col);
    b.setMouseClickGrabsKeyboardFocus (false);
    b.setTooltip (tooltip);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter (paramID)))
        b.addItemList (choice->choices, 1);
    b.onChange = [this] { refocus(); };
    addAndMakeVisible (b);
    att = std::make_unique<ComboBoxAttachment> (processorRef.apvts, paramID, b);
}

void FractalAdditiveAudioProcessorEditor::layoutFaders (juce::GroupComponent& g, juce::Rectangle<int> area, juce::Colour,
                                                        std::initializer_list<juce::Slider*> faders,
                                                        std::initializer_list<juce::Label*> labels)
{
    g.setBounds (area.reduced (3));
    auto in = area.reduced (3).reduced (5, 4); in.removeFromTop (17); // clear the title chip
    const int n = (int) faders.size();
    const int cw = in.getWidth() / juce::jmax (1, n);
    auto fit = faders.begin(); auto lit = labels.begin();
    for (int i = 0; i < n; ++i, ++fit, ++lit)
    {
        auto c = in.removeFromLeft (cw);
        // Named faders (A/D/S/R) carry the letter inside the thumb -> no label above.
        const bool inThumb = (*fit)->getName().isNotEmpty();
        (*lit)->setVisible (! inThumb);
        if (! inThumb) (*lit)->setBounds (c.removeFromTop (13));
        (*fit)->setBounds (c);
    }
}

//==============================================================================
void FractalAdditiveAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0c0c12));

    auto titleArea = getLocalBounds().removeFromTop (68);

    // Title layout is computed FIRST (positions + colours + glyph paths) but the
    // squares are DRAWN LAST, so the connector lines can tuck their ends under the
    // letters. Each letter is an equal square, keyboard-hue coloured (first/last
    // red), the glyph stretched to fill it.
    const auto title  = juce::String::fromUTF8 ("Fibl\xc3\xb3 Gal\xc3\xa0" "ctic");
    const int  L      = title.length();
    const int  period = juce::jmax (1, L - 1);
    float       side  = 52.0f;
    const float gap   = 4.0f;
    int squaresN = 0, spaces = 0;
    for (int i = 0; i < L; ++i) (title[i] == ' ' ? spaces : squaresN)++;
    float total = (float) squaresN * (side + gap) + (float) spaces * (side * 0.5f);
    const float freeLeft  = 214.0f;                      // clears credit/Menu/Presets/Save
    const float freeRight = (float) getWidth() - 186.0f; // clears the Spectrum-to-Rhythm button
    const float freeW     = juce::jmax (120.0f, freeRight - freeLeft);
    if (total > freeW) { side *= freeW / total; total = freeW; }
    const float margin = side * 0.10f;
    const juce::Font glyphFont = titleTypeface() != nullptr
        ? juce::Font (juce::FontOptions (titleTypeface()).withHeight (64.0f))
        : juce::Font (juce::FontOptions (64.0f).withStyle ("Bold"));

    struct Sq { juce::Rectangle<float> rect; juce::Colour col; juce::Path glyph; juce::Colour glyphCol; };
    std::vector<Sq> squares;
    {
        // Centre the title on the FULL window width (clamped to the free zone), so
        // "Fibló Galàctic" sits truly centred rather than centred-between-buttons.
        float x = ((float) getWidth() - total) * 0.5f;
        x = juce::jmax (x, freeLeft);
        if (x + total > freeRight) x = freeRight - total;
        const float cy = (float) titleArea.getCentreY();
        for (int i = 0; i < L; ++i)
        {
            if (title[i] == ' ') { x += side * 0.5f; continue; }
            const float hue = 0.82f * (float) (i % period) / (float) period;
            Sq s;
            s.rect = juce::Rectangle<float> (x, cy - side * 0.5f, side, side);
            s.col  = juce::Colour::fromHSV (hue, 0.62f, 0.9f, 1.0f);
            juce::GlyphArrangement ga;
            ga.addLineOfText (glyphFont, title.substring (i, i + 1), 0.0f, 0.0f);
            juce::Path gp; ga.createPath (gp);
            const auto gb = gp.getBounds();
            if (! gb.isEmpty() && gb.getWidth() > 0.0f && gb.getHeight() > 0.0f)
            {
                const auto target = s.rect.reduced (margin);
                gp.applyTransform (juce::AffineTransform::translation (-gb.getCentreX(), -gb.getCentreY())
                                       .scaled (target.getWidth() / gb.getWidth(),
                                                target.getHeight() / gb.getHeight())
                                       .translated (target.getCentreX(), target.getCentreY()));
                s.glyph    = gp;
                s.glyphCol = s.col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white;
            }
            squares.push_back (std::move (s));
            x += side + gap;
        }
    }

    // Harmonic Engine macro-group: the WHOLE box is drawn here - frame + fill + title
    // (like the other boxes but in ONE colour) + the SHARED internal dividers between
    // its sub-cells + each cell's small inline uppercase label. Drawn in the editor
    // paint so it sits UNDER the child controls but is never covered by a child fill.
    // Shared frame + outlined title for a macro-group box (Harmonic Engine,
    // Essentials): interiorAlpha < 1 greys just the fill (the frame + title stay
    // bright), used to dim the Harmonic Engine's INSIDE when a custom engine runs.
    auto macroFrameTitle = [&] (juce::Rectangle<int> boxI, juce::Colour col, const juce::String& ttl,
                                float interiorAlpha)
    {
        constexpr float titleH = 21.0f;
        auto box = boxI.toFloat();
        auto frame = juce::Rectangle<float> (box.getX() + 1.3f, box.getY() + titleH * 0.5f,
                                             box.getWidth() - 2.6f, box.getHeight() - titleH * 0.5f - 1.3f);
        const auto shape = topRoundedFrame (frame, 9.0f); // rounded top, angular bottom
        g.setColour (juce::Colour (0xff17171f).interpolatedWith (col, 0.05f * interiorAlpha));
        g.fillPath (shape);
        g.setColour (col.withAlpha (0.95f));
        g.strokePath (shape, juce::PathStrokeType (2.6f));
        drawTitleChip (g, box, col, ttl); // colour chip, bounded to the text
    };
    auto subLabel = [&] (const juce::Rectangle<int>& c, juce::Colour col, const juce::String& s, float al)
    {
        g.setColour (col.withMultipliedAlpha (al));
        g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
        g.drawText (s, c.withHeight (13).reduced (5, 0), juce::Justification::centredLeft, false);
    };

    if (! harmonicEngineBounds.isEmpty())
    {
        // Grey the Harmonic Engine INTERIOR (fill + dividers + labels) when a custom
        // engine is driving the timbre - the engine's controls do nothing then.
        const float dimF = processorRef.isTimbreActive() ? 0.4f : 1.0f;
        macroFrameTitle (harmonicEngineBounds, cHarmonic, tr ("HARMONIC ENGINE", "MOTOR HARM\xc3\x92NIC"), dimF);
        g.setColour (cHarmonic.withAlpha (0.5f * dimF)); // shared internal dividers
        const float top = (float) harmonicEngineBounds.getY() + 11.5f; // frame's inner top edge (was hcBase.getY(), ~10px low)
        const float bot = (float) harmonicEngineBounds.getBottom() - 3.0f;
        g.drawLine ((float) hcHarm.getX(),  top, (float) hcHarm.getX(),  bot, 1.0f);
        g.drawLine ((float) hcSlope.getX(), top, (float) hcSlope.getX(), bot, 1.0f);
        g.drawLine ((float) hcHarm.getX(), (float) hcPrime.getY(),
                    (float) hcHarm.getRight(), (float) hcPrime.getY(), 1.0f);
        subLabel (hcBase,  cHarmonic, tr ("BASE", "BASE"),           dimF);
        subLabel (hcHarm,  cHarmonic, tr ("HARMONICS", "HARM\xc3\x92NICS"), dimF);
        // PRIME sits right under the shared divider -> nudge it down for breathing room.
        subLabel (hcPrime.translated (0, 5), cHarmonic, tr ("PRIME", "PRIMERS"), dimF);
        subLabel (hcSlope, cHarmonic, tr ("SLOPE", "PENDENT"),       dimF);
    }

    // Essentials macro-group: one framed box, one colour, PHASE / GAIN / REFERENCE /
    // OCTAVE stacked with shared horizontal dividers + inline labels (like the ADSR
    // letters). Same treatment as the Harmonic Engine.
    if (! essentialsBounds.isEmpty())
    {
        macroFrameTitle (essentialsBounds, cEssentials, tr ("ESSENTIALS", "ESSENCIALS"), 1.0f);
        g.setColour (cEssentials.withAlpha (0.5f)); // shared horizontal dividers between the cells
        const float lx = (float) essentialsBounds.getX() + 4.0f;
        const float rx = (float) essentialsBounds.getRight() - 4.0f;
        for (auto* cell : { &ecGain, &ecRef, &ecOct })
            g.drawLine (lx, (float) cell->getY(), rx, (float) cell->getY(), 1.0f);
        subLabel (ecPhase, cEssentials, tr ("PHASE", "FASE"),        1.0f);
        subLabel (ecGain,  cEssentials, tr ("GAIN", "GUANY"),        1.0f);
        subLabel (ecRef,   cEssentials, tr ("REFERENCE", "REFER\xc3\x88NCIA"), 1.0f);
        subLabel (ecOct,   cEssentials, tr ("OCTAVE", "OCTAVA"),     1.0f);
    }

    // Inverted-L TUNING box: the dissonance BAR (right, tall) + the controls FOOT
    // (bottom-left), one frame, one colour, with an internal divider between them.
    if (! lFootBounds.isEmpty() && ! lBarBounds.isEmpty())
    {
        constexpr float titleH = 21.0f;
        auto barF = juce::Rectangle<float> ((float) lBarBounds.getX() + 1.3f, (float) lBarBounds.getY() + 1.3f,
                                            (float) lBarBounds.getWidth() - 2.6f, (float) lBarBounds.getHeight() - 2.6f);
        auto footF = juce::Rectangle<float> ((float) lFootBounds.getX() + 1.3f, (float) lFootBounds.getY() + titleH * 0.5f,
                                             barF.getX() - ((float) lFootBounds.getX() + 1.3f), 0.0f);
        footF.setBottom (barF.getBottom());
        const auto shape = lShapedFrame (barF, footF, 9.0f);
        g.setColour (juce::Colour (0xff17171f).interpolatedWith (cTuning, 0.05f));
        g.fillPath (shape);
        g.setColour (cTuning.withAlpha (0.95f));
        g.strokePath (shape, juce::PathStrokeType (2.6f));
        g.setColour (cTuning.withAlpha (0.5f)); // divider between FOOT (controls) and BAR (dissonance)
        g.drawLine (barF.getX(), footF.getY(), barF.getX(), barF.getBottom(), 1.0f);
        drawTitleChip (g, juce::Rectangle<float> ((float) lFootBounds.getX(), (float) lFootBounds.getY(),
                                                  (float) lFootBounds.getWidth(), (float) lFootBounds.getHeight()),
                       cTuning, tr ("TUNING", "AFINACI\xc3\x93"));
    }

    // Title squares + glyphs, drawn LAST so each connector line disappears under
    // the letter it points to (its end tucks beneath the square).
    for (const auto& s : squares)
    {
        g.setColour (s.col);
        g.fillRect (s.rect); // sharp corners
        if (! s.glyph.isEmpty())
        {
            g.setColour (s.glyphCol);
            g.fillPath (s.glyph);
        }
    }
}

void FractalAdditiveAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto titleBar = full.removeFromTop (68);
    langButton.setVisible (false); // language now lives inside the Menu popup
    {
        // Left cluster, two rows: the credit at the very top-left (just above the
        // preset selector) with Menu after it, then [Presets][Save] - Save directly
        // BELOW Menu, the preset selector directly LEFT of Save.
        auto lc = titleBar.removeFromLeft (208).withTrimmedLeft (8);
        auto r1 = lc.removeFromTop (30).withTrimmedTop (8);
        {
            auto credit = r1.removeFromLeft (150).withHeight (18);
            vibeLabel.setJustificationType (juce::Justification::centredLeft);
            vibeLabel.setBounds (credit.removeFromLeft (92));
            credit.removeFromLeft (2);
            vibeLink.setBounds (credit);
        }
        r1.removeFromLeft (4);
        menuButton.setBounds (r1.removeFromLeft (46).withHeight (22));
        auto r2 = lc.withTrimmedTop (2).withHeight (22);
        presetBox.setBounds (r2.removeFromLeft (150));
        r2.removeFromLeft (4);
        savePresetButton.setBounds (r2.removeFromLeft (46));
    }
    // Spectrum-to-Rhythm: top right, high-contrast.
    rhythmBtn.setBounds (titleBar.removeFromRight (178).withSizeKeepingCentre (168, 26));

    auto area = full.reduced (6, 4);

    {
        auto kb = area.removeFromBottom (88).reduced (2, 5);
        int kbLo = 36, kbNum = 0;
        processorRef.getKeyboardKeyRange (kbLo, kbNum);
        lastKbLo = kbLo; lastKbNum = kbNum;
        keyboard.setRange (kbLo, kbNum);
        keyboard.setBounds (kb);
    }
    helpLabel.setBounds (area.removeFromBottom (16));
    meterLabel.setBounds (area.removeFromBottom (18));

    auto compact = [] (juce::Component& c, juce::Rectangle<int> r)
    { c.setBounds (r.withSizeKeepingCentre (r.getWidth(), juce::jmin (24, r.getHeight()))); };
    auto oneFader = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Slider& s, juce::Label* lbl)
    {
        // Box top aligned to the row top (like the Harmonic-Engine / Essentials macro
        // boxes) so all Row-1 titles sit on the same line; the CONTENT is unchanged.
        grp.setBounds (b.reduced (3, 0));
        auto r = b.reduced (3).reduced (5, 4); r.removeFromTop (14);
        if (lbl) lbl->setBounds (r.removeFromTop (13));
        s.setBounds (r);
    };
    auto oneCombo = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Component& c)
    {
        grp.setBounds (b.reduced (3));
        auto r = b.reduced (3).reduced (6, 4); r.removeFromTop (16);
        compact (c, r);
    };

    //== Right column: Spectrum to Tuning (top) over the dissonance BAR of the L
    //   Tuning box (its FOOT is the Tuning-controls cell of row 2, to the left) =====
    {
        auto sp = area.removeFromRight (208);
        const bool seg = specModeBox.getSelectedId() == 2; // "from spectrum's segment"
        // Segment mode is taller (partial list) but capped so the dissonance BAR
        // still starts ABOVE the Tuning FOOT top - otherwise the inverted-L notch
        // would self-cross (foot taller than bar). The list scrolls for the rest.
        auto spTop = sp.removeFromTop (seg ? 190 : 124);
        sp.removeFromTop (4);
        lBarBounds = sp;                                   // dissonance BAR of the L

        gSpecScale.setBounds (spTop.reduced (3));
        auto si = spTop.reduced (3).reduced (8, 5); si.removeFromTop (16);
        compact (specModeBox, si.removeFromTop (22));
        si.removeFromTop (4);

        // Mode-gated visibility.
        juce::Component* sethC[] = { &dissBtn, &dissCountBox, &lblSteps, &periodEdit, &dissModeBox, &dissSaveBtn, &lblPeriod };
        juce::Component* segC[]  = { &segView, &segPeriodEdit, &segTopToggle, &segPeriodLbl, &segHelpLbl };
        for (auto* c : sethC) c->setVisible (! seg);
        for (auto* c : segC)  c->setVisible (seg);

        if (! seg)
        {
            auto r1 = si.removeFromTop (24);
            dissBtn.setBounds (r1.removeFromLeft (r1.getWidth() - 92).reduced (1, 0));
            r1.removeFromLeft (3);
            lblSteps.setJustificationType (juce::Justification::centredLeft);
            lblSteps.setBounds (r1.removeFromLeft (38));
            compact (dissCountBox, r1);
            si.removeFromTop (4);
            auto r2 = si.removeFromTop (24);
            lblPeriod.setJustificationType (juce::Justification::centredLeft);
            lblPeriod.setBounds (r2.removeFromLeft (44));
            periodEdit.setBounds (r2.removeFromLeft (36).reduced (0, 1));
            r2.removeFromLeft (3);
            compact (dissModeBox, r2.removeFromLeft (54));
            r2.removeFromLeft (3);
            dissSaveBtn.setBounds (r2.reduced (0, 1));
        }
        else
        {
            auto pr = si.removeFromTop (22);
            segPeriodLbl.setJustificationType (juce::Justification::centredLeft);
            segPeriodLbl.setBounds (pr.removeFromLeft (48));
            segPeriodEdit.setBounds (pr.removeFromLeft (40).reduced (0, 1));
            pr.removeFromLeft (5);
            segTopToggle.setBounds (pr);          // "= top partial"
            si.removeFromTop (2);
            segHelpLbl.setBounds (si.removeFromTop (26));
            si.removeFromTop (2);
            segView.setBounds (si);
            segList.numRows = processorRef.getTemplatePartialCount();
            const int sh  = segList.contentHeight();
            const bool ss = sh > si.getHeight();
            segList.setSize (ss ? si.getWidth() - 10 : si.getWidth(), juce::jmax (si.getHeight(), sh));
        }

        // --- dissonance BAR (upper part of the L Tuning box) ---
        auto di = lBarBounds.reduced (3).reduced (8, 4); di.removeFromTop (3);
        dissHeader.splitX = 92;
        dissHeader.setBounds (di.removeFromTop (24));
        dissView.setBounds (di);
        const int ch     = degreeList.contentHeight();
        const bool scroll = ch > di.getHeight();
        degreeList.setSize (scroll ? di.getWidth() - 10 : di.getWidth(),
                            juce::jmax (di.getHeight(), ch));
    }
    // no gap here: the Tuning FOOT (row-2 cell) touches the BAR to form one L box

    const int rowH = (area.getHeight() - 4) / 2;

    //== Row 1: Base | (Harmonics over Prime) | Slope | Range | Fractal | (Phase over Gain)
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int W = row.getWidth();

        // "Harmonic Engine" = ONE framed box (gHarmonic) holding Base | Harmonics /
        // Prime | Slope, separated by SHARED divider lines + small inline uppercase
        // labels drawn in paint() (from the hc* cell rects stored here).
        const int macroW = W * 60 / 100;
        harmonicEngineBounds = row.removeFromLeft (macroW);
        auto macro = harmonicEngineBounds.reduced (5, 4);
        macro.removeFromTop (17); // clear the "HARMONIC ENGINE" title
        const int mW = macro.getWidth();

        // Base is a touch wider (and Harmonics/Prime a touch narrower) so the Base
        // pitch combo can show "Transposed down" in full without clipping.
        hcBase      = macro.removeFromLeft (mW * 24 / 62);
        auto midCol = macro.removeFromLeft (mW * 26 / 62);
        hcSlope     = macro;
        hcHarm      = midCol.withHeight (midCol.getHeight() * 2 / 5);
        hcPrime     = midCol.withTrimmedTop (midCol.getHeight() * 2 / 5);

        const int lblH = 15; // inline sub-label band (BASE / HARMONICS / PRIME / SLOPE)
        {
            auto bi = hcBase.reduced (5, 3); bi.removeFromTop (lblH);
            auto left = bi.removeFromLeft (bi.getWidth() * 36 / 100);
            lblBaseN.setBounds (left.removeFromTop (13)); baseNFader.setBounds (left);
            bi.removeFromLeft (5);
            lblBasePitch.setBounds (bi.removeFromTop (13));
            compact (basePitchBox, bi.removeFromTop (24));
            bi.removeFromTop (6);
            morphToggle.setBounds (bi.removeFromTop (20));
            lblMorphTime.setJustificationType (juce::Justification::centredLeft);
            auto mr = bi.removeFromTop (19);
            lblMorphTime.setBounds (mr.removeFromLeft (34));
            morphTimeFader.setBounds (mr);
        }
        {
            auto hi = hcHarm.reduced (5, 3); hi.removeFromTop (lblH);
            compact (whitelistBox, hi);

            const bool wl = processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
            primeLimitBox.setVisible (! wl); lblPrimeLimit.setVisible (! wl);
            for (int i = 0; i < 9; ++i) { primeToggle[i].setVisible (wl); primeLbl[i].setVisible (wl); }

            auto pi = hcPrime.reduced (5, 3); pi.removeFromTop (lblH);
            compact (primeModeBox, pi.removeFromTop (22));
            pi.removeFromTop (2);
            if (wl)
            {
                const int tw = pi.getWidth() / 9;
                for (int i = 0; i < 9; ++i)
                {
                    auto cell = pi.removeFromLeft (tw);
                    primeLbl[i].setBounds (cell.removeFromBottom (13));
                    primeToggle[i].setBounds (cell.withSizeKeepingCentre (juce::jmin (22, tw), juce::jmin (22, cell.getHeight())));
                }
            }
            else
            {
                lblPrimeLimit.setBounds (pi.removeFromLeft (42).withSizeKeepingCentre (42, 22));
                compact (primeLimitBox, pi.removeFromLeft (juce::jmin (110, pi.getWidth())));
            }
        }
        {
            auto si = hcSlope.reduced (6, 3); si.removeFromTop (lblH);
            slopeFader.setBounds (si);
        }

        oneFader (row.removeFromLeft (W * 12 / 100), gFractal,       fractalFader,       &lblFractal);

        // Essentials macro (Phase / Gain / Reference / Octave) - one framed box like
        // the Harmonic Engine: 4 stacked cells, shared horizontal dividers + inline
        // labels drawn in paint() (from the ec* cell rects stored here).
        {
            essentialsBounds = row;
            auto e = essentialsBounds.reduced (5, 4);
            e.removeFromTop (17); // clear the "ESSENTIALS" title
            // OCTAVE hosts TWO stepper rows -> it gets a fixed, taller share and the
            // other three cells split the rest evenly.
            const int octH = juce::jmin (58, e.getHeight() / 2);
            const int ch   = (e.getHeight() - octH) / 3;
            ecPhase = e.removeFromTop (ch);
            ecGain  = e.removeFromTop (ch);
            ecRef   = e.removeFromTop (ch);
            ecOct   = e;
            // Tighter label band + slimmer padding = the controls breathe (the
            // Reference value used to clip its descenders).
            const int lblH = 13; // inline PHASE / GAIN / REFERENCE / OCTAVE band
            { auto c = ecPhase.reduced (6, 1); c.removeFromTop (lblH);
              compact (phaseBox, c.removeFromLeft (juce::jmin (116, c.getWidth()))); } // fits "Scattered", not the whole row
            { auto c = ecGain.reduced (6, 1);  c.removeFromTop (lblH);
              gainFader.setBounds (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()))); }
            { auto c = ecRef.reduced (6, 1);   c.removeFromTop (lblH);
              auto rr = c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()));
              lblRefA.setJustificationType (juce::Justification::centredLeft);
              lblRefA.setBounds (rr.removeFromLeft (26));
              lblRefHz.setBounds (rr.removeFromRight (22));
              refAEdit.setBounds (rr); }
            { auto c = ecOct.reduced (6, 1);   c.removeFromTop (13); c.removeFromBottom (4);
              const int rh = juce::jmax (14, c.getHeight() / 2);
              // [label][white value][+ green][- red]: value first, then + before -.
              auto stepRow = [] (juce::Rectangle<int> row, juce::Label& lbl,
                                 juce::TextButton& dn, juce::Label& val, juce::TextButton& up)
              {
                  lbl.setJustificationType (juce::Justification::centredLeft);
                  lbl.setBounds (row.removeFromLeft (66));
                  val.setBounds (row.removeFromLeft (40));
                  up.setBounds  (row.removeFromLeft (24).reduced (0, 1));
                  row.removeFromLeft (4);
                  dn.setBounds  (row.removeFromLeft (24).reduced (0, 1));
              };
              stepRow (c.removeFromTop (rh), lblOctave, octDn, octVal, octUp);
              stepRow (c,                    lblRange,  sprDn, sprVal, sprUp); }
        }
    }

    //== Row 2: Custom Engine | Volume Env | Pitch Env | Filter | Tuning =========
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int W = row.getWidth();

        // Custom Engine (leftmost now): load .scl OR type intervals + Reps.
        {
            auto sb = row.removeFromLeft (W * 14 / 100);
            gTimbre.setBounds (sb.reduced (3));
            auto ti = sb.reduced (3).reduced (8, 5); ti.removeFromTop (16);
            auto brow = ti.removeFromTop (24);
            const bool offVis = processorRef.isTimbreActive();
            clearTimbreButton.setVisible (offVis);
            if (offVis)
            {
                loadTimbreButton.setBounds (brow.removeFromLeft (brow.getWidth() * 6 / 10).reduced (2, 0));
                clearTimbreButton.setBounds (brow.reduced (2, 0));
            }
            else
            {
                loadTimbreButton.setBounds (brow.reduced (2, 0));
            }
            ti.removeFromTop (5);
            intervalEditHome = ti.removeFromTop (24).reduced (1, 0); // type intervals, Enter builds
            intervalEdit.setBounds (intervalEdit.hasKeyboardFocus (false)
                                        ? intervalEditHome.withRight (getWidth() - 10)
                                        : intervalEditHome);
            ti.removeFromTop (5);
            timbreStatusLabel.setBounds (ti.removeFromTop (16));
            ti.removeFromTop (5);
            auto fr = ti.removeFromTop (22);
            lblTimbreRepeats.setJustificationType (juce::Justification::centredLeft);
            lblTimbreRepeats.setBounds (fr.removeFromLeft (38));
            timbreRepeatsFader.setBounds (fr);
        }

        layoutFaders (gAmpEnv, row.removeFromLeft (W * 16 / 100), cEnv,
            { &ampAdsr[0], &ampAdsr[1], &ampAdsr[2], &ampAdsr[3] },
            { &ampAdsrLbl[0], &ampAdsrLbl[1], &ampAdsrLbl[2], &ampAdsrLbl[3] });

        // Pitch envelope: Amount (semitones) + A D S R, like the filter envelope.
        layoutFaders (gPitchEnv, row.removeFromLeft (W * 19 / 100), cPitch,
            { &pEnvAmtFader, &pitchAdsr[0], &pitchAdsr[1], &pitchAdsr[2], &pitchAdsr[3] },
            { &lblPitchAmt, &pitchAdsrLbl[0], &pitchAdsrLbl[1], &pitchAdsrLbl[2], &pitchAdsrLbl[3] });

        layoutFaders (gFilter, row.removeFromLeft (W * 27 / 100), cFilter,
            { &cutoffFader, &resFader, &fEnvAmtFader, &filtAdsr[0], &filtAdsr[1], &filtAdsr[2], &filtAdsr[3] },
            { &lblCutoff, &lblRes, &lblEnvAmt, &filtAdsrLbl[0], &filtAdsrLbl[1], &filtAdsrLbl[2], &filtAdsrLbl[3] });

        // Tuning = the FOOT of the inverted-L box (the dissonance BAR is above-right);
        // mode + the loaded .scl name, the PERIOD (red), load/reset, recent, bend, bank.
        {
            lFootBounds = row; // frame painted in paint() as one L with the bar
            auto ti = row.reduced (3).reduced (8, 4); ti.removeFromTop (17); // clear the TUNING title chip
            auto mrow = ti.removeFromTop (24);
            compact (tuningBox, mrow.removeFromLeft (mrow.getWidth() * 50 / 100));
            mrow.removeFromLeft (5);
            tuningNameLabel.setBounds (mrow); // shows the loaded scale's file name
            // Period of the active scale, in red, always visible.
            ti.removeFromTop (3);
            tuningPeriodLbl.setBounds (ti.removeFromTop (18));
            // Load .scl only exists in Scala mode; Reset appears whenever there is
            // something to reset (any non-12ED2 mode or a loaded scale).
            const int  tmode    = processorRef.getTuningMode();
            const bool loadVis  = tmode == 2;
            const bool resetVis = tmode != 0 || processorRef.tuningHasScala();
            loadSclButton.setVisible (loadVis);
            clearSclButton.setVisible (resetVis);
            if (loadVis || resetVis)
            {
                ti.removeFromTop (4);
                auto brow = ti.removeFromTop (23);
                if (loadVis && resetVis)
                {
                    loadSclButton.setBounds (brow.removeFromLeft (brow.getWidth() / 2).reduced (2, 0));
                    clearSclButton.setBounds (brow.reduced (2, 0));
                }
                else if (loadVis)  loadSclButton.setBounds (brow.reduced (2, 0));
                else               clearSclButton.setBounds (brow.reduced (2, 0));
            }
            // Pitch bend ABOVE Recent; hidden in MPE-input mode (the MPE zone fixes
            // the per-note range at +-48, so the choice is irrelevant there).
            const bool sclMode = tmode == 2;
            const bool bendVis = tmode != 3;
            lblBend.setVisible (bendVis); bendBox.setVisible (bendVis);
            if (bendVis)
            {
                ti.removeFromTop (3);
                auto bendRow = ti.removeFromTop (22);
                lblBend.setJustificationType (juce::Justification::centredLeft);
                lblBend.setBounds (bendRow.removeFromLeft (74));
                compact (bendBox, bendRow.removeFromLeft (juce::jmin (120, bendRow.getWidth())));
            }
            // Recent + Tuning bank only make sense in Scala (.scl) mode.
            recentLabel.setVisible (sclMode);
            for (int i = 0; i < 3; ++i) recentBtn[i].setVisible (sclMode && recentBtn[i].getButtonText().isNotEmpty());
            lblTuningBank.setVisible (sclMode); tuningBankBox.setVisible (sclMode);
            tuningBankLoadBtn.setVisible (sclMode);
            if (sclMode)
            {
                ti.removeFromTop (4);
                recentLabel.setBounds (ti.removeFromTop (14));
                auto rrow = ti.removeFromTop (24);
                const int rw = rrow.getWidth() / 3;
                for (int i = 0; i < 3; ++i) recentBtn[i].setBounds (rrow.removeFromLeft (rw).reduced (2, 1));
                ti.removeFromTop (5);
                lblTuningBank.setBounds (ti.removeFromTop (14));
                auto bkrow = ti.removeFromTop (24);
                tuningBankLoadBtn.setBounds (bkrow.removeFromRight (36).reduced (1, 0));
                bkrow.removeFromRight (3);
                compact (tuningBankBox, bkrow);
            }
        }
    }

    // Floating "silent settings" explainer, centred over everything.
    warningOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (580, 200));
    warningOverlay.toFront (false);
    noticeOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (560, 220));
    rhythmScrim.setBounds (getLocalBounds());
    rhythmPanel.setBounds (getLocalBounds().withSizeKeepingCentre (780, 540));
    rhythmScrim.toFront (false);
    rhythmPanel.toFront (false);
}

void FractalAdditiveAudioProcessorEditor::mouseDown (const juce::MouseEvent&) { grabKeyboardFocus(); }

bool FractalAdditiveAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    const int  period = juce::jmax (1, processorRef.getScaleDegrees());
    if (c == 'z' || c == 'Z') { baseKey = juce::jlimit (0, 112, baseKey - period); return true; }
    if (c == 'x' || c == 'X') { baseKey = juce::jlimit (0, 112, baseKey + period); return true; }
    // SWALLOW every mapped play key: if it escapes to the host, the DAW's own
    // computer-MIDI-keyboard (Live: A S D = C D E, diatonic) fires TOO and its
    // note lands a unison / minor second from ours (our A S D = degrees 0 1 2,
    // chromatic) - the reported "ghost second note". Never let them through.
    const auto lc = (juce::juce_wchar) juce::CharacterFunctions::toLowerCase (c);
    for (const auto& m : kKeyMap)
        if (m.ch == (int) lc) return true;
    return false;
}

bool FractalAdditiveAudioProcessorEditor::keyStateChanged (bool)
{
    const bool hadHeld = ! heldKeys.empty();
    pollComputerKeyboard();
    // Swallow the event whenever it involves our play keys (a mapped key is down
    // now, or one was just released) so key-ups can't reach the DAW's computer
    // keyboard either. Unrelated keys (space, digits...) still pass through.
    bool mappedDown = false;
    for (const auto& m : kKeyMap)
        if (juce::KeyPress::isKeyCurrentlyDown (m.ch)) { mappedDown = true; break; }
    return hadHeld || mappedDown || ! heldKeys.empty();
}

// Reconcile the sounding computer-keyboard notes with the ACTUAL physical key
// state (isKeyCurrentlyDown -> GetAsyncKeyState on Windows). Called both on key
// events (low latency) and on the 30 Hz timer (safety net): a KEYUP missed while
// the editor was unfocused, or a phantom key from keyboard ghosting, otherwise
// leaves a stuck note - and since the top row is an octave up, that stuck note
// appears "in another octave". If we do not have focus, everything is released
// so global key presses in other apps never play notes.
void FractalAdditiveAudioProcessorEditor::pollComputerKeyboard()
{
    // No note-playing while unfocused OR while typing in a text field (the Period
    // box, a preset name...): letters there must never trigger notes. This is
    // folded PER KEY into `active` rather than a mass clear+return, so heldKeys
    // always mirrors exactly what we triggered - a stale entry from a mass-clear
    // was one way a key could later re-fire and leave a note stuck in memory.
    const bool typing = dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;

    // Focus is fragile: clicking a slider (grabs-focus off keeps focus, but a
    // combo popup, a colour picker, a preset dialog... all momentarily steal it),
    // and every such flicker used to release+retrigger the held notes - that is
    // the "the note suddenly sounds different" bug, AND it made it impossible to
    // grab a control while sustaining a note. So we debounce: a real note only
    // releases once focus has been gone for >250 ms (you actually left the
    // plugin). Brief losses keep the notes sounding, so you can freely tweak any
    // parameter with a note held down.
    const bool focusedNow = hasKeyboardFocus (true) && ! typing;
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (focusedNow)
        lastFocusedMs = now;
    // Typing must silence immediately (letters belong to the text field, not the
    // keyboard); otherwise honour the debounce window.
    const bool active = ! typing && (focusedNow || (now - lastFocusedMs) < 250);

    for (const auto& m : kKeyMap)
    {
        const bool wantOn = active && juce::KeyPress::isKeyCurrentlyDown (m.ch);
        const auto it = heldKeys.find (m.ch);
        if (wantOn && it == heldKeys.end())
        {
            const int note = juce::jlimit (0, 127, baseKey + m.off);
            heldKeys[m.ch] = note;
            // Guard against a SECOND note-on for a note that is already sounding
            // (from an overlapping key/source or a reconcile race): a duplicate
            // note-on starts a second voice, which is exactly the "press one key
            // and two sound at once" bug. Only ring it if it is not already on.
            if (! processorRef.keyboardState.isNoteOn (1, note))
                processorRef.keyboardState.noteOn (1, note, 0.8f);
        }
        else if (! wantOn && it != heldKeys.end())
        {
            processorRef.keyboardState.noteOff (1, it->second, 0.0f);
            heldKeys.erase (it);
        }
    }
}

void FractalAdditiveAudioProcessorEditor::timerCallback()
{
    pollComputerKeyboard(); // safety net for missed KEYUPs / ghost keys / focus loss

    refreshRefAText();       // keep the Reference-A box in sync with the parameter (automation)

    // OCTAVE stepper read-outs (params can also move via host automation).
    octVal.setText (juce::String ((int) processorRef.apvts.getRawParameterValue ("octaveShift")->load()),
                    juce::dontSendNotification);
    sprVal.setText (juce::String ((int) processorRef.apvts.getRawParameterValue ("spectrumRange")->load()),
                    juce::dontSendNotification);

    // Rhythm panel: live pulse-rate read-outs; rebuild the partial list whenever
    // the spectrum template changes (Base N moved, .scl loaded...).
    if (rhythmPanel.isVisible())
    {
        const juce::uint32 g = processorRef.getTemplateGeneration();
        if (g != lastRhythmGen)
        {
            lastRhythmGen = g;
            rhythmPanel.numRows = processorRef.getTemplatePartialCount();
            rhythmPanel.list.updateContent();
            rhythmPanel.list.repaint();
            rhythmPanel.refreshSlots(); // ratio labels may have changed
        }
        rhythmPanel.refreshLive();
    }

    // Segment picker: refresh when the spectrum template changes (partial count/ratios
    // move with Base N, fractal, a completing morph...) so it never shows stale rows.
    if (segView.isVisible())
    {
        const juce::uint32 g = processorRef.getTemplateGeneration();
        if (g != lastSegGen)
        {
            lastSegGen = g;
            segList.numRows = processorRef.getTemplatePartialCount();
            const int visH = segView.getHeight();
            const int ch   = segList.contentHeight();
            const bool ss  = ch > visH && visH > 0;
            segList.setSize (ss ? segView.getWidth() - 10 : segView.getWidth(), juce::jmax (visH, ch));
            segList.repaint();
        }
    }
    // Continuous DAW-tempo sync: while the toggle is on, re-send CC20 whenever the
    // rounded target BPM changes (a new note re-speeds pulse 1 -> the tempo follows).
    if (rhythmPanel.tempoT.getToggleState())
    {
        const double bpm = rhythmPanel.tempoBpm();
        const int b = bpm > 0.0 ? (int) std::lround (bpm) : -1;
        if (b > 0 && b != lastTempoSent)
        {
            lastTempoSent = b;
            processorRef.getRhythm().sendController (20, juce::jlimit (0, 127, b - 20));
        }
    }
    else lastTempoSent = -1;
    // Keep the tuning-bank combo's selection in sync with the parameter (host
    // automation can move it) without rebuilding the list every tick.
    if (tuningBankBox.getSelectedId() - 1 != processorRef.getTuningBankSlotParam())
        tuningBankBox.setSelectedId (processorRef.getTuningBankSlotParam() + 1, juce::dontSendNotification);

    // Re-adapt the colour keyboard's visible window whenever the active tuning
    // changes (a scale loaded/generated, or the A reference moved), so a wide
    // period never leaves sub-audio/ultrasonic keys on screen.
    {
        int kbLo = 36, kbNum = 0;
        processorRef.getKeyboardKeyRange (kbLo, kbNum);
        if (kbLo != lastKbLo || kbNum != lastKbNum)
        {
            lastKbLo = kbLo; lastKbNum = kbNum;
            keyboard.setRange (kbLo, kbNum);
        }
    }

    const int  rendered  = processorRef.getPartialCount();
    const int  templ     = processorRef.getTemplatePartialCount();
    const bool truncated = processorRef.isSpectrumTruncated();
    juce::String meter;
    if (templ == 0)
    {
        // Empty template = the selection + prime rules (intersected) left nothing.
        meter = tr ("0 partials \xe2\x80\x94 the selection / prime filters leave nothing to sound",
                    "0 parcials \xe2\x80\x94 la selecci\xc3\xb3 / els filtres de primers no deixen sonar res");
        meterLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
    }
    else
    {
        meter = juce::String (rendered) + tr (" partials audible", " parcials audibles")
              + "   \xe2\x80\x94   " + juce::String (templ) + tr (" in the full timbre", " al timbre complet");
        if (truncated) meter += tr ("   (safety cap)", "   (l\xc3\xadmit de seguretat)");
        meterLabel.setColour (juce::Label::textColourId, truncated ? juce::Colours::orange : juce::Colour (0xff7ff59a));
    }
    meterLabel.setText (meter, juce::dontSendNotification);

    updateWarning (templ);

    // The loaded scale's file name, shown to the RIGHT of the mode combo (no
    // "Loaded:" prefix). Only meaningful in Scala / MTS-ESP modes.
    juce::String sname;
    switch (processorRef.getTuningMode())
    {
        case 1:  sname = processorRef.tuningHasMaster() ? processorRef.tuningScaleName()
                                                        : tr ("(no master)", "(sense mestre)"); break;
        case 2:  sname = processorRef.tuningHasScala()  ? processorRef.tuningScaleName()
                                                        : tr ("(no file)", "(sense fitxer)"); break;
        default: sname = {}; break;
    }
    tuningNameLabel.setText (sname, juce::dontSendNotification);

    juce::String tms = processorRef.isTimbreActive()
        ? "Custom: " + processorRef.getTimbreName() + "  (" + juce::String (processorRef.getTimbreNoteCount())
          + tr (" notes/period)", " notes/per\xc3\xadode)")
        : tr ("Custom engine off", "Motor propi apagat");
    timbreStatusLabel.setText (tms, juce::dontSendNotification);

    // Recent .scl buttons (only in Scala mode - resized() also gates them).
    const auto recents = processorRef.getRecentSclPaths();
    const bool sclMode = processorRef.getTuningMode() == 2;
    for (int i = 0; i < 3; ++i)
    {
        const bool has = sclMode && i < recents.size();
        recentBtn[i].setVisible (has);
        if (i < recents.size()) recentBtn[i].setButtonText (juce::File (recents[i]).getFileNameWithoutExtension());
    }

    // Hide the "undo" buttons when there is nothing to undo: Reset only matters
    // away from plain 12-TET, Off only when Timbre mode is actually on. Relayout
    // so the Load button expands to fill the freed space. The tuning MODE is packed
    // in too: Recent/Tuning-bank/Pitch-bend visibility depends on it.
    const bool resetVis = processorRef.getTuningMode() != 0 || processorRef.tuningHasScala();
    const bool offVis   = processorRef.isTimbreActive();
    const int  packed   = (resetVis ? 1 : 0) | (offVis ? 2 : 0) | (processorRef.getTuningMode() << 2);
    if (packed != lastResetOffState) { lastResetOffState = packed; resized(); }

    const int mode = (int) (processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f);
    if (mode != lastPrimeMode) { lastPrimeMode = mode; resized(); }

    if (processorRef.getDissGeneration() != lastDissGeneration)
    {
        lastDissGeneration = processorRef.getDissGeneration();
        refreshDegreeList();
    }

    // Live active-scale dissonance list (recomputes only when timbre/tuning change).
    updateScaleDissonance();

    // Period of the active scale (red readout in the Tuning box).
    {
        const int D = juce::jmax (1, processorRef.getScaleDegrees());
        tuningPeriodLbl.setText (tr ("Period  ", "Per\xc3\xado" "de  ") + justName (processorRef.getDegreeRatio (D)),
                                 juce::dontSendNotification);
    }

    const bool timbre = processorRef.isTimbreActive();
    if (timbre != lastTimbreActive)
    {
        if (timbre)
        {
            forceDefault ("baseN");     forceDefault ("baseMode"); forceDefault ("selection");
            forceDefault ("phaseMode"); forceDefault ("primeMode"); forceDefault ("primeLimit");
            for (int i = 0; i < 9; ++i) forceDefault ("primeWL" + juce::String (i));
        }
        applyTimbreEnablement (timbre);
        lastTimbreActive = timbre;
    }
}
