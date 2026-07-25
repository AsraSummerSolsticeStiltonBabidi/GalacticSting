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

    // A source-honest description of a scale-degree interval, shared by every place
    // that shows one (key labels, hover, dissonance list, delta strip):
    //  - GENUINE just interval -> the exact ratio "p/q" (any size). Either the degree
    //    is rational BY SOURCE (a ratio in the .scl / a harmonic partial / the 2/1
    //    octave) or it is float-exact within 0.3c of a small fraction - which catches
    //    dissonance-curve picks (~0.07c) yet rejects tempered steps (the 12-EDO fifth
    //    is ~2c off 3/2).
    //  - Otherwise TEMPERED -> its cents; plus the nearest SIMPLE just interval as an
    //    APPROXIMATION only within 2c (small denominator, never a false large ratio),
    //    flagged so the UI can mark it "≈p/q *": not exact - the only interval that is
    //    exact in cents is the octave (1200c = 2/1).
    struct DegInterval
    {
        double    cents     = 0.0;
        bool      genuine   = false;  long long p  = 1, q  = 1; // exact ratio when genuine
        bool      hasApprox = false;  long long ap = 1, aq = 1; int approxErr = 0; // nearest just when tempered
    };
    DegInterval analyzeDegree (FractalAdditiveAudioProcessor& proc, int degree)
    {
        DegInterval d;
        const double ratio = proc.getDegreeRatio (degree);
        if (! (ratio > 0.0) || ! std::isfinite (ratio)) return d;
        d.cents = 1200.0 * std::log2 (ratio);

        long long sp = 1, sq = 1;
        if (proc.getDegreeIsRational (degree, sp, sq)) { d.genuine = true; d.p = sp; d.q = sq; return d; }

        long long p = 1, q = 1;
        dissonance::bestFraction (ratio, p, q, 48);
        // Float-exact counts as genuine ONLY for SIMPLE fractions: a Sethares-refined
        // minimum landing within a hair of 34/29 is an approximation, not that ratio -
        // it must show cents (+ hint), never a confident big fraction.
        if (p * q <= 150 && std::abs (d.cents - 1200.0 * std::log2 ((double) p / (double) q)) < 0.3)
        { d.genuine = true; d.p = p; d.q = q; return d; } // float-exact (e.g. a dissonance-curve pick)

        // The "≈ p/q" hint: scan the SIMPLE fractions directly (denominators 1..12)
        // and prefer the SIMPLEST one whose distance fits its simplicity window - very
        // simple ratios get ~17c (697c AND 700c -> "≈ 3/2"; 400c -> "≈ 5/4 (+14c)";
        // 800c -> "≈ 8/5 (-14c)"), moderately simple ones ~8c (200c -> "≈ 9/8"),
        // complex ones only a hair. A continued-fraction "best" pick would prefer
        // obscure closer fractions (800c is nearer 19/12 than 8/5) - not helpful.
        long long bestP = 0, bestQ = 1; double bestErr = 0.0; double bestSimp = 1.0e18;
        for (long long q2 = 1; q2 <= 17; ++q2)
        {
            const long long p2 = (long long) std::llround (ratio * (double) q2);
            if (p2 <= 0 || std::gcd (p2, q2) != 1) continue;
            const double err  = d.cents - 1200.0 * std::log2 ((double) p2 / (double) q2);
            const double simp = (double) (p2 * q2);
            // Simplicity-tiered windows; the last tier catches VERY close but less
            // simple ratios like 17/16 (100c is only ~1c away from it).
            const double tol  = simp <= 40.0 ? 17.0 : (simp <= 80.0 ? 8.0 : (simp <= 300.0 ? 2.5 : 0.0));
            if (std::abs (err) <= tol && simp < bestSimp)
            { bestSimp = simp; bestP = p2; bestQ = q2; bestErr = err; }
        }
        if (bestP > 0)
        { d.hasApprox = true; d.ap = bestP; d.aq = bestQ; d.approxErr = (int) std::llround (bestErr); }
        return d;
    }
    // Compact scale-degree label: the exact ratio when genuine, else its cents (with a
    // trailing "*" when a nearby just interval exists - the hover spells that out).
    juce::String degreeLabel (FractalAdditiveAudioProcessor& proc, int degree)
    {
        const auto d = analyzeDegree (proc, degree);
        if (d.genuine) return juce::String (d.p) + "/" + juce::String (d.q);
        return juce::String ((long long) std::llround (d.cents)) + juce::String::fromUTF8 ("¢") + (d.hasApprox ? "*" : "");
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
        // The chip sits ABOVE the frame: its BASE lands exactly on the box's top
        // line (frame top = titleH * 0.5 = 16), so the chip reads as a tab on the
        // box instead of a rectangle cutting through its border.
        constexpr float chipH   = 15.0f;
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
            constexpr float titleH = 32.0f; // room ABOVE the frame for the title chip
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
        static constexpr float kAdsrThumbR = 20.0f; // named (ADSR) thumb radius
        static constexpr float kThumbR     = 13.0f; // every other fader's thumb radius
        static float thumbRadiusFor (juce::Slider& s)
        { return s.getName().isNotEmpty() ? kAdsrThumbR : kThumbR; }
        int getSliderThumbRadius (juce::Slider& s) override
        {
            if (s.getSliderStyle() == juce::Slider::LinearVertical
                || s.getSliderStyle() == juce::Slider::LinearHorizontal)
                return (int) thumbRadiusFor (s);
            return juce::LookAndFeel_V4::getSliderThumbRadius (s);
        }

        // Regular heptagon (flat-ish top), used for every fader thumb: its inscribed
        // circle is ~0.90 of the radius, so a letter drawn at 0.8 R sits inside with
        // real margin and never has to be squeezed to fit.
        static juce::Path heptagon (juce::Point<float> c, float r)
        {
            juce::Path p;
            constexpr float twoPi = juce::MathConstants<float>::twoPi;
            for (int i = 0; i < 7; ++i)
            {
                const float a = -juce::MathConstants<float>::halfPi + (float) i * twoPi / 7.0f;
                const juce::Point<float> pt (c.x + r * std::cos (a), c.y + r * std::sin (a));
                if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
            }
            p.closeSubPath();
            return p;
        }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                               float sliderPos, float minPos, float maxPos,
                               juce::Slider::SliderStyle style, juce::Slider& s) override
        {
            const bool vert  = style == juce::Slider::LinearVertical;
            const bool horiz = style == juce::Slider::LinearHorizontal;
            if (! vert && ! horiz)
            {
                juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, minPos, maxPos, style, s);
                return;
            }
            juce::ignoreUnused (minPos, maxPos);

            const auto col   = s.findColour (juce::Slider::thumbColourId);
            const auto track = s.findColour (juce::Slider::trackColourId);
            const float r    = thumbRadiusFor (s);
            constexpr float tw = 5.0f; // track thickness

            if (vert)
            {
                const float cx = (float) x + (float) w * 0.5f;
                const float top = (float) y, bot = (float) (y + h);
                g.setColour (track.withMultipliedAlpha (0.35f));
                g.fillRoundedRectangle (cx - tw * 0.5f, top, tw, bot - top, tw * 0.5f);
                g.setColour (track);
                g.fillRoundedRectangle (cx - tw * 0.5f, sliderPos, tw, juce::jmax (0.0f, bot - sliderPos), tw * 0.5f);
                const auto hept = heptagon ({ cx, sliderPos }, r);
                g.setColour (col);
                g.fillPath (hept);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.strokePath (hept, juce::PathStrokeType (1.0f));
                if (s.getName().isNotEmpty())
                {
                    g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (r * 0.8f).withStyle ("Bold")));
                    g.drawText (s.getName(), juce::Rectangle<float> (cx - r, sliderPos - r, r * 2.0f, r * 2.0f),
                                juce::Justification::centred, false);
                }
            }
            else
            {
                const float cy = (float) y + (float) h * 0.5f;
                const float l = (float) x, rt = (float) (x + w);
                g.setColour (track.withMultipliedAlpha (0.35f));
                g.fillRoundedRectangle (l, cy - tw * 0.5f, rt - l, tw, tw * 0.5f);
                g.setColour (track);
                g.fillRoundedRectangle (l, cy - tw * 0.5f, juce::jmax (0.0f, sliderPos - l), tw, tw * 0.5f);
                const float rr = juce::jmin (r, (float) h * 0.5f);
                const auto hept = heptagon ({ sliderPos, cy }, rr);
                g.setColour (col);
                g.fillPath (hept);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.strokePath (hept, juce::PathStrokeType (1.0f));
                if (s.getName().isNotEmpty())
                {
                    g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (rr * 0.8f).withStyle ("Bold")));
                    g.drawText (s.getName(), juce::Rectangle<float> (sliderPos - rr, cy - rr, rr * 2.0f, rr * 2.0f),
                                juce::Justification::centred, false);
                }
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

        // Custom tooltip rendering: line-based, so the "from the partial: n/1" line of
        // the keyboard hover window is drawn in the Spectrum-to-Tuning ORANGE while the
        // rest stays white. getTooltipBounds is overridden to match this layout.
        static bool isSourceLine (const juce::String& l)
        {
            return l.startsWithIgnoreCase ("from the partial") || l.startsWithIgnoreCase ("del parcial");
        }
        juce::Rectangle<int> keyboardScreenArea; // kept current by the editor
        juce::Point<int>     hoverAnchor;        // hovered key's eye centre (screen)
        bool                 lastWasCallout = false;
        static constexpr int kPtrH = 9;          // callout pointer height

        // Word-wrapped, per-line coloured layout ("from the partial" lines in orange).
        static juce::TextLayout layoutFor (const juce::String& text, float maxW)
        {
            juce::AttributedString as;
            juce::StringArray lines;
            lines.addLines (text);
            for (int i = 0; i < lines.size(); ++i)
                as.append (lines[i] + (i < lines.size() - 1 ? "\n" : ""),
                           juce::Font (juce::FontOptions (13.5f)),
                           isSourceLine (lines[i]) ? juce::Colour (0xffff9a3c) : juce::Colours::white);
            juce::TextLayout tl;
            tl.createLayout (as, maxW);
            return tl;
        }
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                               juce::Rectangle<int> parentArea) override
        {
            const auto tl = layoutFor (tipText, 340.0f);
            const int w = juce::jmax (40, (int) std::ceil (tl.getWidth()) + 18);
            int h = (int) std::ceil (tl.getHeight()) + 10;
            // Keyboard hovers become a CALLOUT: parked right above the hovered key's
            // eye, with a pointer down to it - never covering keys or their numbers.
            lastWasCallout = ! keyboardScreenArea.isEmpty() && keyboardScreenArea.contains (screenPos)
                          && hoverAnchor != juce::Point<int>();
            if (lastWasCallout)
            {
                h += kPtrH;
                return juce::Rectangle<int> (juce::jlimit (parentArea.getX(), parentArea.getRight() - w, hoverAnchor.x - w / 2),
                                             juce::jmax (parentArea.getY(), keyboardScreenArea.getY() - h - 1), w, h)
                           .constrainedWithin (parentArea);
            }
            return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                         screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6,
                                         w, h).constrainedWithin (parentArea);
        }
        void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
        {
            auto body = juce::Rectangle<float> (0, 0, (float) width, (float) height);
            if (lastWasCallout)
            {
                body = body.withTrimmedBottom ((float) kPtrH);
                juce::Path ptr; // the line down to the key (stops above the eye)
                ptr.addTriangle ((float) width * 0.5f - 6.0f, body.getBottom(),
                                 (float) width * 0.5f + 6.0f, body.getBottom(),
                                 (float) width * 0.5f, (float) height);
                g.setColour (juce::Colour (0xff5cb8ff).withAlpha (0.9f));
                g.fillPath (ptr);
            }
            g.setColour (juce::Colour (0xf2141420));
            g.fillRoundedRectangle (body, 6.0f);
            g.setColour (juce::Colour (0xff5cb8ff).withAlpha (0.8f));
            g.drawRoundedRectangle (body.reduced (0.8f), 6.0f, 1.4f);
            layoutFor (text, (float) width - 18.0f)
                .draw (g, body.reduced (9.0f, 5.0f));
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
    addMapListener (morphToggle, "morph");
    configFader (morphTimeFader, "morphTime", morphTimeAtt, cHarmonic, "Morph transition time in seconds: how long partials take to glide to the new spectrum.");
    morphTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    morphTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);
    makeLabel (lblMorphTime, cHarmonic);

    // Harmonics is a magnetic FADER now (Even - All - Odd - Prime), so a modulator
    // can sweep it and it can sit BETWEEN two sets (blended).
    configFader (harmonicsFader, "harmonics", harmonicsAtt, cHarmonic,
                 "Which harmonics sound: Even - All - Odd - Prime. It snaps to those four, but you can stop between two of them (hold Ctrl for free fine setting): there the harmonics that belong to only one of the pair come in at part volume, so the timbre morphs instead of switching.");
    harmonicsFader.setSliderStyle (juce::Slider::LinearHorizontal);
    harmonicsFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 15);
    harmonicsFader.setSliderSnapsToMousePosition (false);
    harmonicsFader.normalSensitivity = 240;
    harmonicsFader.textFromValueFunction = [] (double v)
    {
        static const char* names[] = { "Even", "All", "Odd", "Prime" };
        const int lo = juce::jlimit (0, 3, (int) std::floor (v));
        const double t = v - (double) lo;
        if (t < 0.01) return juce::String (names[lo]);
        if (t > 0.99) return juce::String (names[juce::jmin (3, lo + 1)]);
        return juce::String (names[lo]) + "-" + juce::String (names[juce::jmin (3, lo + 1)])
             + " " + juce::String ((int) std::lround (t * 100.0)) + "%";
    };
    harmonicsFader.updateText();
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
    // Prime LIMIT as a FADER (so it can be driven by the modulators). The value is
    // the index into {Off, 2, 3, 5, 7, 11, 13, 17, 19, 23}, shown by name.
    configFader (primeLimitFader, "primeLimit", primeLimitFaderAtt, cHarmonic,
                 "Largest prime factor allowed (Limit mode). Right-click to map a modulator onto it.");
    primeLimitFader.setSliderStyle (juce::Slider::LinearHorizontal);
    primeLimitFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 15);
    primeLimitFader.textFromValueFunction = [] (double v)
    {
        static const char* names[] = { "Off", "2", "3", "5", "7", "11", "13", "17", "19", "23" };
        return juce::String (names[juce::jlimit (0, 9, (int) std::lround (v))]);
    };
    primeLimitFader.updateText();
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
        addMapListener (primeToggle[i], "primeWL" + juce::String (i));
        makeLabel (primeLbl[i], cHarmonic); primeLbl[i].setText (pn[i], juce::dontSendNotification);
    }

    configGroup (gFilter, cFilter);
    // The GROUP PANELS must be added BEFORE any control that will sit on top of
    // them: the LnF fills the group body with an OPAQUE panel, and siblings paint
    // in add-order - a control added earlier gets buried under the panel (it stays
    // hit-testable because GroupComponent lets clicks through, which is why the
    // "invisible" FILTER faders still showed their hover tooltips).
    configGroup (gAmpEnv, cEnv);
    configGroup (gPitchEnv, cPitch);
    configFader (cutoffFader,  "filterCutoff",    cutoffAtt,  cFilter, "Low-pass cutoff");
    cutoffFader.setTextValueSuffix (" c"); // cents above the played note (note-tracking)
    configFader (resFader,     "filterResonance", resAtt,     cFilter, "Resonance");
    configFader (fEnvAmtFader, "filterEnvAmount", fEnvAmtAtt, cFilter, "How far the filter envelope moves the cutoff (octaves)");
    makeLabel (lblCutoff, cFilter); makeLabel (lblRes, cFilter); makeLabel (lblEnvAmt, cFilter);

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

    // CUSTOM tab: 6 modulators (3 envelopes + 3 LFOs). The controls below re-attach
    // to whichever modulator the selector shows; map any fader via right-click.
    {
        const juce::Colour cCustom (0xff9a7bff);
        for (int i = 0; i < 4; ++i)
        {
            configFader (custAdsr[i], "modE1" + juce::String (adsrTxt[i]), custAdsrAtt[i], cCustom, "Custom envelope");
            custAdsr[i].setName (adsrTxt[i]);
            makeLabel (custAdsrLbl[i], cCustom);
            custAdsrLbl[i].setText (adsrTxt[i], juce::dontSendNotification);
        }
        configFader (custRangeFader, "modE1Range", custRangeAtt, cCustom,
                     "How far this modulator MOVES its mapped parameters (+/- of their full range), riding on each knob's own value. Note: parameters that REBUILD the spectrum (Base, Harmonics, Slope, Fractal, Prime, Range, Reps) follow the modulator at the 60 Hz rebuild rate and glide when Morph is on - everything else follows per audio block.");
        makeLabel (lblCustRange, cCustom);
        configFader (lfoDivSlider, "modL1Div", lfoDivAtt, cCustom,
                     "Rate divider: halves the base rate per step (base = project BPM or the lowest held note's frequency).");
        configFader (lfoHzSlider, "modL1Hz", lfoHzAtt, cCustom, "Free LFO rate in Hz.");
        configFader (lfoPhaseSlider, "modL1Phase", lfoPhaseAtt, cCustom,
                     "Phase offset: where in the wave the oscillation starts.");
        makeLabel (lblLfoDiv, cCustom);   lblLfoDiv.setText ("Div", juce::dontSendNotification);
        makeLabel (lblLfoHz, cCustom);    lblLfoHz.setText ("Hz", juce::dontSendNotification);
        makeLabel (lblLfoPhase, cCustom); lblLfoPhase.setText ("Ph", juce::dontSendNotification);

        modSelBox.addItem ("Env 1", 1); modSelBox.addItem ("Env 2", 2); modSelBox.addItem ("Env 3", 3);
        modSelBox.addItem ("LFO 1", 4); modSelBox.addItem ("LFO 2", 5); modSelBox.addItem ("LFO 3", 6);
        modSelBox.setSelectedId (1, juce::dontSendNotification);
        modSelBox.setColour (juce::ComboBox::outlineColourId, cCustom.withAlpha (0.7f));
        modSelBox.setMouseClickGrabsKeyboardFocus (false);
        modSelBox.onChange = [this] { modSel = modSelBox.getSelectedId() - 1; attachCustomTab(); resized(); refocus(); };
        addChildComponent (modSelBox);
        // The attachments only SYNC the selection - the items themselves must be
        // added here (an empty combo was why the LFO menus showed nothing).
        lfoWaveBox.addItem ("Sine", 1);        lfoWaveBox.addItem ("Triangle", 2);
        lfoWaveBox.addItem ("Saw up", 3);      lfoWaveBox.addItem ("Saw down", 4);
        lfoWaveBox.addItem ("Square", 5);      lfoWaveBox.addItem ("Custom (draw)", 6);
        lfoRateModeBox.addItem ("BPM / 2^n", 1);
        lfoRateModeBox.addItem ("Fundamental / 2^n", 2);
        lfoRateModeBox.addItem ("Free Hz", 3);
        lfoPhaseModeBox.addItem ("Restart each note", 1);
        lfoPhaseModeBox.addItem ("Project transport", 2);
        for (auto* cb : { &lfoWaveBox, &lfoRateModeBox, &lfoPhaseModeBox })
        {
            cb->setColour (juce::ComboBox::outlineColourId, cCustom.withAlpha (0.7f));
            cb->setMouseClickGrabsKeyboardFocus (false);
            addChildComponent (*cb);
        }
        lfoWaveBox.onChange = [this] { resized(); }; // Draw button appears with "Custom (draw)"
        // The LFO combos and the On switch are modulator parameters too: right-click
        // routes ANOTHER modulator to them (a modulator never drives itself).
        addMapListener (lfoWaveBox,      "modL1Wave",      true);
        addMapListener (lfoRateModeBox,  "modL1RateMode",  true);
        addMapListener (lfoPhaseModeBox, "modL1PhaseMode", true);
        modOnT.setName ("onled");
        addChildComponent (modOnT); // NOT modulatable: a modulator gating another
                                    // modulator that gates it back has no defined state

        modUnmapBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
        modUnmapBtn.setMouseClickGrabsKeyboardFocus (false);
        modUnmapBtn.onClick = [this]
        {
            processorRef.getModEngine().unmapAll (modSel);
            refocus();
        };
        addChildComponent (modUnmapBtn);
        lfoWaveEditBtn.setColour (juce::TextButton::buttonColourId, cCustom.withAlpha (0.85f));
        lfoWaveEditBtn.setMouseClickGrabsKeyboardFocus (false);
        lfoWaveEditBtn.onClick = [this]
        {
            const int l = modSel - 3;
            if (l < 0) return;
            waveEditor.fromText (processorRef.getLfoWavePoints (l));
            waveEditor.setVisible (true);
            waveEditor.toFront (false);
        };
        addChildComponent (lfoWaveEditBtn);
        waveEditor.onChange = [this]
        {
            const int l = modSel - 3;
            if (l < 0) return;
            float table[ModEngine::kWaveN];
            waveEditor.fillTable (table, ModEngine::kWaveN);
            processorRef.setLfoWave (l, table, waveEditor.toText());
        };
        waveEditor.onClose = [this] { waveEditor.setVisible (false); refocus(); };
        addChildComponent (waveEditor);
        attachCustomTab();
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
    // X: remove the SELECTED tuning from the bank (the scale itself is untouched).
    tuningBankClearBtn.setButtonText ("X");
    tuningBankClearBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
    tuningBankClearBtn.setMouseClickGrabsKeyboardFocus (false);
    tuningBankClearBtn.setTooltip (tr ("Remove the selected tuning from the bank.",
                                       "Treu l'afinaci\xc3\xb3 seleccionada del banc."));
    tuningBankClearBtn.onClick = [this]
    {
        const int slot0 = tuningBankBox.getSelectedId() - 2; // id i+2 -> slot i
        if (slot0 >= 0) { processorRef.bankClearSlot (slot0); refreshTuningBank(); }
        refocus();
    };
    addAndMakeVisible (tuningBankClearBtn);
    refreshTuningBank();

    // Timbre (.scl as spectrum).
    configGroup (gTimbre, cTimbre);
    loadTimbreButton.setColour (juce::TextButton::buttonColourId, cLoad);
    loadTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    loadTimbreButton.setTooltip ("Use a Scala file's intervals AS the timbre (partials). Slope + fractal + repeats shape it; the harmonic controls turn off.");
    loadTimbreButton.onClick = [this] { openTimbreChooser(); };
    addAndMakeVisible (loadTimbreButton);
    // On/Off toggle: switching OFF keeps the loaded .scl (nothing is erased) - the
    // engine just goes back to harmonic mode until switched on again.
    clearTimbreButton.setColour (juce::TextButton::buttonColourId, cReset);
    clearTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    clearTimbreButton.onClick = [this]
    {
        processorRef.setTimbreEnabled (! processorRef.isTimbreActive());
        resized(); refocus();
    };
    addAndMakeVisible (clearTimbreButton);
    temperToggle.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffb9c0d0));
    temperToggle.setMouseClickGrabsKeyboardFocus (false);
    temperAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "timbreTemper", temperToggle);
    addMapListener (temperToggle, "timbreTemper");
    addAndMakeVisible (temperToggle);
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
        m.addItem (1, tr ("Reset all parameters", "Reinicia tots els par\xc3\xa0metres"));
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
                    // Init also unroutes every Spectrum-to-Rhythm pulse.
                    for (auto& s : processorRef.getRhythm().slots)
                    {
                        s.enabled = false; s.partial = -1;
                        s.steps = 0; s.useRatio = false; s.ratio = 1.0; s.durMs = 60.0f; s.vel = 100;
                    }
                    rhythmPanel.refreshSlots();
                    rhythmPanel.list.repaint();
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

    // Envelope tabs: Amp / Pitch / Filter / Custom share one compact box.
    for (int k = 0; k < 4; ++k)
    {
        envTabB[k].setMouseClickGrabsKeyboardFocus (false);
        envTabB[k].onClick = [this, k] { envTab = k; resized(); repaint(); refocus(); };
        addAndMakeVisible (envTabB[k]);
    }

    // Parameter undo / redo lives on Ctrl-Z / Ctrl-Y (no buttons - see keyPressed).

    // Save the CURRENT tuning (any mode - it is rendered to .scl form) to a chosen
    // folder. Pairs with the keyboard's scale editing (right-click / Ctrl+click).
    saveTuningBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2fb155));
    saveTuningBtn.setMouseClickGrabsKeyboardFocus (false);
    saveTuningBtn.onClick = [this]
    {
        auto ch = std::make_shared<juce::FileChooser> (
            tr ("Save tuning as .scl", "Desa l'afinaci\xc3\xb3 com a .scl"),
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile ("tuning.scl"),
            "*.scl");
        ch->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this, ch] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File()) { refocus(); return; }
                if (! f.hasFileExtension ("scl")) f = f.withFileExtension ("scl");
                juce::String err;
                if (! processorRef.saveCurrentTuningScl (f, err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        tr ("Save tuning", "Desa l'afinaci\xc3\xb3"), err, this);
                refocus();
            });
    };
    addAndMakeVisible (saveTuningBtn);

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
    rhythmSelfAtt = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmSelfPlay", rhythmPanel.selfT);
    rhythmDroneAtt = std::make_unique<ButtonAttachment>  (processorRef.apvts, "rhythmDrone", rhythmPanel.droneT);
    rhythmFbAtt   = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmFeedback", rhythmPanel.feedbackT);
    rhythmQTempoAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "rhythmQTempo", rhythmPanel.qTempoT);
    rhythmMonoAtt   = std::make_unique<ButtonAttachment> (processorRef.apvts, "rhythmMono",   rhythmPanel.monoT);
    rhythmQPulseAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "rhythmQPulse", rhythmPanel.qPulseT);
    rhythmFbDecayAtt = std::make_unique<SliderAttachment>(processorRef.apvts, "rhythmFeedbackDecay", rhythmPanel.feedbackS);
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
            applyTimbreEnablement (processorRef.timbreReplacesEngine());
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
    dissModeBox.addItem ("Minima", 1);
    dissModeBox.addItem ("Maxima", 2);
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

    // Spectrum-to-Tuning MODE selector (order: chords, dyads, segment). The combo is
    // two lines tall so the full mode names fit. Relayout on change.
    specModeBox.addItem ("Chords based on beating", 1);
    specModeBox.addItem ("Dyads based on beating",  2);
    specModeBox.addItem ("From spectrum's segment", 3);
    specModeBox.setSelectedId (processorRef.isSegmentModeActive() ? 3 : 2, juce::dontSendNotification); // restored state
    specModeBox.setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
    specModeBox.setMouseClickGrabsKeyboardFocus (false);
    specModeBox.onChange = [this]
    {
        const bool seg = specModeBox.getSelectedId() == 3;
        processorRef.setSegmentModeActive (seg);
        if (seg) processorRef.rebuildSegmentScale();
        resized(); refocus();
    };
    addAndMakeVisible (specModeBox);
    // Two labelled view buttons: "Results" = the list of ALL found chords;
    // "Tuning info" = the LOADED chord's degrees ranked like any scale.
    for (auto* b : { &chordViewBack, &chordViewFwd })
    {
        b->setMouseClickGrabsKeyboardFocus (false);
        addChildComponent (*b);
    }
    chordViewBack.onClick = [this] { chordDegView = false; resized(); repaint(); refocus(); };
    chordViewFwd.onClick  = [this] { chordDegView = true;  resized(); repaint(); refocus(); };

    // --- Chord search (mode 3): n notes, period, min/max, list count, Transmute ---
    for (int n = 2; n <= 8; ++n) chordNotesBox.addItem (juce::String (n), n);
    chordNotesBox.setSelectedId (3, juce::dontSendNotification);
    for (const int c : { 4, 8, 12, 16, 24 }) chordCountBox.addItem (juce::String (c), c);
    chordCountBox.setSelectedId (12, juce::dontSendNotification);
    chordMinMaxBox.addItem ("Min", 1); // most consonant
    chordMinMaxBox.addItem ("Max", 2); // most dissonant
    chordMinMaxBox.setSelectedId (1, juce::dontSendNotification);
    for (auto* cb : { &chordNotesBox, &chordCountBox, &chordMinMaxBox })
    {
        cb->setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
        cb->setMouseClickGrabsKeyboardFocus (false);
        addChildComponent (*cb);
    }
    makeLabel (chordNotesLbl, cSpecScale);
    chordNotesLbl.setVisible (false);
    makeLabel (chordPerLbl, cSpecScale);
    chordPerLbl.setVisible (false);
    chordPeriodEdit.setText ("2", juce::dontSendNotification);
    chordPeriodEdit.setJustification (juce::Justification::centred);
    chordPeriodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    chordPeriodEdit.setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
    chordPeriodEdit.setIndents (8, 2);
    chordPeriodEdit.setSelectAllWhenFocused (true);
    chordPeriodEdit.onReturnKey = [this] { refocus(); };
    chordPeriodEdit.onEscapeKey = [this] { refocus(); };
    addChildComponent (chordPeriodEdit);
    chordGoBtn.setColour (juce::TextButton::buttonColourId, cSpecScale.withAlpha (0.85f));
    chordGoBtn.onClick = [this] { runChordSearch(); };
    addChildComponent (chordGoBtn);
    // Minimum interval between generated notes (cents) - both Sethares creators.
    for (auto pr : { std::pair<juce::TextEditor*, juce::Label*> { &dissMinEdit, &dissMinLbl },
                     std::pair<juce::TextEditor*, juce::Label*> { &chordMinEdit, &chordMinLbl } })
    {
        auto* ed = pr.first;
        ed->setText ("33", juce::dontSendNotification);
        ed->setJustification (juce::Justification::centred);
        ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
        ed->setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
        ed->setIndents (4, 2);
        ed->setInputRestrictions (5, "0123456789.");
        ed->setSelectAllWhenFocused (true);
        ed->onReturnKey = [this] { refocus(); };
        ed->setTooltip (tr ("Minimum interval between generated notes, in cents (also to the tonic and the period).",
                            "Interval m\xc3\xadnim entre notes generades, en cents (tamb\xc3\xa9 respecte a la t\xc3\xb2nica i el per\xc3\xado" "de)."));
        addChildComponent (*ed);
        makeLabel (*pr.second, cSpecScale);
        pr.second->setVisible (false);
    }
    makeLabel (chordCountLbl, cSpecScale);
    chordCountLbl.setVisible (false);
    chordProgLbl.setJustificationType (juce::Justification::centredLeft);
    chordProgLbl.setColour (juce::Label::textColourId, cSpecScale.brighter (0.2f));
    chordProgLbl.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
    addChildComponent (chordProgLbl);
    chordList.onPlay = [this] (int row) { playChordPreview (row); };
    chordList.onLoad = [this] (int row)
    {
        juce::String err;
        if (! processorRef.loadChordAsScale (row, err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                tr ("Chords based on beating", "Acords basats en batec"), err, this);
        else { chordDegView = true; resized(); } // jump to the loaded chord's ranked degrees
        refocus();
    };
    chordView.setViewedComponent (&chordList, false);
    chordView.setScrollBarsShown (true, true); // wide chords scroll sideways
    addChildComponent (chordView);

    // n-AD selector: "Degrees" = the classic per-degree list (each step vs the
    // tonic); Dyads/Triads/... = ALL combinations of n scale degrees ranked by
    // their TOTAL beating (every pair counted). Items rebuilt live per tuning.
    nadBox.setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
    nadBox.setMouseClickGrabsKeyboardFocus (false);
    nadBox.setTooltip (tr (
        "What the list below shows: Degrees = each scale step against the tonic. Dyads, triads, tetrads... = EVERY combination of that many scale notes, ranked by total beating (all pairs counted, not just against 0).",
        "Qu\xc3\xa8 mostra la llista: Graus = cada pas contra la t\xc3\xb2nica. D\xc3\xad""ades, tr\xc3\xad""ades, t\xc3\xa8trades... = TOTES les combinacions d'aquest nombre de notes, ordenades pel batec total (tots els parells, no nom\xc3\xa9s contra el 0)."));
    nadBox.onChange = [this]
    {
        if (nadBox.getSelectedId() > 1)
        {
            processorRef.startNadCompute (nadBox.getSelectedId());
            lastNadSel = nadBox.getSelectedId(); // the timer must not re-spawn the
            lastNadSig = lastScaleSig;           // worker it would immediately cancel
        }
        resized(); refocus();
    };
    addChildComponent (nadBox);
    nadView.setViewedComponent (&nadList, false);
    nadView.setScrollBarsShown (true, false);
    addChildComponent (nadView);
    nadList.tip = tr ("Combinations of scale degrees with the LEAST total beating first (colour = the value's height on the beating range).",
                      "Combinacions de graus de l'escala amb MENYS batec total primer (color = al\xc3\xa7""ada del valor dins el rang de batec).");

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
    refAEdit.setInputRestrictions (11, "0123456789."); // fits C3's unrounded 261.6255653
    refAEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    refAEdit.setColour (juce::TextEditor::outlineColourId, cEssentials.withAlpha (0.7f));
    refAEdit.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    refAEdit.setIndents (8, 2); // clear the rounded (pill) ends
    refAEdit.setSelectAllWhenFocused (true);
    refAEdit.setText (juce::String ((double) processorRef.apvts.getRawParameterValue ("refA")->load()
                                    * 0.59460355750136051, 7) // shown as C3 (= A * 2^(-9/12)), unrounded
                          .trimCharactersAtEnd ("0").trimCharactersAtEnd ("."),
                      juce::dontSendNotification);
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
    for (juce::Slider* s : { (juce::Slider*) &baseNFader, (juce::Slider*) &fractalFader,
                             (juce::Slider*) &timbreRepeatsFader })
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

    keyboard.setDegreeLabeller ([this] (int d) { return degreeLabel (processorRef, d); });
    keyboard.setHoverTextProvider ([this] (int d) -> juce::String
    {
        // Floating window: ONLY information the key does not already show - never a
        // repetition of its own label.
        const auto di = analyzeDegree (processorRef, d);
        if (di.cents == 0.0 && ! di.genuine && ! di.hasApprox && processorRef.getDegreeRatio (d) <= 0.0)
            return {};

        if (di.genuine) // a true just interval: the key shows the ratio; add its cents
            return "= " + juce::String (di.cents, 1) + tr (" cents", " cents");

        juce::String s;
        if (di.hasApprox) // tempered, near a simple ratio: just the hint
            s << "\xe2\x89\x88 " << juce::String (di.ap) << "/" << juce::String (di.aq)
              << " (" << (di.approxErr >= 0 ? "+" : "-") << juce::String (std::abs (di.approxErr)) << "¢)";
        return s;
    });
    // Segment scales: append the hovered degree's SOURCE partial to the hover window
    // (drawn in the Spectrum-to-Tuning orange by GalacticLnF::drawTooltip).
    {
        auto base = keyboard.getHoverTextProvider();
        keyboard.setHoverTextProvider ([this, base] (int d) -> juce::String
        {
            juce::String s = base ? base (d) : juce::String();
            if (processorRef.activeScaleIsSegment())
            {
                const double src = processorRef.getSegmentDegreeSource (d);
                if (src > 0.0)
                    s << "\n" << tr ("from the partial: ", "del parcial: ") << justName (src);
            }
            return s;
        });
    }
    // A degree is "just" only when it is a GENUINE ratio (rational by source, or
    // float-exact) - this REJECTS tempered/equal-division intervals (a 12-EDO fifth is
    // ~2c off 3/2, so it is NOT treated as just).
    auto degreeIsJust = [this] (int note) -> bool
    {
        const int D = juce::jmax (1, processorRef.getScaleDegrees());
        return analyzeDegree (processorRef, ((note - 60) % D + D) % D).genuine;
    };
    // Below-keyboard hover strip: each other key's distance to the hovered one, and
    // (segment scales) the hovered key's source partial "from n/1".
    keyboard.setDeltaProviders (
        [this, degreeIsJust] (int fromNote, int toNote) -> juce::String
        {
            const double f0 = processorRef.getNoteFrequency (fromNote);
            const double f1 = processorRef.getNoteFrequency (toNote);
            if (! (f0 > 0.0) || ! (f1 > 0.0)) return {};
            const int    steps = toNote - fromNote;
            const double cents = 1200.0 * std::log2 (f1 / f0);
            juce::String s; // steps (no "st" - that reads as semitone), then cents
            s << (steps > 0 ? "+" : "") << steps << "\n"
              << (cents >= 0 ? "+" : "-") << juce::String ((int) std::llround (std::abs (cents))) << juce::String::fromUTF8 ("¢");
            // Exact ratio ONLY when BOTH keys are genuine just degrees - then the ratio
            // between them is truly rational. Bounded to READABLE fractions: the ratio
            // between two large-ratio degrees is real but useless as "986/831" - those
            // show their cents instead (never a giant fraction).
            if (degreeIsJust (fromNote) && degreeIsJust (toNote))
            {
                long long p = 1, q = 1;
                dissonance::bestFraction (f1 / f0, p, q, 99);
                if (p > 0 && p <= 99 && q > 0 && q <= 99
                    && std::abs (cents - 1200.0 * std::log2 ((double) p / (double) q)) < 0.2)
                    s << "\n" << p << "/" << q;
            }
            return s;
        },
        [this] (int note) -> juce::String
        {
            if (! processorRef.activeScaleIsSegment()) return {};
            const int D = juce::jmax (1, processorRef.getScaleDegrees());
            const double src = processorRef.getSegmentDegreeSource (((note - 60) % D + D) % D);
            return (src > 0.0) ? tr ("from\n", "de\n") + justName (src) : juce::String();
        },
        cSpecScale);
    keyboard.setNoteHzProvider ([this] (int note) { return processorRef.getNoteFrequency (note); });
    // Right-click a key (Scala mode): retype that degree as cents or a ratio.
    keyboard.onEditKey = [this] (int note)
    {
        if (processorRef.getTuningMode() != 2) return; // .scl tunings only
        const int D   = juce::jmax (1, processorRef.getScaleDegrees());
        const int deg = ((note - 60) % D + D) % D;
        if (deg == 0)
        {
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                tr ("Edit degree", "Edita el grau"),
                tr ("0 is fixed at 1/1 - right-click any other key.",
                    "El 0 \xc3\xa9s fix a 1/1 - fes clic dret a qualsevol altra tecla."), this);
            return;
        }
        const auto di  = analyzeDegree (processorRef, deg);
        const auto pre = di.genuine ? juce::String (di.p) + "/" + juce::String (di.q)
                                    : juce::String (di.cents, 3);
        auto* w = new juce::AlertWindow (tr ("Edit degree ", "Edita el grau ") + juce::String (deg),
            tr ("New value: cents with a decimal point (\"702.0\") or a ratio (\"3/2\").",
                "Nou valor: cents amb punt decimal (\"702.0\") o un ratio (\"3/2\")."),
            juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("v", pre);
        w->addButton (tr ("OK", "OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (tr ("Cancel", "Cancel\xc2\xb7la"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, safe = juce::Component::SafePointer<juce::AlertWindow> (w), deg] (int r)
            {
                if (safe == nullptr) return;
                const auto txt = safe->getTextEditorContents ("v");
                if (r == 1)
                {
                    juce::String err;
                    if (! processorRef.editTuningDegree (deg, txt, err))
                        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                            tr ("Edit degree", "Edita el grau"), err, this);
                }
                refocus();
            }), true);
    };
    // Ctrl+click anywhere on the keys: add a note at that exact spot - it joins every
    // repetition of the period, sorted, loaded as a new .scl (works from ANY tuning).
    keyboard.onAddKey = [this] (int note, double frac)
    {
        const double f0 = processorRef.getNoteFrequency (60);
        const double fa = processorRef.getNoteFrequency (note);
        double       fb = note < 127 ? processorRef.getNoteFrequency (note + 1) : 0.0;
        if (! (f0 > 0.0) || ! (fa > 0.0)) return;
        if (! (fb > fa)) fb = fa * 1.0594630943592953; // top edge: semitone-ish step
        const double f = fa * std::pow (fb / fa, frac);
        juce::String err;
        if (! processorRef.addNoteToTuning (f / f0, err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                tr ("Add note", "Afegeix nota"), err, this);
        refocus();
    };
    // Hover dissonance: every other key shows its Sethares roughness AGAINST the
    // hovered one, coloured green (consonant) -> red (rough) like the interval list.
    keyboard.setHoverDissProvider ([this] (int hovered, int other) -> ScaleKeyboard::DissInfo
    {
        updateHoverDissCache (hovered);
        if (hoverDissNote != hovered || ! juce::isPositiveAndBelow (other, 128)) return {};
        const float v = hoverDissVal[(size_t) other];
        if (v < 0.0f) return {};
        const float t = juce::jlimit (0.0f, 1.0f,
            (v - hoverDissLo) / juce::jmax (1.0e-9f, hoverDissHi - hoverDissLo));
        const juce::Colour green (0xff3ec659), yellow (0xffe8d44d), red (0xffe0453f);
        const auto col = t < 0.5f ? green.interpolatedWith (yellow, t * 2.0f)
                                  : yellow.interpolatedWith (red, (t - 0.5f) * 2.0f);
        return { juce::String (v, 1), col };
    });
    addAndMakeVisible (keyboard);

    applyLanguage();
    applyTimbreEnablement (processorRef.timbreReplacesEngine());
    lastTimbreActive = processorRef.timbreReplacesEngine();

    // The tooltip window must NEVER swallow clicks: with the shorter hover delay a
    // tip could pop under the cursor and eat the next mouse-down - that was the
    // "can't move faders while a note plays unless I started dragging before" bug.
    tooltipWindow.setInterceptsMouseClicks (false, false);

    setWantsKeyboardFocus (true);
    setSize (1200, 650); // +30 vs before for the keyboard's below-key hover-delta strip
    startTimerHz (30); // also the reconcile rate for stuck/ghost computer-keyboard notes
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });
}

FractalAdditiveAudioProcessorEditor::~FractalAdditiveAudioProcessorEditor()
{
    stopTimer();
    // A preview playing when the UI closes would otherwise ring forever (its
    // scheduled note-offs live in this editor's queue) - stop it explicitly.
    stopChordPreview();
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
    rhythmPanel.heading.setText (tr ("SPECTRUM TO RHYTHM", "ESPECTRE A RITME"), dontSendNotification);
    rhythmPanel.outLbl.setText  (tr ("MIDI out (to DAW)", "Sortida MIDI (al DAW)"), dontSendNotification);
    rhythmPanel.selfLbl.setText (juce::String::fromUTF8 (catalan ? "Sona al Fibl\xc3\xb3" : "Play in Fibl\xc3\xb3"), dontSendNotification);
    rhythmPanel.droneT.setButtonText (tr ("Note under pulses", "Nota sota els polsos"));
    rhythmPanel.droneT.setTooltip (tr (
        "With \"Play in Fibló\" on, ALSO sound the held note normally underneath the pulses. Off = only the rhythm sounds.",
        "Amb \"Sona al Fibl\xc3\xb3\" actiu, fes sonar TAMB\xc3\x89 la nota mantinguda normal sota els polsos. Desactivat = nom\xc3\xa9s sona el ritme."));
    rhythmPanel.feedbackT.setButtonText (tr ("Fractal feedback", "Feedback fractal"));
    rhythmPanel.feedbackT.setTooltip (tr (
        "Play in Fibló only: every pulse becomes a parent - it spawns the WHOLE set of routed trains one level down, anchored at ITS pitch (rates and intervals rebuilt from it), with its velocity faded by the slope and its gate inherited. Those pulses spawn again -> a self-similar fractal rhythm that always dies out (never a runaway).",
        "Nom\xc3\xa9s amb Sona al Fibl\xc3\xb3: cada pols esdev\xc3\xa9 pare - genera TOT el conjunt de trens encaminats un nivell avall, ancorat al SEU to (velocitats i intervals reconstru\xc3\xafts des d'ell), amb la seva velocity esva\xc3\xaf" "da pel pendent i el seu gate heretat. Aquells polsos tornen a generar -> un ritme fractal autosimilar que sempre s'apaga (mai descontrolat)."));
    rhythmPanel.feedbackLbl.setText (tr ("Decay", "Deca\xc3\xafment"), dontSendNotification);
    rhythmPanel.feedbackS.setTooltip (tr (
        "Volume slope of the feedback: how much of each repeat's velocity carries to the next loop. 50% = halve each time (short tail); higher = a longer, louder tail.",
        "Pendent de volum del feedback: quanta velocity de cada repetici\xc3\xb3 passa al seg\xc3\xbc" "ent bucle. 50% = meitat cada cop (cua curta); m\xc3\xa9s alt = cua m\xc3\xa9s llarga i forta."));
    rhythmPanel.selfT.setTooltip (tr (
        "Play in Fibló: this synth interprets the rhythm itself - the played note becomes a trigger and only the pulses sound. On by default; it engages once you route a partial. No feedback loop: pulses never spawn pulses.",
        "Sona al Fibl\xc3\xb3: aquest sinte interpreta el ritme ell mateix - la nota tocada esdev\xc3\xa9 un activador i nom\xc3\xa9s sonen els polsos. Actiu per defecte; s'activa quan encamines un parcial. Sense bucle: els polsos mai generen polsos."));
    rhythmPanel.velHead.setText (tr ("VEL", "VEL"), dontSendNotification);
    rhythmPanel.velHead.setTooltip (tr ("MIDI velocity of every pulse this train sends (1-127).",
                                        "Velocity MIDI de cada pols que envia aquest tren (1-127)."));
    rhythmPanel.octLbl.setText (tr ("Octave (all)", "Octava (tots)"), dontSendNotification);
    for (auto* b : { &rhythmPanel.octAllUp, &rhythmPanel.octAllDn })
        b->setTooltip (tr ("Shift ALL pulses together by whole octaves (-4..+4). Ratio pitches move by exact 2/1s; steps pitches by whole periods of the scale.",
                           "Despla\xc3\xa7""a TOTS els polsos alhora per octaves senceres (-4..+4). Els tons en ratio es mouen per 2/1 exactes; els de passos per per\xc3\xado" "des sencers de l'escala."));
    rhythmPanel.listHead.setText (tr ("Partials of the current patch (click to route)",
                                      "Parcials del patch actual (clica per encaminar)"), dontSendNotification);
    rhythmPanel.pickHint.setText (juce::String::fromUTF8 (catalan
                                      ? "\xe2\x9f\xb5 Clica un parcial per encaminar-lo"
                                      : "\xe2\x9f\xb5 Click a partial to route it"), dontSendNotification);
    rhythmPanel.onT.setButtonText (tr ("On", "Actiu"));
    rhythmPanel.secRate.setText  (tr ("PULSE RATE", "VELOCITAT DELS POLSOS"), dontSendNotification);
    rhythmPanel.secOut.setText   (tr ("OUTPUT", "SORTIDA"), dontSendNotification);
    rhythmPanel.secTempo.setText (tr ("DAW TEMPO", "TEMPO DEL DAW"), dontSendNotification);
    rhythmPanel.secGate.setText  (tr ("NOTE START", "INICI DE NOTA"), dontSendNotification);
    rhythmPanel.qTempoT.setButtonText (tr ("Quantize to project tempo", "Quantitza al tempo del projecte"));
    rhythmPanel.qTempoT.setTooltip (tr (
        "A note may only START on a beat of the host's BPM: pressed mid-beat it stays silent and enters exactly on the next beat. Needs the DAW playing; released early, it is cancelled silently. Off by default.",
        "Una nota nom\xc3\xa9s pot COMEN\xc3\x87""AR en un beat del BPM del projecte: premuda a mig beat queda en sil\xc3\xa8nci i entra exactament al beat seg\xc3\xbc" "ent. Cal que el DAW reprodueixi; si la deixes anar abans, es cancel\xc2\xb7la en sil\xc3\xa8nci. Desactivat per defecte."));
    rhythmPanel.monoT.setButtonText (tr ("Mono", "Mono"));
    rhythmPanel.monoT.setTooltip (tr (
        "Monophonic: when a new note starts, the note currently sounding stops. Off = polyphonic.",
        "Monof\xc3\xb2nic: quan comen\xc3\xa7""a una nota nova, la que sonava s'atura. Desactivat = polif\xc3\xb2nic."));
    rhythmPanel.qPulseT.setButtonText (tr ("Quantize to pulse", "Quantitza al pols"));
    rhythmPanel.qPulseT.setTooltip (tr (
        "A new note waits for the moment the RUNNING pulse pattern resets - when all its polyrhythm subdivisions coincide on the 1 - and joins exactly there. With no pattern running it starts normally.",
        "Una nota nova espera el moment en qu\xc3\xa8 el patr\xc3\xb3 de polsos ACTIU es reinicia - quan totes les subdivisions del poliritme coincideixen a l'1 - i s'hi suma exactament all\xc3\xa0. Sense patr\xc3\xb3 actiu comen\xc3\xa7""a normal."));
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
        "While ON, the DAW tempo FOLLOWS pulse 1 (x2 / /2 to scale). NEEDS a one-time setup: in Ableton, drop the free \"Fibló Tempo Sync\" Max for Live device on a MIDI track that receives this plugin's MIDI output. In other DAWs, MIDI-map the tempo to CC20 with a 20-147 BPM range. Full steps: the \"How to route\" button.",
        "Mentre \xc3\xa9s actiu, el tempo del DAW SEGUEIX el pols 1 (x2 / /2 per escalar). CAL una configuraci\xc3\xb3 \xc3\xbanica: a Ableton, posa el dispositiu Max for Live gratu\xc3\xaft \"Fibl\xc3\xb3 Tempo Sync\" en una pista MIDI que rebi la sortida MIDI del plugin. En altres DAW, mapeja el tempo al CC20 amb rang 20-147 BPM. Passos complets: el bot\xc3\xb3 \"How to route\"."));
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
        "MIDI OUT: send the rhythm pulses out of the plugin to another DAW track. Off by default - turn it on to route the rhythm elsewhere (independent of \"Play in Fibló\").",
        "SORTIDA MIDI: envia els polsos de ritme fora del plugin cap a una altra pista del DAW. Desactivat per defecte - activa'l per encaminar el ritme a un altre lloc (independent de \"Sona al Fibl\xc3\xb3\")."));
    langButton.setTooltip (tr ("Switch the interface language to Catal\xc3\xa0.",
                               "Torna la interf\xc3\xad" "cie a l'angl\xc3\xa8s."));

    gFractal.setText       (tr ("Fractal", "Fractal"));
    gPrime.setText         (tr ("Prime Control", "Control primers"));
    gFilter.setText        (tr ("Filter", "Filtre"));
    gAmpEnv.setText        (tr ("Modulators", "Moduladors")); // shared chip-tabbed box
    gPitchEnv.setText      (tr ("Pitch Envelope", "Envolupant to"));
    envTabB[0].setButtonText (tr ("AMP",    "AMP"));
    envTabB[1].setButtonText (tr ("PITCH",  "TO"));
    envTabB[2].setButtonText (tr ("FILTER", "FILTRE"));
    envTabB[3].setButtonText (tr ("CUSTOM", "CUSTOM"));
    for (auto* b : { &envTabB[0], &envTabB[1], &envTabB[2] })
        b->setTooltip (tr ("The envelopes share this box - click a title to switch.",
                           "Les envolupants comparteixen aquesta caixa - clica un t\xc3\xadtol per canviar."));
    envTabB[3].setTooltip (tr (
        "6 modulators: 3 note-retriggered envelopes + 3 LFOs. Right-click ANY fader -> \"Map to\" routes a modulator onto it (several parameters per modulator allowed); it rides on the knob's own value over +-Range. Each modulator has On/Off and \"Unmap all\".",
        "6 moduladors: 3 envolupants redisparades per nota + 3 LFOs. Clic dret a QUALSEVOL fader -> \"Assigna a\" hi encamina un modulador (diversos par\xc3\xa0metres per modulador); cavalca sobre el valor del knob amb +-Range. Cada modulador t\xc3\xa9 On/Off i \"Unmap all\"."));
    lblCustRange.setText (tr ("Range", "Rang"),  dontSendNotification);
    modUnmapBtn.setButtonText (tr ("Unmap", "Desassigna"));
    modUnmapBtn.setTooltip (tr ("Unmap this modulator from EVERYTHING it modulates.",
                                "Desassigna aquest modulador de TOT el que modula."));
    lfoWaveEditBtn.setButtonText (tr ("Draw", "Dibuixa"));
    lfoWaveEditBtn.setTooltip (tr ("Draw this LFO's wave: points + curved segments, like automation.",
                                   "Dibuixa l'ona d'aquest LFO: punts + segments corbats, com una automatitzaci\xc3\xb3."));
    modOnT.setTooltip (tr ("This modulator on/off (off restores its targets to their own values).",
                           "Aquest modulador on/off (off retorna els seus dest\xc3\xadns al seu valor propi)."));
    temperToggle.setButtonText (tr ("Temper to tuning", "Tempera a l'afinaci\xc3\xb3"));
    temperToggle.setTooltip (tr (
        "ON: the HARMONIC ENGINE keeps generating partials and each one is snapped to the nearest pitch of the ACTIVE TUNING loaded in the Tuning box (degree x period^k), following it live - both engines work TOGETHER and the Custom Engine's own scale is bypassed. In MPE input mode the host does not hand the plugin a scale, so the grid there is plain 12-TET. OFF: no snapping (the Custom Engine's .scl, if loaded and ON, is the spectrum by itself).",
        "ON: el MOTOR HARM\xc3\x92NIC segueix generant parcials i cadascun s'ajusta al to m\xc3\xa9s proper de l'AFINACI\xc3\x93 ACTIVA carregada a la caixa Tuning (grau x per\xc3\xado" "de^k), seguint-la en viu - els dos motors treballen JUNTS i l'escala del Motor propi queda de banda. En mode d'entrada MPE l'amfitri\xc3\xb3 no d\xc3\xb3na cap escala al plugin, aix\xc3\xad que all\xc3\xa0 la graella \xc3\xa9s 12-TET. OFF: sense ajust (l'.scl del Motor propi, si est\xc3\xa0 carregat i actiu, \xc3\xa9s l'espectre per si mateix)."));
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
    lblRefA.setText      ("C3 =", dontSendNotification); // the tuning anchor key itself, not A
    lblTuningBank.setText (tr ("Bank", "Banc"), dontSendNotification);
    degreeList.tip = tr (
        "Each row is a DYAD: the red \"0\" chip (the root key) plus that degree, played together. Colour chip = the degree's keyboard colour, then its interval from 0, then the dyad's Sethares BEATING against the current timbre (how rough 0 + that degree sound together) - lower = more consonant. Ordered most consonant first. The period is the red row.",
        "Cada fila \xc3\xa9s una D\xc3\x8d""ADA: el xip vermell \"0\" (la tecla arrel) m\xc3\xa9s aquell grau, tocats junts. Xip de color = el color del grau al teclat, despr\xc3\xa9s el seu interval des de 0, i el BATEC de Sethares de la d\xc3\xad""ada contra el timbre actual (com d'aspre sonen 0 + aquell grau junts) - m\xc3\xa9s baix = m\xc3\xa9s consonant. Ordenats de m\xc3\xa9s consonant primer. El per\xc3\xado""de \xc3\xa9s la fila vermella.");
    dissHeader.tip = tr (
        "Sethares dissonance: how ROUGH each scale step sounds with the CURRENT timbre. It is not fixed - it recomputes live from the synth's actual partials, so editing the timbre (N, fractal, slope, primes, .scl...) changes these values in real time. Lower = more consonant; the rows are ordered most consonant first.",
        "Dissonancia de Sethares: com d'ASPRE sona cada pas de l'escala amb el timbre ACTUAL. No \xc3\xa9s fixa - es recalcula en viu a partir dels parcials reals del sinte, aix\xc3\xad que editar el timbre (N, fractal, pendent, primers, .scl...) canvia aquests valors en temps real. M\xc3\xa9s baix = m\xc3\xa9s consonant; les files van de m\xc3\xa9s consonant primer.");
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);

    loadSclButton.setButtonText    (tr ("Load .scl", "Obrir .scl"));
    clearSclButton.setButtonText   (tr ("Reset", "Reinicia"));
    tuningBankLoadBtn.setButtonText (tr ("+ .scl", "+ .scl"));
    loadTimbreButton.setButtonText (tr ("Load .scl", "Obrir .scl"));
    clearTimbreButton.setButtonText (processorRef.isTimbreActive() ? tr ("On", "Actiu") : tr ("Off", "Apagat"));
    savePresetButton.setButtonText (tr ("Save", "Desa"));
    presetBox.setTextWhenNothingSelected (tr ("Presets...", "Presets..."));
    dissBtn.setButtonText          (tr ("Find", "Troba"));
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
    harmonicsFader.setTooltip (tr (
        "Sweeps which harmonic numbers sound: Even (1,2,4,6...) - All - Odd (1,3,5...) - Prime (1,2,3,5,7...). It snaps magnetically to those four, but you can stop BETWEEN two of them (hold Ctrl for a free setting): there the harmonics belonging to only one of the pair sound at part volume, so the timbre morphs continuously. The fundamental is always kept so the played pitch stays. A harmonic must ALSO pass Prime Control - both filters apply at once, and if together they exclude everything the sound is silent.",
        "Escombra quins n\xc3\xbameros d'harm\xc3\xb2nic sonen: Parells (1,2,4,6...) - Tots - Senars (1,3,5...) - Primers (1,2,3,5,7...). S'imanta a aquests quatre, per\xc3\xb2 pots quedar-te ENTRE dos (mant\xc3\xa9n Ctrl per a un valor lliure): all\xc3\xa0 els harm\xc3\xb2nics que nom\xc3\xa9s pertanyen a un dels dos sonen a volum parcial, aix\xc3\xad que el timbre es transforma de manera cont\xc3\xadnua. El fonamental sempre es mant\xc3\xa9 perqu\xc3\xa8 quedi el to tocat. L'harm\xc3\xb2nic TAMB\xc3\x89 ha de passar el Control de primers - els dos filtres s'apliquen alhora, i si entre tots dos ho exclouen tot, no sona res."));
    slopeFader.setTooltip (tr (
        "How fast higher partials lose volume: 0 = all equal (brightest), 1 = like a saw (1/n), 2 = like a triangle, higher = darker. Also shapes the stacked scale in Timbre mode.",
        "Com de r\xc3\xa0pid perden volum els parcials aguts: 0 = tots iguals (m\xc3\xa9s brillant), 1 = com una serra (1/n), 2 = com un triangle, m\xc3\xa9s = m\xc3\xa9s fosc. Tamb\xc3\xa9 d\xc3\xb3na forma a l'escala apilada en mode Timbre."));
    fractalFader.setTooltip (tr (
        "Replaces each partial with a scaled copy of the whole spectrum, this many levels deep. 1 = off. Continuous: between whole levels the next level's new partials fade in from low to high. Snaps to the 4 marked levels - hold Ctrl for free fine setting.",
        "Substitueix cada parcial per una c\xc3\xb2pia escalada de tot l'espectre, tants nivells com indiquis. 1 = apagat. Continu: entre nivells sencers els parcials nous del nivell seg\xc3\xbc""ent entren de greu a agut. S'imanta als 4 nivells marcats - mant\xc3\xa9n Ctrl per ajust fi lliure."));
    phaseBox.setTooltip (tr (
        "Starting phase of the partials. 'Aligned': all peak together, forming a periodic pulse - tonal, never clips, and with dense spectra the pulse itself is heard as a low difference tone. 'Scattered': random phases - smoother, louder, no pulse.",
        "Fase inicial dels parcials. 'Alineat': tots fan el pic alhora i formen un pols peri\xc3\xb2""dic - tonal, mai clipa, i amb espectres densos el pols s'escolta com un to diferencial greu. 'Dispers': fases aleat\xc3\xb2ries - m\xc3\xa9s suau, m\xc3\xa9s fort, sense pols."));
    gainFader.setTooltip (tr ("Final output volume, in dB.", "Volum final de sortida, en dB."));
    primeModeBox.setTooltip (tr (
        "How prime numbers filter the harmonics. 'Limit': a harmonic passes only if its largest prime factor is at most the chosen limit. 'Whitelist': it passes only if EVERY prime factor is ticked below.",
        "Com filtren els primers els harm\xc3\xb2nics. 'L\xc3\xadmit': un harm\xc3\xb2nic passa nom\xc3\xa9s si el seu factor primer m\xc3\xa9s gran \xc3\xa9s com a molt el l\xc3\xadmit triat. 'Llista': passa nom\xc3\xa9s si TOTS els seus factors primers estan marcats a sota."));
    primeLimitFader.setTooltip (tr (
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
    clearTimbreButton.setTooltip (tr ("Custom engine ON/OFF. Off keeps the loaded scale - nothing is erased; switch back on any time.",
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
        "Reference pitch: C3 (the tuning anchor key) = this many Hz. The default 261.6255653 is exactly C3 at A=440. Transposes the WHOLE instrument in every tuning mode. Type a value and press Enter.",
        "To de refer\xc3\xa8ncia: C3 (la tecla \xc3\xa0ncora de l'afinaci\xc3\xb3) = aquests Hz. El defecte 261.6255653 \xc3\xa9s exactament C3 amb La=440. Transposa TOT l'instrument en qualsevol mode d'afinaci\xc3\xb3. Escriu un valor i prem Enter."));
    octaveFader.setTooltip (tr (
        "Transpose the WHOLE synth by whole octaves (-5..+5), on top of the reference.",
        "Transposa TOT el sintetitzador per octaves senceres (-5..+5), a sobre de la refer\xc3\xa8ncia."));

    auto localise = [] (juce::ComboBox& b, const juce::StringArray& items)
    {
        for (int i = 0; i < items.size(); ++i) b.changeItemText (i + 1, items[i]);
        b.setSelectedId (b.getSelectedId(), juce::dontSendNotification); // refresh displayed text
    };
    localise (basePitchBox, { tr ("Transposed down", "Transposada avall"), tr ("Original", "Original") });
    localise (phaseBox,     { tr ("Aligned", "Alineat"), tr ("Scattered", "Dispers") });
    localise (primeModeBox, { tr ("Limit", "L\xc3\xadmit"), tr ("Whitelist", "Llista") });
    localise (bendBox,      { "\xc2\xb1""2 st", "\xc2\xb1""12 st", "\xc2\xb1""48 st (MPE)" });
    localise (specModeBox,  { tr ("Chords based on beating", "Acords basats en batec"),
                              tr ("Dyads based on beating", "D\xc3\xad""ades basades en batec"),
                              tr ("From spectrum's segment", "Del segment de l'espectre") });
    lastNadD = 0; // force the n-ad selector's items to rebuild in the new language

    // Chord-search mode texts.
    chordNotesLbl.setText (tr ("Notes", "Notes"), dontSendNotification);
    chordPerLbl.setText   (tr ("Period", "Per\xc3\xado" "de"), dontSendNotification);
    dissMinLbl.setText    (tr ("smallest interval (c)", "interval m\xc3\xa9s petit (c)"), dontSendNotification);
    chordMinLbl.setText   (tr ("smallest interval (c)", "interval m\xc3\xa9s petit (c)"), dontSendNotification);
    chordCountLbl.setText (tr ("Chords", "Acords"), dontSendNotification);
    chordViewBack.setButtonText (tr ("Results", "Resultats"));
    chordViewFwd.setButtonText  (tr ("Tuning info", "Info afinaci\xc3\xb3"));
    chordViewBack.setTooltip (tr ("Show the list of ALL found chords.", "Mostra la llista de TOTS els acords trobats."));
    chordViewFwd.setTooltip  (tr ("Show the LOADED chord's degrees, ranked like any scale.",
                                  "Mostra els graus de l'acord CARREGAT, rankejats com qualsevol escala."));
    saveTuningBtn.setButtonText (tr ("Save tuning .scl", "Desa afinaci\xc3\xb3 .scl"));
    saveTuningBtn.setTooltip (tr (
        "Save the CURRENT tuning as a .scl file to any folder - including scales you edited on the keyboard (right-click a key to retype a degree, Ctrl+click to add a note).",
        "Desa l'afinaci\xc3\xb3 ACTUAL com a fitxer .scl a qualsevol carpeta - incloses les escales editades al teclat (clic dret per reescriure un grau, Ctrl+clic per afegir una nota)."));
    chordGoBtn.setButtonText (tr ("Find", "Troba"));
    chordNotesBox.setTooltip (tr (
        "How many notes each chord has (the root 1/1 is included). More notes = a heavier search - it runs in the background with a progress read-out.",
        "Quantes notes t\xc3\xa9 cada acord (l'arrel 1/1 hi \xc3\xa9s inclosa). M\xc3\xa9s notes = cerca m\xc3\xa9s pesada - corre en segon pla amb progr\xc3\xa9s."));
    chordCountBox.setTooltip (tr ("How many chords to list.", "Quants acords llistar."));
    chordMinMaxBox.setTooltip (tr (
        "Min = the most CONSONANT combinations with this exact timbre (Sethares sensory dissonance of all partial pairs). Max = the roughest.",
        "Min = les combinacions m\xc3\xa9s CONSONANTS amb aquest timbre exacte (disson\xc3\xa0ncia sensorial de Sethares de tots els parells de parcials). Max = les m\xc3\xa9s aspres."));
    chordPeriodEdit.setTooltip (tr (
        "The chord notes are searched inside this period: an integer (\"3\"), a fraction (\"3/2\") or cents (\"1200.0c\").",
        "Les notes de l'acord es cerquen dins d'aquest per\xc3\xado" "de: un enter (\"3\"), una fracci\xc3\xb3 (\"3/2\") o cents (\"1200.0c\")."));
    chordGoBtn.setTooltip (tr (
        "Rank all n-note combinations by their Sethares sensory dissonance against the CURRENT spectrum (grid search + exact refinement, on a background thread). Play previews a chord; double-click loads it as the active scale (.scl).",
        "Ordena totes les combinacions de n notes per la seva disson\xc3\xa0ncia sensorial de Sethares contra l'espectre ACTUAL (cerca en graella + refinament exacte, en segon pla). Play pre-escolta un acord; doble clic el carrega com a escala activa (.scl)."));
    chordList.tip = tr ("Each note in its own colour. Click the green triangle to hear it (all together, then an ascending strum); double-click the notes to LOAD the chord as the active scale.",
                        "Cada nota amb el seu color. Clica el triangle verd per escoltar-lo (tot alhora i despr\xc3\xa9s un strum ascendent); doble clic a les notes per CARREGAR l'acord com a escala activa.");

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
    const double minGap = juce::jlimit (0.0, 600.0, dissMinEdit.getText().getDoubleValue());
    juce::String err;
    if (! processorRef.generateDissonanceScale (n, maxima, periodRatio, periodLine, minGap, err))
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Spectrum to Scale", "Espectre a escala"), err, this);
    refocus();
}

// Chord search (mode 3): parse the period, then hand the heavy grid+refine work to
// the processor's background worker (deliberately NOT realtime - Transmute-triggered).
void FractalAdditiveAudioProcessorEditor::runChordSearch()
{
    double periodRatio = 2.0;
    juce::String periodLine = "2/1";
    if (! TuningManager::parsePeriodSpec (chordPeriodEdit.getText(), periodRatio, periodLine))
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Chords based on beating", "Acords basats en batec"),
            tr ("Invalid period. Use an integer (\"3\"), a fraction (\"3/2\"), or cents with a trailing c (\"701.955c\").",
                "Per\xc3\xado" "de no v\xc3\xa0lid. Fes servir un enter (\"3\"), una fracci\xc3\xb3 (\"3/2\") o cents amb una c final (\"701.955c\")."),
            this);
        refocus();
        return;
    }
    chordDegView = false; // Transmute always returns to the results list below
    resized();
    juce::String err;
    if (! processorRef.startChordSearch (juce::jmax (2, chordNotesBox.getSelectedId()), periodRatio, periodLine,
                                         juce::jmax (1, chordCountBox.getSelectedId()),
                                         chordMinMaxBox.getSelectedId() == 2,
                                         juce::jlimit (0.0, 600.0, chordMinEdit.getText().getDoubleValue()), err))
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Chords based on beating", "Acords basats en batec"), err, this);
    refocus();
}

// Stop the running preview NOW: explicit note-offs for exactly the notes we started
// (channel allNotesOff is ignored on member channels by a real MPE zone, so per-note
// offs are the only reliable stop) + recentre the bends we sent, so no stale preview
// bend can poison a later note landing on the same channel. Safe to call any time.
void FractalAdditiveAudioProcessorEditor::stopChordPreview()
{
    previewQueue.clear();
    if (previewNotes.empty()) return;
    if (processorRef.getSampleRate() > 0.0) // collector needs a prepared processor
    {
        const double nowS = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (const auto& [ch, key] : previewNotes)
        {
            auto off = juce::MidiMessage::noteOff (ch, key);
            off.setTimeStamp (nowS);
            processorRef.previewCollector.addMessageToQueue (off);
        }
        for (int ch = 9; ch <= 16; ++ch)
        {
            auto centre = juce::MidiMessage::pitchWheel (ch, 8192);
            centre.setTimeStamp (nowS);
            processorRef.previewCollector.addMessageToQueue (centre);
        }
    }
    previewNotes.clear();
}

// Preview a chord result with the synth itself, under the CURRENT tuning: each note is
// sent as nearest-key + per-channel pitch bend (channels 9..16), first the whole chord
// together for 1.5 s, then an ascending strum (200 ms between notes).
void FractalAdditiveAudioProcessorEditor::playChordPreview (int row)
{
    const auto results = processorRef.getChordResults();
    if (! juce::isPositiveAndBelow (row, (int) results.size())) return;
    if (! (processorRef.getSampleRate() > 0.0)) return; // collector not prepared yet
    const auto& alphas = results[(size_t) row].alphas;

    stopChordPreview(); // any previous preview ends cleanly first

    // Bend range of the receiver: the MPE-input mode's zone is hard-wired to +-48;
    // the legacy modes follow the bendRange parameter.
    static constexpr int bendValues[] = { 2, 12, 48 };
    const int bendRange = processorRef.getTuningMode() == 3 ? 48
        : bendValues[juce::jlimit (0, 2,
              (int) processorRef.apvts.getRawParameterValue ("bendRange")->load())];
    const double f60 = processorRef.getNoteFrequency (60);
    if (! (f60 > 0.0)) return;

    struct NoteSend { int key, ch, wheel; };
    std::vector<NoteSend> send;
    for (int k = 0; k < (int) alphas.size(); ++k)
    {
        const double target = f60 * alphas[(size_t) k];
        int    bestKey = 60; double bestErr = 1.0e18;
        for (int m = 0; m <= 127; ++m)
        {
            const double f = processorRef.getNoteFrequency (m);
            if (! (f > 0.0)) continue;
            const double e = std::abs (std::log2 (target / f));
            if (e < bestErr) { bestErr = e; bestKey = m; }
        }
        const double semis = 12.0 * std::log2 (target / processorRef.getNoteFrequency (bestKey));
        const int wheel = juce::jlimit (0, 16383,
            8192 + (int) std::lround (semis / (double) bendRange * 8192.0));
        send.push_back ({ bestKey, 9 + (k % 8), wheel });
    }

    const juce::uint32 t0 = juce::Time::getMillisecondCounter();
    auto queue = [this] (juce::uint32 at, const juce::MidiMessage& m) { previewQueue.push_back ({ at, m }); };
    for (const auto& s : send) // the whole chord at once, 1.5 s
    {
        previewNotes.push_back ({ s.ch, s.key }); // tracked for explicit stopping
        queue (t0,        juce::MidiMessage::pitchWheel (s.ch, s.wheel));
        queue (t0,        juce::MidiMessage::noteOn  (s.ch, s.key, (juce::uint8) 100));
        queue (t0 + 1500, juce::MidiMessage::noteOff (s.ch, s.key));
    }
    juce::uint32 lastOff = t0 + 1500;
    for (int k = 0; k < (int) send.size(); ++k) // then the ascending strum
    {
        const auto& s = send[(size_t) k];
        const juce::uint32 on = t0 + 1700 + (juce::uint32) k * 200;
        queue (on,       juce::MidiMessage::pitchWheel (s.ch, s.wheel));
        queue (on,       juce::MidiMessage::noteOn  (s.ch, s.key, (juce::uint8) 100));
        queue (on + 900, juce::MidiMessage::noteOff (s.ch, s.key));
        lastOff = juce::jmax (lastOff, on + 900);
    }
    // After the preview ends, recentre the bends we used so no stale bend can affect
    // a later note landing on the same channel, and forget the tracked notes.
    for (int ch = 9; ch <= 9 + juce::jmin (7, (int) send.size() - 1); ++ch)
        queue (lastOff + 50, juce::MidiMessage::pitchWheel (ch, 8192));
}

// Sethares dissonance of every key AGAINST the hovered one, cached: recomputed only
// when the hovered key, the timbre or the tuning changes (the per-paint provider then
// just reads the table). Analysis-only 64-partial cap, like every dissonance readout.
void FractalAdditiveAudioProcessorEditor::updateHoverDissCache (int hoveredNote)
{
    if (! juce::isPositiveAndBelow (hoveredNote, 128)) return;
    const double f0 = processorRef.getNoteFrequency (hoveredNote);
    if (! (f0 > 0.0)) return;
    const double sig = (double) processorRef.getTemplateGeneration() * 1.0e5 + f0;
    if (hoveredNote == hoverDissNote && std::abs (sig - hoverDissSig) < 1.0e-9) return;

    hoverDissNote = hoveredNote;
    hoverDissSig  = sig;
    hoverDissVal.fill (-1.0f);

    const auto* t = processorRef.getLiveTemplate();
    if (t == nullptr || t->partials.empty()) return;
    std::vector<dissonance::Partial> spec;
    spec.reserve (t->partials.size());
    for (const auto& p : t->partials)
        spec.push_back ({ (double) p.ratio * f0, (double) p.amp });
    if (spec.size() > 64)
    {
        std::sort (spec.begin(), spec.end(),
                   [] (const dissonance::Partial& a, const dissonance::Partial& b) { return a.amp > b.amp; });
        spec.resize (64);
    }

    // Values for every key, but the COLOUR SCALE is anchored to the PLAYABLE range
    // only (like the degree list anchors to its own rows) - extreme off-range keys
    // must not compress the visible green->red sweep into a mush.
    float lo = 1.0e30f, hi = -1.0e30f;
    const int rLo = lastKbLo, rHi = lastKbLo + juce::jmax (1, lastKbNum);
    for (int n = 0; n < 128; ++n)
    {
        const double fn = processorRef.getNoteFrequency (n);
        if (! (fn > 0.0)) continue;
        const float v = (float) dissonance::dissonanceAt (spec, fn / f0);
        hoverDissVal[(size_t) n] = v;
        if (n >= rLo && n < rHi) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
    }
    hoverDissLo = lo; hoverDissHi = hi;
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
        row.label    = degreeLabel (processorRef, d.degree); // source-honest: exact ratio, or cents (+ "*")
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
    for (auto* c : { (juce::Component*) &basePitchBox, (juce::Component*) &phaseBox,
                     (juce::Component*) &primeModeBox, (juce::Component*) &primeLimitFader,
                     (juce::Component*) &harmonicsFader })
        c->setEnabled (en);
    for (auto& t : primeToggle) t.setEnabled (en);

    const float a = active ? 0.4f : 1.0f;
    phaseBox.setAlpha (a);
    baseNFader.setAlpha (a); basePitchBox.setAlpha (a); harmonicsFader.setAlpha (a);
    phaseBox.setAlpha (a); primeModeBox.setAlpha (a); primeLimitFader.setAlpha (a);
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

// The Reference cell shows and takes C3 (middle C, the tuning anchor key 60), not A:
// C3 = A * 2^(-9/12). Internally the parameter stays the A reference, so nothing else
// changes; at A=440 the default display is C3 = 261.6255653 (no rounding).
static constexpr double kC3overA = 0.59460355750136051; // 2^(-9/12)

void FractalAdditiveAudioProcessorEditor::commitRefA()
{
    if (auto* p = processorRef.apvts.getParameter ("refA"))
    {
        const double c3 = refAEdit.getText().getDoubleValue();
        if (c3 > 0.0)
        {
            const float hz = (float) (c3 / kC3overA); // typed C3 -> the internal A reference
            const auto& range = processorRef.apvts.getParameterRange ("refA");
            p->setValueNotifyingHost (range.convertTo0to1 (juce::jlimit (range.start, range.end, hz)));
        }
    }
    refreshRefAText();
}

void FractalAdditiveAudioProcessorEditor::refreshRefAText()
{
    const double a  = (double) processorRef.apvts.getRawParameterValue ("refA")->load();
    const auto txt = juce::String (a * kC3overA, 7).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
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
    // Double-click ANY fader = straight back to its default value.
    if (auto* prm = processorRef.apvts.getParameter (paramID))
        s.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
    // Right-click ANY fader -> map/unmap it to any of the 6 modulators. The CUSTOM
    // tab's own faders carry a modulator id that changes with the selector, so they
    // resolve their target id at click time.
    addMapListener (s, paramID, paramID.startsWith ("modE1") || paramID.startsWith ("modL1"));
}

// A CUSTOM-tab control is built against modulator 1's ids but re-attached to the
// SELECTED modulator: translate the template id to the live one.
juce::String FractalAdditiveAudioProcessorEditor::currentModParamId (const juce::String& templateId) const
{
    const bool isEnv = modSel < 3;
    if (templateId.startsWith ("modE1")) return (isEnv ? "modE" : "modL")
        + juce::String (isEnv ? modSel + 1 : modSel - 2) + templateId.substring (5);
    if (templateId.startsWith ("modL1")) return (isEnv ? "modE" : "modL")
        + juce::String (isEnv ? modSel + 1 : modSel - 2) + templateId.substring (5);
    return templateId;
}

void FractalAdditiveAudioProcessorEditor::addMapListener (juce::Component& c, const juce::String& paramID,
                                                          bool dynamicMod)
{
    auto ml = std::make_unique<ParamMapListener>();
    ml->ed = this; ml->pid = paramID; ml->dynamicMod = dynamicMod;
    c.addMouseListener (ml.get(), false);
    mapListeners.push_back (std::move (ml));
}

// Re-bind the CUSTOM tab's controls to the selected modulator's parameters.
void FractalAdditiveAudioProcessorEditor::attachCustomTab()
{
    const bool isEnv = modSel < 3;
    const auto pfx = (isEnv ? "modE" : "modL") + juce::String (isEnv ? modSel + 1 : modSel - 2);
    if (isEnv)
    {
        const char* sfx[] = { "A", "D", "S", "R" };
        for (int i = 0; i < 4; ++i)
            custAdsrAtt[i] = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + sfx[i], custAdsr[i]);
        lfoWaveAtt.reset(); lfoRateModeAtt.reset(); lfoPhaseModeAtt.reset();
        lfoDivAtt.reset(); lfoHzAtt.reset(); lfoPhaseAtt.reset();
    }
    else
    {
        for (auto& a : custAdsrAtt) a.reset();
        lfoWaveAtt      = std::make_unique<ComboBoxAttachment> (processorRef.apvts, pfx + "Wave", lfoWaveBox);
        lfoRateModeAtt  = std::make_unique<ComboBoxAttachment> (processorRef.apvts, pfx + "RateMode", lfoRateModeBox);
        lfoPhaseModeAtt = std::make_unique<ComboBoxAttachment> (processorRef.apvts, pfx + "PhaseMode", lfoPhaseModeBox);
        lfoDivAtt   = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + "Div",   lfoDivSlider);
        lfoHzAtt    = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + "Hz",    lfoHzSlider);
        lfoPhaseAtt = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + "Phase", lfoPhaseSlider);
    }
    custRangeAtt = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + "Range", custRangeFader);
    modOnAtt     = std::make_unique<ButtonAttachment> (processorRef.apvts, pfx + "On",    modOnT);
}

// Right-click menu on any fader: route one of the 6 modulators to that parameter
// (it rides on the knob's own value; set its Range in the Custom tab).
void FractalAdditiveAudioProcessorEditor::showEnvMapMenu (const juce::String& paramID)
{
    if (processorRef.apvts.getParameter (paramID) == nullptr) return;
    if (paramID.startsWith ("mod") && paramID.endsWith ("On")) return; // see modOnT
    static const char* names[6] = { "Env 1", "Env 2", "Env 3", "LFO 1", "LFO 2", "LFO 3" };
    juce::PopupMenu m;
    juce::PopupMenu mapTo;
    for (int i = 0; i < 6; ++i)
    {
        // A modulator can drive ANOTHER modulator's parameters, never its own.
        if (processorRef.getModEngine().isMappedTo (paramID, i)
            || paramID.startsWith (ModEngine::ownPrefix (i)))
            continue;
        const bool full = processorRef.getModEngine().isFull (i);
        mapTo.addItem (100 + i, juce::String (names[i]) + (full ? " (8/8 full)" : ""), ! full);
    }
    m.addSubMenu (tr ("Map to", "Assigna a"), mapTo);
    for (int i = 0; i < 6; ++i)
        if (processorRef.getModEngine().isMappedTo (paramID, i))
            m.addItem (200 + i, tr ("Unmap from ", "Desassigna de ") + names[i]);
    m.showMenuAsync (juce::PopupMenu::Options(), [this, paramID] (int r)
    {
        if (r >= 200)      processorRef.getModEngine().removeTarget (r - 200, paramID);
        else if (r >= 100)
        {
            processorRef.getModEngine().addTarget (r - 100, paramID);
            envTab = 3; modSel = r - 100;
            modSelBox.setSelectedId (modSel + 1, juce::dontSendNotification);
            attachCustomTab();
            resized(); repaint();
        }
        refocus();
    });
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
    // Choice params are modulable too (the raw write snaps to a valid index), so
    // every combo gets the same right-click routing menu as the faders.
    addMapListener (b, paramID, paramID.startsWith ("modE1") || paramID.startsWith ("modL1"));
}

void FractalAdditiveAudioProcessorEditor::layoutFaders (juce::GroupComponent& g, juce::Rectangle<int> area, juce::Colour,
                                                        std::initializer_list<juce::Slider*> faders,
                                                        std::initializer_list<juce::Label*> labels)
{
    g.setBounds (area.reduced (3));
    auto in = area.reduced (3).reduced (5, 4); in.removeFromTop (20); // clear the title band
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

    auto titleArea = getLocalBounds().removeFromTop (78); // a little more air under the title

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
        // Anchored near the TOP of the title band, so the extra height becomes
        // breathing room between the title and the UI below it.
        const float cy = (float) titleArea.getY() + 8.0f + side * 0.5f;
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
        constexpr float titleH = 32.0f; // matches drawGroupComponentOutline
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
        const float dimF = processorRef.timbreReplacesEngine() ? 0.4f : 1.0f;
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
        // Sub-labels sit LEFT of their control (vertically centred), not above it.
        auto subLeft = [&] (const juce::Rectangle<int>& c, const juce::String& s)
        {
            g.setColour (cEssentials);
            g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            // Same band resized() reserves for the label (6 px in + 78 wide, minus a
            // 2 px gap to the control) so long labels ("REFERENCIA") don't clip.
            g.drawText (s, c.reduced (6, 0).withWidth (76), juce::Justification::centredLeft, false);
        };
        subLeft (ecPhase, tr ("PHASE", "FASE"));
        subLeft (ecGain,  tr ("GAIN", "GUANY"));
        subLeft (ecRef,   tr ("REFERENCE", "REFER\xc3\x88NCIA"));
        subLabel (ecOct,  cEssentials, tr ("OCTAVE", "OCTAVA"), 1.0f);
    }

    // TUNING box: a plain macro-style box now (the result lists moved INTO the
    // Spectrum-to-Tuning column, so the old inverted-L is gone) - base aligned with
    // the Envelopes and Custom Engine boxes beside it.
    if (! lFootBounds.isEmpty())
        macroFrameTitle (lFootBounds, cTuning, tr ("TUNING", "AFINACI\xc3\x93"), 1.0f);

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
    auto titleBar = full.removeFromTop (78); // matches paint()'s titleArea
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
        auto kb = area.removeFromBottom (132).reduced (2, 5); // keys (~78) + top info strip (44: deltas + dissonance line)
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
        auto r = b.reduced (3).reduced (5, 4); r.removeFromTop (19);
        if (lbl) lbl->setBounds (r.removeFromTop (13));
        s.setBounds (r);
    };
    auto oneCombo = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Component& c)
    {
        grp.setBounds (b.reduced (3));
        auto r = b.reduced (3).reduced (6, 4); r.removeFromTop (20);
        compact (c, r);
    };

    //== Right column: ONE full-height Spectrum-to-Tuning box - mode controls on top,
    //   its result list (chords / dyad degrees / segment picker) INSIDE it below.
    //   The Tuning box is now a plain row-2 box (no more inverted L).
    {
        auto sp = area.removeFromRight (208);
        area.removeFromRight (6); // uniform gutter to Essentials/Filter

        const int  mode  = specModeBox.getSelectedId();
        const bool chord = mode == 1; // Chords based on beating
        const bool dy    = mode == 2; // Dyads based on beating
        const bool seg   = mode == 3; // From spectrum's segment

        gSpecScale.setBounds (sp.reduced (3, 2)); // same top/bottom lines as rows 1-2
        auto si = sp.reduced (3).reduced (8, 5); si.removeFromTop (20);
        specModeBox.setBounds (si.removeFromTop (34));     // two lines: full mode names fit
        si.removeFromTop (4);

        // Mode-gated visibility.
        juce::Component* sethC[]  = { &dissBtn, &dissCountBox, &lblSteps, &periodEdit, &dissModeBox, &dissSaveBtn, &lblPeriod,
                                      &dissMinEdit, &dissMinLbl };
        juce::Component* segC[]   = { &segView, &segPeriodEdit, &segTopToggle, &segPeriodLbl, &segHelpLbl };
        juce::Component* chordC[] = { &chordNotesBox, &chordCountBox, &chordMinMaxBox, &chordNotesLbl,
                                      &chordPerLbl, &chordProgLbl, &chordPeriodEdit, &chordGoBtn,
                                      &chordMinEdit, &chordMinLbl, &chordCountLbl };
        for (auto* c : sethC)  c->setVisible (dy);
        for (auto* c : segC)   c->setVisible (seg);
        for (auto* c : chordC) c->setVisible (chord);

        const bool fromChord = processorRef.getGeneratedSclName().startsWith ("chord-");
        chordViewBack.setVisible (chord && fromChord);
        chordViewFwd.setVisible  (chord && fromChord);

        if (dy)
        {
            auto r1 = si.removeFromTop (24);   // Transmute | Save
            dissSaveBtn.setBounds (r1.removeFromRight (88).reduced (0, 1));
            r1.removeFromRight (4);
            dissBtn.setBounds (r1.reduced (1, 0));
            si.removeFromTop (4);
            auto r2 = si.removeFromTop (24);   // Min/Max | Steps (below Transmute)
            compact (dissModeBox, r2.removeFromLeft (78));
            r2.removeFromLeft (6);
            lblSteps.setJustificationType (juce::Justification::centredLeft);
            lblSteps.setBounds (r2.removeFromLeft (40));
            compact (dissCountBox, r2.removeFromLeft (52));
            si.removeFromTop (4);
            auto r3 = si.removeFromTop (24);   // Period
            lblPeriod.setJustificationType (juce::Justification::centredLeft);
            lblPeriod.setBounds (r3.removeFromLeft (44));
            periodEdit.setBounds (r3.removeFromLeft (40).reduced (0, 1));
            si.removeFromTop (4);
            auto r4 = si.removeFromTop (22);   // smallest interval
            dissMinLbl.setJustificationType (juce::Justification::centredLeft);
            dissMinLbl.setBounds (r4.removeFromLeft (112));
            dissMinEdit.setBounds (r4.removeFromLeft (48).reduced (0, 1));
        }
        else if (seg)
        {
            auto pr = si.removeFromTop (22);
            segPeriodLbl.setJustificationType (juce::Justification::centredLeft);
            segPeriodLbl.setBounds (pr.removeFromLeft (48));
            segPeriodEdit.setBounds (pr.removeFromLeft (40).reduced (0, 1));
            pr.removeFromLeft (5);
            segTopToggle.setBounds (pr);          // "= top partial"
            si.removeFromTop (2);
            segHelpLbl.setBounds (si.removeFromTop (26));
        }
        else // chord search: tidy stacked rows, each thing labelled
        {
            auto r1 = si.removeFromTop (24);
            chordNotesLbl.setJustificationType (juce::Justification::centredLeft);
            chordNotesLbl.setBounds (r1.removeFromLeft (44));
            compact (chordNotesBox, r1.removeFromLeft (48));
            r1.removeFromLeft (6);
            compact (chordMinMaxBox, r1);
            si.removeFromTop (4);
            auto r2 = si.removeFromTop (24);
            chordPerLbl.setJustificationType (juce::Justification::centredLeft);
            chordPerLbl.setBounds (r2.removeFromLeft (44));
            chordPeriodEdit.setBounds (r2.removeFromLeft (44).reduced (0, 1));
            r2.removeFromLeft (6);
            chordCountLbl.setJustificationType (juce::Justification::centredLeft);
            chordCountLbl.setBounds (r2.removeFromLeft (48)); // "Chords"
            compact (chordCountBox, r2);
            si.removeFromTop (4);
            auto r3 = si.removeFromTop (22);
            chordMinLbl.setJustificationType (juce::Justification::centredLeft);
            chordMinLbl.setBounds (r3.removeFromLeft (112)); // "smallest interval (c)"
            chordMinEdit.setBounds (r3.removeFromLeft (48).reduced (0, 1));
            si.removeFromTop (4);
            auto r4 = si.removeFromTop (24);
            chordGoBtn.setBounds (r4.reduced (1, 0)); // Transmute
            if (fromChord) // labelled view switch: all results vs the loaded chord's degrees
            {
                si.removeFromTop (3);
                auto r5 = si.removeFromTop (22);
                chordViewBack.setColour (juce::TextButton::buttonColourId,
                    ! chordDegView ? cSpecScale.withAlpha (0.85f) : juce::Colour (0xff44445e));
                chordViewFwd.setColour (juce::TextButton::buttonColourId,
                    chordDegView ? cSpecScale.withAlpha (0.85f) : juce::Colour (0xff44445e));
                chordViewBack.setBounds (r5.removeFromLeft (r5.getWidth() / 2).reduced (1, 0));
                chordViewFwd.setBounds  (r5.reduced (1, 0));
            }
            si.removeFromTop (2);
            chordProgLbl.setBounds (si.removeFromTop (14)); // search progress only
        }

        // The result list fills the REST of this same box.
        si.removeFromTop (4);
        const bool showDeg = (dy) || (chord && fromChord && chordDegView);
        const int  nadSel  = juce::jmax (1, nadBox.getSelectedId());
        nadBox.setVisible (showDeg);
        dissHeader.setVisible (showDeg && nadSel == 1);
        dissView.setVisible (showDeg && nadSel == 1);
        nadView.setVisible (showDeg && nadSel > 1);
        chordView.setVisible (chord && ! showDeg);
        if (seg)
        {
            segView.setBounds (si);
            segList.numRows = processorRef.getTemplatePartialCount();
            const int sh  = segList.contentHeight();
            const bool ss = sh > si.getHeight();
            segList.setSize (ss ? si.getWidth() - 10 : si.getWidth(), juce::jmax (si.getHeight(), sh));
        }
        else if (showDeg)
        {
            // n-ad selector (items follow the scale's degree count, live).
            const int D = juce::jmax (1, processorRef.getScaleDegrees());
            if (D != lastNadD)
            {
                lastNadD = D;
                const int keepSel = nadBox.getSelectedId();
                nadBox.clear (juce::dontSendNotification);
                nadBox.addItem (tr ("Degrees (vs 0)", "Graus (vs 0)"), 1);
                for (int n = 2; n <= juce::jmin (D, 12); ++n)
                {
                    juce::String name;
                    switch (n)
                    {
                        case 2:  name = tr ("Dyads",   "D\xc3\xad""ades");  break;
                        case 3:  name = tr ("Triads",  "Tr\xc3\xad""ades"); break;
                        case 4:  name = tr ("Tetrads", "T\xc3\xa8trades");  break;
                        case 5:  name = tr ("Pentads", "P\xc3\xa8ntades");  break;
                        default: name = juce::String (n) + tr ("-note chords", " notes"); break;
                    }
                    nadBox.addItem (name, n);
                }
                nadBox.setSelectedId (keepSel > 0 && keepSel <= juce::jmin (D, 12) ? keepSel : 1,
                                      juce::dontSendNotification);
            }
            nadBox.setBounds (si.removeFromTop (22));
            si.removeFromTop (3);
            if (nadSel == 1)
            {
                dissHeader.splitX = 92;
                dissHeader.setBounds (si.removeFromTop (24));
                dissView.setBounds (si);
                const int ch     = degreeList.contentHeight();
                const bool scroll = ch > si.getHeight();
                degreeList.setSize (scroll ? si.getWidth() - 10 : si.getWidth(),
                                    juce::jmax (si.getHeight(), ch));
            }
            else
            {
                nadView.setBounds (si);
                nadList.degreesPerPeriod = D;
                const int nh    = nadList.contentHeight();
                const bool scrl = nh > si.getHeight();
                nadList.setSize (scrl ? si.getWidth() - 10 : si.getWidth(),
                                 juce::jmax (si.getHeight(), nh));
            }
        }
        else if (chord)
        {
            chordView.setBounds (si);
            const int chh    = chordList.contentHeight();
            const int chw    = chordList.contentWidth();
            const bool scrlV = chh > si.getHeight();
            const bool scrlH = chw > si.getWidth();
            // Wide chords (many notes) scroll SIDEWAYS - nothing is left unreachable
            // off the right edge any more.
            chordList.setSize (juce::jmax (chw, si.getWidth() - (scrlV ? 10 : 0)),
                               juce::jmax (si.getHeight() - (scrlH ? 10 : 0), chh));
        }
    }

    const int rowH = (area.getHeight() - 4) / 2;

    //== Row 1: Base | (Harmonics over Prime) | Slope | Range | Fractal | (Phase over Gain)
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int W = row.getWidth();

        // "Harmonic Engine" = ONE framed box (gHarmonic) holding Base | Harmonics /
        // Prime | Slope, separated by SHARED divider lines + small inline uppercase
        // labels drawn in paint() (from the hc* cell rects stored here).
        const int macroW = W * 60 / 100;
        // Every macro box uses the SAME 3 px horizontal inset as the group-component
        // boxes, so all frames in both rows share their left and right lines.
        harmonicEngineBounds = row.removeFromLeft (macroW).reduced (3, 0);
        auto macro = harmonicEngineBounds.reduced (5, 4);
        macro.removeFromTop (20); // clear the "HARMONIC ENGINE" title band
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
            harmonicsFader.setBounds (hi.withSizeKeepingCentre (hi.getWidth(), juce::jmin (24, hi.getHeight())));

            const bool wl = processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
            primeLimitFader.setVisible (! wl); lblPrimeLimit.setVisible (! wl);
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
                primeLimitFader.setBounds (pi.removeFromLeft (juce::jmin (150, pi.getWidth())).withSizeKeepingCentre (juce::jmin (150, 150), 22));
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
            essentialsBounds = row.reduced (3, 0);
            auto e = essentialsBounds.reduced (5, 4);
            e.removeFromTop (20); // clear the "ESSENTIALS" title band
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
            // PHASE / GAIN / REFERENCE: the control sits to the RIGHT of its inline
            // sub-label (drawn in paint at the cell's left), slightly narrower.
            { auto c = ecPhase.reduced (6, 1); c.removeFromLeft (78);
              compact (phaseBox, c.removeFromLeft (juce::jmin (112, c.getWidth()))); }
            { auto c = ecGain.reduced (6, 1);  c.removeFromLeft (78);
              gainFader.setBounds (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()))); }
            { auto c = ecRef.reduced (6, 1);   c.removeFromLeft (78);
              auto rr = c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()))
                          .withWidth (juce::jmin (c.getWidth(), 158));
              lblRefA.setJustificationType (juce::Justification::centredLeft);
              lblRefA.setBounds (rr.removeFromLeft (34));
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
        // Takes ALL the remaining height (not another rowH slice) so its boxes end
        // on exactly the same line as the full-height right column: the frames are
        // drawn from the box rect, so every box in this row must share one rect.
        auto row = area.reduced (0, 2);
        const int W = row.getWidth();

        // Custom Engine (leftmost now): load .scl OR type intervals + Reps.
        {
            auto sb = row.removeFromLeft (W * 14 / 100);
            gTimbre.setBounds (sb.reduced (3, 0)); // gutter sideways only: tops/bottoms
                                                   // line up with every other box in the row
            auto ti = sb.reduced (3).reduced (8, 5); ti.removeFromTop (20);
            auto brow = ti.removeFromTop (24);
            const bool offVis = processorRef.timbreLoaded();
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
            ti.removeFromTop (3);
            temperToggle.setBounds (ti.removeFromTop (20)); // temper harmonics to this .scl
        }

        // ONE shared envelope box with three colour-chip tabs (Volume / Pitch /
        // Filter): the selected chip is bright and recolours the frame; the others
        // stay dimmed but readable. Minimal fixed width - the faders adapt - and
        // everything saved goes to the Tuning box on the right.
        {
            auto eb = row.removeFromLeft (juce::jmin (262, W * 24 / 100));
            const juce::Colour tabCols[4] = { cEnv, cPitch, cFilter, juce::Colour (0xff9a7bff) };
            gAmpEnv.setColour (juce::GroupComponent::outlineColourId, tabCols[envTab]);
            gAmpEnv.setColour (juce::GroupComponent::textColourId,    tabCols[envTab]);
            gAmpEnv.setBounds (eb.reduced (3, 0)); // title = "Envelopes" (set in applyLanguage)
            gPitchEnv.setVisible (false);
            gFilter.setVisible (false);

            auto in = eb.reduced (3).reduced (5, 4);
            in.removeFromTop (20);                // clear the box title - chips sit BELOW it
            auto chips = in.removeFromTop (19);
            const int cw4 = chips.getWidth() / 4;
            for (int k = 0; k < 4; ++k)
            {
                envTabB[k].setColour (juce::TextButton::buttonColourId,
                                      envTab == k ? tabCols[k] : tabCols[k].withAlpha (0.20f));
                envTabB[k].setColour (juce::TextButton::textColourOffId,
                                      envTab == k ? juce::Colours::black : juce::Colour (0xffb9c0d0));
                envTabB[k].setBounds (chips.removeFromLeft (k == 3 ? chips.getWidth() : cw4).reduced (1, 0));
            }
            in.removeFromTop (4);

            juce::Slider* setV[] = { &ampAdsr[0], &ampAdsr[1], &ampAdsr[2], &ampAdsr[3] };
            juce::Label*  labV[] = { &ampAdsrLbl[0], &ampAdsrLbl[1], &ampAdsrLbl[2], &ampAdsrLbl[3] };
            juce::Slider* setP[] = { &pEnvAmtFader, &pitchAdsr[0], &pitchAdsr[1], &pitchAdsr[2], &pitchAdsr[3] };
            juce::Label*  labP[] = { &lblPitchAmt, &pitchAdsrLbl[0], &pitchAdsrLbl[1], &pitchAdsrLbl[2], &pitchAdsrLbl[3] };
            juce::Slider* setF[] = { &cutoffFader, &resFader, &fEnvAmtFader, &filtAdsr[0], &filtAdsr[1], &filtAdsr[2], &filtAdsr[3] };
            juce::Label*  labF[] = { &lblCutoff, &lblRes, &lblEnvAmt, &filtAdsrLbl[0], &filtAdsrLbl[1], &filtAdsrLbl[2], &filtAdsrLbl[3] };
            juce::Slider** sets[] = { setV, setP, setF };
            juce::Label**  labs[] = { labV, labP, labF };
            const int counts[] = { 4, 5, 7 };
            for (int t = 0; t < 3; ++t)
            {
                const bool on = t == envTab;
                if (on)
                {
                    const int n  = counts[t];
                    const int cw = in.getWidth() / juce::jmax (1, n);
                    auto area2 = in;
                    for (int i = 0; i < n; ++i)
                    {
                        auto c = area2.removeFromLeft (cw);
                        const bool inThumb = sets[t][i]->getName().isNotEmpty();
                        sets[t][i]->setVisible (true);
                        labs[t][i]->setVisible (! inThumb);
                        if (! inThumb) labs[t][i]->setBounds (c.removeFromTop (13));
                        sets[t][i]->setBounds (c);
                    }
                }
                else
                    for (int i = 0; i < counts[t]; ++i)
                    { sets[t][i]->setVisible (false); labs[t][i]->setVisible (false); }
            }

            // CUSTOM tab: modulator selector + On + Unmap-all row, then the selected
            // modulator's own controls (envelope ADSR+Range, or the LFO set).
            const bool cust  = envTab == 3;
            const bool isEnv = modSel < 3;
            juce::Component* selRowC[] = { &modSelBox, &modOnT, &modUnmapBtn };
            for (auto* c : selRowC) c->setVisible (cust);
            lfoWaveEditBtn.setVisible (cust && ! isEnv && lfoWaveBox.getSelectedId() == 6);
            for (int i = 0; i < 4; ++i)
            { custAdsr[i].setVisible (cust && isEnv); custAdsrLbl[i].setVisible (false); }
            custRangeFader.setVisible (cust);
            lblCustRange.setVisible (cust);
            juce::Component* lfoC[] = { &lfoWaveBox, &lfoRateModeBox, &lfoPhaseModeBox,
                                        &lfoDivSlider, &lfoHzSlider, &lfoPhaseSlider,
                                        &lblLfoDiv, &lblLfoHz, &lblLfoPhase };
            for (auto* c : lfoC) c->setVisible (cust && ! isEnv);
            if (cust)
            {
                auto selRow = in.removeFromTop (20);
                modSelBox.setBounds (selRow.removeFromLeft (72));
                selRow.removeFromLeft (4);
                modOnT.setBounds (selRow.removeFromLeft (46));
                selRow.removeFromLeft (4);
                modUnmapBtn.setBounds (selRow.removeFromLeft (58));
                selRow.removeFromLeft (4);
                if (lfoWaveEditBtn.isVisible()) lfoWaveEditBtn.setBounds (selRow);
                in.removeFromTop (3);
                if (isEnv)
                {
                    const int cw = in.getWidth() / 5;
                    auto a2 = in;
                    for (int i = 0; i < 4; ++i) custAdsr[i].setBounds (a2.removeFromLeft (cw));
                    lblCustRange.setBounds (a2.removeFromTop (13));
                    custRangeFader.setBounds (a2);
                }
                else
                {
                    auto combos = in.removeFromTop (20);
                    lfoWaveBox.setBounds (combos.removeFromLeft (combos.getWidth() / 2).reduced (1, 0));
                    lfoRateModeBox.setBounds (combos.reduced (1, 0));
                    in.removeFromTop (2);
                    lfoPhaseModeBox.setBounds (in.removeFromTop (20).reduced (1, 0));
                    in.removeFromTop (2);
                    const int cw = in.getWidth() / 4;
                    auto a2 = in;
                    { auto c = a2.removeFromLeft (cw); lblLfoDiv.setBounds (c.removeFromTop (12));   lfoDivSlider.setBounds (c); }
                    { auto c = a2.removeFromLeft (cw); lblLfoHz.setBounds (c.removeFromTop (12));    lfoHzSlider.setBounds (c); }
                    { auto c = a2.removeFromLeft (cw); lblLfoPhase.setBounds (c.removeFromTop (12)); lfoPhaseSlider.setBounds (c); }
                    { auto c = a2;                     lblCustRange.setBounds (c.removeFromTop (12)); custRangeFader.setBounds (c); }
                }
            }
        }

        // Tuning = the FOOT of the inverted-L box (the dissonance BAR is above-right);
        // mode + the loaded .scl name, the PERIOD (red), load/reset, recent, bend, bank.
        {
            lFootBounds = row.reduced (3, 0); // frame painted in paint()
            auto ti = row.reduced (3).reduced (8, 4); ti.removeFromTop (20); // clear the TUNING title band
            auto mrow = ti.removeFromTop (24);
            compact (tuningBox, mrow.removeFromLeft (mrow.getWidth() * 50 / 100));
            mrow.removeFromLeft (5);
            tuningNameLabel.setBounds (mrow); // shows the loaded scale's file name
            // Period of the active scale, in red, always visible.
            ti.removeFromTop (2);
            tuningPeriodLbl.setBounds (ti.removeFromTop (17));
            // Load .scl only exists in Scala mode; Reset appears whenever there is
            // something to reset (any non-12ED2 mode or a loaded scale). Load / Reset /
            // Save share ONE row: stacking them overflowed the box in Scala mode and
            // squeezed the tuning-bank controls to zero height (invisible).
            const int  tmode    = processorRef.getTuningMode();
            const bool loadVis  = tmode == 2;
            const bool resetVis = tmode != 0 || processorRef.tuningHasScala();
            const bool saveVis  = tmode != 0;
            loadSclButton.setVisible (loadVis);
            clearSclButton.setVisible (resetVis);
            saveTuningBtn.setVisible (saveVis);
            if (loadVis || resetVis || saveVis)
            {
                ti.removeFromTop (3);
                auto brow = ti.removeFromTop (23);
                const int nBtn = (loadVis ? 1 : 0) + (resetVis ? 1 : 0) + (saveVis ? 1 : 0);
                const int bw   = brow.getWidth() / juce::jmax (1, nBtn);
                int left = nBtn;
                const auto place = [&] (juce::TextButton& b, bool vis)
                {
                    if (! vis) return;
                    b.setBounds ((--left > 0 ? brow.removeFromLeft (bw) : brow).reduced (2, 0));
                };
                place (loadSclButton,  loadVis);
                place (clearSclButton, resetVis);
                place (saveTuningBtn,  saveVis);
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
            tuningBankClearBtn.setVisible (sclMode);
            if (sclMode)
            {
                ti.removeFromTop (3);
                recentLabel.setBounds (ti.removeFromTop (13));
                auto rrow = ti.removeFromTop (23);
                const int rw = rrow.getWidth() / 3;
                for (int i = 0; i < 3; ++i) recentBtn[i].setBounds (rrow.removeFromLeft (rw).reduced (2, 1));
                ti.removeFromTop (3);
                // Bank row: the label rides on the LEFT of the row (a separate label
                // row no longer fits) - [BANK][combo (flex)][+ .scl][X remove].
                auto bkrow = ti.removeFromTop (juce::jmax (0, juce::jmin (24, ti.getHeight())));
                lblTuningBank.setJustificationType (juce::Justification::centredLeft);
                lblTuningBank.setBounds (bkrow.removeFromLeft (38));
                tuningBankClearBtn.setBounds (bkrow.removeFromRight (24).reduced (1, 1));
                bkrow.removeFromRight (3);
                tuningBankLoadBtn.setBounds (bkrow.removeFromRight (46).reduced (1, 0));
                bkrow.removeFromRight (4);
                compact (tuningBankBox, bkrow);
                // Honest failure instead of zero-height ghosts: if the box ran out of
                // room, hide what did not fit rather than laying it out invisibly.
                const bool fits = bkrow.getHeight() >= 14;
                lblTuningBank.setVisible (fits); tuningBankBox.setVisible (fits);
                tuningBankLoadBtn.setVisible (fits); tuningBankClearBtn.setVisible (fits);
            }
        }
    }

    // Floating "silent settings" explainer, centred over everything.
    warningOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (580, 200));
    warningOverlay.toFront (false);
    noticeOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (560, 220));
    rhythmScrim.setBounds (getLocalBounds());
    rhythmPanel.setBounds (getLocalBounds().withSizeKeepingCentre (820, 640));
    waveEditor.setBounds (getLocalBounds().withSizeKeepingCentre (640, 360));
    rhythmScrim.toFront (false);
    rhythmPanel.toFront (false);
}

void FractalAdditiveAudioProcessorEditor::mouseDown (const juce::MouseEvent&) { grabKeyboardFocus(); }

// After switching to another plugin window (or app) and back, nothing in our
// hierarchy holds keyboard focus, so the computer keyboard can't play (and with no
// driving note the Spectrum-to-Rhythm pulses stop too). Reclaim focus when the host
// re-activates our window OR the mouse re-enters the plugin - unless the user is in a
// text field, whose typing must not be hijacked.
void FractalAdditiveAudioProcessorEditor::reclaimFocusUnlessTyping()
{
    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr) return;
    if (! hasKeyboardFocus (true))
        grabKeyboardFocus();
}
void FractalAdditiveAudioProcessorEditor::mouseEnter (const juce::MouseEvent&) { reclaimFocusUnlessTyping(); }
void FractalAdditiveAudioProcessorEditor::broughtToFront()
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FractalAdditiveAudioProcessorEditor> (this)]
    { if (safe != nullptr) safe->reclaimFocusUnlessTyping(); });
}

bool FractalAdditiveAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    const int  period = juce::jmax (1, processorRef.getScaleDegrees());
    // Ctrl-Z / Ctrl-Y: undo / redo any parameter change (also reachable in the DAW
    // because the editor swallows them while focused).
    if (key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier, 0)) { processorRef.undoManager.undo(); return true; }
    if (key == juce::KeyPress ('y', juce::ModifierKeys::ctrlModifier, 0)) { processorRef.undoManager.redo(); return true; }
    // Left/right arrows scroll the colour keyboard's visible window by one period.
    if (key == juce::KeyPress::leftKey)  { keyboard.scrollView (-period); return true; }
    if (key == juce::KeyPress::rightKey) { keyboard.scrollView ( period); return true; }
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

    // Undo granularity: group parameter edits into ~1-second transactions (a slider
    // drag undoes as one step, not hundreds of micro-moves). Same tick keeps the
    // tuning-bank combo in sync with auto-added entries.
    if (++undoTxnTick >= 30)
    {
        undoTxnTick = 0;
        processorRef.undoManager.beginNewTransaction();
        int filled = 0;
        for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i)
            if (processorRef.getTuningBankName (i).isNotEmpty()) ++filled;
        if (filled + 1 != tuningBankBox.getNumItems())
            refreshTuningBank();
    }

    // n-AD list: recompute when the selection or the scale/timbre changes; publish
    // results when the worker finishes (values coloured green -> red like the list).
    if (nadBox.isVisible() && nadBox.getSelectedId() > 1)
    {
        if (lastScaleSig != lastNadSig || nadBox.getSelectedId() != lastNadSel)
        {
            lastNadSig = lastScaleSig;
            lastNadSel = nadBox.getSelectedId();
            processorRef.startNadCompute (lastNadSel);
        }
        if (processorRef.getNadGeneration() != lastNadGen)
        {
            lastNadGen = processorRef.getNadGeneration();
            const auto res = processorRef.getNadResults();
            double lo = 1.0e30, hi = -1.0e30;
            for (const auto& r : res) { lo = juce::jmin (lo, r.value); hi = juce::jmax (hi, r.value); }
            nadList.rows.clear();
            for (const auto& r : res)
            {
                NadList::Row row;
                row.degs  = r.degs;
                row.value = juce::String (r.value, 2);
                const double t = juce::jlimit (0.0, 1.0, (r.value - lo) / juce::jmax (1.0e-12, hi - lo));
                const juce::Colour green (0xff3ec659), yellow (0xffe8d44d), red (0xffe0453f);
                row.valCol = t < 0.5 ? green.interpolatedWith (yellow, (float) (t * 2.0))
                                     : yellow.interpolatedWith (red, (float) ((t - 0.5) * 2.0));
                nadList.rows.push_back (std::move (row));
            }
            resized();
            nadList.repaint();
        }
        // Honest empty states: computing, over budget, or partial degree coverage.
        {
            juce::String txt;
            if (processorRef.isNadBusy())
                txt = tr ("computing...", "calculant...");
            else if (nadList.rows.empty() && processorRef.nadWasOverBudget())
                txt = tr ("Too many combinations for this scale - pick fewer notes.",
                          "Massa combinacions per a aquesta escala - tria menys notes.");
            else if (processorRef.getNadUsedDegrees() < processorRef.getNadTotalDegrees())
                txt = tr ("first ", "nom\xc3\xa9s els primers ") + juce::String (processorRef.getNadUsedDegrees())
                    + tr (" of ", " de ") + juce::String (processorRef.getNadTotalDegrees())
                    + tr (" degrees analysed", " graus analitzats");
            if (txt != nadList.emptyText) { nadList.emptyText = txt; resized(); nadList.repaint(); }
        }
    }

    // Keyboard hovers park their floating window ABOVE the keyboard (never covering
    // keys or the numbers over them) - keep the LnF's notion of that area current.
    // NOTE: the TooltipWindow is parented to this editor, so getTooltipBounds works
    // in EDITOR-LOCAL coordinates - feed it local rects/points, not screen ones.
    if (auto* gl = dynamic_cast<GalacticLnF*> (lnf.get()))
    {
        gl->keyboardScreenArea = keyboard.getBounds();
        gl->hoverAnchor = keyboard.isHoverActive()
                              ? getLocalPoint (nullptr, keyboard.getHoverAnchorScreen())
                              : juce::Point<int>();
    }

    // Chord preview: send the scheduled MIDI events whose time has come.
    if (! previewQueue.empty() && processorRef.getSampleRate() > 0.0)
    {
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        const double nowS = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (size_t i = 0; i < previewQueue.size();)
        {
            if (previewQueue[i].dueMs <= now)
            {
                auto m = previewQueue[i].msg;
                m.setTimeStamp (nowS);
                processorRef.previewCollector.addMessageToQueue (m);
                previewQueue.erase (previewQueue.begin() + (long) i);
            }
            else ++i;
        }
        if (previewQueue.empty())
            previewNotes.clear(); // the preview finished naturally - nothing left to stop
    }

    // Chord search: progress read-out while running; rebuild the list on completion.
    if (specModeBox.getSelectedId() == 1)
    {
        chordProgLbl.setText (processorRef.isChordSearchRunning()
                                  ? tr ("Searching... ", "Cercant... ")
                                    + juce::String ((int) std::lround (processorRef.getChordSearchProgress() * 100.0f)) + "%"
                                  : juce::String(),
                              juce::dontSendNotification);

        if (processorRef.getChordResultsGeneration() != lastChordGen)
        {
            lastChordGen = processorRef.getChordResultsGeneration();
            const auto results = processorRef.getChordResults();
            chordList.rows.clear();
            for (const auto& r : results)
            {
                ChordList::Row row;
                row.value = r.value;
                for (const double a : r.alphas)
                {
                    // Source-honest labels: these are SEARCHED values - show the exact
                    // ratio only when float-exact (a refined minimum can land on one),
                    // else cents. Root is always "1/1".
                    long long p = 1, q = 1;
                    dissonance::bestFraction (a, p, q, 48);
                    const double cents = 1200.0 * std::log2 (a);
                    if (p * q <= 150 && std::abs (cents - 1200.0 * std::log2 ((double) p / (double) q)) < 0.3)
                        row.notes.push_back (juce::String (p) + "/" + juce::String (q));
                    else
                        row.notes.push_back (juce::String ((int) std::lround (cents)) + juce::String::fromUTF8 ("¢"));
                }
                chordList.rows.push_back (std::move (row));
            }
            const int ch = chordList.contentHeight();
            const int cw = chordList.contentWidth();
            const bool scroll  = ch > chordView.getHeight() && chordView.getHeight() > 0;
            const bool scrollH = cw > chordView.getWidth();
            chordList.setSize (juce::jmax (cw, juce::jmax (60, chordView.getWidth()) - (scroll ? 10 : 0)),
                               juce::jmax (chordView.getHeight() - (scrollH ? 10 : 0), ch));
            chordList.repaint();
        }
    }

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
        // Extend the range to include any note CURRENTLY SOUNDING outside it, so the
        // scroll arrows can always reach what you are playing (setRange preserves the
        // view, so this never jumps the window around).
        {
            int heldLo = 128, heldHi = -1;
            for (int n = 0; n < 128; ++n)
                if (processorRef.keyboardState.isNoteOnForChannels (0xffff, n))
                { heldLo = juce::jmin (heldLo, n); heldHi = juce::jmax (heldHi, n); }
            if (heldHi >= 0)
            {
                const int hi = juce::jmax (kbLo + kbNum, heldHi + 1);
                kbLo  = juce::jmin (kbLo, heldLo);
                kbNum = hi - kbLo;
            }
            if (kbLo != lastKbLo || kbNum != lastKbNum)
            {
                lastKbLo = kbLo; lastKbNum = kbNum;
                keyboard.setRange (kbLo, kbNum);
            }
            // Auto-follow: the view shifts just enough to always SHOW what is being
            // played (minimal movement, no jump when it is already on screen).
            if (heldHi >= 0)
                keyboard.ensureVisible (heldLo, heldHi);
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
    const bool offVis   = processorRef.timbreLoaded();
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
        auto per = degreeLabel (processorRef, D);
        if (per.endsWith ("/1")) per = per.dropLastCharacters (2); // "2/1" reads better as "2"
        tuningPeriodLbl.setText (tr ("Period  ", "Per\xc3\xado" "de  ") + per, juce::dontSendNotification);
    }

    const bool timbre = processorRef.timbreReplacesEngine();
    if (timbre != lastTimbreActive)
    {
        // The harmonic-engine controls are DIMMED while a custom engine drives the
        // timbre - they are never reset. (They used to be forced back to their
        // defaults here, which silently destroyed the user's settings the moment a
        // .scl was loaded, and made the sound depend on the editor being open.)
        applyTimbreEnablement (timbre);
        lastTimbreActive = timbre;
        clearTimbreButton.setButtonText (processorRef.isTimbreActive() ? tr ("On", "Actiu") : tr ("Off", "Apagat"));
    }
}
