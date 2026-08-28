#include "PluginEditor.h"
#include "BinaryData.h"
#include "FloatingWindowOwner.h" // keeps the floating windows above the editor (Win32 ownership)
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
    //   Row 1: SPECTRUM = [Harmonic Engine(blue) | Custom Engine(pink) | Both engines(mint)]
    //   Row 2: Essentials(green) | Modulators(tab colour) | Tuning(purple)
    //   Right: Spectrum-to-Scale (full height, orange)
    // The SPECTRUM container itself is deliberately NEUTRAL steel: it holds three
    // coloured boxes and must not compete with them for the eye.
    // Colours are placed around the hue wheel so every ADJACENT pair (horizontal
    // AND vertical) is a clearly different colour - no two near-identical neighbours.
    const juce::Colour cBase      { 0xff5cb8ff }; // Harmonic Engine is one blue
    const juce::Colour cHarmonic  { 0xff5cb8ff }; // blue   210
    const juce::Colour cWhitelist { 0xff5ff2b0 };
    const juce::Colour cSpectrum  { 0xffc3ccdf }; // steel  - the neutral SPECTRUM container
    const juce::Colour cSlope     { 0xff8ef0c4 }; // mint   - Slope/Fractal/Morph/Range (BOTH engines)
    const juce::Colour cOverride  { 0xffffb020 }; // amber  - "this is being overridden" (as FROZEN)
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

    // Every macro box - the GroupComponent ones and the ones the editor paints by hand -
    // draws its border as a 2.6 px stroke whose CENTRE line sits this far inside the
    // box's own rectangle. Named once and used by all of them (drawGroupComponentOutline,
    // macroFrame, and layoutProbe's wall read-out) because "these two boxes share a wall"
    // is a claim about where those centre lines land, and a test that guessed the number
    // would pass while the two lines sat side by side.
    constexpr float kFrameLine = 1.3f;

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
        // The simplest just interval anywhere NEAR it, even when it is too far to be
        // called an approximation - so the hover can always say what the closest
        // thing is instead of showing nothing at all.
        bool      hasNear   = false;  long long np = 1, nq = 1; int nearErr = 0;
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
        long long nearP = 0, nearQ = 1; double nearErr = 0.0, nearSimp = 1.0e18;
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
            // ...and, separately, the simplest ratio within earshot at all (35c), so
            // an interval like 600c can still name its closest just neighbour.
            if (std::abs (err) <= 35.0 && simp <= 200.0 && simp < nearSimp)
            { nearSimp = simp; nearP = p2; nearQ = q2; nearErr = err; }
        }
        if (bestP > 0)
        { d.hasApprox = true; d.ap = bestP; d.aq = bestQ; d.approxErr = (int) std::llround (bestErr); }
        if (nearP > 0)
        { d.hasNear = true; d.np = nearP; d.nq = nearQ; d.nearErr = (int) std::llround (nearErr); }
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
    //
    // `roundTopRight` is false for a panel whose RIGHT EDGE IS A SHARED WALL. SPECTRUM's
    // right border, TUNING's right border and the Spectrum-vs-Tuning column's left border
    // are one drawn line; the column runs the full height and draws that line straight,
    // while the two boxes beside it used to curve their top edge away 9 px before reaching
    // it. The result was two arcs pinching the shared wall at top and middle - it read as
    // a seam, which is the exact opposite of what sharing the wall is for. A shared end
    // gets a SQUARE corner so the wall is one uninterrupted straight line; every corner
    // that faces open space keeps its radius.
    juce::Path topRoundedFrame (juce::Rectangle<float> r, float rad, bool roundTopRight = true)
    {
        rad = juce::jmin (rad, r.getWidth() * 0.5f, r.getHeight() * 0.5f);
        juce::Path p;
        // The TOP-LEFT corner is square: the title chip sits on that corner, and a
        // rounded corner curving away to the right left a visible gap between the
        // chip and the box's left line. The other top corner stays rounded.
        p.startNewSubPath (r.getX(), r.getBottom());
        p.lineTo (r.getX(), r.getY());
        if (roundTopRight)
        {
            p.lineTo (r.getRight() - rad, r.getY());
            p.quadraticTo (r.getRight(), r.getY(), r.getRight(), r.getY() + rad);
        }
        else
        {
            p.lineTo (r.getRight(), r.getY());
        }
        p.lineTo (r.getRight(), r.getBottom());
        p.closeSubPath();
        return p;
    }

    // THE OUTLINE A TEXT EDITOR IS DRAWN WITH: the rect its border is stroked along, the
    // corner radius and the pen width. ONE place, because GalacticLnF strokes exactly
    // this and ratiosInkProbeForTests measures exactly this - a probe that rebuilt the
    // shape from its own copy of the radius could not see a radius paint() had changed.
    //
    // A PILL (radius = half the height) is right for a ONE-LINE field, and wrong for a
    // multi-line text AREA: on the 70 px tall custom-ratios box the same rule gave a
    // 35 px radius, and the arc cut so far into the first and last rows that the digits
    // sat on top of the border - "adapta la caja de custom ratios para que los numeros no
    // toquen su borde". A text area gets the modest corner the macro panels use instead.
    struct EditorOutline { juce::Rectangle<float> rect; float radius, thickness; };
    EditorOutline textEditorOutline (float w, float h, bool multiLine)
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, w, h).reduced (0.9f);
        return { r, multiLine ? 6.0f : h * 0.5f, 1.4f };
    }

    // How far INSIDE its own top-right corner a frame's drawn border passes, walked along
    // the diagonal in half-pixel steps: 0.5 for a square corner (the first sample is
    // already inside), 2.5 for the 9 px rounding - the corner is a quadratic Bezier whose
    // control point IS the corner, so its closest approach is rad/4 on each axis, and the
    // two cases are five steps apart. The measurable form of "the shared
    // wall is one straight line": a rect comparison cannot see it (both frames end on the
    // same x either way), and neither can the wall-line check, because the arc leaves the
    // wall x untouched and simply stops short of the top. Sampled off the very path
    // paint() strokes, so it can never report a rounding the screen has stopped using.
    float topRightCornerInset (const juce::Path& shape, juce::Rectangle<float> frame)
    {
        for (float k = 0.5f; k < 24.0f; k += 0.5f)
            if (shape.contains (frame.getRight() - k, frame.getY() + k))
                return k;
        return 24.0f;
    }

    // The rect a macro box strokes its border around, from the box's own rectangle, and the
    // radius it rounds with. ONE place, because paint() draws from them and layoutProbe()
    // measures from them - a probe that rebuilt the frame from its own copy of the numbers
    // would keep passing after paint() changed them.
    constexpr float kMacroTitleH = 32.0f;   // room ABOVE the frame for the title chip
    constexpr float kMacroRadius = 9.0f;
    juce::Rectangle<float> macroFrameRect (juce::Rectangle<int> boxI)
    {
        const auto box = boxI.toFloat();
        return { box.getX() + kFrameLine, box.getY() + kMacroTitleH * 0.5f,
                 box.getWidth() - 2.0f * kFrameLine,
                 box.getHeight() - kMacroTitleH * 0.5f - kFrameLine };
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

    // The font a title chip is drawn in, and the width the chip ends up: 13px bold,
    // auto-shrunk for a long Catalan title, plus 11px of padding.
    juce::Font titleChipFont (const juce::String& upper, float maxW, float& chipWOut)
    {
        auto measure = [&] (const juce::Font& f)
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (f, upper, 0.0f, 0.0f);
            return ga.getBoundingBox (0, -1, true).getWidth();
        };
        float fontH = 13.0f;
        juce::Font tf (juce::FontOptions (fontH).withStyle ("Bold"));
        float w = measure (tf);
        if (w + 11.0f > maxW && w > 1.0f)
        {
            fontH = juce::jmax (8.0f, fontH * (maxW - 11.0f) / w);
            tf = juce::Font (juce::FontOptions (fontH).withStyle ("Bold"));
            w = measure (tf);
        }
        chipWOut = juce::jmin (maxW, w + 11.0f);
        return tf;
    }
    // (titleChipWidth() lived here: it measured the painted SPECTRUM title so real child
    // buttons could be laid out to its RIGHT, on the panel's top line. The engine chips
    // sit UNDER the title now, like the modulator tabs, so nothing needs that measurement
    // and a helper with no callers is dead weight.)

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

        float chipW = 0.0f;
        const juce::Font tf = titleChipFont (upper, maxW, chipW);
        juce::GlyphArrangement ga;
        ga.addLineOfText (tf, upper, 0.0f, 0.0f);

        // The chip's LEFT edge sits exactly on the frame's left border line (outer
        // edge of the 2.6px stroke), so the box border reads as a continuation of
        // the chip's own left edge. Top stays inside the box (never clipped).
        auto chip = juce::Rectangle<float> (box.getX(), box.getY() + 1.0f, chipW, chipH);

        // Top-LEFT square (it continues the box's left line, which is square too),
        // top-right rounded, bottom corners angular.
        const float rad = juce::jmin (6.0f, chipW * 0.5f);
        juce::Path shape;
        shape.startNewSubPath (chip.getX(), chip.getBottom());
        shape.lineTo (chip.getX(), chip.getY());
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
            // The SAME frame rect and radius the editor's own macro boxes use, from the one
            // place that defines them - these panels sit in the same row and a border half a
            // pixel out of step with its neighbour is visible.
            const auto frame = macroFrameRect (juce::Rectangle<int> (w, h));

            // Interior panel: lighter than the page (0xff0c0c12) and subtly tinted by the
            // section colour. Rounded TOP corners + angular bottom (like the chips). Both
            // group boxes keep their rounded top-right: the Spectrum-vs-Tuning column shares
            // its LEFT wall (that corner is square already, for the title chip) and the
            // Envelopes box shares no wall at all.
            const auto shape = topRoundedFrame (frame, kMacroRadius);
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

            auto area = b.getLocalBounds().reduced (4, 0);
            // A button flagged "fork" gets a real tuning fork drawn at its left edge.
            // There is no tuning-fork character in any font we can rely on - the nearest
            // candidates (Psi, the pitchfork sign) all have THREE prongs, which reads as a
            // trident, not a fork. So it is drawn: two prongs, a U-bend and a stem.
            if (b.getProperties().contains ("fork"))
            {
                const float fh = juce::jmin (13.0f, (float) b.getHeight() - 4.0f);
                auto fr = area.removeFromLeft ((int) std::ceil (fh * 0.62f) + 3).toFloat()
                              .withSizeKeepingCentre (fh * 0.62f, fh);
                g.strokePath (tuningForkPath (fr), juce::PathStrokeType (juce::jmax (1.2f, fh * 0.14f),
                                                                         juce::PathStrokeType::curved,
                                                                         juce::PathStrokeType::rounded));
                area.removeFromLeft (2);
            }
            g.drawFittedText (b.getButtonText(), area, juce::Justification::centred, 2);
        }

        // Two prongs joined by a U-bend, on a stem. Drawn rather than typed for the
        // reason above.
        static juce::Path tuningForkPath (juce::Rectangle<float> r)
        {
            juce::Path p;
            const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
            const float pl = x + w * 0.18f, pr = x + w * 0.82f, cx = x + w * 0.5f;
            const float bend = y + h * 0.52f;
            p.startNewSubPath (pl, y);
            p.lineTo (pl, bend);
            p.quadraticTo (cx, y + h * 0.86f, pr, bend);
            p.lineTo (pr, y);
            p.startNewSubPath (cx, y + h * 0.70f);
            p.lineTo (cx, y + h);
            return p;
        }

        // ADSR-style faders carry their letter INSIDE the drag thumb (the slider's
        // name, e.g. "A"): bigger thumb, letter black/white by the thumb's colour -
        // the same rule as the keyboard keys and the title letters.
        static constexpr float kAdsrThumbR = 20.0f; // named (ADSR) thumb radius
        static constexpr float kThumbR     = 13.0f; // every other fader's thumb radius
        // Never taller than the fader's own row minus a margin, so a thin slider (the
        // rhythm panel's 22 px slot faders) still shows its track either side.
        static float thumbRadiusFor (juce::Slider& s)
        {
            const float r = s.getName().isNotEmpty() ? kAdsrThumbR : kThumbR;
            const bool horiz = s.getSliderStyle() == juce::Slider::LinearHorizontal;
            const float cross = horiz ? (float) s.getHeight() : (float) s.getWidth();
            return cross > 8.0f ? juce::jmin (r, cross * 0.5f - 2.0f) : r;
        }
        int getSliderThumbRadius (juce::Slider& s) override
        {
            // Clamped to half the cross-dimension like V4 does, so a short fader keeps
            // its usable travel instead of losing 26 px of it to the thumb.
            if (s.getSliderStyle() == juce::Slider::LinearVertical
                || s.getSliderStyle() == juce::Slider::LinearHorizontal)
                return (int) thumbRadiusFor (s);
            return juce::LookAndFeel_V4::getSliderThumbRadius (s);
        }

        // HEPTAGRAM - a 7-pointed star - used for every fader thumb. Drawn as a
        // 14-gon alternating the outer radius with an inner one, so it fills as a
        // solid star and its middle stays wide enough for the ADSR letter.
        // SPIRAL knob, finished like the star thumbs: an almost-closed spiral with a
        // pointer growing outwards from where its outer end sits, so the direction it
        // points IS the value.
        // Both draw calls read this: an envelope/LFO/automation currently driving the
        // slider's own parameter stashes its LIVE plain value here (see
        // refreshModWatch()); the marker below is drawn wherever that value actually
        // sits, separate from the thumb, which stays at the user's BASE setting.
        static bool liveModProportion (juce::Slider& s, float& proportionOut)
        {
            const auto& props = s.getProperties();
            if (! props.contains ("modLive")) return false;
            proportionOut = (float) s.valueToProportionOfLength ((double) props["modLive"]);
            return true;
        }

        // PER-VOICE targets have no single live value to point at: with eight notes
        // sounding, each one is at a different place along this control. So instead of a
        // dot they get the REACHABLE RANGE, drawn as a band from the base setting out to
        // wherever the modulation can carry it - the same convention Serum and Vital use,
        // and the honest answer to "where is this control right now?" when the answer is
        // "in eight places at once".
        static bool modSpanProportions (juce::Slider& s, float& loOut, float& hiOut)
        {
            const auto& props = s.getProperties();
            if (! props.contains ("modLo") || ! props.contains ("modHi")) return false;
            loOut = (float) s.valueToProportionOfLength ((double) props["modLo"]);
            hiOut = (float) s.valueToProportionOfLength ((double) props["modHi"]);
            if (hiOut < loOut) std::swap (loOut, hiOut);
            return hiOut - loOut > 1.0e-4f;
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                               float pos, float startAngle, float endAngle,
                               juce::Slider& s) override
        {
            const auto col = s.findColour (juce::Slider::thumbColourId);
            auto area = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
            const float r = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 2.0f;
            if (r <= 2.0f) return;
            const auto c = area.getCentre();
            const float ang = startAngle + pos * (endAngle - startAngle);

            g.setColour (col.withAlpha (0.16f)); // seat
            g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);

            // The spiral itself: 2.25 turns growing from the middle out to r.
            juce::Path sp;
            constexpr int steps = 120;
            constexpr float turns = 2.25f;
            for (int i = 0; i <= steps; ++i)
            {
                const float t  = (float) i / (float) steps;
                const float a  = -juce::MathConstants<float>::halfPi + t * turns * juce::MathConstants<float>::twoPi;
                const float rr = r * (0.16f + 0.84f * t);
                const juce::Point<float> p (c.x + rr * std::cos (a), c.y + rr * std::sin (a));
                if (i == 0) sp.startNewSubPath (p); else sp.lineTo (p);
            }
            g.setColour (col);
            g.strokePath (sp, juce::PathStrokeType (juce::jmax (1.4f, r * 0.14f),
                                                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // The pointer: it leaves the spiral's outer rim and sticks out.
            const juce::Point<float> from (c.x + r * 0.86f * std::cos (ang), c.y + r * 0.86f * std::sin (ang));
            const juce::Point<float> to   (c.x + (r + 3.5f) * std::cos (ang), c.y + (r + 3.5f) * std::sin (ang));
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.drawLine ({ from, to }, juce::jmax (3.0f, r * 0.30f));
            g.setColour (col.brighter (0.35f));
            g.drawLine ({ from, to }, juce::jmax (1.8f, r * 0.20f));

            // Per-voice modulation: the band this control can be carried across.
            float mLo = 0.0f, mHi = 0.0f;
            if (modSpanProportions (s, mLo, mHi))
            {
                const float a0 = startAngle + juce::jlimit (0.0f, 1.0f, mLo) * (endAngle - startAngle);
                const float a1 = startAngle + juce::jlimit (0.0f, 1.0f, mHi) * (endAngle - startAngle);
                juce::Path arc;
                arc.addCentredArc (c.x, c.y, r * 0.86f, r * 0.86f, 0.0f, a0, a1, true);
                g.setColour (col.withAlpha (0.55f));
                g.strokePath (arc, juce::PathStrokeType (juce::jmax (2.0f, r * 0.13f),
                                                         juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
            }

            // Live modulation marker: a small bright dot riding the spiral's rim at
            // the ACTUAL (base + modulation) position, distinct from the pointer
            // above (which always shows the user's base setting). Global targets only -
            // a per-voice one gets the band above instead.
            float modProp = 0.0f;
            if (liveModProportion (s, modProp))
            {
                const float mang = startAngle + juce::jlimit (0.0f, 1.0f, modProp) * (endAngle - startAngle);
                const juce::Point<float> mp (c.x + r * 0.86f * std::cos (mang), c.y + r * 0.86f * std::sin (mang));
                g.setColour (juce::Colours::white);
                g.fillEllipse (mp.x - 3.2f, mp.y - 3.2f, 6.4f, 6.4f);
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.drawEllipse (mp.x - 3.2f, mp.y - 3.2f, 6.4f, 6.4f, 1.0f);
            }
        }

        static juce::Path heptagon (juce::Point<float> c, float r)
        {
            juce::Path p;
            constexpr float twoPi = juce::MathConstants<float>::twoPi;
            const float rIn = r * 0.62f;              // {7/2}-like proportions
            for (int i = 0; i < 14; ++i)
            {
                const float a  = -juce::MathConstants<float>::halfPi + (float) i * twoPi / 14.0f;
                const float rr = (i % 2 == 0) ? r : rIn;
                const juce::Point<float> pt (c.x + rr * std::cos (a), c.y + rr * std::sin (a));
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
                // Per-voice modulation band, under the thumb (see modSpanProportions).
                { float mLo = 0.0f, mHi = 0.0f;
                  if (modSpanProportions (s, mLo, mHi))
                  {
                      const float ya = bot - juce::jlimit (0.0f, 1.0f, mHi) * (bot - top);
                      const float yb = bot - juce::jlimit (0.0f, 1.0f, mLo) * (bot - top);
                      g.setColour (col.withAlpha (0.55f));
                      g.fillRoundedRectangle (cx - tw * 0.5f, ya, tw, juce::jmax (1.0f, yb - ya), tw * 0.5f);
                  } }
                // Never wider than the fader's own column (narrow ADSR columns would
                // otherwise have their thumbs overlap the neighbouring fader).
                const float rv = juce::jmin (r, (float) w * 0.5f);
                const auto hept = heptagon ({ cx, sliderPos }, rv);
                g.setColour (col);
                g.fillPath (hept);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.strokePath (hept, juce::PathStrokeType (1.0f));
                if (s.getName().isNotEmpty())
                {
                    g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (rv * 0.8f).withStyle ("Bold")));
                    g.drawText (s.getName(), juce::Rectangle<float> (cx - rv, sliderPos - rv, rv * 2.0f, rv * 2.0f),
                                juce::Justification::centred, false);
                }
                // Live modulation marker: a bright bar across the track at the ACTUAL
                // position (base + modulation), separate from the thumb above (base only).
                float modProp = 0.0f;
                if (liveModProportion (s, modProp))
                {
                    const float my = bot - juce::jlimit (0.0f, 1.0f, modProp) * (bot - top);
                    g.setColour (juce::Colours::white);
                    g.fillRect (cx - tw * 0.9f, my - 1.2f, tw * 1.8f, 2.4f);
                    g.setColour (juce::Colours::black.withAlpha (0.5f));
                    g.drawRect (cx - tw * 0.9f, my - 1.2f, tw * 1.8f, 2.4f, 0.6f);
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
                // Per-voice modulation band, under the thumb (see modSpanProportions).
                { float mLo = 0.0f, mHi = 0.0f;
                  if (modSpanProportions (s, mLo, mHi))
                  {
                      const float xa = l + juce::jlimit (0.0f, 1.0f, mLo) * (rt - l);
                      const float xb = l + juce::jlimit (0.0f, 1.0f, mHi) * (rt - l);
                      g.setColour (col.withAlpha (0.55f));
                      g.fillRoundedRectangle (xa, cy - tw * 0.5f, juce::jmax (1.0f, xb - xa), tw, tw * 0.5f);
                  } }
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
                float modProp = 0.0f;
                if (liveModProportion (s, modProp))
                {
                    const float mx = l + juce::jlimit (0.0f, 1.0f, modProp) * (rt - l);
                    g.setColour (juce::Colours::white);
                    g.fillRect (mx - 1.2f, cy - tw * 0.9f, 2.4f, tw * 1.8f);
                    g.setColour (juce::Colours::black.withAlpha (0.5f));
                    g.drawRect (mx - 1.2f, cy - tw * 0.9f, 2.4f, tw * 1.8f, 0.6f);
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

        // Word-wrapped, per-line coloured layout, with a tiny inline markup so the
        // hover texts can point the eye straight at what matters instead of being a
        // wall of prose:
        //     *term*   -> BOLD + amber: the thing the control IS (a concept/mode name)
        //     `value`  -> BOLD + blue:  a literal value, unit or example you can type
        //     !warn!   -> BOLD + red:   a consequence worth noticing before you act
        // Anything unmarked stays plain white; a "from the partial" line stays orange.
        static juce::TextLayout layoutFor (const juce::String& text, float maxW)
        {
            const juce::Font plain (juce::FontOptions (13.5f));
            const juce::Font bold  (juce::FontOptions (13.5f).withStyle ("Bold"));
            const juce::Colour cPlain  { juce::Colours::white };
            const juce::Colour cTerm   { 0xffffc95c }; // amber - concepts
            const juce::Colour cValue  { 0xff7fd4ff }; // blue  - literals you can type
            const juce::Colour cWarn   { 0xffff8a7a }; // red   - consequences
            const juce::Colour cSource { 0xffff9a3c }; // orange - "from the partial"

            juce::AttributedString as;
            juce::StringArray lines;
            lines.addLines (text);
            for (int i = 0; i < lines.size(); ++i)
            {
                const auto& line = lines[i];
                const auto tail = (i < lines.size() - 1 ? juce::String ("\n") : juce::String());
                if (isSourceLine (line)) { as.append (line + tail, plain, cSource); continue; }

                // Walk the line, splitting on the three delimiters. An unmatched
                // delimiter is treated as ordinary text, so a stray * never eats
                // the rest of a tooltip.
                // A delimiter only OPENS a span when it starts a word: it must not be
                // wedged against a letter or digit on its left, and must not be followed
                // by a space. Without that rule "keeps 14 (2*7) and 25 (5*5)" parsed as a
                // highlighted "7) and 25 (5" - which is exactly where the meaningless
                // coloured runs in the prime tooltip came from. Arithmetic in a tooltip
                // is ordinary text and has to stay that way.
                const auto opensHere = [&line] (int i)
                {
                    if (i + 1 >= line.length()) return false;
                    const auto prev = i > 0 ? line[i - 1] : (juce::juce_wchar) ' ';
                    return ! juce::CharacterFunctions::isLetterOrDigit (prev)
                        && ! juce::CharacterFunctions::isWhitespace (line[i + 1]);
                };

                int pos = 0;
                while (pos < line.length())
                {
                    int at = -1; juce::juce_wchar delim = 0;
                    for (int i = pos; i < line.length(); ++i)
                    {
                        const auto ch = line[i];
                        if ((ch == '*' || ch == '`' || ch == '!') && opensHere (i))
                        { at = i; delim = ch; break; }
                    }

                    if (at < 0) { as.append (line.substring (pos), plain, cPlain); break; }
                    const int close = line.indexOfChar (at + 1, delim);
                    if (close < 0) { as.append (line.substring (pos), plain, cPlain); break; }

                    if (at > pos) as.append (line.substring (pos, at), plain, cPlain);
                    const auto inner = line.substring (at + 1, close);
                    as.append (inner, bold, delim == '*' ? cTerm : delim == '`' ? cValue : cWarn);
                    pos = close + 1;
                }
                if (tail.isNotEmpty()) as.append (tail, plain, cPlain);
            }
            juce::TextLayout tl;
            tl.createLayout (as, maxW);
            return tl;
        }
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                               juce::Rectangle<int> parentArea) override
        {
            // Keyboard hovers become a CALLOUT: parked right above the hovered key's
            // eye, with a pointer down to it - it must NEVER cover keys or numbers.
            lastWasCallout = ! keyboardScreenArea.isEmpty() && keyboardScreenArea.contains (screenPos)
                          && hoverAnchor != juce::Point<int>();

            if (lastWasCallout)
            {
                // Only the strip ABOVE the keyboard is fair game. A tall read-out used
                // to be clamped to the top of that strip and then simply overflow DOWN
                // across the keys - and how bad it looked depended on where the editor
                // sat, which is why it behaved differently in a host and standalone.
                // Instead, widen the layout until the text is short enough to fit the
                // strip: a wide-and-short card always beats covering the keys.
                const int availH = keyboardScreenArea.getY() - 1 - parentArea.getY();
                const int maxW   = juce::jmax (200, parentArea.getWidth() - 16);
                int bw = 0, bh = 0;
                for (const float tryW : { 340.0f, 460.0f, 620.0f, 820.0f })
                {
                    const auto t = layoutFor (tipText, juce::jmin ((float) maxW, tryW));
                    bw = juce::jmax (40, (int) std::ceil (t.getWidth()) + 18);
                    bh = (int) std::ceil (t.getHeight()) + 10 + kPtrH;
                    if (bh <= availH) break;                 // fits the strip: done
                    if (juce::jmin ((float) maxW, tryW) >= (float) maxW) break; // no room to widen
                }
                bw = juce::jmin (bw, parentArea.getWidth());
                // If even the widest layout is taller than the strip, sit flush with
                // the top and let the CARD be clipped there - never over the keys.
                const int top = juce::jmax (parentArea.getY(), keyboardScreenArea.getY() - 1 - bh);
                const int bot = juce::jmax (top + 1, keyboardScreenArea.getY() - 1);
                return juce::Rectangle<int> (juce::jlimit (parentArea.getX(), juce::jmax (parentArea.getX(),
                                                            parentArea.getRight() - bw), hoverAnchor.x - bw / 2),
                                             top, bw, juce::jmin (bh, bot - top));
            }

            const auto tl = layoutFor (tipText, 340.0f);
            const int w = juce::jmax (40, (int) std::ceil (tl.getWidth()) + 18);
            const int h = (int) std::ceil (tl.getHeight()) + 10;
            return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                         screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6,
                                         w, h).constrainedWithin (parentArea);
        }
        void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
        {
            // The rounded corners used to leave the window's own (unpainted) canvas
            // showing through as a pale border. Nothing may be left uncovered: the
            // whole rectangle is filled opaque first, then the card is drawn on it.
            g.fillAll (juce::Colour (0xff141420));
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
            g.setColour (juce::Colour (0xff141420));
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
        //
        // The usable text width is stated ONCE, here, because three places need to agree
        // on it: this function, resized() (which sizes the Shepard row's boxes to the
        // words they hold) and layoutProbe() (which measures whether the word fits). A
        // combo neither clips visibly nor resizes - it just draws as much as the cell
        // holds - so a formula copied out by hand would let "Sync Project Tempo" appear
        // on screen as "Sync Project Te" with every check in the build still green.
        static int comboTextWidth (int boxWidth, int boxHeight) noexcept
        { return boxWidth - boxHeight / 3 - boxHeight / 2; }
        // ...and the inverse: the box width a word of `textWidth` needs.
        static int comboWidthForText (int textWidth, int boxHeight) noexcept
        { return textWidth + boxHeight / 3 + boxHeight / 2 + 2; } // +2: the label's own rounding

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            const int inset = box.getHeight() / 3;
            label.setBounds (inset, 1, comboTextWidth (box.getWidth(), box.getHeight()), box.getHeight() - 2);
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
            const auto o = textEditorOutline ((float) width, (float) height, te.isMultiLine());
            g.setColour (te.findColour (juce::TextEditor::outlineColourId));
            g.drawRoundedRectangle (o.rect, o.radius, o.thickness);
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

    //== SPECTRUM: the two ENGINES as the title chips of ONE shared box =========
    // Built like the modulator tabs (AMP / PITCH / FILTER / CUSTOM): the chip row IS
    // the engine box's title, exactly one lit, the selected engine's controls shown
    // inside the box (the other's are hidden, not dimmed - resized() gates them), and
    // the selected one's colour worn by the WHOLE panel. The engines are mutually
    // exclusive, so two side-by-side framed boxes said "two independent things" and
    // wasted most of the Custom Engine's box on air.
    //
    // Two chips, not three: TEMPERED is a MODE of the harmonic engine, not a third
    // engine - a tick box laid out in BOTH views (it is the way in AND out of temper
    // mode, so it must never disappear with the view). Temper mode lights HARMONIC
    // (which is what is generating then).
    {
        const juce::Colour chipCol[2] = { cHarmonic, cTimbre };
        for (int i = 0; i < 2; ++i)
        {
            engineChip[i].setColour (juce::TextButton::buttonColourId, chipCol[i]);
            engineChip[i].setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (engineChip[i]);
        }
        // Both chips run the SAME function (see selectEngineView): picking a view IS
        // picking the engine, in one place, so the two directions cannot be wired
        // differently. With the Custom-Engine On/Off pill gone these chips are the only
        // switch between the engines, which is exactly why there must be only one of them.
        engineChip[0].onClick = [this] { selectEngineView (false); refocus(); };
        engineChip[1].onClick = [this] { selectEngineView (true);  refocus(); };
        // Right-click still maps a modulator onto the parameter the CUSTOM chip owns.
        addMapListener (engineChip[1], "timbreOn");
    }

    // Harmonic Engine box (nested in SPECTRUM): one framed box, one colour, with the
    // sub-sections (Base / Harmonics / Prime) separated by shared divider lines - the
    // whole box (frame/title/dividers/labels) is drawn in paint(), not as a
    // GroupComponent. SLOPE is deliberately NOT in here any more: it shapes whichever
    // engine is running, and living inside this box got it greyed out with the box while
    // its fader went on working.
    configFader (baseNFader,   "baseN",    baseNAtt,     cHarmonic, "Base harmonic N");
    configCombo (basePitchBox, "baseMode", basePitchAtt, cHarmonic, "Original = harmonics at natural positions.  Transposed down = lowest partial at the played pitch.");
    makeLabel (lblBaseN, cHarmonic); makeLabel (lblBasePitch, cHarmonic);
    lblBasePitch.setJustificationType (juce::Justification::centredLeft); // top-LEFT of its combo, like the others

    // Morphing: when ON, a spectrum change glides each partial to its new position
    // (a per-partial pitch bend) over the Morph time, instead of snapping. It glides
    // ANY template change, from either engine, so it wears the shared (mint) colour and
    // lives in the BOTH ENGINES box - inside the Harmonic Engine it was dimmed whenever
    // a custom scale took over, while it went on morphing that scale's partials.
    // An ON/OFF pill, not a tick box: the painted MORPH sub-label above it is already the
    // name, so a checkbox ALSO labelled "Morph" read as a second, different control.
    morphToggle.setName ("onled"); // green(on)/red(off) pill, drawn by GalacticLnF
    morphToggle.setMouseClickGrabsKeyboardFocus (false);
    morphToggle.onClick = [this] { refocus(); };
    addAndMakeVisible (morphToggle);
    morphAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "morph", morphToggle);
    addMapListener (morphToggle, "morph");
    configFader (morphTimeFader, "morphTime", morphTimeAtt, cSlope, "Morph transition time in seconds: how long partials take to glide to the new spectrum.");
    morphTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    morphTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);
    makeLabel (lblMorphTime, cSlope);

    // Harmonics is a magnetic FADER now (Even - All - Odd - Prime), so a modulator
    // can sweep it and it can sit BETWEEN two sets (blended).
    configFader (harmonicsFader, "harmonics", harmonicsAtt, cHarmonic,
                 "Which harmonics sound: *Even* - *All* - *Odd* - *Prime*.\nIt snaps to those four, but you can stop BETWEEN two of them (hold `Ctrl` to drag free): there the harmonics belonging to only one of the pair come in at part volume, so the timbre *morphs* instead of switching.");
    harmonicsFader.setSliderStyle (juce::Slider::LinearHorizontal);
    harmonicsFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 86, 15); // fits "Even-All 50%"
    harmonicsFader.setSliderSnapsToMousePosition (false);
    // Finer than the other magnetic faders: this axis is only 0..3 wide but the
    // interesting part is the BLEND between two named sets, so it needs real room
    // per step (~160 px) instead of 80. The detent window is narrowed to match -
    // at 0.12 on a 3-wide axis the first and last 12% of every blend were simply
    // unreachable, which is what made it feel coarse and jumpy.
    harmonicsFader.normalSensitivity = 480;
    harmonicsFader.snapWindow = 0.06;
    harmonicsFader.textFromValueFunction = [this] (double v)
    {
        const juce::String names[] = { tr ("Even", "Parells"), tr ("All", "Tots"),
                                       tr ("Odd", "Senars"),   tr ("Prime", "Primers") };
        const int lo = juce::jlimit (0, 3, (int) std::floor (v));
        const double t = v - (double) lo;
        if (t < 0.01) return names[lo];
        if (t > 0.99) return names[juce::jmin (3, lo + 1)];
        return names[lo] + "-" + names[juce::jmin (3, lo + 1)]
             + " " + juce::String ((int) std::lround (t * 100.0)) + "%";
    };
    // The attachment installs a NUMERIC text parser, so typing one of the names
    // shown above would otherwise read as 0 (= Even). Accept both.
    harmonicsFader.valueFromTextFunction = [] (const juce::String& t)
    {
        const auto s = t.trim().toLowerCase();
        if (s.startsWith ("even") || s.startsWith ("parell")) return (double) spectrum::Even;
        if (s.startsWith ("all")  || s.startsWith ("tot"))    return (double) spectrum::All;
        if (s.startsWith ("odd")  || s.startsWith ("senar"))  return (double) spectrum::Odd;
        if (s.startsWith ("prim"))                            return (double) spectrum::Prime;
        return juce::jlimit (0.0, 3.0, s.getDoubleValue());
    };
    harmonicsFader.updateText();
    // SLOPE shapes the amplitudes of WHATEVER spectrum is running (harmonic, tempered or
    // a custom scale), so it gets the shared mint colour and its own cell in the BOTH
    // ENGINES box. It used to sit inside the Harmonic Engine frame and be greyed with it
    // while the fader itself stayed live - a control that looks dead and isn't.
    configFader (slopeFader, "curveSlope", slopeAtt, cSlope, "Amplitude roll-off. 0 = flat/bright, 1 = saw, 2 = triangle.  (Also thins the highs when a Custom Engine scale is the spectrum.)");
    //== Per-partial amplitudes: the pencil + its ON/OFF pill, both in the SLOPE cell ==
    partialEditBtn.col = cSlope;
    partialEditBtn.onClick = [this] { openPartialAmpWindow(); };
    addAndMakeVisible (partialEditBtn);
    partialManualT.setName ("onled"); // big green(on)/red(off) pill, drawn by GalacticLnF
    partialManualT.setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (partialManualT);
    // The SAME parameter as the pill inside the window: two attachments, one value, so
    // switching either one moves the other with no code to keep them in step (and both
    // land on the undo stack and in host automation like every other control).
    partialManualAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "partialManual", partialManualT);
    partialPanel.trF      = [this] (const char* en, const char* ca) { return tr (en, ca); };
    partialPanel.grid.trF = partialPanel.trF;
    // The panel lives in its OWN top-level window, so it is not a descendant of this
    // editor and does NOT inherit the LookAndFeel from it. Without this the ON/OFF pill
    // fell back to a stock JUCE tick-box and the tooltips lost their colour markup.
    partialPanel.setLookAndFeel (lnf.get());
    // No onEdited hook: the main window shows nothing that a knob move changes (the
    // FROZEN tag follows the PARAMETER, and refreshSlopeFreeze already watches that),
    // and repainting the whole Harmonic Engine box on every drag frame was pure cost.
    //
    // onManualArmed is the exception, and it is not cosmetic. When the first pencil drag
    // switches manual mode on by itself, AMP DECAY silently stops working, and somebody
    // who reaches for it afterwards has been ambushed. So the freeze is shown the instant
    // it happens rather than up to a 30 Hz tick later: grey fader, FROZEN tag, and the
    // fader's tooltip already rewritten by the time a hand arrives at it.
    //
    // Deliberately NOT a forced TooltipWindow::displayTip here. The tip would be aimed at
    // the main window while the mouse is mid-drag in the floating knob window - JUCE hides
    // a displayed tip on the next mouse move, so it would flash and vanish, and it can sit
    // behind that always-on-top window anyway. The warning the user actually sees is the
    // banner in the panel they are looking at (PartialAmpPanel::refreshText), which says
    // the same thing and stays up for as long as it is true.
    partialPanel.onManualArmed = [this] { refreshSlopeFreeze(); };
    partialPanel.init (processorRef);
    // Fractal is a cell of the BOTH ENGINES box now (it multiplies whatever spectrum the
    // running engine produced), so it has no GroupComponent of its own - its frame is
    // that box's, and its title is a painted sub-label like SLOPE and MORPH.
    configFader (fractalFader, "fractalLevel", fractalAtt, cFractal, "Fractal self-similarity level (1 = off)");
    // Essentials macro (Phase / Gain / Reference) - one framed box drawn
    // in paint() like the Harmonic Engine, so no per-cell GroupComponents here.
    configCombo (phaseBox, "phaseMode", phaseAtt, cEssentials, "Aligned: tonal pulse, never clips.  Scattered: louder, noisier.");
    configFader (gainFader, "masterGain", gainAtt, cEssentials, "Master output gain");
    gainFader.setSliderStyle (juce::Slider::LinearHorizontal);
    gainFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 14);
    // Two +1/-1 stepper rows (no faders), each now titled by its cell's painted
    // sub-label: OCTAVE (whole-synth transpose, +-5) stays in Essentials, RANGE (the
    // spectrum's octave span - partials above 2^N x the note are CUT, 10 = full) moved
    // into the SPECTRUM panel's BOTH ENGINES box, because that cut applies to either
    // engine and hiding it among the output essentials made a partial-remover easy to
    // miss. The value labels are refreshed by the timer.
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
    primeLimitFader.setSliderSnapsToMousePosition (false);
    primeLimitFader.normalSensitivity = 300;
    primeLimitFader.textFromValueFunction = [] (double v)
    {
        static const char* names[] = { "Off", "23", "19", "17", "13", "11", "7", "5", "3", "2" };
        const int lo = juce::jlimit (0, 9, (int) std::floor (v));
        const double t = v - (double) lo;
        if (t < 0.01) return juce::String (names[lo]);
        return juce::String (names[lo]) + juce::String::fromUTF8 ("\xe2\x86\x92")
             + juce::String (names[juce::jmin (9, lo + 1)])
             + " " + juce::String ((int) std::lround (t * 100.0)) + "%";
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

    // MONOPHONIC GLIDE. Both controls live on the PITCH tab and are shown only while it
    // is the tab on screen (see resized()). They are ordinary parameter attachments -
    // everything that makes the mode work (the one-voice pool, the legato takeover, the
    // portamento itself) is in the processor and the voice, so it works with this window
    // shut and under host automation.
    monoGlideT.setMouseClickGrabsKeyboardFocus (false);
    monoGlideT.onClick = [this] { refocus(); };
    addChildComponent (monoGlideT);
    monoGlideAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "monoGlide", monoGlideT);
    addMapListener (monoGlideT, "monoGlide");
    configFader (glideFader, "glideTime", glideAtt, cPitch,
                 "How long the pitch takes to travel from the previous note to the new one (ms).");
    glideFader.setTextValueSuffix (" ms");
    makeLabel (lblGlide, cPitch);
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
        // The envelope is EDITED as a curve with nodes (the faders below stay as the
        // automation targets, but are no longer shown).
        envEditor.col = cCustom;
        envEditor.onGesture = [this] (const juce::String& suffix, bool starting)
        {
            if (suffix.isEmpty()) return;
            if (auto* p = processorRef.apvts.getParameter (currentModParamId ("modE1" + suffix)))
            { if (starting) p->beginChangeGesture(); else p->endChangeGesture(); }
        };
        envEditor.onEdit = [this] (const juce::String& suffix, float v)
        {
            if (auto* p = processorRef.apvts.getParameter (currentModParamId ("modE1" + suffix)))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        envEditor.getParam = [this] (const juce::String& suffix)
        {
            auto* p = processorRef.apvts.getRawParameterValue (currentModParamId ("modE1" + suffix));
            return p != nullptr ? p->load() : 0.0f;
        };

        // The SAME editor drives the Amp / Pitch / Filter envelopes. Their parameter
        // ids differ per tab, so the id is built from whichever tab is showing.
        const auto voiceId = [this] (const juce::String& suffix)
        {
            static const char* pfx[3] = { "amp", "pitch", "filter" };
            const juce::String base (pfx[juce::jlimit (0, 2, voiceEnvTab)]);
            if (suffix.startsWith ("Curve")) return base + suffix;             // ampCurveD...
            static const char* full[4] = { "Attack", "Decay", "Sustain", "Release" };
            const int i = suffix == "A" ? 0 : suffix == "D" ? 1 : suffix == "S" ? 2 : 3;
            return base + full[i];                                             // ampDecay...
        };
        voiceEnv.onGesture = [this, voiceId] (const juce::String& suffix, bool starting)
        {
            if (suffix.isEmpty()) return;
            if (auto* p = processorRef.apvts.getParameter (voiceId (suffix)))
            { if (starting) p->beginChangeGesture(); else p->endChangeGesture(); }
        };
        voiceEnv.onEdit = [this, voiceId] (const juce::String& suffix, float v)
        {
            if (auto* p = processorRef.apvts.getParameter (voiceId (suffix)))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        voiceEnv.getParam = [this, voiceId] (const juce::String& suffix)
        {
            auto* p = processorRef.apvts.getRawParameterValue (voiceId (suffix));
            return p != nullptr ? p->load() : 0.0f;
        };
        addChildComponent (voiceEnv);
        addChildComponent (envEditor);
        for (int i = 0; i < 4; ++i)
        {
            configFader (custAdsr[i], "modE1" + juce::String (adsrTxt[i]), custAdsrAtt[i], cCustom, "Custom envelope");
            custAdsr[i].setName (adsrTxt[i]);
            makeLabel (custAdsrLbl[i], cCustom);
            custAdsrLbl[i].setText (adsrTxt[i], juce::dontSendNotification);
        }
        configFader (custRangeFader, "modE1Range", custRangeAtt, cCustom,
                     "How far this modulator MOVES its mapped parameters (+/- of their full range), riding on each knob's own value. Note: parameters that REBUILD the spectrum (Base, Harmonics, Decay, Fractal, Prime, Range, Reps) follow the modulator at the 60 Hz rebuild rate and glide when Morph is on - everything else follows per audio block.");
        makeLabel (lblCustRange, cCustom);
        configFader (lfoDivSlider, "modL1Div", lfoDivAtt, cCustom,
                     "Rate divider: halves the base rate per step (base = project BPM or the lowest held note's frequency).");
        configFader (lfoHzSlider, "modL1Hz", lfoHzAtt, cCustom, "Free LFO rate in Hz.");
        configFader (lfoPhaseSlider, "modL1Phase", lfoPhaseAtt, cCustom,
                     "Phase offset: where in the wave the oscillation starts.");
        // The LFO values are KNOBS (spiral - see GalacticLnF::drawRotarySlider). This
        // must come AFTER configFader, which sets a linear style.
        for (auto* k : { &lfoDivSlider, &lfoHzSlider, &lfoPhaseSlider, &custRangeFader })
        {
            k->setSliderStyle (juce::Slider::RotaryVerticalDrag);
            k->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                    juce::MathConstants<float>::pi * 2.75f, true);
            k->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 13);
        }
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
        lfoWaveBox.addItem ("Random", 7);
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
        lfoWaveBox.onChange     = [this] { resized(); }; // Draw button appears with "Custom (draw)"
        lfoRateModeBox.onChange = [this] { resized(); }; // Div vs Hz: only the one in use is shown
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
                 "Where the tuning comes from.\n*MTS-ESP* - follow a connected ODDSound master; with none connected this is plain `12ED2`.\n*Scala* - the loaded `.scl` scale.\n*MPE* - the host sends the tuning as per-note bend.");
    // 12ED2 is NOT a tuning mode - it is just a scale, and it now lives inside Scala as
    // the default content. It used to sit in this list greyed out, which still made it
    // look like a mode you were not allowed to pick. It is gone from the list entirely.
    //
    // The parameter keeps all four entries so projects saved on 12ED2 restore exactly,
    // which means the box can no longer use a ComboBoxAttachment: that maps by item
    // POSITION, and dropping an item would shift every mode by one. Wired by ID instead
    // (id = parameter index + 1), explicitly, in both directions.
    tuningAtt.reset();
    tuningBox.clear (juce::dontSendNotification);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter ("tuningMode")))
        for (int i = 1; i < choice->choices.size(); ++i)     // skip index 0 (12ED2)
            tuningBox.addItem (choice->choices[i], i + 1);
    tuningBox.onChange = [this]
    {
        const int id = tuningBox.getSelectedId();
        if (id <= 0) return;
        if (auto* p = processorRef.apvts.getParameter ("tuningMode"))
        {
            const float want = p->convertTo0to1 ((float) (id - 1));
            if (std::abs (p->getValue() - want) > 1.0e-4f)
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (want);
                p->endChangeGesture();
            }
        }
        refocus();
    };
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

    // Absolute 128-key table: the period is Fibló's own ESTIMATE and the user may set it.
    // It cannot move a pitch (the table fixes all 128) - it only answers the features that
    // need a period: the keyboard colouring, the snapping grid, the chord search and the
    // dissonance lists. Applied on Return, so a half-typed "3/" is never read as a period.
    tablePeriodEdit.setJustification (juce::Justification::centred);
    tablePeriodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    tablePeriodEdit.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xffe0453f).withAlpha (0.7f));
    tablePeriodEdit.setIndents (6, 2);
    tablePeriodEdit.setSelectAllWhenFocused (true);
    tablePeriodEdit.onReturnKey = [this]
    {
        juce::String err;
        if (! processorRef.setTuningTablePeriod (tablePeriodEdit.getText(), err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         tr ("Invalid period", "Per\xc3\xado" "de inv\xc3\xa0lid"),
                                                         err, this);
        // Whatever happened, show what is actually in force - never leave the field
        // displaying a period the tuning did not accept.
        tablePeriodEdit.setText (processorRef.tuningTablePeriodSpec(), juce::dontSendNotification);
        refocus();
    };
    tablePeriodEdit.onEscapeKey = [this]
    {
        tablePeriodEdit.setText (processorRef.tuningTablePeriodSpec(), juce::dontSendNotification);
        refocus();
    };
    addChildComponent (tablePeriodEdit);
    tablePeriodResetBtn.setColour (juce::TextButton::buttonColourId, cReset.withAlpha (0.55f));
    tablePeriodResetBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    tablePeriodResetBtn.setMouseClickGrabsKeyboardFocus (false);
    tablePeriodResetBtn.onClick = [this]
    {
        processorRef.resetTuningTablePeriod();
        tablePeriodEdit.setText (processorRef.tuningTablePeriodSpec(), juce::dontSendNotification);
        refocus();
    };
    addChildComponent (tablePeriodResetBtn);

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
    tuningBankLoadBtn.setTooltip (tr ("Load a .tun / .scl / .ascl into the selected bank slot.",
                                      "Carrega un .tun / .scl / .ascl a la ranura seleccionada del banc."));
    tuningBankLoadBtn.onClick = [this] { openTuningBankChooser(); };
    // Put the tuning that is playing RIGHT NOW into the bank (any mode: a generated
    // scale, an edited one, an EDO...), without going through a file.
    tuningBankAddBtn.setColour (juce::TextButton::buttonColourId, cTuning.withAlpha (0.5f));
    tuningBankAddBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    tuningBankAddBtn.setMouseClickGrabsKeyboardFocus (false);
    tuningBankAddBtn.onClick = [this]
    {
        auto name = processorRef.tuningScaleName();
        if (name.isEmpty()) name = "tuning " + juce::String (processorRef.getScaleDegrees()) + " notes";
        processorRef.syncBankSelector (
            processorRef.bankAutoAdd (name, processorRef.currentTuningAsBankSource (name)));
        refreshTuningBank();
        refocus();
    };
    addChildComponent (tuningBankAddBtn);
    // Scales this synth writes carry the patch they were made with. When one of
    // those is loaded, offer to bring that sound back (the tuning stays put).
    loadItsSpectrumBtn.setColour (juce::TextButton::buttonColourId, cSpecScale.withAlpha (0.85f));
    loadItsSpectrumBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    loadItsSpectrumBtn.setMouseClickGrabsKeyboardFocus (false);
    loadItsSpectrumBtn.onClick = [this]
    {
        if (! processorRef.loadSpectrumFromScl (processorRef.activeSclSource()))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                tr ("Original spectrum", "Espectre original"),
                tr ("This file does not carry a Fibló Galàctic patch.",
                    "Aquest fitxer no porta cap patch de Fibl\xc3\xb3 Gal\xc3\xa0" "ctic."), this);
        resized(); repaint(); refocus();
    };
    addChildComponent (loadItsSpectrumBtn);
    // Loading a preset never touches the tuning - only the sound changes. When the
    // preset it just loaded carries a DIFFERENT tuning of its own, offer it here
    // instead, so switching sounds can never yank the scale out from under you.
    loadPresetTuningBtn.setColour (juce::TextButton::buttonColourId, cLoad);
    loadPresetTuningBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadPresetTuningBtn.setMouseClickGrabsKeyboardFocus (false);
    loadPresetTuningBtn.onClick = [this]
    {
        juce::String err;
        if (! processorRef.loadPendingPresetTuning (err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                tr ("Load preset's tuning", "Carrega l'afinaci\xc3\xb3 del preset"), err, this);
        resized(); repaint(); refocus();
    };
    addChildComponent (loadPresetTuningBtn);
    // The current custom spectrum was DERIVED FROM a tuning (adapted to it, or loaded
    // from a scale file): offer that tuning here, same idiom as the preset's tuning
    // above. Word/enabled/tooltip live in refreshSpectrumTuningButton().
    loadSpectrumTuningBtn.setColour (juce::TextButton::buttonColourId, cLoad);
    loadSpectrumTuningBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadSpectrumTuningBtn.setMouseClickGrabsKeyboardFocus (false);
    loadSpectrumTuningBtn.onClick = [this]
    {
        juce::String err;
        if (! processorRef.loadSpectrumProvenanceTuning (err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                tr ("Load spectrum's tuning", "Carrega l'afinaci\xc3\xb3 de l'espectre"), err, this);
        resized(); repaint(); refocus();
    };
    addChildComponent (loadSpectrumTuningBtn);
    // Recent: one button that opens the floating list of everything ever loaded.
    recentOpenBtn.setColour (juce::TextButton::buttonColourId, cTuning.withAlpha (0.28f));
    recentOpenBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    recentOpenBtn.setMouseClickGrabsKeyboardFocus (false);
    recentOpenBtn.onClick = [this]
    {
        recentOverlay.setPaths (processorRef.getRecentSclPaths());
        recentOverlay.setVisible (true);
        recentOverlay.toFront (true);
    };
    addChildComponent (recentOpenBtn);
    recentOverlay.list.onPick = [this] (int i)
    {
        loadRecent (i);
        recentOverlay.setVisible (false);
        refocus();
    };
    addChildComponent (recentOverlay);
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

    // Custom Engine (.scl as spectrum) - the CUSTOM view of the shared engine box.
    // (Its bilingual tooltip is set in applyLanguage, which the ctor calls below.)
    // ONE button, two jobs (see refreshCustomEngineButtons, which sets its word and its
    // colour): "Load" while nothing is loaded, "Remove" once a spectrum exists. The
    // Custom-Engine On/Off pill that used to sit beside it is gone: the engine chips
    // already decide which engine is in force, so the pill was a second switch for the
    // same thing that could visibly disagree with them. The non-destructive off it
    // offered now lives on the HARMONIC chip (which keeps the loaded scale); this
    // button is the destructive one, and calls itself Remove so it can never be mistaken
    // for the other. Removal goes through the undo stack: one Ctrl+Z brings it back.
    loadTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    loadTimbreButton.onClick = [this]
    {
        if (processorRef.timbreLoaded())
        {
            processorRef.clearTimbreUndoable();
            // Stay in the custom view rather than snapping to harmonic: the user removed
            // a spectrum inside this workspace, and being thrown out of it after every
            // Remove would make "remove, then load another" a two-click hunt for the chip.
            // The SOUND is honestly harmonic again (there is no spectrum), and the
            // HARMONIC chip is right there to make the view agree.
            customViewPinned = true;
            applyEngineState();
        }
        else
        {
            openTimbreChooser();
        }
        refocus();
    };
    addAndMakeVisible (loadTimbreButton);
    // RANDOM: fires the automatable trigger, it does not build anything itself. A click
    // and a host automation step therefore take the identical path through the processor
    // - the one way to be sure the automated version does what the button does.
    randomSpectrumBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    randomSpectrumBtn.setMouseClickGrabsKeyboardFocus (false);
    randomSpectrumBtn.onClick = [this]
    {
        // Pressing Random inside the pinned empty workspace has to leave the CUSTOM
        // engine in force, not just draw a spectrum: loadTimbreText switches it on, and
        // pinning keeps the view where the user is while the processor catches up on its
        // next tick (the chips would otherwise flicker back to harmonic for one frame).
        customViewPinned = true;
        processorRef.pressRandomSpectrum();
        refocus();
    };
    // NO right-click modulator routing on this one, deliberately. ModEngine writes the
    // parameters' RAW ATOMICS on the audio thread and never calls setValueNotifyingHost
    // (see ModEngine.h) - so an LFO mapped here would move the value and the trigger's
    // listener would never see an edge. A routing menu that offers a mapping which
    // silently does nothing is worse than no menu at all. The HOST's automation lane
    // does go through setValueNotifyingHost, which is what makes it work there.
    addAndMakeVisible (randomSpectrumBtn);
    // TEMPER is laid out in BOTH views of the shared engine box (resized() places it
    // per view): ticking it in the custom view switches the view to HARMONIC - the
    // engine that generates in temper mode - and if the toggle only existed in the
    // custom view it would vanish under the user's own click, with no way to untick
    // it short of knowing the chips undo it. Its TICK is the harmonic engine's blue,
    // because that is the engine it hands the spectrum to.
    temperToggle.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffb9c0d0));
    temperToggle.setColour (juce::ToggleButton::tickColourId, cHarmonic);
    temperToggle.setColour (juce::ToggleButton::tickDisabledColourId, cHarmonic.withAlpha (0.4f));
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
                    processorRef.requestHardSilence(); // never leave a note stuck sounding through Init
                    for (auto* p : processorRef.getParameters())
                        p->setValueNotifyingHost (p->getDefaultValue());
                    processorRef.clearScala();
                    processorRef.clearTimbre();
                    setIntervalText ({});
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
        // .tun is the default: it carries the reference frequency, which .scl cannot, so
        // an exported scale reopens at the same absolute pitch. .scl stays available in
        // the file-type dropdown for anything that only speaks Scala.
        auto ch = std::make_shared<juce::FileChooser> (
            tr ("Save tuning", "Desa l'afinaci\xc3\xb3"),
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile ("tuning.tun"),
            "*.tun;*.scl");
        ch->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this, ch] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File()) { refocus(); return; }
                // Whatever the dialog gave back decides the format; no extension at all
                // means the default, .tun.
                const bool asScl = f.hasFileExtension ("scl");
                if (! asScl && ! f.hasFileExtension ("tun")) f = f.withFileExtension ("tun");
                juce::String err;
                if (! (asScl ? processorRef.saveCurrentTuningScl (f, err)
                             : processorRef.saveCurrentTuningTun (f, err)))
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
        "SPECTRUM VS RHYTHM: turns partials of the current patch into MIDI pulse trains - exact polyrhythms derived from the spectrum's frequency ratios, sample-locked to the sound - sent out of this plugin's MIDI OUTPUT to any other track/instrument. Click to open the panel.",
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
    // Seed the JI switch from its parameter, so a reopened window agrees with what the
    // pulses are actually doing. Seeded from the PARAMETER only - never inferred from
    // "some slot has useRatio", because a hand-typed ratio sets that with JI off and
    // the next click would then wipe those hand-typed ratios.
    rhythmPanel.jiB.setToggleState (
        processorRef.apvts.getRawParameterValue ("rhythmJi")->load() > 0.5f,
        juce::dontSendNotification);
    rhythmPanel.updateJiText();
    addChildComponent (rhythmPanel);
    rhythmOnAtt   = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmOn",   rhythmPanel.onT);
    rhythmSelfAtt = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmSelfPlay", rhythmPanel.selfT);
    rhythmDroneAtt = std::make_unique<ButtonAttachment>  (processorRef.apvts, "rhythmDrone", rhythmPanel.droneT);
    rhythmFbAtt   = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmFeedback", rhythmPanel.feedbackT);
    rhythmQTempoAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "rhythmQTempo", rhythmPanel.qTempoT);
    rhythmMonoAtt   = std::make_unique<ButtonAttachment> (processorRef.apvts, "rhythmMono",   rhythmPanel.monoT);
    rhythmQPulseAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "rhythmQPulse", rhythmPanel.qPulseT);
    rhythmFbDecayAtt = std::make_unique<SliderAttachment>(processorRef.apvts, "rhythmFeedbackDecay", rhythmPanel.feedbackS);
    rhythmVelSlopeAtt = std::make_unique<SliderAttachment>(processorRef.apvts, "rhythmVelSlope", rhythmPanel.velSlopeS);
    rhythmStepsAllAtt = std::make_unique<SliderAttachment>(processorRef.apvts, "rhythmStepsAll", rhythmPanel.stepsAllS);
    addMapListener (rhythmPanel.stepsAllS, "rhythmStepsAll");
    rhythmPanel.stepsAllLbl.setText (tr ("Steps", "Passos"), juce::dontSendNotification);
    rhythmPanel.stepsAllS.setTooltip (tr (
        "Shifts EVERY step-pitched pulse by this many scale steps from the played note (JI/ratio pulses have no step, so they are unaffected). Right-click to map a modulator.",
        "Desplaç""a TOTS els polsos amb to per passos aquests passos d'""escala des de la nota tocada (els polsos JI/raó no en tenen, i no els afecta). Clic dret per assignar-hi un modulador."));
    // Modulatable, like every other fader: right-click to route a modulator.
    addMapListener (rhythmPanel.velSlopeS, "rhythmVelSlope");
    addMapListener (rhythmPanel.octAllVal, "rhythmOctAll");     // Octave (all pulses)
    rhythmPanel.velSlopeS.setTooltip (tr (
        "Velocity SLOPE across the spectrum. 0 = flat (every pulse keeps its own velocity). To the right the HIGH partials start losing velocity; to the left the low ones. At the end of its travel the ramp is vertical and no pulse has any velocity left. Right-click to map a modulator.",
        "PENDENT de velocitat sobre l'espectre. 0 = plana (cada pols mant\xc3\xa9 la seva). Cap a la dreta els parcials AGUTS comencen a perdre velocitat; cap a l'esquerra els greus. Al final del recorregut la pendent \xc3\xa9s vertical i cap pols conserva velocitat. Clic dret per assignar-hi un modulador."));
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
    // The drop-down is a SEARCHABLE list, not a plain menu (see PresetSearchPanel).
    presetBox.onShowPopup   = [this] { return openPresetSearch(); };
    presetBox.onChange = [this]
    {
        // The field is frozen while a playground corner is being edited: its text is a
        // corner's name, not a preset's, and loading from it would drop a whole preset
        // onto the corner's scratch surface. It is disabled there, so this is belt and
        // braces for a programmatic setText that forgot dontSendNotification.
        if (processorRef.getPlaygroundEditCorner() >= 0) return;
        const auto name = presetBox.getText();
        if (name.isNotEmpty() && processorRef.loadPreset (name))
            applyEngineState();
        refreshPresetDirtyMark();
        resized(); repaint(); refocus(); // the Tuning panel's "Load preset's tuning" button may now show
    };
    addAndMakeVisible (presetBox);
    // "*" after the preset name: some parameter's BASE value (a knob, automation,
    // undo...) no longer matches what is saved in the preset - see presetHasDrifted().
    presetDirtyMark.setText ("*", juce::dontSendNotification);
    presetDirtyMark.setColour (juce::Label::textColourId, juce::Colour (0xffffc95c));
    presetDirtyMark.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
    presetDirtyMark.setJustificationType (juce::Justification::centredLeft);
    presetDirtyMark.setInterceptsMouseClicks (false, false);
    presetDirtyMark.setTooltip (tr (
        "*Changed since loaded.*\nSome parameter no longer matches this preset - moved by something other than one of its OWN modulators (a knob, host automation, an undo).\n!Save stores it as a NEW preset! instead of overwriting the original.",
        "*Canviat des de la c\xc3\xa0rrega.*\nAlg\xc3\xban par\xc3\xa0metre ja no coincideix amb aquest preset - l'ha mogut alguna cosa que no \xc3\xa9s un dels seus PROPIS moduladors (un bot\xc3\xb3, automatitzaci\xc3\xb3, un desfer).\n!Desar el guarda com un preset NOU! en lloc de sobreescriure l'original."));
    addChildComponent (presetDirtyMark);
    refreshPresetList();

    savePresetButton.setColour (juce::TextButton::buttonColourId, cLoad);
    savePresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    savePresetButton.setMouseClickGrabsKeyboardFocus (false);
    savePresetButton.onClick = [this] { savePresetFlow(); };
    addAndMakeVisible (savePresetButton);

    // The preset playground lives WITH the preset selector, because that is what it is
    // made of: it takes presets off this same list and puts them on the corners of a
    // morph pad.
    playgroundButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7a4fd0));
    playgroundButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    playgroundButton.setMouseClickGrabsKeyboardFocus (false);
    playgroundButton.onClick = [this] { openPlaygroundWindow(); };
    addAndMakeVisible (playgroundButton);
    playgroundPanel.proc = &processorRef;
    playgroundPanel.pad.proc = &processorRef;
    playgroundPanel.trF     = [this] (const char* en, const char* ca) { return tr (en, ca); };
    playgroundPanel.pad.trF = [this] (const char* en, const char* ca) { return tr (en, ca); };
    playgroundPanel.attachVoices();   // needs proc: the pgVoices attachment
    // CORNER EDIT: the panel owns the Edit buttons, the MAIN window owns the preset field
    // and the Save flow they redirect (savePresetFlow reroutes itself), so the two are
    // wired here rather than the panel reaching into the editor.
    playgroundPanel.onEditModeChanged = [this]
    {
        refreshCornerEditUi();
        applyEngineState();   // the corner's engine/scale are on the controls now
        resized(); repaint();
    };

    // Pitch-bend range (inside the Tuning box).
    configCombo (bendBox, "bendRange", bendAtt, cTuning, {});
    makeLabel (lblBend, cTuning);

    // SHEPARD MODE (the Tuning box's bottom row). It lives in this panel because the
    // one tuning-dependent choice it offers - rise by an octave or by the tuning's own
    // period - only exists here. Words and tooltips are written by applyLanguage();
    // which of the two rate dresses (ms fader / beats box) is on screen, and whether
    // the rise-interval box exists at all, is decided by resized().
    shepOnT.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    shepOnT.setColour (juce::ToggleButton::tickColourId, cTuning);
    shepOnT.setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (shepOnT);
    shepOnAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "shepOn", shepOnT);
    configCombo (shepModeBox, "shepMode", shepModeAtt, cTuning, {});
    // Switching mode swaps which rate control is on screen, so it must re-lay the
    // row NOW - the attachment only moves the parameter, never the layout.
    shepModeBox.onChange = [this] { resized(); repaint(); refocus(); };
    configCombo (shepDivBox, "shepDiv", shepDivAtt, cTuning, {});
    // configFader dresses them as the standard vertical fader; these two live in a
    // 22 px row, so they are re-dressed horizontal with a value box at their right.
    configFader (shepTimeFader, "shepTime", shepTimeAtt, cTuning, {});
    shepTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    shepTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 15);
    shepTimeFader.setNumDecimalPlacesToDisplay (0);
    configCombo (shepRiseBox, "shepRise", shepRiseAtt, cTuning, {});

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
    // One beating mode (its two former entries are the SOURCE selector below) plus
    // the segment picker.
    specModeBox.addItem ("Find tunings based on beating", 1);
    specModeBox.addItem ("Adapt spectrum to tuning based on beating", 2);
    specModeBox.addItem ("From spectrum's segment", 3);
    tuneSourceBox.addItem ("From current tuning", 1);
    tuneSourceBox.addItem ("From all possible tunings", 2);
    tuneSourceBox.setSelectedId (1, juce::dontSendNotification);
    tuneScopeBox.addItem ("0 vs all", 1);
    tuneScopeBox.addItem ("all vs all", 2);
    tuneScopeBox.setTooltip (
        "How a combination is SCORED.\n"
        "*0 vs all* - each note is judged only by its beating against the root (0).\n"
        "*all vs all* - every pair of notes in the combination counts.");
    tuneScopeBox.setSelectedId (2, juce::dontSendNotification); // "all vs all" by default
    for (auto* b : { &tuneSourceBox, &tuneScopeBox })
    {
        b->setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
        b->setMouseClickGrabsKeyboardFocus (false);
        b->onChange = [this] { syncRealtimeLink(); resized(); repaint(); refocus(); };
        addChildComponent (*b);
    }
    specModeBox.setSelectedId (processorRef.isSegmentModeActive() ? 3 : 1, juce::dontSendNotification); // restored state
    specModeBox.setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
    specModeBox.setMouseClickGrabsKeyboardFocus (false);
    lastSpecMode = specModeBox.getSelectedId();
    specModeBox.onChange = [this]
    {
        switchPeriodTextForMode (specModeBox.getSelectedId());
        const bool seg = specModeBox.getSelectedId() == 3;
        processorRef.setSegmentModeActive (seg);
        if (seg) processorRef.rebuildSegmentScale();
        // Leaving a mode leaves its realtime link: syncRealtimeLink sees the new mode and
        // switches it off, which is also what COMMITS its one undo step (see
        // FractalAdditiveAudioProcessor::commitRealtimeBaseline) - so a realtime session
        // can never be abandoned half-way with no way back.
        syncRealtimeLink();
        resized(); refocus();
    };
    addAndMakeVisible (specModeBox);

    // --- Adapt-spectrum mode (2): target, partial count, goal, range, amplitudes ---
    adaptTargetBox.addItem ("To the loaded tuning", 1);
    adaptTargetBox.addItem ("To the chord held now", 2);
    adaptTargetBox.setSelectedId (1, juce::dontSendNotification);
    for (const int n : { 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24 })
        adaptPartialsBox.addItem (juce::String (n), n);
    adaptPartialsBox.setSelectedId (8, juce::dontSendNotification);
    adaptGoalBox.addItem ("Min", 1);
    adaptGoalBox.addItem ("Max", 2);
    adaptGoalBox.addItem ("Range", 3);
    adaptGoalBox.setSelectedId (1, juce::dontSendNotification);
    for (auto* b : { &adaptTargetBox, &adaptPartialsBox, &adaptGoalBox })
    {
        b->setColour (juce::ComboBox::outlineColourId, cSpecScale.withAlpha (0.7f));
        b->setMouseClickGrabsKeyboardFocus (false);
        b->onChange = [this] { syncRealtimeLink(); resized(); repaint(); refocus(); };
        addChildComponent (*b);
    }
    for (auto* ed : { &adaptLoEdit, &adaptHiEdit })
    {
        ed->setText (ed == &adaptLoEdit ? "0" : "1", juce::dontSendNotification);
        ed->setJustification (juce::Justification::centred);
        ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
        ed->setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
        ed->setIndents (4, 2);
        ed->setInputRestrictions (8, "0123456789.-");
        ed->setSelectAllWhenFocused (true);
        ed->onReturnKey  = [this] { syncRealtimeLink(); refocus(); };
        ed->onFocusLost  = [this] { syncRealtimeLink(); };
        addChildComponent (*ed);
    }
    // --- The REGISTER RANGE the adaptation is optimised over -------------------
    // ONE number: N STEPS of the loaded tuning, counted UP from the note being played (see
    // the block comment at registerDegreesPerPeriod). A step is the only unit every tuning
    // this instrument loads really has, and an anchor the hands move makes the range follow
    // the register actually being played instead of a pair of fixed pitches.
    {
        auto* ed = &adaptRegStepsEdit;
        ed->setJustification (juce::Justification::centred);
        ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
        ed->setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
        ed->setIndents (4, 2);
        // A COUNT: up to three digits, no sign. The range runs upward from the note being
        // played, so a negative width is not a direction, it is a typo - and the old field
        // admitted "-" only because a period NUMBER could legitimately be negative.
        ed->setInputRestrictions (3, "0123456789");
        ed->setSelectAllWhenFocused (true);
        ed->onReturnKey = [this] { commitAdaptRegisterSteps(); refocus(); };
        ed->onFocusLost = [this] { commitAdaptRegisterSteps(); };
        addChildComponent (*ed);
    }
    makeLabel (adaptPartialsLbl, cSpecScale); adaptPartialsLbl.setVisible (false);
    makeLabel (adaptRangeLbl,    cSpecScale); adaptRangeLbl.setVisible (false);
    makeLabel (adaptRegLbl,      cSpecScale); adaptRegLbl.setVisible (false);
    // A size smaller than the other captions, kept from when three controls shared this row
    // (the caption, the field and an aggregate box that has since gone): the caption still
    // has to leave the field its full width in Catalan, and RhythmVerify measures that.
    adaptRegLbl.setFont (juce::Font (juce::FontOptions (10.0f).withStyle ("Bold")));
    // WHAT N AMOUNTS TO, and WHICH NOTE it is counted from - one line, under the field.
    // Without it N is a magic number: "62" says nothing about being two periods, and a
    // range that hangs off a note nobody named is a control the user cannot aim.
    adaptRegSpanLbl.setJustificationType (juce::Justification::topLeft);
    adaptRegSpanLbl.setColour (juce::Label::textColourId, cSpecScale.withAlpha (0.8f));
    // 9.5 pt, half a point under its neighbours: this line carries the widest numbers the
    // panel can produce (a one-degree-per-period scale reaches anchor "-10:0" and 127
    // steps), and at 10 pt the Catalan form of that overflowed its single line by 8 px.
    // A Label answers that by condensing to 70%, which is unreadable and looks intentional.
    adaptRegSpanLbl.setFont (juce::Font (juce::FontOptions (9.5f)));
    adaptRegSpanLbl.setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
    adaptRegSpanLbl.setMinimumHorizontalScale (1.0f); // it must FIT, not condense
    addChildComponent (adaptRegSpanLbl);
    // The default width, written DIRECTLY - not through syncAdaptRegisterSpan, which pushes
    // the panel's settings to the processor. Doing that here ran syncRealtimeLink() from
    // inside the constructor, before specModeBox and adaptRtOn had been restored, so
    // reopening the window while a REALTIME link was running computed "no link wanted" and
    // switched the user's running follower off. The timer picks it up on its first tick,
    // when the panel really does know what it is showing.
    adaptRegStepsEdit.setText (juce::String (registerStepsEffective()), juce::dontSendNotification);
    adaptInfoLbl.setJustificationType (juce::Justification::topLeft);
    adaptInfoLbl.setColour (juce::Label::textColourId, cSpecScale.brighter (0.2f));
    adaptInfoLbl.setFont (juce::Font (juce::FontOptions (10.5f)));
    adaptInfoLbl.setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
    adaptInfoLbl.setMinimumHorizontalScale (1.0f); // same rule: fit or say less
    addChildComponent (adaptInfoLbl); // its bilingual tooltip is set in applyLanguage
    adaptAmpsToggle.setToggleState (true, juce::dontSendNotification);
    adaptAmpsToggle.setMouseClickGrabsKeyboardFocus (false);
    adaptAmpsToggle.onClick = [this] { syncRealtimeLink(); refocus(); };
    addChildComponent (adaptAmpsToggle);
    adaptGoBtn.setColour (juce::TextButton::buttonColourId, cSpecScale.withAlpha (0.85f));
    // ONE button, two jobs, and the second one is what makes an expensive search safe to
    // offer at all: while a run is going this STOPS it. Pressing it used to start another,
    // and startSpectrumAdapt joins the running thread first - so a heavy search froze the
    // window until it noticed the cancel flag. Cancelling only sets the flag; the worker
    // sees it at its next progress callback and drops its own partial result.
    adaptGoBtn.onClick = [this]
    {
        if (processorRef.isSpectrumAdaptRunning()) processorRef.cancelSpectrumAdapt();
        else                                       runSpectrumAdapt();
        refreshAdaptButtonText();
        refocus();
    };
    addChildComponent (adaptGoBtn);
    adaptView.setViewedComponent (&adaptList, false);
    adaptView.setScrollBarsShown (true, false);
    addChildComponent (adaptView);

    // The two REALTIME buttons. They are plain toggles in the editor; everything that
    // makes them safe - the throttle, the coalescing, the feedback-loop refusal and the
    // single undo step - lives in the processor, so it also works with this window shut.
    for (auto* b : { &chordRtBtn, &adaptRtBtn, &lookaheadBtn })
    {
        b->setMouseClickGrabsKeyboardFocus (false);
        addChildComponent (*b);
    }
    chordRtBtn.onClick = [this] { chordRtOn = ! chordRtOn; syncRealtimeLink(); resized(); refocus(); };
    adaptRtBtn.onClick = [this]
    { adaptRtOn = ! adaptRtOn; syncAdaptTargetLock(); syncRealtimeLink(); resized(); refocus(); };
    // LOOKAHEAD rides on the same syncRealtimeLink path as everything else on this panel,
    // so the processor learns about it through the settings it already receives on every
    // touch - and re-reports its latency from there (see applyLookahead).
    lookaheadBtn.onClick = [this] { lookaheadOn = ! lookaheadOn; syncRealtimeLink(); resized(); refocus(); };
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
        // A running realtime link searches with whatever these say NOW, so every one of
        // them has to reach the processor the moment it changes - otherwise the follower
        // would keep using the note count or the Min/Max the panel had when it engaged.
        cb->onChange = [this] { syncRealtimeLink(); };
        addChildComponent (*cb);
    }
    makeLabel (chordNotesLbl, cSpecScale);
    chordNotesLbl.setVisible (false);
    makeLabel (chordPerLbl, cSpecScale);
    chordPerLbl.setVisible (false);
    chordPeriodEdit.setText (periodTextByMode[specModeBox.getSelectedId() == 2 ? 1 : 0],
                             juce::dontSendNotification);
    chordPeriodEdit.setJustification (juce::Justification::centred);
    chordPeriodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    chordPeriodEdit.setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
    chordPeriodEdit.setIndents (8, 2);
    chordPeriodEdit.setSelectAllWhenFocused (true);
    chordPeriodEdit.onReturnKey = [this] { syncRealtimeLink(); refocus(); };
    chordPeriodEdit.onEscapeKey = [this] { refocus(); };
    chordPeriodEdit.onFocusLost = [this] { syncRealtimeLink(); };
    addChildComponent (chordPeriodEdit);
    chordGoBtn.setColour (juce::TextButton::buttonColourId, cSpecScale.withAlpha (0.85f));
    chordGoBtn.onClick = [this] { runChordSearch(); };
    addChildComponent (chordGoBtn);
    // Minimum interval between generated notes (cents) - both Sethares creators.
    for (auto pr : { std::pair<juce::TextEditor*, juce::Label*> { &dissMinEdit, &dissMinLbl },
                     std::pair<juce::TextEditor*, juce::Label*> { &chordMinEdit, &chordMinLbl } })
    {
        auto* ed = pr.first;
        ed->setText ("50", juce::dontSendNotification);
        ed->setJustification (juce::Justification::centred);
        ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
        ed->setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
        ed->setIndents (4, 2);
        ed->setInputRestrictions (5, "0123456789.");
        ed->setSelectAllWhenFocused (true);
        ed->onReturnKey  = [this] { syncRealtimeLink(); refocus(); };
        ed->onFocusLost  = [this] { syncRealtimeLink(); };
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
    // Hover a row to hear it (no play button): the chord search results, the n-ad
    // combinations of the current tuning, and the per-degree dyads all behave the same.
    chordList.onHover = [this] (int row)
    {
        const auto res = processorRef.getChordResults();
        if (! juce::isPositiveAndBelow (row, (int) res.size())) { hoverPreview ({}); return; }
        hoverPreview (res[(size_t) row].alphas);
    };
    nadList.onHover = [this] (int row)
    {
        if (! juce::isPositiveAndBelow (row, (int) nadList.rows.size())) { hoverPreview ({}); return; }
        std::vector<double> r;
        for (const int d : nadList.rows[(size_t) row].degs) r.push_back (processorRef.getDegreeRatio (d));
        hoverPreview (r);
    };
    degreeList.onHover = [this] (int row)
    {
        if (! juce::isPositiveAndBelow (row, (int) degreeList.rows.size())) { hoverPreview ({}); return; }
        hoverPreview ({ 1.0, degreeList.rows[(size_t) row].ratio });  // the dyad 0 + this degree
    };
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
    addChildComponent (nadBox); // kept for state; the shared "Notes" box drives it now
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
    // "adapta la caja de custom ratios para que los numeros no toquen su borde."
    // A TextEditor's clearances are NOT symmetric out of the box: setIndents gives a left
    // and a TOP indent with no bottom or right twin, and the default border is
    // {1,1,1,3} - so the last row of digits was drawn one pixel from the outline and the
    // first row started inside the pill's arc. The border is what actually CLIPS, so the
    // room on all four sides is set there, and the indents only add to the two sides that
    // have them. Measured by ratiosInkProbeForTests, which reads these values back out of
    // JUCE rather than repeating them.
    intervalEdit.setBorder (juce::BorderSize<int> (4, 5, 5, 6)); // top, left, bottom, right
    intervalEdit.setIndents (4, 2);
    intervalEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
    // A real multi-line text AREA now: the shared engine box hands the custom view far
    // more room than the old one-line strip, so a long interval list wraps and stays
    // readable instead of scrolling away sideways. Return still BUILDS (it never
    // inserts a newline) - the parser has always split on newlines anyway, so pasted
    // multi-line lists keep working unchanged.
    intervalEdit.setMultiLine (true, true);
    intervalEdit.setReturnKeyStartsNewLine (false);
    intervalEdit.setScrollbarsShown (true);
    intervalEdit.onReturnKey = [this]
    {
        juce::String err;
        if (intervalEdit.getText().trim().isNotEmpty())
        {
            if (processorRef.loadTimbreTextUndoable (intervalEdit.getText(), err))
            {
                // Same as loading a .scl: re-read which engine is in force and redress
                // the panel for it immediately, not on the next tick. It is ASKED, not
                // assumed: this used to hard-code "a custom scale now rules", which was
                // wrong whenever temper was on - typing intervals then greyed a harmonic
                // engine that was still the one making the sound.
                applyEngineState();
                resized();
            }
            else
                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                    tr ("Spectrum Builder", "Constructor d'espectre"), err, this);
        }
        refocus();
    };
    intervalEdit.onEscapeKey = [this] { refocus(); };
    // Every value in its own colour, re-done on each edit. See recolourIntervalTokens().
    intervalEdit.onTextChange = [this] { recolourIntervalTokens(); };
    // No focus-grow any more (the box is already big); on blur just show the start,
    // so a long list never sits scrolled to its tail looking like it begins mid-way.
    intervalEdit.onFocusChanged = [this] (bool focused)
    {
        if (! focused)
            intervalEdit.setCaretPosition (0);
    };
    addAndMakeVisible (intervalEdit);
    // Show the typed spectrum that is ALREADY loaded (a reopened window used to come
    // up blank even though the text was sitting in the state). Only for a typed one:
    // a .scl-derived timbre keeps its own name, not an interval list.
    if (processorRef.getTimbreName() == "typed")
    {
        setIntervalText (processorRef.getTimbreSource());
        lastSeenTimbreSource = processorRef.getTimbreSource();
    }

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
        // Floating window: EVERY key answers. A degree with no just interval near
        // enough to approximate used to return nothing at all, which read as a
        // broken hover - now it says so, and names its closest neighbour anyway.
        const auto di = analyzeDegree (processorRef, d);
        if (processorRef.getDegreeRatio (d) <= 0.0)
            return {};

        if (di.genuine) // a true just interval: the key shows the ratio; add its cents
            return "= " + juce::String (di.cents, 1) + tr (" cents", " cents");

        juce::String s;
        s << juce::String (di.cents, 1) << tr (" cents", " cents");
        if (di.hasApprox) // tempered, near a simple ratio: the hint the "*" promises
            s << "\n\xe2\x89\x88 " << juce::String (di.ap) << "/" << juce::String (di.aq)
              << " (" << (di.approxErr >= 0 ? "+" : "-") << juce::String (std::abs (di.approxErr)) << "¢)";
        else if (di.hasNear) // too far to claim it, but the user still gets told
            s << tr ("\nno just interval this close.\nthe nearest simple one is ",
                     "\ncap interval just tan a prop.\nel simple m\xc3\xa9s proper \xc3\xa9s ")
              << juce::String (di.np) << "/" << juce::String (di.nq)
              << " (" << (di.nearErr >= 0 ? "+" : "-") << juce::String (std::abs (di.nearErr)) << "¢)";
        else
            s << tr ("\nno simple just interval anywhere near it.",
                     "\ncap interval just simple a prop.");
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
        // Works from ANY tuning, exactly like Ctrl+click's "add a note": the edit
        // renders the CURRENT tuning to .scl form and reloads it. Refusing unless
        // the mode already happened to be Scala made right-click look broken - it
        // silently did nothing, which is now the common case since MTS-ESP became
        // the default mode.
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
        // Delete takes the degree out of the scale, so the key disappears from
        // EVERY period, not just the one that was right-clicked.
        w->addButton (tr ("Delete", "Esborra"), 2);
        w->addButton (tr ("Cancel", "Cancel\xc2\xb7la"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, safe = juce::Component::SafePointer<juce::AlertWindow> (w), deg] (int r)
            {
                if (safe == nullptr) return;
                const auto txt = safe->getTextEditorContents ("v");
                juce::String err;
                if (r == 1 && ! processorRef.editTuningDegree (deg, txt, err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        tr ("Edit degree", "Edita el grau"), err, this);
                if (r == 2 && ! processorRef.deleteTuningDegree (deg, err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        tr ("Delete degree", "Esborra el grau"), err, this);
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
        // The same window as Edit, minus Delete: the spot that was clicked is only
        // the STARTING value - the interval against 0 is typed, exactly.
        const auto pre = juce::String (1200.0 * std::log2 (juce::jmax (1.0e-9, f / f0)), 3);
        auto* w = new juce::AlertWindow (tr ("Add note", "Afegeix nota"),
            tr ("Interval from 0: cents with a decimal point (\"702.0\") or a ratio (\"3/2\"). It joins every period.",
                "Interval respecte 0: cents amb punt decimal (\"702.0\") o un ratio (\"3/2\"). S'afegeix a tots els per\xc3\xado" "des."),
            juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("v", pre);
        w->addButton (tr ("OK", "OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (tr ("Cancel", "Cancel\xc2\xb7la"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [this, safe = juce::Component::SafePointer<juce::AlertWindow> (w)] (int r)
            {
                if (safe == nullptr) return;
                const auto txt = safe->getTextEditorContents ("v").trim();
                if (r == 1)
                {
                    double ratio = 0.0;
                    TuningManager::DegreeSrc src;
                    juce::String err;
                    if (! TuningManager::parsePitchSpec (txt, ratio, src) || ! (ratio > 0.0))
                        err = tr ("Invalid value. Use cents with a decimal point (\"702.0\") or a ratio (\"3/2\").",
                                  "Valor inv\xc3\xa0lid. Fes servir cents amb punt decimal (\"702.0\") o un ratio (\"3/2\").");
                    else
                        processorRef.addNoteToTuning (ratio, err, txt);
                    if (err.isNotEmpty())
                        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                            tr ("Add note", "Afegeix nota"), err, this);
                }
                refocus();
            }), true);
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

    // ADOPT A REALTIME LINK THAT IS ALREADY RUNNING.
    //
    // The link lives in the PROCESSOR and deliberately keeps following with this window
    // shut - the throttle, the coalescing and the single undo step are all down there.
    // But a reopened editor built its panel from scratch: mode box on "Find tunings",
    // both REALTIME flags false. So the follower went on rewriting the spectrum under
    // every chord with NOTHING lit to say so - the exact state syncRealtimeLink()'s rule
    // ("a link may never run invisibly behind another panel") exists to forbid - and the
    // first press of REALTIME only caught the button up with the link, so it took two
    // presses to stop it. Worse, any unrelated touch on the panel (the scope box, the
    // period field) called syncRealtimeLink() and switched the user's follower off
    // without being asked. Adopting it here makes the panel tell the truth on open.
    // Not while the segment picker is the active mode: the mode box belongs to it there,
    // and the processor refuses a spectrum link in segment mode anyway.
    {
        using RtLink = FractalAdditiveAudioProcessor::RtLink;
        const auto live = processorRef.getRealtimeLink();
        if (live != RtLink::Off && ! processorRef.isSegmentModeActive())
        {
            const int mode = live == RtLink::SpectrumFromChord ? 2 : 1;
            specModeBox.setSelectedId (mode, juce::dontSendNotification);
            lastSpecMode = mode;
            chordPeriodEdit.setText (periodTextByMode[mode == 2 ? 1 : 0], juce::dontSendNotification);
            if (live == RtLink::SpectrumFromChord) { adaptRtOn = true; syncAdaptTargetLock(); }
            else { chordRtOn = true; tuneSourceBox.setSelectedId (2, juce::dontSendNotification); }
        }
        // The LOOKAHEAD toggle is adopted whether or not a link is running: it is part of
        // the saved realtime settings, and a reopened panel showing it OFF while the
        // processor is reporting latency would be the same lie the block above fixes.
        lookaheadOn = processorRef.isLookaheadArmed();
    }

    applyLanguage();
    applyEngineState(); // sets lastEngineShown, so the timer only re-does it on a change

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
    // The amplitude window is a real top-level window: it must go before the panel it
    // shows (which is a member of this editor) and before the editor leaves the desktop,
    // or a host that keeps the process alive is left with an orphaned window pointing at
    // freed content.
    partialWindow.reset();
    partialPanel.setLookAndFeel (nullptr); // it borrowed this editor's lnf (see the ctor)
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
// NEVER MOVE THE KEYBOARD FOCUS WHILE A MOUSE BUTTON IS DOWN.
//
// "mover controles con notas apretadas sigue siendo buggy, a veces no deja mover el
//  control y a veces si, solo si has movido el control un instante antes de empezar a
//  tocar teclas"
//
// A drag is not just a stream of events, it is a mouse CAPTURE held by the window the
// press landed in. When the OS revokes that capture, JUCE does not report an error - it
// synthesises a mouse-UP (HWNDComponentPeer::doCaptureChanged) and the gesture is simply
// over, mid-drag, with the control left wherever it happened to be. A focus change is
// one of the things that can take a capture away when the editor is a child window
// inside a host, and this editor asks for the keyboard from ~40 places, including from
// MOUSE MOVEMENT (mouseEnter -> reclaimFocusUnlessTyping) - i.e. in the instant before
// every press, and only when the focus is not already ours. That is exactly the
// asymmetry in the report: moving a control first leaves the focus with us
// (onDragEnd -> refocus), so the grab is a no-op and the next drag is undisturbed.
//
// A focus grab in the middle of a press can never help, so it does not happen. The
// button-down test is Desktop's own: it is false again by the time mouseUp is
// dispatched (setButtons clears buttonState before sending it), so onDragEnd, onClick
// and every modal callback still take the keyboard back exactly as before.
bool FractalAdditiveAudioProcessorEditor::mouseButtonHeld()
{
    return juce::Desktop::getInstance().getNumDraggingMouseSources() > 0;
}

void FractalAdditiveAudioProcessorEditor::refocus()
{
    if (mouseButtonHeld()) return;
    grabKeyboardFocus();
}

FractalAdditiveAudioProcessorEditor::FocusReclaimProbe
FractalAdditiveAudioProcessorEditor::focusReclaimState() const
{
    FocusReclaimProbe s;
    // Typing wins: letters belong to the text field, never to the keyboard mapping.
    s.typing      = dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;
    s.mouseHeld   = mouseButtonHeld();
    s.alreadyOurs = hasKeyboardFocus (true);
    // ...and at most one attempt per debounce window. The reclaim runs off mouseEnter,
    // which fires every time the pointer crosses between the editor and any of its ~90
    // children; while the focus is NOT ours (a host that wants it back) that is an
    // unbounded retry, once per mouse move, each one a focus change. The same 250 ms the
    // play-state debounce uses, and for the same reason: below that it is a flicker.
    s.tooSoon     = (juce::Time::getMillisecondCounter() - lastFocusReclaimMs) < 250;
    s.wouldGrab   = ! s.typing && ! s.mouseHeld && ! s.alreadyOurs && ! s.tooSoon;
    return s;
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
    rhythmBtn.setButtonText (tr ("Spectrum vs Rhythm", "Espectre vs ritme"));
    partialEditBtn.setTooltip (tr (
        "Draw the spectrum by hand: opens a resizable window with *one knob per partial* of the current sound. Every partial is there, however many - nothing is hidden or capped.\nThe switch below is the same switch as the one inside the window; while it is on, !SLOPE is frozen!.",
        "Dibuixa l'espectre a m\xc3\xa0: obre una finestra redimensionable amb *un control per parcial* del so actual. Hi ha tots els parcials, siguin quants siguin - res amagat ni limitat.\nL'interruptor de sota \xc3\xa9s el mateix que el de dins la finestra; mentre estigui actiu, !el PENDENT queda congelat!."));
    partialManualT.setTooltip (tr (
        "Manual per-partial amplitudes on/off. The SAME switch as the one inside the pencil's window - one control in two places. Off by default, but your first drag on a partial knob switches it on for you (otherwise that drag would be stored and never heard); while it is on, !SLOPE is frozen! where it stood and the amplitudes come from the knobs.",
        "Activa/desactiva les amplituds manuals per parcial. \xc3\x89s EL MATEIX interruptor que el de dins la finestra del llapis - un control en dos llocs. Apagat per defecte, per\xc3\xb2 el teu primer arrossegament d'un parcial l'activa (si no, aquell arrossegament es desaria i no sonaria mai); mentre estigui actiu, !el PENDENT queda congelat! on era i les amplituds venen dels controls."));
    if (partialWindow != nullptr)
        partialWindow->setName (tr ("Fibl\xc3\xb3 - partial amplitudes", "Fibl\xc3\xb3 - amplituds dels parcials"));
    partialPanel.applyLanguage();
    lastManualFrozenShown = -1;   // re-apply the slope fader's tooltip in the new language
    lastEngineShown       = -1;   // and re-light the engine chips with the new words
    rhythmPanel.heading.setText (tr ("SPECTRUM VS RHYTHM", "ESPECTRE VS RITME"), dontSendNotification);
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
        "Monophonic: when a new note starts, the note currently sounding stops. Off = polyphonic.\nThis one CUTS the old note (a restrike at the new pitch). \"Monophonic glide\" on the PITCH tab is the other kind - it BENDS the old note to the new one - and while that is on this greys out, because cutting the note first is exactly what stops it gliding.",
        "Monof\xc3\xb2nic: quan comen\xc3\xa7""a una nota nova, la que sonava s'atura. Desactivat = polif\xc3\xb2nic.\nAquest TALLA la nota antiga (un atac nou a la nova al\xc3\xa7""ada). El \"Glide monof\xc3\xb2nic\" de la pestanya PITCH \xc3\xa9s l'altra mena - DOBLEGA la nota antiga fins a la nova - i mentre aquell estigui actiu aquest queda desactivat, perqu\xc3\xa8 tallar la nota primer \xc3\xa9s justament el que impedeix que llisqui."));
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
    // The SPECTRUM panel's two engine chips. Their tooltips spell out what each state
    // does to the OTHER engine, and where temper sits now - that is the question the
    // panel exists to answer.
    engineChip[0].setButtonText (tr ("HARMONIC", "HARM\xc3\x92NIC"));
    engineChip[1].setButtonText (tr ("CUSTOM",   "PROPI"));
    engineChip[0].setTooltip (tr (
        "The HARMONIC ENGINE builds the spectrum. Click to make it the one that sounds AND to show its controls in this box - the whole panel takes its colour. The loaded custom scale is KEPT, just not used. It also stays lit with *Temper to tuning* on, because the harmonic engine is still the one generating every partial there.",
        "El MOTOR HARM\xc3\x92NIC fa l'espectre. Clica perqu\xc3\xa8 sigui el que sona I perqu\xc3\xa8 aquesta caixa en mostri els controls - tot el panell n'agafa el color. L'escala pr\xc3\xb2pia carregada es CONSERVA, nom\xc3\xa9s que no s'usa. Tamb\xc3\xa9 queda enc\xc3\xa8s amb *Tempera a l'afinaci\xc3\xb3* activat, perqu\xc3\xa8 all\xc3\xa0 el motor harm\xc3\xb2nic segueix generant tots els parcials."));
    engineChip[1].setTooltip (tr (
        "The CUSTOM ENGINE's scale IS the spectrum. Click to make it the one that sounds AND to show its controls in this box - the whole panel takes its colour (the harmonic engine's settings are kept, never reset). With no scale yet, the click opens this engine's box so you can load or type one. *Temper to tuning* beats it.",
        "L'escala del MOTOR PROPI \xc3\x89S l'espectre. Clica perqu\xc3\xa8 sigui el que sona I perqu\xc3\xa8 aquesta caixa en mostri els controls - tot el panell n'agafa el color (els ajustos del motor harm\xc3\xb2nic es conserven, mai no es reinicien). Sense cap escala encara, el clic obre la caixa d'aquest motor perqu\xc3\xa8 en carreguis o escriguis una. *Tempera a l'afinaci\xc3\xb3* hi guanya."));
    intervalEdit.setTextToShowWhenEmpty (tr ("intervals: 3/2 2 700c ...", "intervals: 3/2 2 700c ..."),
                                         juce::Colour (0xff8f96a8));
    gSpecScale.setText     (tr ("Spectrum vs Tuning", "Espectre vs afinaci\xc3\xb3"));
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
    lblPrimeLimit.setText(tr ("Limit", "L\xc3\xadmit"), dontSendNotification);
    lblCutoff.setText    (tr ("Cut", "Tall"), dontSendNotification);
    lblRes.setText       (tr ("Res", "Res"), dontSendNotification);
    lblEnvAmt.setText    (tr ("Env", "Env"), dontSendNotification);
    lblPitchAmt.setText  (tr ("St", "St"), dontSendNotification);
    lblGlide.setText     (tr ("Glide", "Glide"), dontSendNotification);
    monoGlideT.setButtonText (tr ("Monophonic glide", "Glide monof\xc3\xb2nic"));
    monoGlideT.setTooltip (tr (
        "MONOPHONIC GLIDE. ON, the synth plays ONE note at a time - it cannot make chords - and a new key does not start a new voice: it TAKES OVER the sounding one and BENDS its pitch there over the Glide time, through the tuning's own frequencies (per-note bend included), never through 12-TET semitones.\nLAST-NOTE priority: the newest key always wins, so pressing a second note always moves the pitch.\nLegato does NOT retrigger: while a key is held, the takeover keeps the envelopes, the modulators and a running Shepard exactly where they are - the glide IS the articulation. Lift every key and the next note starts fresh, at its own pitch, with everything retriggered.\nRelease the key you are on while another is still down and the pitch glides BACK to the most recently pressed key still held.\nIn playground MIX mode every corner engine is mono too, on the same note stream - \"Mix Voices per Corner\" reads 1 and waits while this is on.",
        "GLIDE MONOF\xc3\x92NIC. Actiu, el sinte toca UNA nota alhora - no pot fer acords - i una tecla nova no engega cap veu nova: PREN la que sona i li DOBLEGA l'al\xc3\xa7""ada fins all\xc3\xa0 en el temps de Glide, per les freq\xc3\xbc\xc3\xa8ncies de la mateixa afinaci\xc3\xb3 (amb el bend per nota inclos), mai per semitons de 12-TET.\nPrioritat a l'\xc3\x9aLTIMA nota: la tecla m\xc3\xa9s nova sempre guanya, o sigui que prement-ne una segona l'al\xc3\xa7""ada sempre es mou.\nEl legato NO redispara: mentre mantens una tecla, la presa de rel\xc3\xa8u deixa les envolupants, els moduladors i un Shepard en marxa exactament on eren - el glide \xc3\x89S l'articulaci\xc3\xb3. Deixa anar totes les tecles i la nota seg\xc3\xbc" "ent comen\xc3\xa7""a de nou, a la seva al\xc3\xa7""ada i amb tot redisparat.\nDeixa anar la tecla on ets mentre en mantens una altra i l'al\xc3\xa7""ada torna lliscant a la darrera tecla premuda que encara mantens.\nEn mode MIX del playground cada motor de cantonada tamb\xc3\xa9 \xc3\xa9s mono, sobre el mateix flux de notes - \"Veus per cantonada\" marca 1 i espera mentre aix\xc3\xb2 estigui actiu."));
    glideFader.setTooltip (tr (
        "How long the pitch takes to travel from the previous note to the new one, in milliseconds. It is a TIME, not a rate: two octaves take exactly as long as a semitone. The fader is logarithmic (1 ms to 5 s, half its travel below 100 ms). Only does anything while Monophonic glide is on.",
        "Quant triga l'al\xc3\xa7""ada a anar de la nota anterior a la nova, en millisegons. \xc3\x89s un TEMPS, no una velocitat: dues octaves triguen exactament el mateix que un semit\xc3\xb2. El fader \xc3\xa9s logar\xc3\xadtmic (1 ms a 5 s, mitja cursa per sota de 100 ms). Nom\xc3\xa9s fa res amb el Glide monof\xc3\xb2nic actiu."));
    lblMorphTime.setText (tr ("Time", "Temps"), dontSendNotification);
    // No text: the painted MORPH sub-label above it is the name, and the toggle saying
    // "Morph" under a heading that says MORPH read as two different controls.
    morphToggle.setButtonText ({});
    // The tooltip must name the SCOPE: morph rides on every spectrum change from any
    // source. Users read it as a Base-only glide, because Base is where they first
    // hear it - so the causes are spelled out.
    morphToggle.setTooltip (tr (
        "Morph ON: whenever the spectrum CHANGES - for ANY reason: Base, Harmonics, Prime, Fractal, Range, switching engine, loading or typing a scale, an adapted spectrum from Spectrum vs Tuning, a modulator moving any of those - every partial GLIDES from its old position to its place in the new spectrum over the Time below, instead of jumping. OFF: every change lands instantly.",
        "Morph ACTIU: quan l'espectre CANVIA - per QUALSEVOL motiu: Base, Harm\xc3\xb2nics, Primers, Fractal, Abast, canviar de motor, carregar o escriure una escala, un espectre adaptat d'Espectre vs afinaci\xc3\xb3, un modulador que mogui qualsevol d'aquests - cada parcial LLISCA de la seva posici\xc3\xb3 antiga al seu lloc del nou espectre durant el Temps de sota, en comptes de saltar-hi. APAGAT: cada canvi cau a l'instant."));
    morphTimeFader.setTooltip (tr (
        "How long the glide takes, in seconds. It applies to EVERY spectrum change, whatever caused it (Base, Harmonics, Prime, Fractal, engine switch, an adapted spectrum, a modulator...) - not just Base.",
        "Quant dura el lliscament, en segons. S'aplica a QUALSEVOL canvi d'espectre, el causi el que el causi (Base, Harm\xc3\xb2nics, Primers, Fractal, canvi de motor, un espectre adaptat, un modulador...) - no nom\xc3\xa9s Base."));
    lblTimbreRepeats.setText (tr ("Reps", "Reps"), dontSendNotification);
    lblBend.setText      (tr ("Pitch bend", "Pitch bend"), dontSendNotification);
    lblPeriod.setText    (tr ("Period", "Per\xc3\xado" "de"), dontSendNotification);
    lblSteps.setText     (tr ("Notes", "Notes"), dontSendNotification);
    lblRefA.setText      ("C3 =", dontSendNotification); // the tuning anchor key itself, not A
    lblTuningBank.setText (tr ("Bank", "Banc"), dontSendNotification);
    degreeList.tip = tr (
        "Each row is a DYAD: the red \"0\" chip (the root key) plus that degree, played together. Colour chip = the degree's keyboard colour, then its interval from 0, then the dyad's Sethares BEATING against the current timbre (how rough 0 + that degree sound together) - lower = more consonant. Ordered most consonant first. The period is the red row.",
        "Cada fila \xc3\xa9s una D\xc3\x8d""ADA: el xip vermell \"0\" (la tecla arrel) m\xc3\xa9s aquell grau, tocats junts. Xip de color = el color del grau al teclat, despr\xc3\xa9s el seu interval des de 0, i el BATEC de Sethares de la d\xc3\xad""ada contra el timbre actual (com d'aspre sonen 0 + aquell grau junts) - m\xc3\xa9s baix = m\xc3\xa9s consonant. Ordenats de m\xc3\xa9s consonant primer. El per\xc3\xado""de \xc3\xa9s la fila vermella.");
    dissHeader.tip = tr (
        "Sethares dissonance: how ROUGH each scale step sounds with the CURRENT timbre. It is not fixed - it recomputes live from the synth's actual partials, so editing the timbre (N, fractal, decay, primes, .scl...) changes these values in real time. Lower = more consonant; the rows are ordered most consonant first.",
        "Dissonancia de Sethares: com d'ASPRE sona cada pas de l'escala amb el timbre ACTUAL. No \xc3\xa9s fixa - es recalcula en viu a partir dels parcials reals del sinte, aix\xc3\xad que editar el timbre (N, fractal, caiguda, primers, .scl...) canvia aquests valors en temps real. M\xc3\xa9s baix = m\xc3\xa9s consonant; les files van de m\xc3\xa9s consonant primer.");
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);
    recentOpenBtn.setButtonText (tr ("Recent...", "Recents..."));
    recentOpenBtn.setTooltip (tr ("Every scale file loaded in this plugin, newest first. Click one to load it.",
                                  "Tots els fitxers d'""escala carregats en aquest plugin, del més recent al més antic. Clica'n un per carregar-lo."));
    recentOverlay.title = tr ("Recent scales", "Escales recents");

    // These two used to BOTH read "Load .scl" in adjacent boxes, and clicking the
    // wrong one greys out the whole Harmonic Engine (it loads a TIMBRE), which reads
    // as the plugin breaking. They say what they load now.
    loadSclButton.setButtonText    (tr ("Load", "Obrir"));
    loadSclButton.getProperties().set ("fork", true);
    clearSclButton.setButtonText   (tr ("Reset", "Reinicia"));
    // "Add to Bank", not "+". A lone plus beside Load and Save named neither the thing
    // it adds nor where it adds it, and the two other + buttons on screen (the octave
    // steppers, and the bank's own "+ .scl") add something else entirely.
    tuningBankAddBtn.setButtonText (tr ("Add to Bank", "Afegeix al banc"));
    tuningBankAddBtn.getProperties().set ("fork", true);
    loadItsSpectrumBtn.setButtonText (tr ("Load its original spectrum", "Carrega el seu espectre original"));
    loadItsSpectrumBtn.setTooltip (tr (
        "This scale was generated by Fibló Galà""ctic and carries the patch it was made with. This brings that SOUND back; the tuning you just loaded stays as it is.",
        "Aquesta escala l'ha generada Fibló Galà""ctic i porta a dins el patch amb què es va fer. Això en recupera el SO; l'""afinació que acabes de carregar es queda com està."));
    loadPresetTuningBtn.setButtonText (tr ("Load preset's tuning", "Carrega l'afinaci\xc3\xb3 del preset"));
    loadPresetTuningBtn.setTooltip (tr (
        "The preset you just loaded was saved with a *different tuning*.\nLoading a preset only changes the *sound*, !never the scale!. Click to bring that tuning in too.",
        "El preset que acabes de carregar es va desar amb una *afinaci\xc3\xb3 diferent*.\nCarregar un preset nom\xc3\xa9s canvia el *so*, !mai l'escala!. Clica per portar tamb\xc3\xa9 aquella afinaci\xc3\xb3."));
    refreshSpectrumTuningButton(); // its word/tooltip re-render in the new language (sig includes it)
    tuningBankLoadBtn.setButtonText (tr ("+ .scl", "+ .scl"));
    tablePeriodResetBtn.setButtonText (tr ("est.", "est."));
    tablePeriodResetBtn.setTooltip (tr ("Back to Fibló's own estimate of the period.",
                                        "Torna a l'estimaci\xc3\xb3 del per\xc3\xado" "de que fa Fibl\xc3\xb3."));
    tablePeriodEdit.setTooltip (tr (
        "This tuning is a *128-key table* (a .tun whose mapping does not repeat), so its !pitches are the file's own! and are never touched by this field.\nThe period is only needed by the features that assume one: keyboard colours, the snapping grid, the chord search and the dissonance lists. Fibló estimates it (the readout says how far the file strays); type a ratio (\"2\", \"3/2\") or cents (\"1200c\") to set your own, Return to apply.",
        "Aquesta afinaci\xc3\xb3 \xc3\xa9s una *taula de 128 tecles* (un .tun que no es repeteix), aix\xc3\xad que les !seves altures s\xc3\xb3n les del fitxer! i aquest camp no en toca cap.\nEl per\xc3\xado" "de nom\xc3\xa9s el necessiten les funcions que en pressuposen un: colors del teclat, graella d'imantaci\xc3\xb3, cerca d'acords i llistes de dissonància. Fibl\xc3\xb3 l'estima (el r\xc3\xa8tol diu quant se'n desvia el fitxer); escriu una ra\xc3\xb3 (\"2\", \"3/2\") o cents (\"1200c\") per posar-hi el teu, i Retorn per aplicar-lo."));
    // The custom view's Load/Remove and Random buttons: word, colour and tooltip in ONE
    // place (see refreshCustomEngineButtons) so the word can never disagree with what the
    // click does. While it loads, it loads a SCALE file - same fork, same word as the
    // tuning Load, so the pair reads as two destinations for one kind of file.
    refreshCustomEngineButtons();
    savePresetButton.setButtonText (tr ("Save", "Desa"));
    playgroundButton.setButtonText (tr ("Open Preset Playground", "Obre el Laboratori de Presets"));
    playgroundButton.setTooltip (tr (
        "*Preset Playground.* Put up to four presets on the corners of a rectangle and drag a point between them: everything you hear morphs, including !across engines! (a custom scale on one corner and the harmonic engine on another blend their compiled PARTIALS, because their knobs have nothing in common to average).\nThe point is two automatable parameters, so a move recorded in the DAW plays back with the window shut.\n!The tuning is never touched! - mode, scale, reference A, octave and bend range all stay exactly as you left them.",
        "*Laboratori de Presets.* Posa fins a quatre presets a les cantonades d'un rectangle i arrossega un punt entre ells: tot el que sents es transforma, !fins i tot entre motors! (una escala pr\xc3\xb2pia a una cantonada i el motor harm\xc3\xb2nic a una altra barregen els seus PARCIALS compilats, perqu\xc3\xa8 els seus controls no tenen res en com\xc3\xba per promitjar).\nEl punt s\xc3\xb3n dos par\xc3\xa0metres automatitzables, o sigui que un moviment gravat al DAW es reprodueix amb la finestra tancada.\n!L'afinaci\xc3\xb3 no es toca mai! - mode, escala, refer\xc3\xa8ncia A, octava i rang de bend queden exactament com els tenies."));
    playgroundPanel.applyLanguage();
    if (playgroundWindow != nullptr)
        playgroundWindow->setName (tr ("Fibl\xc3\xb3 - preset playground", "Fibl\xc3\xb3 - laboratori de presets"));
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
        "How fast higher partials lose volume: 0 = all equal (brightest), 1 = like a saw (1/n), 2 = like a triangle, higher = darker. Also shapes the stacked scale when a Custom Engine scale is the spectrum.",
        "Com de r\xc3\xa0pid perden volum els parcials aguts: 0 = tots iguals (m\xc3\xa9s brillant), 1 = com una serra (1/n), 2 = com un triangle, m\xc3\xa9s = m\xc3\xa9s fosc. Tamb\xc3\xa9 d\xc3\xb3na forma a l'escala apilada quan una escala del Motor propi \xc3\xa9s l'espectre."));
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
        "Largest prime factor a harmonic may contain. Example: limit 7 keeps 14 (2×7) and 25 (5×5) but drops 11 and 22 (2×11). Off = no prime filtering.",
        "Factor primer m\xc3\xa9s gran que pot tenir un harm\xc3\xb2nic. Exemple: l\xc3\xadmit 7 mant\xc3\xa9 14 (2×7) i 25 (5×5) per\xc3\xb2 elimina 11 i 22 (2×11). No = sense filtre de primers."));
    const char* pn2[] = { "2","3","5","7","11","13","17","19","23" };
    for (int i = 0; i < 9; ++i)
        primeToggle[i].setTooltip (tr ("Allow harmonics whose prime factors include ",
                                       "Permet harm\xc3\xb2nics amb el factor primer ") + juce::String (pn2[i]));
    cutoffFader.setTooltip (tr (
        "Low-pass cutoff as an INTERVAL above the played note, in cents (1200 = one octave, 12000 = fully open). It TRACKS the note, so the same partials are cut at every pitch - a partial at ratio r sits at 1200·log2(r) cents (e.g. harmonic 2 = 1200c, harmonic 3 = 1902c). An absolute-Hz cutoff would eat different partials depending on the note.",
        "Tall del passa-baixos com a INTERVAL per sobre de la nota tocada, en cents (1200 = una octava, 12000 = tot obert). SEGUEIX la nota, aix\xc3\xad que talla els mateixos parcials a qualsevol al\xc3\xa7""ada - un parcial de ratio r \xc3\xa9s a 1200·log2(r) cents (p. ex. harm\xc3\xb2nic 2 = 1200c, harm\xc3\xb2nic 3 = 1902c). Un tall en Hz absoluts es menjaria parcials diferents segons la nota."));
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
        "Load an AnaMark .tun, a Scala .scl or an Ableton .ascl file to retune the keys. The 1/1 sits on middle C unless the file sets its own reference (@ABL REFERENCE_PITCH, or a .tun's basefreq - a .tun always sounds at the absolute pitches it states). A .tun whose mapping does not repeat cannot be held as a scale and is refused rather than approximated.",
        "Carrega un fitxer AnaMark .tun, un Scala .scl o un Ableton .ascl per reafinar les tecles. L'1/1 queda al Do central tret que el fitxer porti la seva refer\xc3\xa8ncia (@ABL REFERENCE_PITCH, o el basefreq d'un .tun - un .tun sempre sona als tons absoluts que indica). Un .tun amb un mapatge que no es repeteix no es pot desar com a escala i es rebutja en comptes d'aproximar-lo."));
    clearSclButton.setTooltip (tr ("Unload the scale and return to standard 12-TET.",
                                   "Descarrega l'escala i torna al 12-TET est\xc3\xa0ndard."));
    for (int i = 0; i < 3; ++i)
        recentBtn[i].setTooltip (tr ("Reload this recently used file as the scale.",
                                     "Torna a carregar aquest fitxer recent com a escala."));
    bendBox.setTooltip (tr (
        "Per-note pitch-bend range. +-48 st is the MPE standard and is REQUIRED for Ableton Live 12's tuning systems and the Microtuner device (enable MPE on this plugin in Live). Use +-2 for a classic keyboard pitch wheel.",
        "Rang del pitch bend per nota. +-48 st \xc3\xa9s l'est\xc3\xa0ndard MPE i \xc3\x89S NECESSARI per als sistemes d'afinaci\xc3\xb3 d'Ableton Live 12 i el Microtuner (activa MPE al plugin dins de Live). Fes servir +-2 per a la roda d'un teclat cl\xc3\xa0ssic."));
    //-- SHEPARD MODE (bottom row of the Tuning box) ---------------------------
    shepOnT.setButtonText ("Shepard");
    shepOnT.setTooltip (tr (
        "SHEPARD MODE. The key you press becomes the root of a tone that rises for ever without ever getting higher - and it is built from THIS synth's spectrum, not from sine waves. The voice sounds its whole spectrum at up to 8 transpositions AT ONCE, one rise interval apart, anchored on the pressed key's own tuned pitch, all climbing together under a FIXED spectral window whose edges reach exactly zero: the copy leaving the top leaves silent and the one entering underneath enters silent, so there is no click and no reset to hide. Everything else about the note is untouched - the three envelopes, the filter, the modulators, morph, manual amplitudes - and letting go of the key releases it like any other note. Latched PER NOTE: flipping this changes the notes you play NEXT, never the ones already sounding. The bounds are stated, never silent: at most 8 copies (the band narrows, centred, rather than losing one) and up to 1638 partials each; a spectrum dense enough to need more lifts the window's bottom edge instead of dropping a single partial.",
        "MODE SHEPARD. La tecla que prems es converteix en l'arrel d'un to que puja sense parar sense arribar mai m\xc3\xa9s amunt - i est\xc3\xa0 fet amb l'espectre D'AQUEST sinte, no amb sinusoides. La veu fa sonar tot el seu espectre en fins a 8 transposicions ALHORA, separades un interval de pujada, ancorades al to real de la tecla premuda i pujant totes juntes sota una finestra espectral FIXA amb vores que arriben exactament a zero: la c\xc3\xb2pia que surt per dalt surt en silenci i la que entra per sota entra en silenci, o sigui sense clics ni cap reinici a amagar. La resta de la nota no canvia gens - les tres envolupants, el filtre, els moduladors, el morph, les amplituds manuals - i deixar anar la tecla l'allibera com qualsevol nota. Es fixa PER NOTA: canviar-ho afecta les notes que toquis DESPR\xc3\x88S, mai les que ja sonen. Els l\xc3\xadmits es diuen, mai s'amaguen: com a molt 8 c\xc3\xb2pies (la banda s'estreny, centrada, en comptes de perdre'n cap) i fins a 1638 parcials cadascuna; un espectre prou dens puja la vora inferior de la finestra en comptes de perdre ni un sol parcial."));
    shepModeBox.setTooltip (tr (
        "How the rise time is set. *Free*: the fader beside gives the time to rise one interval, in ms. *Sync* (the default): rising one interval lasts the chosen number of host beats. Switching mode (or moving the rate) GLIDES the speed - a sounding Shepard never jumps.",
        "Com es fixa el temps de pujada. *Lliure*: el fader del costat d\xc3\xb3na el temps de pujar un interval, en ms. *Sinc* (per defecte): pujar un interval dura els beats triats del tempo del DAW. Canviar de mode (o moure la velocitat) fa LLISCAR la marxa - un Shepard que sona mai salta."));
    shepTimeFader.setTooltip (tr (
        "Time to rise ONE interval, 100 ms to 60 s (log fader). Changes glide - a sounding Shepard never jumps.",
        "Temps de pujar UN interval, de 100 ms a 60 s (fader logar\xc3\xadtmic). Els canvis llisquen - un Shepard que sona mai salta."));
    shepRiseBox.setTooltip (tr (
        "What ONE rise step is. *Octave*: 2/1, the default in every tuning - an octave is what the ear hears as the same note again, so the illusion works whatever scale is loaded. *Period*: the active tuning's own repeat interval (3/1 under Bohlen-Pierce), which makes the rise land on the scale's own steps instead. This control only appears when the two differ; with an octave-period tuning there is nothing to choose.",
        "Qu\xc3\xa8 \xc3\xa9s UN pas de pujada. *Octava*: 2/1, el valor per defecte en qualsevol afinaci\xc3\xb3 - una octava \xc3\xa9s el que l'orella sent com la mateixa nota un altre cop, aix\xc3\xad la il\xc2\xb7lusi\xc3\xb3 funciona amb l'escala que sigui. *Per\xc3\xad" "ode*: l'interval de repetici\xc3\xb3 de l'afinaci\xc3\xb3 activa (3/1 amb Bohlen-Pierce), que fa que la pujada caigui sobre els graus de l'escala. Aquest control nom\xc3\xa9s apareix quan els dos difereixen; amb una afinaci\xc3\xb3 d'octava no hi ha res a triar."));
    // The beats box's tooltip carries the no-BPM state and is written by
    // refreshShepardSyncState(); resetting the memo makes it re-render in this language.
    lastShepNoBpm = -1;
    refreshShepardSyncState();
    // (loadTimbreButton's and randomSpectrumBtn's tooltips are set by
    //  refreshCustomEngineButtons above: they depend on which job the button has.)
    timbreRepeatsFader.setTooltip (tr (
        "How many periods of the scale are stacked as partials. More repeats = the timbre reaches higher in the spectrum.",
        "Quants per\xc3\xado""des de l'escala s'apilen com a parcials. M\xc3\xa9s repeticions = el timbre arriba m\xc3\xa9s amunt de l'espectre."));
    presetBox.setTooltip (tr ("Load a saved preset. The drop-down has a search box: type to filter the list "
                              "(accents and capitals are ignored).",
                              "Carrega un preset desat. El desplegable t\xc3\xa9 un cercador: escriu per filtrar "
                              "la llista (els accents i les maj\xc3\xbascules s'ignoren)."));
    savePresetButton.setTooltip (tr ("Save the current sound as a preset (stored in Documents/Galactic Sting/Presets).",
                                     "Desa el so actual com a preset (a Documents/Galactic Sting/Presets)."));
    // Corner-edit mode gives both of these a different meaning - re-apply it, or a
    // language switch mid-edit would put the live patch's tooltips back on a frozen field.
    lastEditCornerSeen = -12345;   // a value no corner index can be: force one rebuild
    refreshCornerEditUi();
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
    // Re-word a combo's items WITHOUT losing what is selected.
    //
    // The id has to be read BEFORE the texts change. juce::ComboBox::getSelectedId()
    // returns 0 the moment the visible label stops matching the selected item's text
    // (juce_ComboBox.cpp: `if (getText() == item->text) return item->itemID; return 0;`)
    // - which is precisely what changeItemText() does to it. Reading it afterwards
    // therefore handed setSelectedId a 0 and CLEARED the selection for good: one
    // language switch emptied the Spectrum-vs-Tuning mode box, and with mode == 0 that
    // panel's every gate (beat / adapt / segment) went false, so Notes, Period, Find,
    // REALTIME and the results list all vanished until the plugin was reopened.
    auto localise = [] (juce::ComboBox& b, const juce::StringArray& items)
    {
        const int keep = b.getSelectedId();
        for (int i = 0; i < items.size(); ++i) b.changeItemText (i + 1, items[i]);
        b.setSelectedId (keep, juce::dontSendNotification); // refresh displayed text
    };
    localise (basePitchBox, { tr ("Transposed down", "Transposada avall"), tr ("Original", "Original") });
    localise (phaseBox,     { tr ("Aligned", "Alineat"), tr ("Scattered", "Dispers") });
    localise (primeModeBox, { tr ("Limit", "L\xc3\xadmit"), tr ("Whitelist", "Llista") });
    localise (bendBox,      { "\xc2\xb1""2 st", "\xc2\xb1""12 st", "\xc2\xb1""48 st (MPE)" });
    // "Sync (beats)" said nothing about WHOSE beats: the user read it as a beat
    // subdivision of the rise rather than a lock to the host clock. Name the thing it
    // actually locks to. (Only the TEXT changes - changeItemText keeps the item's
    // POSITION, which is the whole contract with the ComboBoxAttachment.)
    localise (shepModeBox,  { tr ("Free (ms)", "Lliure (ms)"),
                              tr ("Sync Project Tempo", "Sinc tempo projecte") });
    localise (shepRiseBox,  { tr ("Octave pitch bend", "Bend d'octava"),
                              tr ("Period pitch bend", "Bend de per\xc3\xad" "ode") });
    voiceEnv.tip = tr (
        "This envelope, drawn as a line with nodes. Drag a node for the attack time, the decay time + sustain level, or the release time; drag the LINE between nodes to bend it concave or convex; double-click a segment to straighten it.",
        "Aquesta envolupant, dibuixada com una línia amb nodes. Arrossega un node per al temps d'""atac, el de caï""da + nivell de sosteniment, o el d'""alliberament; arrossega la LÍNIA entre nodes per corbar-la; doble clic en un tram per aplanar-lo.");
    envEditor.tip = tr (
        "The selected envelope, drawn as a line with nodes. DRAG A NODE to set the attack time, the decay time + sustain level, or the release time. DRAG THE LINE between two nodes to bend it concave or convex - that curve is exactly what the modulator sends. Double-click a segment for a straight line again.",
        "L'envolupant seleccionada, dibuixada com una l\xc3\xadnia amb nodes. ARROSSEGA UN NODE per fixar el temps d'atac, el de ca\xc3\xaf""da + nivell de sosteniment, o el d'alliberament. ARROSSEGA LA L\xc3\x8dNIA entre dos nodes per corbar-la c\xc3\xb2ncava o convexa - aquesta corba \xc3\xa9s exactament el que envia el modulador. Doble clic en un tram per tornar a la recta.");
    // The CUSTOM (modulators) tab: its combos and tooltips used to be English-only.
    localise (modSelBox,      { "Env 1", "Env 2", "Env 3", "LFO 1", "LFO 2", "LFO 3" });
    localise (lfoWaveBox,     { tr ("Sine", "Sinus"), tr ("Triangle", "Triangle"),
                                tr ("Saw up", "Serra amunt"), tr ("Saw down", "Serra avall"),
                                tr ("Square", "Quadrada"), tr ("Custom (draw)", "Pr\xc3\xb2pia (dibuixa)"),
                                tr ("Random", "Aleat\xc3\xb2ria") });
    localise (lfoRateModeBox, { tr ("BPM / 2^n", "BPM / 2^n"),
                                tr ("Fundamental / 2^n", "Fonamental / 2^n"),
                                tr ("Free Hz", "Hz lliures") });
    localise (lfoPhaseModeBox,{ tr ("Restart each note", "Reinicia a cada nota"),
                                tr ("Project transport", "Transport del projecte") });
    modUnmapBtn.setButtonText  (tr ("Unmap", "Desassigna"));
    lfoWaveEditBtn.setButtonText (tr ("Draw", "Dibuixa"));
    rhythmPanel.helpB.setButtonText (tr ("How to route", "Com encaminar"));
    chordList.emptyText = tr ("Press Find to search.\nhover a row to hear it\ndouble-click loads it as the scale",
                              "Prem Troba per cercar.\npassa-hi el cursor per escoltar-lo\ndoble clic el carrega com a escala");
    // The adaptation's read-out is one plain sentence with ONE job at a time; every number
    // it used to print is in its tooltip, which is rebuilt with each result (see
    // adaptReadoutTip). This is the fallback text for before anything has happened.
    adaptInfoTipBase = tr (
        "What is happening, or what will happen if you press Adapt: how long the search will take, whether a REALTIME link can keep up, and how much beating the last adaptation removed. The raw Sethares numbers appear here once a run has finished.",
        "Qu\xc3\xa8 est\xc3\xa0 passant, o qu\xc3\xa8 passar\xc3\xa0 si prems Adapta: quant trigar\xc3\xa0 la cerca, si un v\xc3\xadncle en TEMPS REAL hi pot seguir el ritme, i quant batec ha tret l'\xc3\xbaltima adaptaci\xc3\xb3. Els n\xc3\xbameros de Sethares en cru surten aqu\xc3\xad quan una cerca ha acabat.");
    adaptInfoLbl.setTooltip (adaptInfoTipBase);
    {
        const int keepMode = specModeBox.getSelectedId(); // BEFORE the texts move - see localise
        specModeBox.changeItemText (1, tr ("Find tunings based on beating", "Troba afinacions pel batec"));
        specModeBox.changeItemText (2, tr ("Adapt spectrum to tuning based on beating",
                                           "Adapta l'espectre a l'afinaci\xc3\xb3 pel batec"));
        specModeBox.changeItemText (3, tr ("From spectrum's segment", "Del segment de l'espectre"));
        specModeBox.setSelectedId (keepMode, juce::dontSendNotification);
    }
    localise (tuneSourceBox, { tr ("From current tuning", "De l'afinaci\xc3\xb3 actual"),
                               tr ("From all possible tunings", "De totes les afinacions possibles") });
    localise (tuneScopeBox,  { tr ("0 vs all", "0 vs tot"), tr ("all vs all", "tot vs tot") });
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
    saveTuningBtn.setButtonText (tr ("Save", "Desa"));
    saveTuningBtn.getProperties().set ("fork", true);
    saveTuningBtn.setTooltip (tr (
        "Save the CURRENT tuning as a .scl file to any folder - including scales you edited on the keyboard (right-click a key to retype a degree, Ctrl+click to add a note).",
        "Desa l'afinaci\xc3\xb3 ACTUAL com a fitxer .scl a qualsevol carpeta - incloses les escales editades al teclat (clic dret per reescriure un grau, Ctrl+clic per afegir una nota)."));
    chordGoBtn.setButtonText (tr ("Find", "Troba"));
    chordNotesBox.setTooltip (tr (
        "How many notes each combination has (the root 1/1 is included). From the current tuning it stops at the number of degrees the scale has; from all possible tunings it goes to 8. More notes = a heavier search.",
        "Quantes notes t\xc3\xa9 cada combinaci\xc3\xb3 (l'arrel 1/1 hi \xc3\xa9s inclosa). Des de l'afinaci\xc3\xb3 actual arriba fins als graus que t\xc3\xa9 l'escala; des de totes les afinacions possibles arriba a 8. M\xc3\xa9s notes = cerca m\xc3\xa9s pesada."));
    chordCountBox.setTooltip (tr ("How many chords to list.", "Quants acords llistar."));
    chordMinMaxBox.setTooltip (tr (
        "Min = the most CONSONANT combinations with this exact timbre (Sethares sensory dissonance of all partial pairs). Max = the roughest.",
        "Min = les combinacions m\xc3\xa9s CONSONANTS amb aquest timbre exacte (disson\xc3\xa0ncia sensorial de Sethares de tots els parells de parcials). Max = les m\xc3\xa9s aspres."));
    chordPeriodEdit.setTooltip (tr (
        "The *period* the search works in.\nWrite it as a whole number `3`, a fraction `3/2`, or cents `1200c` / `1200 cents` / just `1200`.\n"
        "Each mode keeps its own: the tuning search starts at `2`, the spectrum adaptation at `8` (its partials want room to spread). What you type stays with the mode you typed it in.",
        "El *per\xc3\xado" "de* amb qu\xc3\xa8 treballa la cerca.\nEscriu-lo com a nombre enter `3`, fracci\xc3\xb3 `3/2`, o cents `1200c` / `1200 cents` / nom\xc3\xa9s `1200`.\n"
        "Cada mode t\xc3\xa9 el seu: la cerca d'afinacions comen\xc3\xa7""a a `2`, l'adaptaci\xc3\xb3 de l'espectre a `8` (els parcials volen espai). El que escriguis es queda al mode on l'has escrit."));
    chordGoBtn.setTooltip (tr (
        "Rank all n-note combinations by their Sethares sensory dissonance against the CURRENT spectrum (grid search + exact refinement, on a background thread). Play previews a chord; double-click loads it as the active scale (.scl).",
        "Ordena totes les combinacions de n notes per la seva disson\xc3\xa0ncia sensorial de Sethares contra l'espectre ACTUAL (cerca en graella + refinament exacte, en segon pla). Play pre-escolta un acord; doble clic el carrega com a escala activa (.scl)."));
    chordList.tip = tr ("Each note in its own colour.\n*Hover* a row to hear it (all together, then an ascending strum).\n*Double-click* it to LOAD the chord as the active scale.",
                        "Cada nota amb el seu color.\n*Passa-hi el cursor* per escoltar-lo (tot alhora i despr\xc3\xa9s un strum ascendent).\n*Doble clic* per CARREGAR l'acord com a escala activa.");

    //== "Adapt spectrum to tuning based on beating" + the two REALTIME buttons =====
    localise (adaptTargetBox, { tr ("To the loaded tuning", "A l'afinaci\xc3\xb3 carregada"),
                                tr ("To the chord held now", "A l'acord que sona ara") });
    localise (adaptGoalBox,   { tr ("Min", "Min"), tr ("Max", "Max"), tr ("Range", "Rang") });
    adaptPartialsLbl.setText (tr ("Partials", "Parcials"), dontSendNotification);
    adaptRangeLbl.setText    (tr ("between", "entre"), dontSendNotification);
    // "Steps", not "Steps up": at 10 pt bold the Catalan "Passos amunt" needs more than this
    // row can spare and juce::Label answers that by CONDENSING to 70% rather than by
    // clipping, which looks deliberate and reads as a different font (RhythmVerify measured
    // it). The direction is in the line underneath, which says it in words - "24 steps above
    // 3:0" - where there is room for it.
    adaptRegLbl.setText      (tr ("Steps", "Passos"), dontSendNotification);
    refreshAdaptButtonText();
    adaptAmpsToggle.setButtonText (tr ("+ amps", "+ amplituds"));
    chordRtBtn.setButtonText (tr ("REALTIME", "TEMPS REAL"));
    adaptRtBtn.setButtonText (tr ("REALTIME", "TEMPS REAL"));
    adaptTargetBox.setTooltip (tr (
        "WHICH NOTES the spectrum is adapted to.\n*The loaded tuning* - its degrees.\n*The chord held now* - literally the notes under your fingers, at the pitch they are really sounding. Play a major chord in 31-EDO and the partials go where that chord beats least.\nONE note counts as a target too: with nothing else sounding, the thing being minimised is the beating of the partials against EACH OTHER, at that note's own pitch.\nREALTIME always follows the held chord, so this box shows that and waits while the link is on.",
        "QUINES NOTES fan d'objectiu.\n*L'afinaci\xc3\xb3 carregada* - els seus graus.\n*L'acord que sona ara* - literalment les notes sota els dits, a l'al\xc3\xa7""ada real. Toca un acord major en 31-EDO i els parcials van on aquell acord bat menys.\nUNA sola nota tamb\xc3\xa9 val com a objectiu: si no sona res m\xc3\xa9s, el que es minimitza \xc3\xa9s el batec dels parcials ENTRE ELLS, a l'al\xc3\xa7""ada d'aquella nota.\nTEMPS REAL sempre segueix l'acord que mantens, per aix\xc3\xb2 aquesta casella ho mostra i espera mentre el v\xc3\xadncle est\xc3\xa0 actiu."));
    adaptPartialsBox.setTooltip (tr (
        "How many partials the adapted spectrum has. They are all kept: the search MOVES them and never merges or removes one - each keeps at least the smallest interval below from its neighbours.",
        "Quants parcials t\xc3\xa9 l'espectre adaptat. Es conserven tots: la cerca els MOU i mai no en fusiona ni n'elimina cap - cada un guarda com a m\xc3\xadnim l'interval m\xc3\xa9s petit de sota respecte als ve\xc3\xafns."));
    adaptGoalBox.setTooltip (tr (
        "The TARGET. Min = the partials go where these notes beat least. Max = where they beat most. Range = hit a beating value between the two numbers you type (the read-out below shows the scale those numbers are on).",
        "L'OBJECTIU. Min = els parcials van on aquestes notes baten menys. Max = on baten m\xc3\xa9s. Rang = arriba a un valor de batec entre els dos n\xc3\xbameros que escriguis (el marcador de sota mostra l'escala d'aquests n\xc3\xbameros)."));
    adaptRegStepsEdit.setTooltip (tr (
        "HOW WIDE a band of registers the spectrum is optimised for, counted in STEPS of the loaded tuning and running UPWARD from the note you are playing.\nThe anchor is the lowest note you are HOLDING; with nothing down it is the last note you played, and before you have played anything it is the reference key. The line below names it as period:degree - \"3:0\" is that reference key - and says what your number comes to in periods, because a step count is exact in every tuning and unreadable as a width: 24 steps is two octaves in 12-EDO, 62 steps is the same two periods in 31-EDO, 26 in Bohlen-Pierce. That is why this is not typed in octaves - most of this instrument's tunings do not have one.\nWhy a band and not one pitch: Sethares' critical band is a width in Hz, not in cents, so the answer really is different up and down the keyboard - the interval that beats worst is about 434 cents low and only 61 cents four octaves up, and a partial pair closer than 33 cents gets steadily WORSE the higher you play. A spectrum optimised at ONE pitch is optimal only there, and optimised in the bass it packs partials into tight clusters that cost almost nothing down there and dominate the roughness two periods up.\nThe default is two periods, and that is measured rather than guessed: across three anchors, nothing narrower wins, while three periods and more buy a little insurance high up and pay for it at the register you are actually playing (11.9 -> 13.7 on the search's own scale). 0 is legal and means the played register alone. Your number stays YOUR number when the tuning changes; until you type, it follows the loaded scale's own two periods.\nHOW THE BAND IS COMBINED: every register in it counts EQUALLY - the spectrum is optimised for the AVERAGE beating across the whole span, so a wider band trades a little at the note under your fingers for the registers above it. That is not a setting: there used to be a *worst case* choice beside this field, and with the band anchored at the played note and running UPWARD its worst register is always the anchor, so it optimised that one pitch alone and this width stopped changing anything (10.742 at every value). The objective is evaluated at one point per period, capped at 7 for the Adapt button and 3 for the REALTIME follower (the cost is exactly linear in that count, and 3 is still exact sampling for a two-period range). Nothing is degraded silently.",
        "L'AMPLADA de la banda de registres per als quals s'optimitza l'espectre, comptada en PASSOS de l'afinaci\xc3\xb3 carregada i cap AMUNT des de la nota que toques.\nL'\xc3\xa0ncora \xc3\xa9s la nota m\xc3\xa9s greu que MANTENS; si no en mantens cap, l'\xc3\xbaltima que has tocat, i abans de tocar res, la tecla de refer\xc3\xa8ncia. La l\xc3\xadnia de sota l'anomena com a per\xc3\xado" "de:grau - \"3:0\" \xc3\xa9s aquella tecla de refer\xc3\xa8ncia - i diu a quants per\xc3\xado" "des equival el teu n\xc3\xbamero, perqu\xc3\xa8 un compte de passos \xc3\xa9s exacte en qualsevol afinaci\xc3\xb3 i illegible com a amplada: 24 passos s\xc3\xb3n dues octaves en 12-EDO, 62 passos s\xc3\xb3n els mateixos dos per\xc3\xado" "des en 31-EDO, 26 en Bohlen-Pierce. Per aix\xc3\xb2 no s'escriu en octaves - la majoria d'afinacions d'aquest instrument no en tenen.\nPer qu\xc3\xa8 una banda i no una sola al\xc3\xa7""ada: la banda cr\xc3\xadtica de Sethares \xc3\xa9s una amplada en Hz, no en cents, i per aix\xc3\xb2 la resposta \xc3\xa9s realment diferent amunt i avall del teclat - l'interval que m\xc3\xa9s bat \xc3\xa9s d'uns 434 cents als greus i nom\xc3\xa9s 61 quatre octaves m\xc3\xa9s amunt, i una parella de parcials a menys de 33 cents empitjora com m\xc3\xa9s amunt toques. Un espectre optimitzat per a UNA sola al\xc3\xa7""ada nom\xc3\xa9s ho \xc3\xa9s all\xc3\xa0, i optimitzat als greus amuntega els parcials en grups atapa\xc3\xafts que all\xc3\xa0 baix no costen gaire i dos per\xc3\xado" "des m\xc3\xa9s amunt dominen l'aspresa.\nPer defecte s\xc3\xb3n dos per\xc3\xado" "des, i aix\xc3\xb2 est\xc3\xa0 mesurat, no suposat: des de tres \xc3\xa0ncores, res m\xc3\xa9s estret no guanya, mentre que tres per\xc3\xado" "des o m\xc3\xa9s compren una mica d'asseguran\xc3\xa7""a amunt i la paguen al registre que realment toques (11,9 -> 13,7 a l'escala de la mateixa cerca). 0 \xc3\xa9s legal i vol dir nom\xc3\xa9s el registre tocat. El teu n\xc3\xbamero segueix sent EL TEU quan canvia l'afinaci\xc3\xb3; fins que no escriguis, segueix els dos per\xc3\xado" "des de l'escala carregada.\nCOM ES COMBINA LA BANDA: tots els seus registres compten IGUAL - l'espectre s'optimitza per a la MITJANA del batec de tot el tram, de manera que una banda m\xc3\xa9s ampla cedeix una mica a la nota que tens sota els dits per guanyar als registres de sobre. Aix\xc3\xb2 no \xc3\xa9s cap opci\xc3\xb3: al costat d'aquest camp hi havia un *pitjor cas*, i amb la banda ancorada a la nota tocada i cap AMUNT el pitjor registre \xc3\xa9s sempre l'\xc3\xa0ncora, o sigui que nom\xc3\xa9s optimitzava aquella al\xc3\xa7""ada i aquesta amplada deixava de canviar res (10,742 amb qualsevol valor).\nL'objectiu s'avalua a un punt per per\xc3\xado" "de, amb un m\xc3\xa0xim de 7 per al bot\xc3\xb3 Adapta i de 3 per al seguidor en TEMPS REAL (el cost \xc3\xa9s exactament lineal amb aquest nombre, i 3 encara \xc3\xa9s mostreig exacte per a un rang de dos per\xc3\xado" "des). No es degrada res en silenci."));
    // The SAME hover on the caption and on the line under the field: the sentence that
    // explains the aggregate now lives here (the box that used to say it in words is gone),
    // and a user reaching for "what happened to the average/worst choice" is as likely to
    // hover the words as the number.
    adaptRegSpanLbl.setTooltip (adaptRegStepsEdit.getTooltip());
    adaptRegLbl.setTooltip     (adaptRegStepsEdit.getTooltip());
    adaptAmpsToggle.setTooltip (tr (
        "Also shape each partial's AMPLITUDE, not only where it sits. The amplitudes are written as per-partial edits (the pencil window's own values), bounded and never muted - a silenced partial would be a deleted one.",
        "Modela tamb\xc3\xa9 l'AMPLITUD de cada parcial, no nom\xc3\xa9s la posici\xc3\xb3. Les amplituds s'escriuen com a edicions per parcial (els mateixos valors de la finestra del llapis), acotades i mai silenciades - un parcial silenciat seria un parcial esborrat."));
    adaptGoBtn.setTooltip (tr (
        "Adapt the spectrum once, now. It is written into the Custom Engine (partial ratios) plus the per-partial amplitudes, as ONE undo step.\nThis is a ONE-SHOT calculation, so it is allowed to take time: when it will take more than a moment the button says roughly how long, and pressing it again while it runs STOPS it. Nothing is refused for being big except a request estimated past ten minutes, which is stated as a number rather than a verdict. The search runs on a background thread - you can keep playing while it works.",
        "Adapta l'espectre un cop, ara. S'escriu al Motor Propi (raons dels parcials) m\xc3\xa9s les amplituds per parcial, com UN sol pas d'anul\xc2\xb7laci\xc3\xb3.\n\xc3\x89s un c\xc3\xa0lcul D'UN SOL COP, i per tant t\xc3\xa9 dret a trigar: quan trigui m\xc3\xa9s d'un instant, el bot\xc3\xb3 diu aproximadament quant, i tornar-lo a pr\xc3\xa9mer mentre treballa l'ATURA. No es rebutja res per ser gran, tret d'una petici\xc3\xb3 estimada en m\xc3\xa9s de deu minuts, i aix\xc3\xb2 es diu amb un n\xc3\xbamero, no amb un veredicte. La cerca va en un fil de fons - pots seguir tocant mentre treballa."));
    chordRtBtn.setTooltip (tr (
        "REALTIME: the tuning follows the TOP result on its own, every time the spectrum changes (a knob, a modulator, a preset). At most three searches a second, never two at once, and many changes while it is busy collapse into one - so a sweeping modulator cannot pile searches up. A full search takes about 0.3 s with a rich spectrum (0.1 s when the spectrum's tables can be reused), so each retune lands as its own search finishes. Realtime updates do NOT fill the tuning bank: the tuning you had joins it ONCE when you switch this on, and switching it off leaves ONE Ctrl+Z back to it.",
        "TEMPS REAL: l'afinaci\xc3\xb3 segueix sola el PRIMER resultat cada cop que canvia l'espectre (un potenci\xc3\xb2metre, un modulador, un preset). Com a m\xc3\xa0xim tres cerques per segon, mai dues alhora, i molts canvis mentre treballa es fusionen en un - aix\xc3\xad un modulador escombrant no pot acumular cerques. Una cerca completa triga uns 0,3 s amb un espectre ric (0,1 s quan es poden reaprofitar les taules de l'espectre), per aix\xc3\xb2 cada reafinaci\xc3\xb3 arriba quan acaba la seva cerca. Les actualitzacions en temps real NO omplen el banc: l'afinaci\xc3\xb3 que tenies hi entra UN sol cop en activar-ho, i en desactivar-ho queda UN Ctrl+Z per recuperar-la."));
    adaptRtBtn.setTooltip (tr (
        "REALTIME: the spectrum follows WHAT YOU PLAY - it re-adapts to the chord under your fingers, at the pitch it is really sounding, every time that chord changes. A SINGLE note works: with nothing else sounding the target is the partials' beating against each other, so a lead or a bass line still gets its own smoothest (or roughest) timbre, and a different one in each register. Hold nothing and it does nothing; hold the same chord and it stays put instead of re-adapting to it over and over. Throttled to about four adaptations a second and coalesced. (The loaded tuning is a target for the Adapt button, not for a follower: it would just keep writing the same scale into the spectrum.) Switching it off leaves ONE Ctrl+Z back to the spectrum you had before.",
        "TEMPS REAL: l'espectre segueix el QUE TOQUES - es torna a adaptar a l'acord que tens sota els dits, a l'al\xc3\xa7""ada real, cada cop que aquell acord canvia. UNA sola nota ja funciona: si no sona res m\xc3\xa9s, l'objectiu \xc3\xa9s el batec dels parcials entre ells, de manera que una melodia o un baix tamb\xc3\xa9 tenen el seu timbre m\xc3\xa9s llis (o m\xc3\xa9s aspre), i un de diferent a cada registre. Si no mantens res, no fa res; si mantens el mateix acord, es queda quiet en comptes de tornar-s'hi a adaptar un cop i un altre. Limitat a ~quatre adaptacions per segon i fusionat. (L'afinaci\xc3\xb3 carregada \xc3\xa9s un objectiu per al bot\xc3\xb3 Adapta, no per a un seguidor: nom\xc3\xa9s escriuria la mateixa escala a l'espectre.) En desactivar-ho queda UN Ctrl+Z per recuperar l'espectre anterior."));
    lookaheadBtn.setTooltip (tr (
        "LOOKAHEAD: the spectrum is ready BEFORE the note sounds, for written MIDI played back by the DAW.\nA plugin cannot see future MIDI - so this makes the plugin ARRIVE LATE on purpose. It reports 250 ms of latency, the host's delay compensation shifts everything back into place, and the incoming MIDI is buffered here by exactly that much. The adaptive search runs on each note the moment it ARRIVES, and the adapted spectrum is live by the time that note actually sounds: on playback the partials change sample-aligned with the note instead of ~50-200 ms into it.\nWHERE 250 ms COMES FROM (all measured): up to 100 ms of the follower's own throttle before the search may start, one 60 Hz poll to notice the notes, the search itself (11 ms at 8 partials, 33 at 12, ~104-137 at 16), one more poll to apply it and the 50 ms de-click fade.\nSO IT DOES NOT FIT EVERY SETTING, and the button SAYS SO instead of pretending: beside the budget it prints how long the LAST chord change really took on your machine, and marks it LATE when it did not fit. If you see LATE, use fewer partials.\nTHE TRADE, honestly: PLAYING LIVE FEELS THOSE 250 ms. This is for composing, not for performing. The plugin's rhythm MIDI OUT is delayed by the same amount (it is generated from the same shifted stream, which is what keeps the pulses locked to the audio) and hosts differ in whether they compensate a plugin's MIDI output.\nIt only costs latency while a REALTIME spectrum follower is actually running; otherwise it waits and reports zero. A loop or a locator jump flushes what is in flight, so no stale note is replayed.",
        "ANTICIPACI\xc3\x93: l'espectre est\xc3\xa0 llest ABANS que soni la nota, per a MIDI escrit i reprodu\xc3\xaft pel DAW.\nUn plugin no pot veure el MIDI futur - o sigui que aix\xc3\xb2 fa que el plugin ARRIBI TARD expressament. Declara 250 ms de lat\xc3\xa8ncia, la compensaci\xc3\xb3 de retard de l'amfitri\xc3\xb3 ho torna tot al seu lloc, i el MIDI d'entrada es reserva aqu\xc3\xad exactament aquest temps. La cerca adaptativa s'executa sobre cada nota quan ARRIBA, i l'espectre adaptat ja \xc3\xa9s viu quan la nota sona de debo: en reproducci\xc3\xb3 els parcials canvien clavats a la mostra amb la nota, en comptes de ~50-200 ms m\xc3\xa9s tard.\nD'ON SURTEN ELS 250 ms (tot mesurat): fins a 100 ms del limitador del seguidor abans que la cerca pugui comen\xc3\xa7""ar, un sondeig de 60 Hz per adonar-se'n, la cerca mateixa (11 ms amb 8 parcials, 33 amb 12, ~104-137 amb 16), un altre sondeig per aplicar-la i els 50 ms d'esvaniment anti-clic.\nAIX\xc3\x92 VOL DIR QUE NO ENCAIXA A TOTES LES CONFIGURACIONS, i el bot\xc3\xb3 HO DIU en comptes de dissimular: al costat del pressupost escriu quant ha trigat de debo el darrer canvi d'acord a la teva m\xc3\xa0quina, i el marca TARD quan no hi ha cabut. Si veus TARD, fes servir menys parcials.\nEL PREU, sense enganys: TOCAR EN VIU NOTA aquests 250 ms. Aix\xc3\xb2 \xc3\xa9s per compondre, no per tocar. La sortida MIDI de ritme del plugin es retarda igual (es genera del mateix flux despla\xc3\xa7""at, que \xc3\xa9s el que mant\xc3\xa9 els polsos clavats a l'\xc3\xa0udio) i els amfitrions difereixen en si compensen la sortida MIDI d'un plugin.\nNom\xc3\xa9s costa lat\xc3\xa8ncia mentre un seguidor d'espectre en TEMPS REAL est\xc3\xa0 realment en marxa; si no, espera i declara zero. Un bucle o un salt de localitzador buida el que hi ha en vol, o sigui que no es repeteix cap nota vella."));
    adaptList.tip = tr ("The adapted spectrum: every partial's interval from the played note, and the amplitude the search gave it. All of them are here - the search moves partials, it never drops them.",
                        "L'espectre adaptat: l'interval de cada parcial respecte a la nota tocada i l'amplitud que li ha donat la cerca. Hi s\xc3\xb3n tots - la cerca mou parcials, no en descarta cap.");
    adaptList.emptyText = tr ("Press Adapt to move the partials to where these notes beat least (or most).",
                              "Prem Adapta per moure els parcials on aquestes notes baten menys (o m\xc3\xa9s).");

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

juce::StringArray FractalAdditiveAudioProcessorEditor::presetSearchProbeForTests (
    const juce::StringArray& names, const juce::String& typed)
{
    PresetSearchPanel panel (names, {}, {});
    panel.search.setText (typed, false);
    // ...then the very function onTextChange calls. NOT setText's own notification:
    // TextEditor::textChanged POSTS the callback (postCommandMessage), so with no
    // message loop running it never arrives and a probe that trusted it would report
    // "everything matches" for every query - which is a filter that is not being tested
    // at all rather than one that works.
    panel.refilter();
    return panel.shown;
}

FractalAdditiveAudioProcessorEditor::DragProbe
FractalAdditiveAudioProcessorEditor::dragFaderForTests (const juce::String& name, int dy, int settleTicks)
{
    DragProbe d;
    juce::Slider* s = nullptr;
    if      (name == "slope")     s = &slopeFader;
    else if (name == "baseN")     s = &baseNFader;
    else if (name == "fractal")   s = &fractalFader;
    else if (name == "morphTime") s = &morphTimeFader;
    else if (name == "harmonics") s = &harmonicsFader;
    if (s == nullptr) return d;

    d.found   = true;
    d.visible = s->isVisible();
    d.enabled = s->isEnabled();
    d.before  = s->getValue();

    // The state a failure would have to be explained by, captured BEFORE the drag.
    d.heldNotes = (int) heldKeys.size();
    if (auto* f = juce::Component::getCurrentlyFocusedComponent())
        d.focusOwner = juce::String (typeid (*f).name());
    d.editorHasFocus = hasKeyboardFocus (true);
    d.msSinceFocused = (int) (juce::Time::getMillisecondCounter() - lastFocusedMs);

    const auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto t0  = juce::Time::getCurrentTime();
    const juce::Point<float> down ((float) (s->getWidth() / 2), (float) (s->getHeight() / 2));
    const juce::Point<float> to   (down.x, down.y + (float) dy);
    const auto press = juce::ModifierKeys::leftButtonModifier;

    s->mouseDown (juce::MouseEvent (src, down, press, 1.0f, 0, 0, 0, 0, s, s, t0, down, t0, 1, false));
    s->mouseDrag (juce::MouseEvent (src, to,   press, 1.0f, 0, 0, 0, 0, s, s, t0, down, t0, 1, true));
    d.afterDrag = s->getValue();
    s->mouseUp   (juce::MouseEvent (src, to, juce::ModifierKeys(), 1.0f, 0, 0, 0, 0, s, s, t0, down, t0, 1, true));

    for (int i = 0; i < settleTicks; ++i) timerCallback();
    d.afterSettle = s->getValue();
    return d;
}

FractalAdditiveAudioProcessorEditor::NadShownProbe
FractalAdditiveAudioProcessorEditor::nadShownProbeForTests() const
{
    NadShownProbe n;
    n.rows             = (int) nadList.rows.size();
    n.equalStep        = nadList.equalStep;
    n.degreesPerPeriod = processorRef.getScaleDegrees();
    for (const auto& r : nadList.rows)
        for (const int d : r.degs)
            n.maxDegree = juce::jmax (n.maxDegree, d);
    return n;
}

FractalAdditiveAudioProcessorEditor::RatiosInkProbe
FractalAdditiveAudioProcessorEditor::ratiosInkProbeForTests (const juce::String& typed)
{
    RatiosInkProbe r;
    intervalEdit.setText (typed, false);

    r.box = intervalEdit.getLocalBounds();
    // The BORDER is what the viewport is inset by, so it is what clips: no glyph can be
    // drawn outside this, whatever the text says. Read back from JUCE, not restated.
    const auto b = intervalEdit.getBorder();
    r.clip = r.box.reduced (0, 0)
                  .withTrimmedLeft (b.getLeft()).withTrimmedRight (b.getRight())
                  .withTrimmedTop (b.getTop()).withTrimmedBottom (b.getBottom());
    r.ink  = intervalEdit.getTextBounds (juce::Range<int> (0, intervalEdit.getTotalNumChars())).getBounds();
    r.inkInsideClip = r.ink.isEmpty() || r.clip.contains (r.ink);

    const auto o = textEditorOutline ((float) r.box.getWidth(), (float) r.box.getHeight(),
                                      intervalEdit.isMultiLine());
    r.radius = o.radius;
    // The stroke is centred on o.rect, so its INNER edge is half a pen-width further in.
    const auto inner = o.rect.reduced (o.thickness * 0.5f);
    const float innerRad = juce::jmax (0.0f, o.radius - o.thickness * 0.5f);
    r.minGap = juce::jmin (juce::jmin ((float) r.clip.getX() - inner.getX(),
                                       (float) r.clip.getY() - inner.getY()),
                           juce::jmin (inner.getRight()  - (float) r.clip.getRight(),
                                       inner.getBottom() - (float) r.clip.getBottom()));
    juce::Path shape;
    shape.addRoundedRectangle (inner, innerRad);
    const auto c = r.clip.toFloat();
    for (auto pt : { c.getTopLeft(), c.getTopRight(), c.getBottomLeft(), c.getBottomRight() })
        if (! shape.contains (pt.x, pt.y))
            r.clipInsideOutline = false;   // the pill's arc is eating the text area's corner
    return r;
}

int FractalAdditiveAudioProcessorEditor::comboReopenProbeForTests (int attempts)
{
    RefreshingComboBox box;
    box.addItem ("one", 1);
    box.setSelectedId (1, juce::dontSendNotification);

    int opened = 0;
    // The presets box's contract exactly: "I handled it, do not show the plain menu."
    // No CallOutBox here - the failure is entirely in ComboBox's own bookkeeping, and a
    // real callout would need a desktop window and a user to dismiss it.
    box.onShowPopup = [&opened] { ++opened; return true; };

    for (int i = 0; i < attempts; ++i)
    {
        const bool wasActive = box.isPopupActive();
        // The Return key runs the SAME showPopupIfNotActive() a mouseDown does (that one
        // is private, so this is the honest way in from outside). If menuActive is stuck
        // up, this call does nothing at all - precisely what the user saw.
        box.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));

        // showPopupIfNotActive() raises menuActive and then defers showPopup() with
        // MessageManager::callAsync. This build has no dispatch loop to deliver that
        // (JUCE_MODAL_LOOPS_PERMITTED is off in the verifier), so the callback's one
        // statement - safePointer->showPopup() - is delivered by hand, and ONLY on the
        // transition that actually queued it. Calling showPopup() unconditionally would
        // be a probe that opens the box itself and could never see the bug.
        if (! wasActive && box.isPopupActive())
            box.showPopup();
    }
    return opened;
}

bool FractalAdditiveAudioProcessorEditor::openPresetSearch()
{
    const auto names = processorRef.getPresetNames();
    if (names.isEmpty())
        return false;   // nothing to filter: let the ordinary popup say "empty" its own way

    auto panel = std::make_unique<PresetSearchPanel> (
        names, presetBox.getText(),
        tr ("Search presets...", "Cerca presets..."));
    // Tall enough for ~12 rows, short enough that the callout still fits inside the
    // editor above a box sitting near the top - CallOutBox flips it below/above by itself.
    panel->setSize (juce::jmax (200, presetBox.getWidth() + 90),
                    30 + juce::jlimit (60, 240, names.size() * 20 + 8));
    panel->onPick = [safe = juce::Component::SafePointer<FractalAdditiveAudioProcessorEditor> (this)]
                    (const juce::String& n)
    {
        if (safe == nullptr) return;
        // Straight through the combo, so the ONE onChange handler loads it - a second
        // load path here is how a preset ends up applied without the dirty mark, the
        // tuning offer and the relayout that go with it.
        safe->presetBox.setText (n, juce::sendNotificationSync);
    };
    // Whatever closes it - a pick, Escape, a click outside - the editor takes the
    // keyboard back, or the computer-keyboard play the search field suspended would
    // stay suspended (pollComputerKeyboard only re-arms once no TextEditor has focus).
    panel->onDismissed = [safe = juce::Component::SafePointer<FractalAdditiveAudioProcessorEditor> (this)]
    { if (safe != nullptr) safe->refocus(); };

    auto* raw = panel.get();
    juce::CallOutBox::launchAsynchronously (std::move (panel),
                                            getLocalArea (&presetBox, presetBox.getLocalBounds()), this);
    raw->search.grabKeyboardFocus();
    return true;
}

void FractalAdditiveAudioProcessorEditor::saveCornerFlow (int corner)
{
    const auto base = processorRef.getPlaygroundCornerName (corner);
    // Suggest a name that is NOT the corner's own, for the same reason savePresetFlow
    // does: a reflex Enter must not overwrite the preset the corner was loaded FROM,
    // which is very probably still on another corner or in somebody's project.
    const auto existing = processorRef.getPresetNames();
    juce::String suggested = base + " 2";
    for (int n = 3; existing.contains (suggested); ++n) suggested = base + " " + juce::String (n);

    auto* aw = new juce::AlertWindow (
        tr ("Save corner ", "Desar cantonada ") + juce::String::charToString ((juce::juce_wchar) ('A' + corner)),
        tr ("Name for this edited corner (it is saved as a normal preset, and the corner "
            "takes the new name):",
            "Nom per a aquesta cantonada editada (es desa com un preset normal, i la "
            "cantonada passa a dir-se aix\xc3\xad):"),
        juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("name", suggested, {});
    aw->addButton (tr ("Save", "Desa"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (tr ("Cancel", "Cancel\xc2\xb7la"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    activePrompt = aw;

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<FractalAdditiveAudioProcessorEditor> (this), aw, corner] (int result)
        {
            if (safe == nullptr) return;
            const auto name = aw->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty())
            {
                const auto stored = safe->processorRef.savePlaygroundCornerAs (corner, name);
                if (stored.isEmpty())
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        safe->tr ("Save corner", "Desar cantonada"),
                        safe->tr ("Could not save the corner. Try a different name.",
                                  "No s'ha pogut desar la cantonada. Prova un altre nom."),
                        safe.getComponent());
                else
                {
                    safe->refreshPresetList();            // the new file is a preset like any other
                    safe->playgroundPanel.refresh (true); // the corner's "*" is gone
                    safe->refreshCornerEditUi();
                }
            }
            safe->refocus();
        }), true);
}

// WHOSE PATCH ARE THESE CONTROLS? While a playground corner is being edited the answer
// is "not yours", and the preset field is the only place the main window ever said
// whose it was - so it says THAT instead, and stops being a way to load anything.
// See FractalAdditiveAudioProcessor::beginPlaygroundCornerEdit, rules 1 and 3.
void FractalAdditiveAudioProcessorEditor::refreshCornerEditUi()
{
    const int  ed   = processorRef.getPlaygroundEditCorner();
    const auto disp = ed >= 0 ? juce::String::charToString ((juce::juce_wchar) ('A' + ed)) + ": "
                                  + processorRef.getPlaygroundCornerDisplayName (ed)
                              : juce::String();
    // The FIELD's text, not just the mode, is the change detector: a "*" appears the
    // moment any control moves - with the corner, the mode and everything else unchanged.
    if (ed == lastEditCornerSeen && (ed < 0 || presetBox.getText() == disp))
        return;
    lastEditCornerSeen = ed;

    if (ed >= 0)
    {
        // Park the LIVE patch's own name on the way in - once, on the transition, or a
        // later refresh would "park" the corner's name over it and the real one is gone.
        if (! cornerEditUiActive)
        {
            cornerEditUiActive         = true;
            presetTextBeforeCornerEdit = presetBox.getText();
        }
        presetBox.setText (disp, juce::dontSendNotification);
        presetBox.setEnabled (false);   // a corner is not "the loaded preset" (rule 3)
        presetDirtyMark.setVisible (false);
        presetBox.setTooltip (tr ("These controls are a PLAYGROUND CORNER right now, not your patch. "
                                  "Your own sound is kept and comes back when you press OK in the "
                                  "playground window. A * means this corner no longer matches the preset "
                                  "it was loaded from.",
                                  "Aquests controls s\xc3\xb3n ARA una CANTONADA del laboratori, no el teu "
                                  "patch. El teu so es guarda i torna quan prems OK a la finestra del "
                                  "laboratori. Un * vol dir que aquesta cantonada ja no coincideix amb el "
                                  "preset d'on ve."));
        savePresetButton.setButtonText (tr ("Save ", "Desa ")
                                        + juce::String::charToString ((juce::juce_wchar) ('A' + ed)));
        savePresetButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffffc95c));
        savePresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        savePresetButton.setTooltip (tr ("Save this edited corner as a preset of its own, under a new name. "
                                         "The corner then takes that name and loses its *. Your own patch is "
                                         "not written anywhere.",
                                         "Desa aquesta cantonada editada com un preset propi, amb un nom nou. "
                                         "La cantonada passa a dir-se aix\xc3\xad i perd l'*. El teu patch no "
                                         "s'escriu enlloc."));
    }
    else
    {
        // Only put the parked name back if we were REALLY in edit mode. A plain refresh
        // (a language switch, a first paint) must never overwrite the field with a name
        // from an edit session that never happened.
        if (cornerEditUiActive)
        {
            cornerEditUiActive = false;
            presetBox.setText (presetTextBeforeCornerEdit, juce::dontSendNotification);
        }
        presetBox.setEnabled (true);
        presetBox.setTooltip (tr ("Load a saved preset. The drop-down has a search box: type to filter the "
                                  "list (accents and capitals are ignored).",
                                  "Carrega un preset desat. El desplegable t\xc3\xa9 un cercador: escriu per "
                                  "filtrar la llista (els accents i les maj\xc3\xbascules s'ignoren)."));
        savePresetButton.setButtonText (tr ("Save", "Desa"));
        savePresetButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2d6da8));
        savePresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        savePresetButton.setTooltip (tr ("Save the current sound as a preset (stored in Documents/Galactic "
                                         "Sting/Presets).",
                                         "Desa el so actual com a preset (a Documents/Galactic Sting/Presets)."));
        refreshPresetDirtyMark();
    }
}

void FractalAdditiveAudioProcessorEditor::savePresetFlow()
{
    // In corner-edit mode Save belongs to the CORNER: the controls are its values, so
    // saving "the current sound" would otherwise write somebody's corner into the user's
    // own preset name. The corner flow asks for a name of its own (the user's words:
    // "que dandole a save se pueda guardar luego con otro nombre").
    if (const int ed = processorRef.getPlaygroundEditCorner(); ed >= 0)
    { saveCornerFlow (ed); return; }

    // Drifted (some parameter no longer matches what THIS preset was saved with):
    // suggest a name that is NOT the original, so a reflex Save/Enter creates a
    // separate preset instead of silently overwriting the one that still sounds
    // as originally saved.
    auto suggested = presetBox.getText();
    if (processorRef.presetHasDrifted())
    {
        auto base = suggested.trimCharactersAtEnd ("*").trim();
        const auto existing = processorRef.getPresetNames();
        int n = 2;
        suggested = base + " " + juce::String (n);
        while (existing.contains (suggested)) suggested = base + " " + juce::String (++n);
    }
    auto* aw = new juce::AlertWindow (tr ("Save preset", "Desar preset"),
                                      tr ("Name for this preset:", "Nom per a aquest preset:"),
                                      juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("name", suggested, {});
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
                    safe->refreshPresetDirtyMark(); // the fresh save matches itself - no drift
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
                                         16, // fixed result count

                                         chordMinMaxBox.getSelectedId() == 2,
                                         juce::jlimit (0.0, 600.0, chordMinEdit.getText().getDoubleValue()),
                                         tuneScopeBox.getSelectedId() != 2, // SCOPE: 0 vs all
                                         err))
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Chords based on beating", "Acords basats en batec"), err, this);
    refocus();
}

//==============================================================================
// STEPS AND PERIODS for the register range.
//
// The range that decides WHICH REGISTERS a spectrum is optimised for is ONE number: N
// STEPS - degrees of the loaded scale - counted upward from the note being played. It was
// note names once ("C2".."C4"), then period numbers, and the notation was never the real
// problem: both named ABSOLUTE pitches, so the adaptation answered with the same spectrum
// whichever register was under the fingers. A count anchored on the played note follows
// the hands, and a step is the one unit every tuning here really has - in 31-EDO nothing
// is a C and in Bohlen-Pierce "octave" is simply the wrong word, but both have steps.
//
// PERIODS are still how the width is READ BACK (N steps / degrees per period), and the
// anchor is named the way the keyboard names it: the period number printed beside every
// degree-0 key, counted as octaves are counted with the reference key (60, degree 0) as
// period 3 - what the rest of the product calls C3 - plus the degree inside it. So "3:0"
// can be read straight off the instrument. In 12-EDO periods and octaves coincide.
int FractalAdditiveAudioProcessorEditor::registerDegreesPerPeriod() const
{
    return juce::jmax (1, processorRef.getScaleDegrees());
}

juce::String FractalAdditiveAudioProcessorEditor::registerPeriodText (int key) const
{
    return juce::String (ScaleKeyboard::periodNumberForNote (juce::jlimit (0, 127, key),
                                                             registerDegreesPerPeriod()));
}

// A key, named the way the keyboard names it: the PERIOD it falls in and its DEGREE inside
// that period ("3:0" is the reference key). This is how the anchor is shown, and it has to
// be readable off the instrument - the period number is printed beside every degree-0 key,
// and the degree is what the user counts up from there.
juce::String FractalAdditiveAudioProcessorEditor::registerKeyText (int key) const
{
    const int D = registerDegreesPerPeriod();
    const int k = juce::jlimit (0, 127, key);
    const int p = ScaleKeyboard::periodNumberForNote (k, D);
    const int first = ScaleKeyboard::noteForPeriodNumber (p, D);
    return registerPeriodText (k) + ":" + juce::String (first >= 0 ? k - first : 0);
}

// N as it stands: what the user typed, or - until they type - the default width for the
// scale loaded right now (see adaptRegSteps and kAdaptRegisterDefaultPeriods).
int FractalAdditiveAudioProcessorEditor::registerStepsEffective() const
{
    return adaptRegSteps >= 0 ? adaptRegSteps : processorRef.defaultRegisterSteps();
}

// ...and how many of those steps the 128-key board really has above the anchor. They differ
// only near the top of the keyboard, and when they do the panel SAYS the shorter number:
// the alternative - quietly moving the anchor down so N fits - would optimise for registers
// the user never asked about, and nothing on screen would show it.
int FractalAdditiveAudioProcessorEditor::registerStepsFitting() const
{
    return juce::jlimit (0, 127 - processorRef.registerAnchorKey(), registerStepsEffective());
}

// The key whose ACTIVE-TUNING pitch is nearest `hz`. Through the tuning, never through
// 12-EDO arithmetic: an octave shift, a reference-A change and a .tun table anchor all
// move what a key sounds at, and this is how a frequency is named back to the user.
int FractalAdditiveAudioProcessorEditor::nearestKeyForHz (double hz) const
{
    if (! (hz > 0.0) || ! std::isfinite (hz)) return 60;
    int best = 60; double bestErr = 1.0e18;
    for (int m = 0; m < 128; ++m)
    {
        const double f = processorRef.getNoteFrequency (m);
        if (! (f > 0.0)) continue;
        const double e = std::abs (std::log2 (hz / f));
        if (e < bestErr) { bestErr = e; best = m; }
    }
    return best;
}

// WHAT N MEANS RIGHT NOW, in one line: the note the range is counted from, the count
// itself, and the width that comes to in periods. A step count is exact in every tuning and
// unreadable as a musical width - "62" is two periods in 31-EDO and nobody should have to
// do that division - so the panel does the arithmetic out loud.
//
// The steps that really FIT are said whenever they differ from the steps asked for: at the
// top of the keyboard the range is genuinely shorter than N, and a control that showed N
// while the search used seven steps would be the panel lying about its own settings.
// ...as a function of its own numbers, so the layout probe can measure BOTH of its forms at
// their widest. Only one is ever on screen, and the one that overflows is always the other.
juce::String FractalAdditiveAudioProcessorEditor::adaptRegSpanLine (const juce::String& anchor,
                                                                    int steps, int fit,
                                                                    const juce::String& per) const
{
    // Built by hand rather than through tr(): the numbers are runtime values, so this is
    // the one place where the two languages are assembled instead of chosen. Both forms
    // still exist in both languages - the layout probe measures all four.
    // "126 of 127" rather than a sentence about the keyboard running out: the long form
    // ("only 126 steps fit") overflowed this one line in Catalan by 7 px at the widest
    // numbers a one-degree-per-period scale can produce, and RhythmVerify measures exactly
    // that case because it is the one nobody sees until the day they play up there.
    const juce::String count = fit == steps ? juce::String (steps)
                                            : juce::String (fit) + "/" + juce::String (steps);
    if (! catalan)
        return count + " steps above " + anchor + " = " + per + " periods";
    return count + " passos sobre " + anchor + " = " + per.replaceCharacter ('.', ',')
         + juce::String::fromUTF8 (" per\xc3\xado" "des");
}

juce::String FractalAdditiveAudioProcessorEditor::adaptRegSpanText() const
{
    const int steps = registerStepsEffective(), fit = registerStepsFitting();
    return adaptRegSpanLine (registerKeyText (processorRef.registerAnchorKey()), steps, fit,
                             juce::String (fit / (double) registerDegreesPerPeriod(), 1));
}

// The anchor MOVES - it is the note being played - so the line under the field has to be
// rebuilt as the hands move, and the follower has to hear about it. Written only when the
// text really changed: this runs at timer rate, and re-setting a Label's text repaints it.
void FractalAdditiveAudioProcessorEditor::syncAdaptRegisterSpan()
{
    const auto want = adaptRegSpanText();
    const auto n = juce::String (registerStepsEffective());
    // The FIELD only follows the tuning while N is untyped (adaptRegSteps < 0). Never with
    // sendNotification and never when it already reads that, so a field the user is typing
    // in is not yanked out from under the caret.
    if (adaptRegStepsEdit.getText() != n && ! adaptRegStepsEdit.hasKeyboardFocus (true))
        adaptRegStepsEdit.setText (n, juce::dontSendNotification);
    if (want == adaptRegSpanLbl.getText()) return;
    adaptRegSpanLbl.setText (want, juce::dontSendNotification);
    syncRealtimeLink();   // the follower's settings carry the range, so it must hear this
}

// The user typed a width. Unreadable text snaps back to the number in force instead of
// being taken as "no range" - which the processor would read as the default and quietly
// undo what they were trying to type. A typed number is theirs from then on: it survives
// every tuning change, because N is a statement about the tuning's own units and
// re-deriving it on a scale swap is the silent rewrite this control exists to prevent.
void FractalAdditiveAudioProcessorEditor::commitAdaptRegisterSteps()
{
    const auto s = adaptRegStepsEdit.getText().trim();
    if (s.isNotEmpty() && s.containsOnly ("0123456789"))
        // 0 IS legal and means "just the note I am playing" - one register, the old
        // single-pitch objective, chosen on purpose rather than arrived at by accident.
        // 127 is the whole board and cannot depend on the tuning, so this clamp can never
        // reinterpret a width the way a clamp in periods would.
        adaptRegSteps = juce::jlimit (0, 127, s.getIntValue());
    adaptRegStepsEdit.setText (juce::String (registerStepsEffective()), juce::dontSendNotification);
    adaptRegSpanLbl.setText (adaptRegSpanText(), juce::dontSendNotification);
    syncRealtimeLink();
    resized();
}

// Everything the panel's controls say, in the form the processor's realtime links need.
// ONE place: the two links, the manual Find and the manual Adapt all read the same boxes,
// and a second copy of "what the panel currently means" would be free to drift out of
// step with this one - a follower quietly searching with a period the user had changed.
FractalAdditiveAudioProcessor::RtSettings
FractalAdditiveAudioProcessorEditor::realtimeSettingsFromPanel() const
{
    FractalAdditiveAudioProcessor::RtSettings s;
    s.notes = juce::jmax (2, chordNotesBox.getSelectedId());
    if (! TuningManager::parsePeriodSpec (chordPeriodEdit.getText(), s.periodRatio, s.periodLine))
    { s.periodRatio = 2.0; s.periodLine = "2/1"; }
    s.maxima      = chordMinMaxBox.getSelectedId() == 2;
    s.minGapCents = juce::jlimit (0.0, 600.0, chordMinEdit.getText().getDoubleValue());
    s.rootOnly    = tuneScopeBox.getSelectedId() != 2;

    s.adapt.partials    = juce::jmax (2, adaptPartialsBox.getSelectedId());
    s.adapt.periodRatio = s.periodRatio;
    s.adapt.minGapCents = s.minGapCents;
    s.adapt.rootOnly    = s.rootOnly;
    s.adapt.goal        = juce::jlimit (0, 2, adaptGoalBox.getSelectedId() - 1);
    s.adapt.rangeLo     = adaptLoEdit.getText().getDoubleValue();
    s.adapt.rangeHi     = adaptHiEdit.getText().getDoubleValue();
    s.adapt.shapeAmps   = adaptAmpsToggle.getToggleState();
    // THE REGISTER RANGE, as the STEP COUNT the field shows. Not as two frequencies: the
    // range hangs off the note being played, and Hz resolved here would freeze the anchor
    // at whatever was under the fingers when this ran - which, with the window shut, is for
    // ever. The processor turns anchor + N into Hz through the active tuning (see
    // adaptRegistersFor), which is also where the octave shift, the reference A and a .tun
    // table's anchor live, so nothing here has to know about any of them.
    s.adapt.regSteps = registerStepsEffective();
    s.lookahead      = lookaheadOn;
    return s;
}

//==============================================================================
// Test hooks - see PanelProbe in the header. Each one goes through the SAME callback a
// click or a keystroke would, so what the verifier exercises is the shipping wiring.
FractalAdditiveAudioProcessorEditor::PanelProbe
FractalAdditiveAudioProcessorEditor::panelProbeForTests() const
{
    PanelProbe pr;
    pr.mode               = specModeBox.getSelectedId();
    pr.adaptTarget        = adaptTargetBox.getSelectedId();
    pr.adaptTargetEnabled = adaptTargetBox.isEnabled();
    pr.period             = chordPeriodEdit.getText();
    return pr;
}

FractalAdditiveAudioProcessorEditor::RegisterProbe
FractalAdditiveAudioProcessorEditor::registerProbeForTests() const
{
    RegisterProbe r;
    r.steps     = registerStepsEffective();
    r.fitting   = registerStepsFitting();
    r.anchorKey = processorRef.registerAnchorKey();
    r.keyLo     = r.anchorKey;
    r.keyHi     = juce::jlimit (0, 127, r.anchorKey + r.fitting);
    r.loHz      = processorRef.getNoteFrequency (r.keyLo);
    r.hiHz      = processorRef.getNoteFrequency (r.keyHi);
    r.stepsText = adaptRegStepsEdit.getText();
    r.spanText  = adaptRegSpanText();
    return r;
}

void FractalAdditiveAudioProcessorEditor::setAdaptRegisterStepsForTests (const juce::String& n)
{
    adaptRegStepsEdit.setText (n, juce::dontSendNotification);
    commitAdaptRegisterSteps();   // exactly what Return and focus-lost do
}

void FractalAdditiveAudioProcessorEditor::setSpecModeForTests (int mode)
{
    specModeBox.setSelectedId (mode, juce::sendNotificationSync);
}

void FractalAdditiveAudioProcessorEditor::setAdaptTargetForTests (int id)
{
    adaptTargetBox.setSelectedId (id, juce::sendNotificationSync);
}

void FractalAdditiveAudioProcessorEditor::setTuneSourceForTests (int id)
{
    tuneSourceBox.setSelectedId (id, juce::sendNotificationSync);
}

void FractalAdditiveAudioProcessorEditor::setAdaptGoalForTests (int id)
{
    adaptGoalBox.setSelectedId (id, juce::sendNotificationSync);
}

void FractalAdditiveAudioProcessorEditor::setPeriodTextForTests (const juce::String& t)
{
    chordPeriodEdit.setText (t, juce::dontSendNotification);
    syncRealtimeLink();                       // exactly what onFocusLost does
}

void FractalAdditiveAudioProcessorEditor::pressAdaptRealtimeForTests()
{
    if (adaptRtBtn.onClick) adaptRtBtn.onClick();
}

// Park the Period the user typed under the mode they typed it in, and show the mode being
// entered its own value (see periodTextByMode). Only modes 1 and 2 own this field - the
// segment picker has a period box of its own - so arriving at or leaving mode 3 changes
// nothing except which value is waiting when the field comes back.
void FractalAdditiveAudioProcessorEditor::switchPeriodTextForMode (int newMode)
{
    if (newMode == lastSpecMode) return;
    if (lastSpecMode == 1 || lastSpecMode == 2)
        periodTextByMode[lastSpecMode == 2 ? 1 : 0] = chordPeriodEdit.getText();
    if (newMode == 1 || newMode == 2)
        chordPeriodEdit.setText (periodTextByMode[newMode == 2 ? 1 : 0], juce::dontSendNotification);
    lastSpecMode = newMode;
}

// The adaptation's target selector, kept honest about the realtime link. While the link
// is on the box READS "to the chord held now" and cannot be changed, because that is the
// only target the follower uses; when it goes off - by the button, by leaving the mode, or
// because the processor refused the link - the user's own choice comes straight back, so
// the hand-pressed Adapt still works on whichever target they picked.
void FractalAdditiveAudioProcessorEditor::syncAdaptTargetLock()
{
    if (adaptRtOn)
    {
        if (adaptTargetBeforeRt == 0) adaptTargetBeforeRt = adaptTargetBox.getSelectedId();
        adaptTargetBox.setSelectedId (2, juce::dontSendNotification);
    }
    else if (adaptTargetBeforeRt > 0)
    {
        adaptTargetBox.setSelectedId (adaptTargetBeforeRt, juce::dontSendNotification);
        adaptTargetBeforeRt = 0;
    }
    adaptTargetBox.setEnabled (! adaptRtOn);
}

void FractalAdditiveAudioProcessorEditor::syncRealtimeLink()
{
    using RtLink = FractalAdditiveAudioProcessor::RtLink;
    const auto s = realtimeSettingsFromPanel();

    const int mode = specModeBox.getSelectedId();
    // A realtime button only counts while its own submenu is the one on screen: leaving
    // the mode ends the link (and commits its undo step) instead of leaving it running
    // invisibly behind another panel.
    RtLink want = RtLink::Off;
    if (mode == 1 && tuneSourceBox.getSelectedId() == 2 && chordRtOn)
        want = RtLink::TuningFromSpectrum;
    else if (mode == 2 && adaptRtOn)
        // REALTIME MEANS "FOLLOW WHAT I PLAY". The other target - the loaded tuning - is a
        // fixed set of notes that only changes when the user loads another scale, so a
        // follower aimed at it re-applied the SAME adaptation for ever: in 12ED2 the panel
        // simply put the twelve degrees of 12ED2 into the spectrum and kept putting them
        // there. That is what the Adapt button is for (one press, one result); the realtime
        // button is aimed at the chord under the fingers, always. The target selector is
        // forced to say so while the link is lit (see adaptRtBtn.onClick), so the panel
        // never claims a target the link is not using.
        want = RtLink::SpectrumFromChord;

    if (want != processorRef.getRealtimeLink()) processorRef.setRealtimeLink (want, s);
    else                                       processorRef.updateRealtimeSettings (s);
}

//==============================================================================
// THE COST OF A RUN, AND THE ONE LINE THAT SAYS WHAT IS GOING ON.
//
// A duration a musician reads at a glance. "45231 ms" is a number; "45 s" is an answer.
// The units are the same word in both languages, so this needs no tr().
static juce::String durationText (double ms, bool compact)
{
    if (ms < 950.0)   return juce::String (ms * 0.001, 1) + (compact ? "s" : " s");
    if (ms < 60000.0) return juce::String ((int) std::lround (ms * 0.001)) + (compact ? "s" : " s");
    return juce::String (ms / 60000.0, 1) + (compact ? "min" : " min");
}

// What ONE run costs with the panel's settings as they stand. The target's note count is
// what makes this big or small, so it is read from the same two calls the run itself would
// use - not from the degree box, which is the SEARCH's control and says nothing here.
double FractalAdditiveAudioProcessorEditor::adaptCostMs (bool realtime) const
{
    const auto s = realtimeSettingsFromPanel();
    double f0 = 0.0;
    // REALTIME always aims at the held chord (see syncRealtimeLink), so its price is the
    // held chord's price even while the target box shows something else.
    const auto alphas = (realtime || adaptTargetBox.getSelectedId() == 2)
                            ? processorRef.heldChordAsTargetAlphas (f0)
                            : processorRef.tuningAsTargetAlphas (f0);
    if (alphas.empty()) return 0.0;
    if (! (f0 > 0.0)) f0 = processorRef.analysisAnchorHz();
    return processorRef.estimateAdaptMs (s.adapt, (int) alphas.size(), f0, realtime);
}

// The button carries its own price. Under a second it says nothing extra - that is not a
// wait anybody plans around - and while a run is going it becomes the way to stop it,
// which is the whole reason an expensive search can be offered instead of refused.
void FractalAdditiveAudioProcessorEditor::refreshAdaptButtonText()
{
    if (processorRef.isSpectrumAdaptRunning())
    {
        adaptGoBtn.setButtonText (tr ("Stop", "Atura"));
        return;
    }
    const double ms = adaptCostMs (false);
    adaptGoBtn.setButtonText (ms >= 950.0
        ? tr ("Adapt ~", "Adapta ~") + durationText (ms, true)
        : tr ("Adapt", "Adapta"));
}

// ONE READ-OUT, ONE JOB: what is happening, or what will happen if you press. In priority
// order, because only one of these is ever the thing the player needs to know:
//   1. a run is going          -> how far along it is
//   2. REALTIME cannot keep up -> say the results will LAG, and let it run anyway
//   3. REALTIME is running     -> that it is following, and that it is alive
//   4. the next press is heavy -> how long, and that it can be stopped
//   5. something was adapted   -> how much beating it removed, in percent
// The numbers this replaced ("avg C2-C4: 769.9 -> 27.8", "sounding now: 31.4") are in the
// tooltip. They were two lines of raw Sethares roughness with no scale attached to them,
// and the second line was the one that clipped.
// The five sentences, each in ONE place. Built as functions of their numbers rather than
// written where they are used, so the layout probe can measure every one of them with its
// widest possible content - only one is on screen at a time, and the state that clips is
// always the one nobody happened to be looking at.
juce::String FractalAdditiveAudioProcessorEditor::adaptLineWorking (int pct) const
{
    return tr ("Working... ", "Treballant... ") + juce::String (pct)
         + tr ("% - press Stop to cancel.", "% - prem Atura per cancel\xc2\xb7lar.");
}

// NOT a refusal. A follower whose search outlasts its own update slot still works - it
// simply answers the chord you played a moment ago. Saying so is the honest thing;
// switching the link off, or quietly refusing the settings, is not.
juce::String FractalAdditiveAudioProcessorEditor::adaptLineLag (double ms) const
{
    return tr ("Realtime: ~", "Temps real: ~") + durationText (ms, false)
         + tr (" per update - results lag behind your playing.",
               " per canvi - els resultats van endarrerits.");
}

juce::String FractalAdditiveAudioProcessorEditor::adaptLineFollowing (int updates) const
{
    return tr ("Realtime: following what you play - ", "Temps real: segueix el que toques - ")
         + juce::String (updates) + tr (" updates.", " canvis.");
}

juce::String FractalAdditiveAudioProcessorEditor::adaptLineCost (double ms) const
{
    return tr ("Adapt takes about ", "Adapta triga uns ") + durationText (ms, false)
         + tr (". Keep playing; Stop cancels it.", ". Segueix tocant; Atura la cancel\xc2\xb7la.");
}

juce::String FractalAdditiveAudioProcessorEditor::adaptLineResult (int pct, bool less,
                                                                   const juce::String& span,
                                                                   bool drift) const
{
    juce::String t;
    if (pct >= 0)
        t << juce::String (pct)
          << (less ? tr ("% less beating over periods ", "% menys batec als per\xc3\xado" "des ")
                   : tr ("% more beating over periods ", "% m\xc3\xa9s batec als per\xc3\xado" "des "))
          << span << ".";
    else
        t << tr ("Adapted over periods ", "Adaptat als per\xc3\xado" "des ") << span << ".";
    // The caveat is a TAG, not a sentence: the read-out has two lines and the full
    // explanation ("Reps stacks the adapted set again one period up and a fractal level
    // self-convolves it, so what sounds has partials the search never saw") lives in the
    // tooltip. What has to be on screen is that the two are not the same spectrum.
    if (drift)
        t << tr (" (+ Reps/Fractal partials)", " (+ parcials de Reps/Fractal)");
    return t;
}

juce::String FractalAdditiveAudioProcessorEditor::adaptReadoutText() const
{
    if (processorRef.isSpectrumAdaptRunning())
        return adaptLineWorking ((int) std::lround (processorRef.getSpectrumAdaptProgress() * 100.0f));

    if (processorRef.getRealtimeLink() == FractalAdditiveAudioProcessor::RtLink::SpectrumFromChord)
    {
        const double live = adaptCostMs (true);
        return live > FractalAdditiveAudioProcessor::kRtAdaptBudgetMs
                   ? adaptLineLag (live)
                   : adaptLineFollowing (processorRef.getRealtimeApplies());
    }

    const double ms = adaptCostMs (false);
    if (ms > dissonance::kAdaptWarnMs) return adaptLineCost (ms);
    return adaptResultLine;
}

// Everything the read-out no longer prints. Rebuilt with each result so the raw numbers
// are always the LAST run's, never a stale set left over from the one before it.
juce::String FractalAdditiveAudioProcessorEditor::adaptReadoutTip() const
{
    juce::String t = adaptInfoTipBase;
    if (adaptResultTip.isNotEmpty()) t << "\n\n" << adaptResultTip;
    const double ms = adaptCostMs (false), live = adaptCostMs (true);
    if (ms > 0.0)
        t << "\n\n" << tr ("Next Adapt: about ", "Propera Adapta: uns ") << durationText (ms, false)
          << tr (" (one realtime update: ", " (una actualitzaci\xc3\xb3 en temps real: ")
          << durationText (live, false) << ", "
          << tr ("budget ", "pressupost ")
          << durationText (FractalAdditiveAudioProcessor::kRtAdaptBudgetMs, false) << ").";
    return t;
}

// The two REALTIME buttons' look, plus the one case they must OWN UP to: the processor
// refused the link (a feedback loop) and switched it off under us. The button has to stop
// claiming to be on, and the reason has to be said out loud, not swallowed.
void FractalAdditiveAudioProcessorEditor::paintRealtimeButtons()
{
    using RtLink = FractalAdditiveAudioProcessor::RtLink;
    const auto live = processorRef.getRealtimeLink();
    if ((chordRtOn || adaptRtOn) && live == RtLink::Off)
    {
        const auto why = processorRef.getRealtimeRefusal();
        if (why.isNotEmpty())
        {
            chordRtOn = adaptRtOn = false;
            noticeOverlay.title = tr ("Realtime switched off", "Temps real desactivat");
            noticeOverlay.body  = why;
            noticeOverlay.setVisible (true);
            noticeOverlay.toFront (false);
            noticeOverlay.repaint();
            resized();
        }
    }
    syncAdaptTargetLock();   // a link refused above releases the target box with it
    const juce::Colour lit (0xffd23b6b), dim (0xff44445e);
    chordRtBtn.setColour (juce::TextButton::buttonColourId,
                          live == RtLink::TuningFromSpectrum ? lit : dim);
    adaptRtBtn.setColour (juce::TextButton::buttonColourId,
                          (live == RtLink::SpectrumFromTuning || live == RtLink::SpectrumFromChord)
                              ? lit : dim);
    // LOOKAHEAD has THREE states and the colour says which: off; armed but idle (the
    // follower is not running, so no latency is being paid and none would help); and
    // actually delaying. A single lit/dim pair would have let "armed" look like "working"
    // while the plugin reported zero latency.
    const bool armed  = lookaheadOn;
    const bool active = processorRef.isLookaheadActive();
    lookaheadBtn.setColour (juce::TextButton::buttonColourId,
                            active ? lit : (armed ? lit.withAlpha (0.45f) : dim));
    // The button STATES ITS PRICE while it is paying it: a latency you cannot see the
    // size of is the thing people distrust about lookahead in any plugin.
    //
    // ...and, once there is one, THE MEASURED LANDING beside it: how long the last chord
    // change really took to reach the spectrum, on this machine, at these settings. The
    // budget is a fixed 250 ms and heavy adapt settings (16 or 24 partials against a wide
    // chord) can outgrow it - so the alternative to printing this number was a lookahead
    // that quietly stopped being early and never said so. "LATE" is the whole point of
    // the read-out; it is not decoration.
    juce::String txt;
    if (active)
    {
        const double budMs = (double) processorRef.getLookaheadSamples() * 1000.0
                           / juce::jmax (1.0, processorRef.getSampleRate());
        txt = tr ("LOOKAHEAD ", "ANTICIPACI\xc3\x93 ") + juce::String ((int) std::lround (budMs)) + " ms";
        if (const double land = processorRef.getLastRealtimeLandingMs(); land >= 0.0)
            txt << tr (" | last ", " | darrer ") << juce::String ((int) std::lround (land)) << " ms"
                << (land <= budMs ? juce::String() : tr (" LATE", " TARD"));
    }
    else if (armed) txt = tr ("LOOKAHEAD (idle)", "ANTICIPACI\xc3\x93 (en espera)");
    else            txt = tr ("LOOKAHEAD", "ANTICIPACI\xc3\x93");
    lookaheadBtn.setButtonText (txt);
}

// The hand-pressed Adapt: one run against the selected target, then apply it as ONE undo
// step. The realtime button uses the same processor call with undo left to its baseline.
void FractalAdditiveAudioProcessorEditor::runSpectrumAdapt()
{
    const auto s = realtimeSettingsFromPanel();
    double f0 = 0.0;
    const bool toChord = adaptTargetBox.getSelectedId() == 2;
    const auto alphas = toChord ? processorRef.heldChordAsTargetAlphas (f0)
                                : processorRef.tuningAsTargetAlphas (f0);
    juce::String err;
    // ONE held note is a legal target, and refusing it was the bug: with a single note the
    // score IS the spectrum's own internal roughness (dissonance::selfDissonance at that
    // note's real pitch - see updateRealtimeLinks), so Adapt can still move the partials to
    // where they beat least against EACH OTHER. Only "nothing held at all" has no target.
    if (toChord ? alphas.empty() : alphas.size() < 2)
        err = toChord ? tr ("Hold at least one note and press Adapt again - this target IS "
                            "the notes under your fingers.",
                            "Mant\xc3\xa9n com a m\xc3\xadnim una nota i torna a pr\xc3\xa9mer Adapta - "
                            "aquest objectiu S\xc3\x93N les notes que tens sota els dits.")
                      : tr ("The loaded tuning has no degrees to adapt to.",
                            "L'afinaci\xc3\xb3 carregada no t\xc3\xa9 graus als quals adaptar-se.");
    else
        // No cost veto here any more: the price is on the button (see refreshAdaptButtonText)
        // and the only refusal left is the processor's ten-minute bound, which comes back
        // through `err` with the estimate in it rather than a verdict.
        // `targetIsTuning` = the target selector, stated by the caller: a tuning-targeted
        // result records the tuning as the spectrum's provenance, a chord-targeted one
        // must not (see startSpectrumAdapt).
        processorRef.startSpectrumAdapt (s.adapt, alphas, f0, err, /*realtime*/ false,
                                         /*targetIsTuning*/ ! toChord);
    if (err.isNotEmpty())
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Adapt spectrum to tuning", "Adapta l'espectre a l'afinaci\xc3\xb3"), err, this);
    refreshAdaptButtonText();
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

// HOVER preview: sound a set of ratios (relative to middle C) for as long as the
// pointer stays on that row. Any previous preview is stopped first, so only one
// combination ever sounds; hoverPreview({}) just stops.
void FractalAdditiveAudioProcessorEditor::hoverPreview (const std::vector<double>& ratios)
{
    stopChordPreview();
    if (ratios.empty() || ! (processorRef.getSampleRate() > 0.0)) return;
    // Heard as before: the whole chord together, then an ascending arpeggio - and
    // it stops the moment the pointer leaves the row (stopChordPreview above).

    static constexpr int bendValues[] = { 2, 12, 48 };
    const int bendRange = processorRef.getTuningMode() == 3 ? 48
        : bendValues[juce::jlimit (0, 2, (int) processorRef.apvts.getRawParameterValue ("bendRange")->load())];
    const double f60 = processorRef.getNoteFrequency (60);
    if (! (f60 > 0.0)) return;

    const juce::uint32 now = juce::Time::getMillisecondCounter();
    int k = 0;
    for (const double a : ratios)
    {
        if (! (a > 0.0)) continue;
        const double target = f60 * a;
        int bestKey = 60; double bestErr = 1.0e18;
        for (int m = 0; m <= 127; ++m)
        {
            const double f = processorRef.getNoteFrequency (m);
            if (! (f > 0.0)) continue;
            const double e = std::abs (std::log2 (target / f));
            if (e < bestErr) { bestErr = e; bestKey = m; }
        }
        const double semis = 12.0 * std::log2 (target / processorRef.getNoteFrequency (bestKey));
        const int wheel = juce::jlimit (0, 16383, 8192 + (int) std::lround (semis / (double) bendRange * 8192.0));
        const int ch = 9 + (k % 8);
        previewQueue.push_back ({ now, juce::MidiMessage::pitchWheel (ch, wheel) });
        previewQueue.push_back ({ now, juce::MidiMessage::noteOn (ch, bestKey, (juce::uint8) 100) });
        previewNotes.push_back ({ ch, bestKey });
        // ...then the same notes again, ascending, as an arpeggio. Everything is
        // cut the instant the pointer leaves (stopChordPreview sends all the offs).
        const juce::uint32 arpAt = now + 1500 + (juce::uint32) k * 200;
        previewQueue.push_back ({ arpAt,       juce::MidiMessage::noteOff (ch, bestKey) });
        previewQueue.push_back ({ arpAt + 10,  juce::MidiMessage::noteOn  (ch, bestKey, (juce::uint8) 100) });
        ++k;
    }
}

// Sethares dissonance of every key AGAINST the hovered one, cached: recomputed only
// when the hovered key, the timbre or the tuning changes (the per-paint provider then
// just reads the table). Analysis-only 64-partial cap, like every dissonance readout.
void FractalAdditiveAudioProcessorEditor::updateHoverDissCache (int hoveredNote)
{
    if (! juce::isPositiveAndBelow (hoveredNote, 128)) return;
    const double f0 = processorRef.getNoteFrequency (hoveredNote);
    if (! (f0 > 0.0)) return;
    // The gains revision is part of the signature because a per-partial amplitude
    // edit deliberately does NOT bump the template generation (that would restart
    // voice morphs ~60x a second while dragging) - yet it changes the sounding
    // amplitudes this strip analyses, so the cache must refresh on it.
    const double sig = (double) processorRef.getTemplateGeneration() * 1.0e5
                     + (double) processorRef.getPartialGainsRevision() * 17.0 + f0;
    if (hoveredNote == hoverDissNote && std::abs (sig - hoverDissSig) < 1.0e-9) return;

    hoverDissNote = hoveredNote;
    hoverDissSig  = sig;
    hoverDissVal.fill (-1.0f);

    // The shared analysis spectrum (sounding amplitudes, 64-cap), anchored at the
    // HOVERED key's real pitch: this strip measures every key against that key, so
    // the pair actually played is the pair analysed - same measure, honest anchor.
    const auto spec = processorRef.analysisSpectrumAt (f0);
    if (spec.empty()) return;

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
               // A per-partial amplitude edit changes the SOUNDING spectrum this
               // analyses without bumping the template generation (see
               // updateHoverDissCache) - it must refresh this list too.
               + (double) processorRef.getPartialGainsRevision() * 19.0
               + (double) processorRef.getTuningMode() * 7.0
               + (double) D * 0.5
               + (double) processorRef.apvts.getRawParameterValue ("octaveShift")->load() * 13.0
               + (double) processorRef.apvts.getRawParameterValue ("refA")->load() * 0.01;
    for (int d = 1; d <= juce::jmin (D, 16); ++d) sig += processorRef.getDegreeRatio (d) * (double) (d + 3);
    if (std::abs (sig - lastScaleSig) < 1.0e-6) return;

    // The signature includes the template generation, so a modulator on ANY spectrum
    // control makes it change on essentially every tick. The analysis is advisory UI, so
    // rate-limit it to ~4 Hz: without this it ran at the full 30 Hz timer rate for as
    // long as the modulator kept moving. (The analysis itself no longer recompiles the
    // spectrum - see computeActiveScaleDissonance - but it is still an O(partials x
    // degrees) roughness sweep and there is no reason to run it faster than the eye.)
    const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
    if (nowMs - lastScaleDissMs < 250) return;
    lastScaleDissMs = nowMs;
    lastScaleSig    = sig;

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
            + tr (" usable extremes in the chosen period. A curve only forms a notch where partials interact; sparse spectra (few partials, e.g. a near-sine) form few notches - a pure sine forms none. The scale was generated with all the extremes that exist. Add partials (higher N, Fractal, flatter Decay) or widen the period for more.",
                  " extrems usables dins del per\xc3\xado" "de triat. La corba nom\xc3\xa9s fa un clot on els parcials interactuen; els espectres escassos (pocs parcials) en fan pocs - un sinus pur no en fa cap. L'escala s'ha generat amb tots els extrems existents. Afegeix parcials (N m\xc3\xa9s alta, Fractal, Caiguda m\xc3\xa9s plana) o eixampla el per\xc3\xado" "de per obtenir-ne m\xc3\xa9s.");
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
            tr ("Generate a scale first - press 'Find'.",
                "Primer genera una escala - prem 'Troba'."), this);
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
    // Harmonics is the continuous axis now (Even 0 - All 1 - Odd 2 - Prime 3): reading
    // the old "selection" id returned NULLPTR and crashed the host on this very path.
    const float selAx    = ap.getRawParameterValue ("harmonics")->load();
    const bool  isPrime  = selAx > 2.5f;
    const bool wl        = ap.getRawParameterValue ("primeMode")->load() > 0.5f;
    const int  limitIdx  = (int) ap.getRawParameterValue ("primeLimit")->load();
    unsigned mask = 0;
    for (int i = 0; i < 9; ++i)
        if (ap.getRawParameterValue ("primeWL" + juce::String (i))->load() > 0.5f)
            mask |= (1u << i);

    // Plain-language cause + the single most direct fix. The synth has two
    // harmonic filters (Harmonics and Prime Control) that BOTH must let a
    // harmonic through; when they overlap to nothing, every partial is removed.
    juce::String why;
    if (isPrime && wl)
        why = tr ("Two filters are cancelling out.  'Harmonics = Prime' keeps only prime-numbered harmonics, and the Prime whitelist then keeps only harmonics built from the ticked primes.  A prime is only itself, so together they can keep nothing above 23.\n\nFix: set Harmonics to 'All', or set Prime Control to 'Limit / Off'.",
                  "Dos filtres s'anul\xc2\xb7len.  'Harm\xc3\xb2nics = Primers' nom\xc3\xa9s deixa els harm\xc3\xb2nics de n\xc3\xbamero primer, i la Llista de primers nom\xc3\xa9s deixa els fets amb els primers marcats.  Un primer nom\xc3\xa9s \xc3\xa9s ell mateix, aix\xc3\xad que junts no poden deixar res per sobre de 23.\n\nSoluci\xc3\xb3: posa Harm\xc3\xb2nics en 'Tots', o el Control de primers en 'L\xc3\xadmit / No'.");
    else if (wl && mask == 0)
        why = tr ("The Prime whitelist has no primes ticked, so no harmonic is allowed through.\n\nFix: tick at least one prime, or switch Prime Control to 'Limit'.",
                  "La Llista de primers no t\xc3\xa9 cap primer marcat, aix\xc3\xad que no passa cap harm\xc3\xb2nic.\n\nSoluci\xc3\xb3: marca almenys un primer, o posa el Control de primers en 'L\xc3\xadmit'.");
    else if (isPrime && ! wl && limitIdx > 0)
        why = tr ("'Harmonics = Prime' keeps only prime harmonics, but the Prime Limit then removes every prime bigger than the limit - and your Base N starts the series above it.\n\nFix: raise the Prime Limit (or set it to Off), or lower Base N.",
                  "'Harm\xc3\xb2nics = Primers' nom\xc3\xa9s deixa harm\xc3\xb2nics primers, per\xc3\xb2 el L\xc3\xadmit de primers treu tot primer m\xc3\xa9s gran que el l\xc3\xadmit - i la teva Base N comen\xc3\xa7""a la s\xc3\xa8rie per sobre.\n\nSoluci\xc3\xb3: puja el L\xc3\xadmit de primers (o posa'l en No), o abaixa la Base N.");
    else
        why = tr ("The Harmonics filter and the Prime Control filter, applied together, remove every harmonic of the current Base N.\n\nFix: relax either filter (Harmonics = All, or Prime Control = Off), or change Base N.",
                  "El filtre d'Harm\xc3\xb2nics i el de Control de primers, aplicats alhora, treuen tots els harm\xc3\xb2nics de la Base N actual.\n\nSoluci\xc3\xb3: relaxa qualsevol dels dos (Harm\xc3\xb2nics = Tots, o Control de primers = No), o canvia la Base N.");

    warningOverlay.title = tr ("No sound: every partial is filtered out",
                               "Sense so: s'han filtrat tots els parcials");
    warningOverlay.body  = why;
    warningOverlay.setVisible (true);
    warningOverlay.repaint();
}

//==============================================================================
// Both floating tool windows stay ABOVE the editor through OS-level ownership - the
// ONE mechanism shared by the partial-amplitude window and the playground. Without
// it they are sibling top-level windows, and on Windows the first click on the main
// Fiblo window raised it over the window the user had just opened: the pad or the
// knob grid vanished mid-gesture. Ownership (unlike setAlwaysOnTop) keeps them above
// the editor's top-level window ONLY - never above other applications - and follows
// it on minimise. Re-applied on every open because a host can re-dock the editor
// under a different top-level window without recreating it; attachToEditorRoot is
// idempotent when nothing changed.
void FractalAdditiveAudioProcessorEditor::attachFloatingWindow (juce::TopLevelWindow& w)
{
    if (auto* wp = w.getPeer())
        if (auto* ep = getPeer())
            floatwin::attachToEditorRoot (wp->getNativeHandle(), ep->getNativeHandle());
}

//==============================================================================
// The pencil's window. Built on first use and then kept alive (hidden) for the rest of
// the session, so reopening it lands on the size and scroll position you left it at.
void FractalAdditiveAudioProcessorEditor::ensurePartialWindow()
{
    if (partialWindow == nullptr)
    {
        partialWindow = std::make_unique<PartialAmpWindow> (
            tr ("Fibl\xc3\xb3 - partial amplitudes", "Fibl\xc3\xb3 - amplituds dels parcials"));
        partialWindow->setLookAndFeel (lnf.get());   // ditto for the frame and the resizer
        partialWindow->setContentNonOwned (&partialPanel, false);
        // Wide enough for ~13 columns: a harmonic patch's first two octaves of partials
        // are then on screen without scrolling at all.
        partialWindow->centreWithSize (860, 560);
        partialWindow->onHide = [this] { if (partialWindow != nullptr) partialWindow->setVisible (false); refocus(); };
        // Play from the computer keyboard while this window is in front. The events go
        // through the editor's OWN handlers, so there is one key map, one undo binding
        // and one set of swallowed keys - not a second copy here to drift out of step.
        // Everything the window itself uses keeps working: it is the FOCUSED component
        // that gets first refusal (the close button, the On pill, the Reset button), the
        // grid is driven entirely by the mouse (SHIFT+click ranges, knob drags), and the
        // one keystroke the window documents - Ctrl+Z - is handled by keyPressed before
        // any play key is looked at, so undo never also rings a note.
        partialWindow->onKey      = [this] (const juce::KeyPress& k) { return keyPressed (k); };
        partialWindow->onKeyState = [this] { return keyStateChanged (true); };
    }
}

void FractalAdditiveAudioProcessorEditor::openPartialAmpWindow()
{
    ensurePartialWindow();
    partialPanel.applyLanguage();
    partialPanel.resync();          // never open on a stale partial list
    partialWindow->setVisible (true);
    attachFloatingWindow (*partialWindow); // AFTER setVisible: that is what creates the peer
    partialWindow->toFront (true);
}

void FractalAdditiveAudioProcessorEditor::ensurePlaygroundWindow()
{
    if (playgroundWindow == nullptr)
    {
        playgroundWindow = std::make_unique<PlaygroundWindow> (
            tr ("Fibl\xc3\xb3 - preset playground", "Fibl\xc3\xb3 - laboratori de presets"));
        playgroundWindow->setLookAndFeel (lnf.get());
        playgroundWindow->setContentNonOwned (&playgroundPanel, false);
        // Square-ish by default: the pad is a rectangle whose two axes mean the same
        // thing, so a wide, short window would make the same movement read as a bigger
        // change horizontally than vertically.
        playgroundWindow->centreWithSize (520, 480);
        playgroundWindow->onHide = [this] { if (playgroundWindow != nullptr) playgroundWindow->setVisible (false); refocus(); };
        // Keep playing from the computer keyboard while the pad is in front - hearing
        // the morph move IS the feature. Through the editor's own handlers, so there is
        // one key map and one set of swallowed keys (see openPartialAmpWindow).
        playgroundWindow->onKey      = [this] (const juce::KeyPress& k) { return keyPressed (k); };
        playgroundWindow->onKeyState = [this] { return keyStateChanged (true); };
    }
}

void FractalAdditiveAudioProcessorEditor::openPlaygroundWindow()
{
    ensurePlaygroundWindow();
    playgroundPanel.applyLanguage();   // also refills the corner lists from the folder
    playgroundWindow->setVisible (true);
    attachFloatingWindow (*playgroundWindow); // AFTER setVisible: that is what creates the peer
    playgroundWindow->toFront (true);
}

FractalAdditiveAudioProcessorEditor::FloatingOwnershipProbe
FractalAdditiveAudioProcessorEditor::floatingOwnershipProbeForTests (bool playground)
{
    FloatingOwnershipProbe pr;
    if (getPeer() == nullptr)   // the mechanism is defined relative to the editor's
        return pr;              // native window; without one there is nothing to own
    if (playground) ensurePlaygroundWindow(); else ensurePartialWindow();
    auto& w = playground ? static_cast<juce::TopLevelWindow&> (*playgroundWindow)
                         : static_cast<juce::TopLevelWindow&> (*partialWindow);
    // addToDesktop() creates the peer while the component stays INVISIBLE, so the
    // verifier never flashes windows on screen - openXxx does the same via
    // setVisible(true), which is the only difference from the real click path.
    if (w.getPeer() == nullptr)
        w.addToDesktop();
    attachFloatingWindow (w);
    pr.windowOnDesktop = w.getPeer() != nullptr;
    if (auto* wp = w.getPeer())
        if (auto* ep = getPeer())
        {
            const auto owner = floatwin::ownerOf (wp->getNativeHandle());
            pr.ownerIsEditorRoot = owner != nullptr
                                && owner == floatwin::topLevelRootOf (ep->getNativeHandle());
        }
    return pr;
}

FractalAdditiveAudioProcessorEditor::PlaygroundProbe
FractalAdditiveAudioProcessorEditor::playgroundProbeForTests (int w, int h, bool useCatalan)
{
    const bool was = catalan;
    catalan = useCatalan;
    playgroundPanel.applyLanguage();
    playgroundPanel.setSize (w, h);
    playgroundPanel.refresh (true);
    // Run paint() for real. It draws four corner dots sized by weight, the blend-tinted
    // point and every corner's name - all of it off live processor state, so a null or a
    // stale index in there is a crash in a window nothing else in the build ever opens.
    {
        juce::Image img (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        juce::Graphics g (img);
        playgroundPanel.paintEntireComponent (g, true);
    }
    PlaygroundProbe p;
    p.panel   = playgroundPanel.getLocalBounds();
    p.pad     = playgroundPanel.pad.getBounds();
    p.padArea = playgroundPanel.pad.padArea().toNearestInt() + playgroundPanel.pad.getPosition();
    for (int i = 0; i < 4; ++i)
    {
        p.box[i]   = playgroundPanel.cornerBox[i].getBounds();
        p.clear[i] = playgroundPanel.cornerClear[i].getBounds();
        p.lbl[i]   = playgroundPanel.cornerLbl[i].getBounds();
        p.edit[i]       = playgroundPanel.cornerEdit[i].getBounds();
        p.editText[i]   = playgroundPanel.cornerEdit[i].getButtonText();
        p.editEnabled[i]= playgroundPanel.cornerEdit[i].isEnabled();
        p.cornerName[i] = processorRef.getPlaygroundCornerDisplayName (i);
    }
    for (int m = 0; m < 2; ++m)
    {
        p.mode[m]     = playgroundPanel.modeBtn[m].getBounds();
        p.modeText[m] = playgroundPanel.modeBtn[m].getButtonText();
    }
    p.voicesL    = playgroundPanel.voicesLbl.getBounds();
    p.voicesC    = playgroundPanel.voicesSlider.getBounds();
    p.voicesText = playgroundPanel.voicesLbl.getText();
    p.status = playgroundPanel.statLbl.getText();
    catalan = was;
    playgroundPanel.applyLanguage();
    return p;
}

void FractalAdditiveAudioProcessorEditor::playgroundDragForTests (float nx, float ny)
{
    auto& pad = playgroundPanel.pad;
    const auto a = pad.padArea();
    const juce::Point<float> pt { a.getX() + a.getWidth() * nx, a.getY() + a.getHeight() * ny };
    // Through mouseDown/mouseDrag/mouseUp, not writePoint: the begin/endChangeGesture
    // pair lives in the first and the last of those, and a host in touch or latch mode
    // records nothing at all without it.
    const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(),
                              pt, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                              &pad, &pad, juce::Time::getCurrentTime(), pt,
                              juce::Time::getCurrentTime(), 1, false);
    pad.mouseDown (e);
    pad.mouseDrag (e);
    pad.mouseUp (e);
}

// SLOPE is frozen while manual per-partial amplitudes are on (the processor freezes it
// at the value it had when manual came on, so switching is silent). A fader that quietly
// does nothing is the worst of both worlds: grey it out, take its mouse away, and let
// paint() write FROZEN next to the SLOPE label.
// THE ONE WRITER of the SLOPE (AMP DECAY) fader's colour, and why it has to be one.
//
// Two places used to set it and they disagreed. recolourSpectrumControls painted every
// control in SPECTRUM the selected engine's hue - correct while the fader is live -
// while refreshSlopeFreeze painted it grey when frozen and cSlope when not. cSlope is
// the MINT this panel stopped using when the two engines merged into one box, so ONE
// pencil (manual per-partial amplitudes) on/off cycle left the fader mint green in a
// blue panel and nothing ever put it back. The mirror-image bug was there too: with
// manual ON, any engine reselection repainted the frozen fader the panel hue and
// deleted the "this control is inert" grey, which refreshSlopeFreeze then refused to
// restore because its own before/after guard had not changed.
//
// So: grey while frozen, the PANEL's colour otherwise - decided here, from the live
// parameter, by both callers.
void FractalAdditiveAudioProcessorEditor::applySlopeFaderColour()
{
    const bool frozen = processorRef.apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
    const auto col = frozen ? juce::Colour (0xff5a6070) : spectrumPanelColour;
    slopeFader.setColour (juce::Slider::thumbColourId, col);
    slopeFader.setColour (juce::Slider::trackColourId, col.withAlpha (0.55f));
}

void FractalAdditiveAudioProcessorEditor::refreshSlopeFreeze()
{
    const bool frozen = processorRef.apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
    if ((frozen ? 1 : 0) == lastManualFrozenShown) return;
    lastManualFrozenShown = frozen ? 1 : 0;
    slopeFader.setEnabled (! frozen);
    applySlopeFaderColour();
    slopeFader.setTooltip (frozen
        // Names the CAUSE, because the commonest way to arrive here is not the pill: it
        // is dragging one knob in the pencil's window, which arms manual mode for you.
        // "Frozen" on its own left that user hunting for a switch they never touched.
        ? tr ("!Frozen!: *manual per-partial amplitudes* are on - switched on either by the pill below the pencil or by your first drag inside the pencil's window - so the amplitudes come from those knobs and this fader does nothing. It is kept exactly where it stood: switch manual off and it applies again.",
              "!Congelat!: les *amplituds manuals per parcial* estan actives - activades amb la pastilla de sota el llapis o pel teu primer arrossegament dins la finestra del llapis - i per aix\xc3\xb2 les amplituds venen d'aquells controls i aquest fader no fa res. Es conserva tal com estava: apaga el manual i torna a aplicar-se.")
        : tr ("Amplitude roll-off. 0 = flat/bright, 1 = saw, 2 = triangle.  (Also thins the highs when a Custom Engine scale is the spectrum.)",
              "Caiguda d'amplitud. 0 = pla/brillant, 1 = serra, 2 = triangle.  (Tamb\xc3\xa9 aprima els aguts quan l'espectre ve d'una escala del Motor Personalitzat.)"));
    repaint (bothEnginesBounds); // SLOPE (and its FROZEN tag) live in that box now
    if (partialWindow != nullptr && partialWindow->isVisible())
        partialPanel.refreshText();
}

void FractalAdditiveAudioProcessorEditor::openScalaChooser()
{
    // .tun is offered first because it is what the Save button writes by default:
    // a tuning exported from here has to be reloadable from here.
    fileChooser = std::make_unique<juce::FileChooser> ("Load a tuning (.tun / .scl / .ascl)",
                                                       juce::File(), "*.tun;*.scl;*.ascl");
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
    fileChooser = std::make_unique<juce::FileChooser> ("Load a scale into the tuning bank (.tun / .scl / .ascl)",
                                                       juce::File(), "*.tun;*.scl;*.ascl");
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
    fileChooser = std::make_unique<juce::FileChooser> ("Load a scale file as timbre (.tun / .scl / .ascl)",
                                                       juce::File(), "*.tun;*.scl;*.ascl");
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

//==============================================================================
// One colour per interval in the Custom Engine's text box, so "3/2 2 700c" reads as
// three values instead of one run of characters. Presentation only: the text, and
// therefore what loadTimbreText() parses, is byte-for-byte what the user typed - the
// token boundaries come from IntervalTokens.h, which copies the parser's own
// separator set.
//
// HOW, given juce::TextEditor has no "colour this range" call: setText() and
// insertTextAtCaret() bake the CURRENT textColourId into the run they insert, so the
// text is rebuilt run by run with the colour switched between them. The caret is put
// back where it was, or typing would jump to the end on every keystroke.
//
// The cost, stated plainly: rebuilding the text drops the BOX's own undo history, so
// Ctrl+Z inside it no longer walks back through what was typed. Enter applies the list
// and Escape leaves it, and a parameter undo (the processor's) is untouched.
void FractalAdditiveAudioProcessorEditor::recolourIntervalTokens()
{
    const auto text = intervalEdit.getText();
    if (text == lastColouredIntervals) return; // already painted from exactly this text
    lastColouredIntervals = text;
    if (text.isEmpty()) return;
    // A line break would come back out as a SPACE: insertTextAtCaret flattens "\r\n" in a
    // single-line editor, and rewriting what the user typed is the one thing this is not
    // allowed to do. Nothing typed or pasted into this box can contain one (JUCE flattens
    // those on the way in too), so this only ever fires on a programmatic value - and it
    // drops the colour, never a character.
    if (text.containsAnyOf ("\r\n")) return;

    // Bright and far apart on the 0xff20202b background, and spread around the hue
    // wheel so NEIGHBOURING tokens are never a near-miss of each other: consecutive
    // entries are at least 60 degrees apart, including the wrap from the last back to
    // the first (which is what a 6-token list does).
    static const juce::Colour palette[] = { juce::Colour (0xff5cd6ff),   // cyan-blue 195
                                            juce::Colour (0xffffc857),   // amber      40
                                            juce::Colour (0xff8cf06a),   // green     105
                                            juce::Colour (0xffff8fd2),   // pink      325
                                            juce::Colour (0xffb79bff) }; // violet    258
    constexpr int paletteSize = (int) (sizeof (palette) / sizeof (palette[0]));

    // Character indices, not bytes: TextEditor ranges count characters, and a typed
    // non-ASCII character (a stray accent from another keyboard layout) would put every
    // range after it out by one if this walked the UTF-8 bytes.
    std::vector<int> cps;
    cps.reserve ((size_t) text.length());
    for (auto p = text.getCharPointer(); ! p.isEmpty(); ++p)
        cps.push_back ((int) *p);

    const auto spans = intervaltokens::spans (cps, paletteSize);
    if (spans.empty()) return;

    const int caret = intervalEdit.getCaretPosition();
    const juce::Colour gapCol (0xff8f96a8); // the whitespace between values: no colour to carry

    intervalEdit.clear();
    int pos = 0;
    auto emit = [this, &text, &pos] (int end, juce::Colour col)
    {
        if (end <= pos) return;
        intervalEdit.setColour (juce::TextEditor::textColourId, col);
        intervalEdit.insertTextAtCaret (text.substring (pos, end));
        pos = end;
    };
    for (const auto& s : spans)
    {
        emit (s.start, gapCol);                                  // separators
        emit (s.start + s.length, palette[s.colour]);            // the value itself
    }
    emit ((int) cps.size(), gapCol);                             // trailing separators
    intervalEdit.setCaretPosition (caret);
}

// setText() alone would paint the whole thing in whatever colour was current, so the
// two always travel together - a scale loaded from a preset, an undo or the timer must
// arrive coloured exactly like one that was typed.
void FractalAdditiveAudioProcessorEditor::setIntervalText (const juce::String& text)
{
    intervalEdit.setText (text, false); // false: our own recolour follows, not a re-entry
    // setText paints the lot in ONE colour, so the runs must be redone even when the
    // string is the one we painted last (reloading the same spectrum, an undo back to
    // it): clearing the record is what stops the guard above from skipping that.
    lastColouredIntervals.clear();
    recolourIntervalTokens();
}

// The custom view's two buttons, dressed for what they do RIGHT NOW. Word, colour and
// tooltip are set together here and nowhere else: a button reading "Load" in green while
// its click deletes the loaded spectrum is the one mistake this panel must not make, and
// that is exactly what happens when the word is set in one place and the action in another.
void FractalAdditiveAudioProcessorEditor::refreshCustomEngineButtons()
{
    const bool loaded = processorRef.timbreLoaded();
    loadTimbreButton.setButtonText (loaded ? tr ("Remove", "Treu") : tr ("Load", "Obrir"));
    // The tuning fork is the "this loads a scale file" glyph; a Remove does not load one.
    loadTimbreButton.getProperties().set ("fork", ! loaded);
    loadTimbreButton.setColour (juce::TextButton::buttonColourId, loaded ? cReset : cLoad);
    loadTimbreButton.setTooltip (loaded
        ? tr ("Delete the loaded custom spectrum and hand the sound back to the harmonic engine. One Ctrl+Z brings it back. To keep the spectrum but stop it sounding, click the HARMONIC chip instead - that switches engines without erasing anything.",
              "Esborra l'espectre propi carregat i torna el so al motor harm\xc3\xb2nic. Un Ctrl+Z el recupera. Si el vols conservar per\xc3\xb2 que no soni, clica el xip HARM\xc3\x92NIC: aix\xc3\xb2 canvia de motor sense esborrar res.")
        : tr ("Load a .tun/.scl/.ascl whose intervals become the PARTIALS of the sound - the CUSTOM chip lights and this engine replaces the harmonic one. Decay, Fractal, Range and Reps still shape it.",
              "Carrega un .tun/.scl/.ascl els intervals del qual passen a ser els PARCIALS del so - s'enc\xc3\xa9n el xip PROPI i aquest motor substitueix l'harm\xc3\xb2nic. Caiguda, Fractal, Abast i Reps encara hi actuen."));
    randomSpectrumBtn.setButtonText (tr ("Random", "Atzar"));
    randomSpectrumBtn.setTooltip (tr (
        "Build a brand-new random spectrum. Every press is a different one: 5 to 12 partials per period, spread evenly in PITCH (not in Hz) over the reach your Octave Range and Reps already set, never closer than 35 cents so no two of them collapse into one beating partial. The period stays a whole number of octaves, so Reps stacks it cleanly instead of smearing. Automatable: it is a trigger parameter, so a 0-to-1 step drawn in the host fires it exactly like this click, with the window shut. One Ctrl+Z brings the previous spectrum back.",
        "Construeix un espectre nou a l'atzar. Cada pulsaci\xc3\xb3 \xc3\xa9s diferent: de 5 a 12 parcials per per\xc3\xado" "de, repartits uniformement en TO (no en Hz) dins l'abast que ja fixen l'Abast d'octaves i les Reps, i mai a menys de 35 cents perqu\xc3\xa8 cap parell no es fongui en un sol parcial batent. El per\xc3\xado" "de es queda en un nombre enter d'octaves, aix\xc3\xad que les Reps l'apilen net en comptes d'esborronar-lo. Automatitzable: \xc3\xa9s un par\xc3\xa0metre disparador, aix\xc3\xad que un escal\xc3\xb3 de 0 a 1 dibuixat a l'amfitri\xc3\xb3 el dispara igual que aquest clic, amb la finestra tancada. Un Ctrl+Z recupera l'espectre anterior."));
}

// The "Load spectrum's tuning" button: the current custom spectrum was DERIVED from a
// tuning (adapted to it, or loaded from a scale file), and this button loads that
// tuning. Word, enabled state and tooltip in one function - when the provenance tuning
// is already ACTIVE the button must say so and refuse, and a "Load" word over a click
// that refuses (or a greyed button whose word still promises a load it never names)
// is exactly the disagreement single-writer functions exist to prevent.
void FractalAdditiveAudioProcessorEditor::refreshSpectrumTuningButton()
{
    const bool has    = processorRef.hasSpectrumProvenance();
    const bool active = has && processorRef.spectrumProvenanceIsActive();
    const auto name   = processorRef.getSpectrumProvenanceName();
    // Cheap per-tick guard: language + the three inputs above are the whole output.
    const juce::String sig = (catalan ? "c|" : "e|") + juce::String (has ? 1 : 0)
                           + (active ? "1" : "0") + "|" + name;
    if (sig == lastSpectrumTuningSig) return;
    lastSpectrumTuningSig = sig;
    if (! has) return; // invisible (resized() gates on hasSpectrumProvenance): nothing to word
    loadSpectrumTuningBtn.setEnabled (! active);
    loadSpectrumTuningBtn.setButtonText (active
        ? tr ("Spectrum's tuning (active)", "Afinaci\xc3\xb3 de l'espectre (activa)")
        : tr ("Load spectrum's tuning", "Carrega l'afinaci\xc3\xb3 de l'espectre"));
    const auto shown = name.isNotEmpty() ? name : tr ("(unnamed)", "(sense nom)");
    loadSpectrumTuningBtn.setTooltip (active
        ? tr ("This custom spectrum was derived from the tuning !", "Aquest espectre propi es va derivar de l'afinaci\xc3\xb3 !")
          + shown + tr ("!, which is already the active tuning.", "!, que ja \xc3\xa9s l'afinaci\xc3\xb3 activa.")
        : tr ("This custom spectrum was derived from the tuning !", "Aquest espectre propi es va derivar de l'afinaci\xc3\xb3 !")
          + shown + tr ("! (adapted to it, or loaded from its scale file). Click to make that tuning the ACTIVE one - the spectrum itself stays as it is, and Ctrl+Z brings the current tuning back.",
                        "! (adaptat a ella, o carregat del seu fitxer d'escala). Clica per fer aquella afinaci\xc3\xb3 l'ACTIVA - l'espectre es queda com est\xc3\xa0, i Ctrl+Z recupera l'afinaci\xc3\xb3 actual."));
}

// The Shepard beats box's HONESTY dress. Sync mode with no host BPM means the free
// ms value is what actually runs; a beats box sitting there in its normal colour
// would be claiming a sync that is not happening. Colour and tooltip are decided
// together HERE - two writers is how a box ends up amber with a tooltip that still
// promises beats. Cheap per-tick: one atomic read and an int compare.
void FractalAdditiveAudioProcessorEditor::refreshShepardSyncState()
{
    const bool noBpm = shepModeBox.getSelectedId() == 2 && processorRef.shepardSyncHasNoBpm();
    if ((int) noBpm == lastShepNoBpm) return;
    lastShepNoBpm = (int) noBpm;
    const juce::Colour amber (0xffffb020); // cOverride: the "this is being overridden" colour
    shepDivBox.setColour (juce::ComboBox::outlineColourId, noBpm ? amber : cTuning.withAlpha (0.7f));
    shepDivBox.setColour (juce::ComboBox::arrowColourId,   noBpm ? amber : cTuning);
    shepDivBox.setTooltip (tr (
        "Rise of one period as host beats: 1/4 = a quarter of a beat, 4 = four beats; 1/3 and 2/3 are the triplet-friendly ones.",
        "La pujada d'un per\xc3\xado" "de en beats del DAW: 1/4 = un quart de beat, 4 = quatre beats; 1/3 i 2/3 s\xc3\xb3n els de treset.")
        + (noBpm ? tr ("\n!NO BPM from the host right now!, so the FREE time (ms) is what is running - the beats take over the moment the host offers a tempo.",
                       "\n!ARA MATEIX el DAW no d\xc3\xb3na cap BPM!, o sigui que mana el temps LLIURE (ms) - els beats prendran el control quan el DAW ofereixi un tempo.")
                 : juce::String()));
    shepDivBox.repaint();
}

// ONE PANEL, ONE COLOUR. Every control inside SPECTRUM takes the selected engine's hue,
// so nothing on screen is left claiming the old nested boxes still exist: the panel used
// to be three framed boxes in three different colours (blue harmonic / pink custom /
// mint "both engines" with a magenta Fractal), and after the frames became plain divider
// lines those leftover hues were the only thing still drawing the boundaries.
//
// Deliberately NOT recoloured: the +/- steppers (green = up, red = down, the same pair
// as Essentials' OCTAVE) and the Load/Remove button (green loads, red deletes). Those
// colours mean an ACTION, not a compartment, and washing them into the panel hue would
// erase the one warning that says which of the two the button is about to do.
void FractalAdditiveAudioProcessorEditor::recolourSpectrumControls (juce::Colour col)
{
    auto fader = [&col] (juce::Slider& s)
    {
        s.setColour (juce::Slider::thumbColourId, col);
        s.setColour (juce::Slider::trackColourId, col.withAlpha (0.55f));
    };
    auto combo = [&col] (juce::ComboBox& b)
    {
        b.setColour (juce::ComboBox::outlineColourId, col.withAlpha (0.7f));
        b.setColour (juce::ComboBox::arrowColourId, col);
    };
    auto label = [&col] (juce::Label& l) { l.setColour (juce::Label::textColourId, col.brighter (0.15f)); };
    auto tick  = [&col] (juce::ToggleButton& t)
    {
        t.setColour (juce::ToggleButton::tickColourId, col);
        t.setColour (juce::ToggleButton::tickDisabledColourId, col.withAlpha (0.4f));
    };

    // Spelled as juce::Slider* because these are four different Slider SUBCLASSES
    // (stepped, magnetic, plain) and a deduced initializer-list has no common type.
    for (juce::Slider* s : { (juce::Slider*) &baseNFader, (juce::Slider*) &harmonicsFader,
                             (juce::Slider*) &primeLimitFader, (juce::Slider*) &timbreRepeatsFader,
                             (juce::Slider*) &fractalFader, (juce::Slider*) &morphTimeFader })
        fader (*s);
    // slopeFader is deliberately NOT in that list: it has a SECOND state (frozen by manual
    // per-partial amplitudes) and exactly one function is allowed to write its colour.
    // See applySlopeFaderColour - two writers is what turned it green.
    applySlopeFaderColour();
    for (juce::ComboBox* b : { &basePitchBox, &primeModeBox }) combo (*b);
    for (auto* l : { &lblBaseN, &lblBasePitch, &lblPrimeLimit, &lblTimbreRepeats, &lblMorphTime })
        label (*l);
    for (int i = 0; i < 9; ++i) { tick (primeToggle[i]); label (primeLbl[i]); }
    // TEMPER's tick used to be forced to the harmonic blue "because that is the engine it
    // hands the spectrum to" - which was the whole point when the two engines were two
    // boxes, and is now just one control refusing to join the panel it lives in.
    tick (temperToggle);
    partialEditBtn.col = col;
    randomSpectrumBtn.setColour (juce::TextButton::buttonColourId, col);
}

// SELECTING A VIEW IS SELECTING AN ENGINE.
//
// The view on screen and the engine making the sound are ONE choice, so there is one
// function that makes it - called by both chips and by every other route that changes
// the view. It writes the PARAMETERS engineInForce() reads, never a member, so the
// choice lands in the undo history and in host automation exactly as a fader would.
//
// custom = true  : temper off, the Custom Engine on. With a scale loaded, that engine
//                  is now the one generating and the custom view is the real view.
//                  With NOTHING loaded there is no spectrum to sound, so the sound
//                  honestly stays harmonic and the view is held open by the pin - that
//                  view is the only place Load, Random and the intervals editor live,
//                  and refusing the click (as the disabled chip once did) made creating
//                  a custom spectrum flatly impossible.
// custom = false : temper off, the Custom Engine off. setTimbreEnabled only flips the
//                  parameter, so a loaded scale is KEPT - this is the way BACK to the
//                  harmonic engine, and it must never be the thing that loses a scale.
//                  (That non-destructive off-switch is what the deleted On/Off pill did;
//                  the Remove button is the destructive one, and says so.)
void FractalAdditiveAudioProcessorEditor::selectEngineView (bool custom)
{
    if (auto* p = processorRef.apvts.getParameter ("timbreTemper"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (0.0f);
        p->endChangeGesture();
    }
    customViewPinned = custom;
    processorRef.setTimbreEnabled (custom);
    applyEngineState();
}

// Dress the SPECTRUM panel for whichever engine is in force: light the right chip and
// recolour the whole panel with it. There is no dimming any more: the engines share ONE
// box and only the SELECTED engine's controls are inside it (resized() gates their
// visibility), so a control that cannot affect the sound is simply not on screen -
// which also retires the OVERRIDDEN / BYPASSED badges and the "controls are inert"
// banner the old two-box layout needed to explain itself.
void FractalAdditiveAudioProcessorEditor::applyEngineState()
{
    using E = FractalAdditiveAudioProcessor::EngineInForce;
    const auto st = processorRef.engineInForce();
    const bool loaded = processorRef.timbreLoaded();
    lastEngineShown = (int) st * 2 + (loaded ? 1 : 0);

    // The pin only ever holds the custom view open as an EMPTY workspace: any real
    // non-harmonic engine state observed here (a scale switched on, temper engaged -
    // from any source: clicks, automation, an undo, a preset) supersedes it, so it can
    // never contradict what the processor says is sounding.
    if (st != E::Harmonic)
        customViewPinned = false;

    // The engine chips: exactly one lit, in its own colour, with black text (the
    // modulator-tab treatment). The lit chip = the view inside the shared box; that is
    // engineInForce()'s view except for the empty-workspace pin above.
    // TEMPERED lights HARMONIC, because in temper mode the harmonic engine is the one
    // generating every partial - the tuning only says where they may land.
    const int sel = customViewShown() ? 1 : 0;
    const juce::Colour chipCol[2] = { cHarmonic, cTimbre };
    for (int i = 0; i < 2; ++i)
    {
        const bool on = i == sel;
        engineChip[i].setColour (juce::TextButton::buttonColourId,
                                 on ? chipCol[i] : chipCol[i].withAlpha (0.20f));
        engineChip[i].setColour (juce::TextButton::textColourOffId,
                                 on ? juce::Colours::black : juce::Colour (0xffb9c0d0));
    }
    // ...and the WHOLE panel wears that colour (frame + title chip + interior tint), the
    // way the Modulators box wears its selected tab's. The old neutral steel said nothing
    // and read as "white" beside the coloured panels around it.
    spectrumPanelColour = chipCol[sel];
    // Every control inside it too, not just the chrome. The panel is one box with
    // divider lines now, so a fader still wearing a nested box's old hue would be the
    // only thing left on screen drawing the boundaries the frames used to draw.
    recolourSpectrumControls (spectrumPanelColour);
    // And the Load/Remove button, whose word depends on whether a spectrum exists -
    // which is precisely the thing that changes under this call.
    refreshCustomEngineButtons();

    // Chip selection = VIEW switching: relayout so the selected engine's controls are
    // the ones inside the shared box. Guarded because the ctor calls this before
    // setSize(), when there is no geometry to lay out yet. Without the relayout here,
    // an engine change arriving from OUTSIDE the chips (host automation, an undo, a
    // preset, Spectrum-vs-Tuning writing an adapted spectrum - the timer catches all
    // of these) would light the right chip but keep showing the old engine's controls.
    if (getWidth() > 0)
        resized();
    repaint();
}

// The ONE list of inline sub-labels the editor paints. paint() draws it; layoutProbe()
// measures it. Kept in one place because a painted label has no component to squash and
// so fails only by CLIPPING - which is invisible to every other check in the build, and
// is how "FROZEN - DRAWN BY HAND" shipped clipped at "DRA...".
std::vector<FractalAdditiveAudioProcessorEditor::PaintedSub>
FractalAdditiveAudioProcessorEditor::paintedSubLabels() const
{
    std::vector<PaintedSub> v;
    // The band every sub-label is drawn into: the top 13 px of its cell, 5 px in.
    auto band = [] (juce::Rectangle<int> cell) { return cell.withHeight (13).reduced (5, 0); };
    auto add  = [&v] (const char* nm, juce::String txt, juce::Rectangle<int> cell,
                      juce::Colour col, float h = 11.5f, bool fitted = false)
    { v.push_back ({ nm, std::move (txt), cell, col, h, fitted }); };

    if (! spectrumBounds.isEmpty())
    {
        // ONE PANEL, ONE COLOUR: every sub-label in SPECTRUM is the SELECTED ENGINE's
        // hue, including the Amp Decay / Fractal / Morph / Octave Range compartment,
        // which used to keep a mint-and-magenta scheme of its own. Those hues were the
        // last thing on screen still drawing the nested boxes that are now dividers.
        const auto cPanel = spectrumPanelColour;
        // Harmonic view only: the custom view's controls name themselves and resized()
        // leaves the hc* cells empty.
        if (! engineBoxBounds.isEmpty() && ! customViewShown() && ! hcBase.isEmpty())
        {
            add ("eng.base",  tr ("BASE", "BASE"),                     band (hcBase),  cPanel);
            add ("eng.harm",  tr ("HARMONICS", "HARM\xc3\x92NICS"),    band (hcHarm),  cPanel);
            // PRIME sits right under the shared divider -> nudge it down for air.
            add ("eng.prime", tr ("PRIME", "PRIMERS"), band (hcPrime.translated (0, 5)), cPanel);
        }
        if (! bothEnginesBounds.isEmpty())
        {
            // AMP DECAY says so when it is inert: with manual per-partial amplitudes on
            // the processor freezes the compile slope where it stood, so the fader stops
            // affecting the sound entirely. ("DECAY", not "SLOPE": it is how fast the
            // partials' amplitudes fall off, and "slope" read as maths, not sound.)
            const bool man = processorRef.apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
            const auto decay = tr ("AMP DECAY", "CAIGUDA AMP");
            const auto slopeBand = band (beSlope);
            // The greyed AMP DECAY and the amber FROZEN are the two exceptions to "one
            // colour": they are not compartment labels, they are the pair that says this
            // control cannot affect the sound right now. Painting them the panel hue
            // would unify the panel by deleting its only warning.
            add ("both.decay", decay, slopeBand, man ? juce::Colour (0xff8f96a8) : cPanel);
            if (man)
            {
                // The FROZEN tag starts after the MEASURED width of the label beside it,
                // not at a hard-coded 72 px: at 72 the Catalan "CAIGUDA AMP" (wider than
                // the English "AMP DECAY") ran straight under "CONGELAT" and the two
                // words overprinted each other. Measured, the gap is a gap in both.
                const juce::Font f (juce::FontOptions (11.5f).withStyle ("Bold"));
                const int used = juce::GlyphArrangement::getStringWidthInt (f, decay) + 8;
                add ("both.frozen", tr ("FROZEN", "CONGELAT"),
                     slopeBand.withTrimmedLeft (juce::jmin (used, juce::jmax (0, slopeBand.getWidth() - 46))),
                     cOverride, 10.0f, true);
            }
            add ("both.fractal", tr ("FRACTAL", "FRACTAL"),                   band (beFractal), cPanel);
            add ("both.morph",   tr ("MORPH", "MORPHING"),                    band (beMorph),   cPanel);
            add ("both.range",   tr ("OCTAVE RANGE", "ABAST OCTAVES"), band (beRange.translated (0, 3)), cPanel);
        }
    }
    if (! essentialsBounds.isEmpty())
    {
        // Essentials' labels sit LEFT of their control (vertically centred), in the same
        // band resized() reserves for them (6 px in, 78 wide, minus a 2 px gap).
        auto left = [] (juce::Rectangle<int> c) { return c.reduced (6, 0).withWidth (76); };
        add ("ess.phase", tr ("PHASE", "FASE"),                    left (ecPhase), cEssentials);
        add ("ess.gain",  tr ("GAIN", "GUANY"),                    left (ecGain),  cEssentials);
        add ("ess.ref",   tr ("REFERENCE", "REFER\xc3\x88NCIA"),   left (ecRef),   cEssentials);
        // OCTAVE reads like the three rows above it now: label LEFT of the stepper,
        // not above it - it was the one cell in the box laid out the other way round.
        add ("ess.oct",   tr ("OCTAVE", "OCTAVA"),                 left (ecOct),   cEssentials);
    }
    return v;
}

FractalAdditiveAudioProcessorEditor::LayoutProbe
FractalAdditiveAudioProcessorEditor::layoutProbeInLanguage (bool useCatalan)
{
    const bool was = catalan;
    catalan = useCatalan;
    // applyLanguage(), not just the flag: the PAINTED labels come from tr() at probe
    // time, but every Label COMPONENT's text was written once by applyLanguage - probing
    // without re-running it would measure Catalan geometry against English strings and
    // silently pass. resized() after it, because some rows only exist in one language's
    // width (the tuning buttons are laid out from their own text width).
    applyLanguage();
    if (getWidth() > 0) resized();
    auto p = layoutProbe();
    catalan = was;
    applyLanguage();
    if (getWidth() > 0) resized();
    return p;
}

// See the header. SPECTRUM and TUNING own the shared wall on their right, so that end is
// SQUARE; every other corner keeps the 9 px radius. One function, because paint() draws it
// and layoutProbe() measures it and a second copy of the rule is a check that cannot fail.
std::vector<std::pair<juce::String, juce::Line<float>>>
FractalAdditiveAudioProcessorEditor::spectrumDividers() const
{
    std::vector<std::pair<juce::String, juce::Line<float>>> out;
    if (spectrumBounds.isEmpty()) return out;

    // Lines are drawn INSIDE the outer frame: its top line sits at box.y + 16 with a
    // 2.6 px stroke, so anything starting above ~17.5 pokes through the panel's edge.
    const float top = (float) spectrumBounds.getY() + 17.5f;
    const float bot = (float) spectrumBounds.getBottom() - 4.0f;

    // EVERY VERTICAL DIVIDER IN THIS PANEL RUNS THE WHOLE HEIGHT, top to bot. That is
    // the rule, not a coincidence, and it is why this helper is the only place allowed
    // to build one: the BASE | HARMONICS wall used to stop at hcPrime's bottom instead,
    // 23 px above the floor, because the full-width TEMPER row sits under both columns.
    // A ToggleButton paints nothing but a tick and its text, so the rest of that row is
    // bare panel and the wall simply ended in mid-air - "la linea de la pared izquierda
    // de la subcaja de prime esta incompleta, tiene un hueco abajo". TEMPER's text ends
    // far to the left of that wall, so the line passes through empty button.
    auto vert = [&out, top, bot] (const char* name, float x)
    { out.push_back ({ name, juce::Line<float> (x, top, x, bot) }); };
    auto horiz = [&out] (const char* name, const juce::Rectangle<int>& r)
    { out.push_back ({ name, juce::Line<float> ((float) r.getX(),     (float) r.getY(),
                                                (float) r.getRight(), (float) r.getY()) }); };

    // THE WALL between the engine compartment and the shared-controls compartment. It
    // replaces two frame edges and the gutter between them, so the two compartments
    // share one line instead of facing each other across three.
    if (! engineBoxBounds.isEmpty() && ! bothEnginesBounds.isEmpty())
        vert ("engine|both", 0.5f * (float) (engineBoxBounds.getRight() + bothEnginesBounds.getX()));

    // Harmonic view: the dividers of its three cells. (In the custom view resized()
    // leaves hc* empty, which reads here as "draw none".)
    //
    // The CUSTOM view draws no divider at all any more. It had one horizontal line
    // between the Load/Remove row and the typed-ratios box below - "hay una linea fina
    // horizontal en custom spectrum entre el boton remove y la caja de ratios de abajo,
    // es inecesaria y fea, quitala". The two rows it separated are already separated by
    // the ratios box's own border.
    if (! customViewShown() && ! hcBase.isEmpty())
    {
        vert  ("base|harmonics", (float) hcHarm.getX());
        horiz ("harmonics|prime", hcPrime);
    }

    // The shared-controls compartment: Amp Decay | Fractal | Morph over Range.
    if (! bothEnginesBounds.isEmpty())
    {
        vert  ("slope|fractal", (float) beFractal.getX());
        vert  ("fractal|morph", (float) beMorph.getX());
        horiz ("morph|range",   beRange);
    }
    return out;
}

juce::Path FractalAdditiveAudioProcessorEditor::macroFramePath (const juce::Rectangle<int>& box) const
{
    const bool sharedRight = box == spectrumBounds || box == lFootBounds;
    return topRoundedFrame (macroFrameRect (box), kMacroRadius, ! sharedRight);
}

FractalAdditiveAudioProcessorEditor::LayoutProbe
FractalAdditiveAudioProcessorEditor::layoutProbe() const
{
    LayoutProbe p;
    p.spectrum       = spectrumBounds;
    p.engineBox      = engineBoxBounds;
    p.bothEngines    = bothEnginesBounds;
    p.essentials     = essentialsBounds;
    p.envelopes      = gAmpEnv.getBounds();
    p.tuning         = lFootBounds;
    p.engineChips    = engineChip[0].getBounds().getUnion (engineChip[1].getBounds());
    p.specColumn     = gSpecScale.getBounds();
    // The 78 px title strip resized() carves off the top - the same number, so the probe
    // cannot end up measuring against a height paint() has stopped using.
    p.titleBar       = juce::Rectangle<int> (0, 0, getWidth(), 78);
    // Every macro panel's top-right corner, sampled off the path that is really stroked
    // there (see LayoutProbe::panelCorners). The two panels whose right edge IS the shared
    // wall must come back square; every corner facing open space must come back rounded.
    {
        auto corner = [&p, this] (const char* name, juce::Rectangle<int> box)
        {
            if (box.isEmpty()) return;
            // macroFramePath, NOT a rebuild of it: the probe measuring its own copy of the
            // rounding rule is exactly how this check first passed against a paint() that
            // had been sabotaged into rounding the shared corner again.
            p.panelCorners.push_back ({ name, topRightCornerInset (macroFramePath (box),
                                                                   macroFrameRect (box)) });
        };
        corner ("spectrum",   spectrumBounds);
        corner ("tuning",     lFootBounds);
        corner ("essentials", essentialsBounds);
        corner ("envelopes",  gAmpEnv.getBounds());
        corner ("column",     gSpecScale.getBounds());
    }
    p.panelColour    = spectrumPanelColour;
    p.slopeColour    = slopeFader.findColour (juce::Slider::thumbColourId);
    p.slopeFrozen    = processorRef.apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
    // Invisible controls are left out on purpose: several are mode-dependent (the prime
    // whitelist vs the limit fader, the Off button, ...) and a hidden one has no geometry
    // worth asserting.
    auto add = [&p] (const char* name, const juce::Component& c)
    {
        if (! c.isVisible()) return;
        p.controls.push_back ({ name, c.getBounds() });
        // A Label carries TEXT that has to fit the cell it was given. Recorded here
        // rather than in a separate list so it can never name a control the layout no
        // longer produces.
        if (const auto* l = dynamic_cast<const juce::Label*> (&c); l != nullptr && l->getText().isNotEmpty())
            p.componentLabels.push_back ({ name, l->getText(), l->getBounds(),
                (float) juce::GlyphArrangement::getStringWidthInt (l->getFont(), l->getText()) });
    };
    // A read-out that is MEANT to wrap. Its cell is the text area drawLabel really uses
    // (the Label's own border removed - JUCE's default is 5 px each side, which is 10 px
    // of a 192 px column and enough on its own to push the last word off the bottom), and
    // what it needs is the height the same text takes when word-wrapped at that width in
    // that font. Registered in `controls` too, so the bounds and overlap rules still see it.
    auto measureWrapped = [] (const char* name, const juce::Label& l, const juce::String& text)
    {
        const auto area = l.getBorderSize().subtractedFrom (l.getLocalBounds());
        juce::AttributedString as;
        as.setText (text);
        as.setFont (l.getFont());
        as.setJustification (l.getJustificationType());
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, (float) area.getWidth());
        return LayoutProbe::WrappedLabel { name, text, area + l.getBounds().getPosition(),
                                           tl.getWidth(), tl.getHeight() };
    };
    auto addWrapped = [&p, &measureWrapped] (const char* name, const juce::Label& l)
    {
        if (! l.isVisible() || l.getText().isEmpty()) return;
        p.controls.push_back ({ name, l.getBounds() });
        p.wrappedLabels.push_back (measureWrapped (name, l, l.getText()));
    };

    add ("harm.baseN",       baseNFader);      add ("harm.baseNLbl",  lblBaseN);
    add ("harm.basePitch",   basePitchBox);    add ("harm.pitchLbl",  lblBasePitch);
    add ("harm.harmonics",   harmonicsFader);
    add ("harm.primeMode",   primeModeBox);    add ("harm.primeLimit", primeLimitFader);
    add ("harm.primeLimitLbl", lblPrimeLimit);
    add ("harm.prime0",      primeToggle[0]);  add ("harm.prime8",    primeToggle[8]);
    add ("harm.prime0Lbl",   primeLbl[0]);     add ("harm.prime8Lbl", primeLbl[8]);

    add ("cust.load",        loadTimbreButton); add ("cust.random",   randomSpectrumBtn);
    add ("cust.intervals",   intervalEdit);     add ("cust.status",   timbreStatusLabel);
    add ("cust.reps",        timbreRepeatsFader); add ("cust.repsLbl", lblTimbreRepeats);
    add ("cust.temper",      temperToggle); // laid out in BOTH views of the engine box

    add ("both.slope",       slopeFader);      add ("both.pencil",    partialEditBtn);
    add ("both.manual",      partialManualT);  add ("both.fractal",   fractalFader);
    add ("both.morph",       morphToggle);
    add ("both.morphTime",   morphTimeFader);  add ("both.morphLbl",  lblMorphTime);
    add ("both.rangeVal",    sprVal);          add ("both.rangeUp",   sprUp);
    add ("both.rangeDn",     sprDn);

    add ("ess.phase",        phaseBox);        add ("ess.gain",       gainFader);
    add ("ess.refA",         refAEdit);        add ("ess.refALbl",    lblRefA);
    add ("ess.refHz",        lblRefHz);
    add ("ess.octVal",       octVal);          add ("ess.octUp",      octUp);
    add ("ess.octDn",        octDn);

    // The title-bar cluster. Probed because the playground button's WORD is the whole
    // feature and Catalan is eight characters longer than English there: a TextButton
    // does not condense, it CLIPS, so the only way a build can see it is by measuring
    // the string against the cell in both languages.
    add ("top.preset",       presetBox);       add ("top.save",       savePresetButton);
    add ("top.playground",   playgroundButton);

    // The TUNING box and the Spectrum-vs-Tuning column. They were never probed, and
    // they are the two densest stacks in the window: TUNING piles up to six optional
    // rows inside a fixed height (a .scl with its own patch, a preset's tuning, bend,
    // Recent, the bank) and the right column re-lays itself for every mode. Both are
    // where a caption sized in English quietly condenses in Catalan.
    add ("tun.mode",         tuningBox);       add ("tun.load",       loadSclButton);
    add ("tun.save",         saveTuningBtn);   add ("tun.toBank",     tuningBankAddBtn);
    add ("tun.name",         tuningNameLabel); add ("tun.period",     tuningPeriodLbl);
    add ("tun.tablePeriod",  tablePeriodEdit); add ("tun.tableReset", tablePeriodResetBtn);
    add ("tun.itsSpectrum",  loadItsSpectrumBtn);
    add ("tun.presetTuning", loadPresetTuningBtn);
    add ("tun.specTuning",   loadSpectrumTuningBtn);
    add ("tun.bendLbl",      lblBend);         add ("tun.bend",       bendBox);
    add ("tun.recent",       recentOpenBtn);   add ("tun.bankLbl",    lblTuningBank);
    add ("tun.bank",         tuningBankBox);   add ("tun.bankLoad",   tuningBankLoadBtn);
    add ("tun.bankClear",    tuningBankClearBtn);
    // The Shepard row. Exactly ONE of shepTime/shepDiv is visible (the mode's own
    // dress), and the rise-interval box only exists under a non-octave tuning, so
    // `add`'s visibility gate records exactly what is really on screen.
    add ("tun.shepOn",       shepOnT);         add ("tun.shepMode",   shepModeBox);
    add ("tun.shepTime",     shepTimeFader);   add ("tun.shepDiv",    shepDivBox);
    add ("tun.shepRise",     shepRiseBox);

    add ("spec.mode",        specModeBox);     add ("spec.source",    tuneSourceBox);
    add ("spec.scope",       tuneScopeBox);    add ("spec.notesLbl",  chordNotesLbl);
    add ("spec.notes",       chordNotesBox);   add ("spec.minMax",    chordMinMaxBox);
    add ("spec.perLbl",      chordPerLbl);     add ("spec.period",    chordPeriodEdit);
    add ("spec.minLbl",      chordMinLbl);     add ("spec.min",       chordMinEdit);
    add ("spec.find",        chordGoBtn);      add ("spec.rt",        chordRtBtn);
    add ("spec.adaptTarget", adaptTargetBox);  add ("spec.partialsLbl", adaptPartialsLbl);
    add ("spec.partials",    adaptPartialsBox); add ("spec.goal",     adaptGoalBox);
    add ("spec.amps",        adaptAmpsToggle); add ("spec.rangeLbl",  adaptRangeLbl);
    add ("spec.rangeLo",     adaptLoEdit);     add ("spec.rangeHi",   adaptHiEdit);
    add ("spec.adapt",       adaptGoBtn);      add ("spec.adaptRt",   adaptRtBtn);
    add ("spec.regLbl",      adaptRegLbl);     add ("spec.regSteps",  adaptRegStepsEdit);
    // The result list, never probed before, and the first thing a new row starves.
    add ("spec.adaptList",   adaptView);       add ("spec.prog",      chordProgLbl);
    // ...and the two WRAPPING read-outs, measured by the rule that fits them (see
    // LayoutProbe::WrappedLabel). They carry the sentences this panel's whole clarity
    // rework rests on, so "it fits" has to be a measurement, not a glance.
    addWrapped ("spec.info",    adaptInfoLbl);
    addWrapped ("spec.regSpan", adaptRegSpanLbl);
    // The OTHER four states of that read-out, at their widest: 100% progress, a run priced
    // in minutes, a five-figure update count, a lagging follower, and a result line
    // carrying both its percentage and the Reps/Fractal caveat. One of them is on screen;
    // the rest have to be measured here or not at all.
    if (adaptInfoLbl.isVisible())
    {
        const juce::String widestSpan ("-2" + juce::String::fromUTF8 ("\xe2\x80\x93") + "10");
        p.readoutVariants.push_back (measureWrapped ("readout.working",   adaptInfoLbl, adaptLineWorking (100)));
        p.readoutVariants.push_back (measureWrapped ("readout.lag",       adaptInfoLbl, adaptLineLag (44.4 * 60000.0)));
        p.readoutVariants.push_back (measureWrapped ("readout.following", adaptInfoLbl, adaptLineFollowing (99999)));
        p.readoutVariants.push_back (measureWrapped ("readout.cost",      adaptInfoLbl, adaptLineCost (44.4 * 60000.0)));
        p.readoutVariants.push_back (measureWrapped ("readout.result",    adaptInfoLbl,
                                                     adaptLineResult (100, false, widestSpan, true)));
        // ...and the register line in BOTH forms, at their widest numbers: the anchor a
        // one-degree-per-period scale can reach ("-10:0"), a three-digit step count, and the
        // "only N fit" wording that only appears at the top of the keyboard. That second
        // form is the longer one and it is precisely the one a player never sees until the
        // day they play up there.
        p.readoutVariants.push_back (measureWrapped ("regSpan.plain",   adaptRegSpanLbl,
                                                     adaptRegSpanLine ("-10:0", 127, 127, "10.6")));
        p.readoutVariants.push_back (measureWrapped ("regSpan.clamped", adaptRegSpanLbl,
                                                     adaptRegSpanLine ("-10:0", 127, 127 - 1, "10.5")));
    }
    add ("spec.segPerLbl",   segPeriodLbl);    add ("spec.segPeriod", segPeriodEdit);
    add ("spec.segTop",      segTopToggle);

    // The sub-CELLS. Empty ones are skipped: the harmonic cells are deliberately empty
    // in the custom view (paint() reads that as "draw no harmonic dividers").
    auto cell = [&p] (const char* name, const juce::Rectangle<int>& r)
    { if (! r.isEmpty()) p.cells.push_back ({ name, r }); };
    cell ("eng.base",    hcBase);    cell ("eng.harm",    hcHarm);   cell ("eng.prime", hcPrime);
    cell ("both.slope",  beSlope);   cell ("both.fractal", beFractal);
    cell ("both.morph",  beMorph);   cell ("both.range",  beRange);
    cell ("ess.phase",   ecPhase);   cell ("ess.gain",    ecGain);
    cell ("ess.ref",     ecRef);     cell ("ess.oct",     ecOct);

    // Every combo applyLanguage() re-words, and what it currently has selected.
    auto combo = [&p] (const char* name, const juce::ComboBox& c)
    {
        p.comboIds.push_back ({ name, c.getSelectedId() });
        // The WIDEST item this box can show, against the room it has. The widest one,
        // not the selected one: the box does not resize when the user changes modes, so
        // a slot that only fits the current choice clips the other one the moment it is
        // picked - and a probe that measured the selection would have to catch the box
        // in that state to see it. The room is GalacticLnF's own comboTextWidth, the
        // same function positionComboBoxText lays the label out with, so this check can
        // never drift from what is actually drawn. Measured in the box's live font.
        if (c.isVisible())
        {
            const auto f = c.getLookAndFeel().getComboBoxFont (const_cast<juce::ComboBox&> (c));
            juce::String widest; float need = 0.0f;
            for (int i = 0; i < c.getNumItems(); ++i)
            {
                const auto t = c.getItemText (i);
                const auto w = (float) juce::GlyphArrangement::getStringWidthInt (f, t);
                if (w > need) { need = w; widest = t; }
            }
            p.comboTexts.push_back ({ name, widest, need,
                                      GalacticLnF::comboTextWidth (c.getWidth(), c.getHeight()) });
        }
    };
    combo ("basePitch",   basePitchBox);   combo ("phase",       phaseBox);
    combo ("primeMode",   primeModeBox);   combo ("bend",        bendBox);
    combo ("modSel",      modSelBox);      combo ("lfoWave",     lfoWaveBox);
    combo ("lfoRateMode", lfoRateModeBox); combo ("lfoPhaseMode", lfoPhaseModeBox);
    combo ("specMode",    specModeBox);    combo ("tuneSource",  tuneSourceBox);
    combo ("tuneScope",   tuneScopeBox);   combo ("adaptTarget", adaptTargetBox);
    combo ("adaptGoal",   adaptGoalBox);   combo ("shepMode",    shepModeBox);
    combo ("shepRise",    shepRiseBox);

    // The painted sub-labels, each with the width its text really needs in the font
    // paint() sets for it.
    for (const auto& s : paintedSubLabels())
        p.paintedLabels.push_back ({ s.name, s.text, s.cell,
            (float) juce::GlyphArrangement::getStringWidthInt (
                juce::Font (juce::FontOptions (s.fontHeight).withStyle ("Bold")), s.text),
            s.col });

    // The words that ARE the feature (see LayoutProbe::buttonTexts).
    p.buttonTexts.push_back ({ "cust.load",  loadTimbreButton.getButtonText() });
    p.buttonTexts.push_back ({ "tun.toBank", tuningBankAddBtn.getButtonText() });
    p.buttonTexts.push_back ({ "top.playground", playgroundButton.getButtonText() });
    // Recorded only while it exists (its two words are state-dependent, and probing a
    // hidden button would measure a word resized() never put on screen).
    if (loadSpectrumTuningBtn.isVisible())
        p.buttonTexts.push_back ({ "tun.specTuning", loadSpectrumTuningBtn.getButtonText() });

    // The three border lines that must be one (see LayoutProbe::wall*). Derived from
    // kFrameLine, the same constant both frame painters use, so the probe can never be
    // measuring an offset paint() has stopped using.
    p.wallSpectrumRight = (float) spectrumBounds.getRight() - kFrameLine;
    p.wallTuningRight   = (float) lFootBounds.getRight()    - kFrameLine;
    p.wallColumnLeft    = (float) gSpecScale.getX()         + kFrameLine;
    // const_cast because juce::SettableTooltipClient::getTooltip() is not const and this
    // probe is. Deliberately the LIVE string the component will hand the tooltip window,
    // not a copy kept beside it: a copy is exactly the thing that goes on passing after
    // the real tooltip has been reworded.
    p.regStepsTip       = const_cast<juce::TextEditor&> (adaptRegStepsEdit).getTooltip();
    p.dividers          = spectrumDividers(); // the very lines paint() strokes
    return p;
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

// For every watched fader, check whether ANY of the 6 modulators currently
// targets its parameter; if so, read the RAW atomic ModEngine writes into
// (base + modulation, in the parameter's own plain units - see ModEngine.h) and
// stash it as a "modLive" property so GalacticLnF can draw a second marker at
// that live position. A fader with nothing mapped simply loses the property
// (no marker) the tick after it is unmapped.
void FractalAdditiveAudioProcessorEditor::refreshModWatch()
{
    auto& mod = processorRef.getModEngine();
    for (auto* s : modWatchSliders)
    {
        const auto pid = s->getProperties()["modId"].toString();
        bool mapped = false;
        for (int m = 0; m < ModEngine::kMods && ! mapped; ++m)
            mapped = mod.isMappedTo (pid, m);

        if (! mapped)
        {
            auto& pr = s->getProperties();
            if (pr.contains ("modLive") || pr.contains ("modLo"))
            { pr.remove ("modLive"); pr.remove ("modLo"); pr.remove ("modHi"); s->repaint(); }
            continue;
        }

        // PER-VOICE target: the parameter deliberately does not move (each voice adds its
        // own modulation internally), so there is no live value to point at - and with
        // several notes sounding there would be several. Show the REACHABLE BAND instead.
        if (ModEngine::perVoiceTargetIndex (pid) >= 0)
        {
            auto* p = processorRef.apvts.getParameter (pid);
            if (p == nullptr) continue;
            float lo = 0.0f, hi = 0.0f;
            mod.modulationSpan (pid, lo, hi);
            const float b = p->getValue();
            const double vLo = (double) p->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, b + lo));
            const double vHi = (double) p->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, b + hi));
            auto& pr = s->getProperties();
            const bool changed = ! pr.contains ("modLo")
                              || std::abs ((double) pr["modLo"] - vLo) > 1.0e-6
                              || std::abs ((double) pr["modHi"] - vHi) > 1.0e-6;
            if (changed || pr.contains ("modLive"))
            {
                pr.set ("modLo", vLo); pr.set ("modHi", vHi); pr.remove ("modLive");
                s->repaint();
            }
            continue;
        }

        auto* raw = processorRef.apvts.getRawParameterValue (pid);
        if (raw == nullptr) continue;
        const float live = raw->load();
        const auto& props = s->getProperties();
        const bool hadIt = props.contains ("modLive");
        if (! hadIt || std::abs ((float) (double) props["modLive"] - live) > 1.0e-4f)
        { s->getProperties().set ("modLive", (double) live); s->repaint(); }
    }
}

void FractalAdditiveAudioProcessorEditor::refreshPresetDirtyMark()
{
    const bool drifted = ! processorRef.getLoadedPresetName().isEmpty() && processorRef.presetHasDrifted();
    if (drifted != presetDirtyMark.isVisible())
    {
        presetDirtyMark.setVisible (drifted);
        // The tuning offer is specific to the preset AS SAVED; once it has drifted,
        // showing it (or letting Load-tuning silently re-sync it) would be lying
        // about what "this preset" even means any more.
        resized();
    }
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
    // Watch it for live modulation (see modWatchSliders above). CUSTOM-tab faders
    // start with a template id ("modE1A"...); attachCustomTab() keeps this property
    // pointed at whichever modulator is actually selected.
    s.getProperties().set ("modId", paramID);
    modWatchSliders.push_back (&s);
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
        {
            custAdsrAtt[i] = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + sfx[i], custAdsr[i]);
            custAdsr[i].getProperties().set ("modId", pfx + sfx[i]); // keep the live-mod watch pointed here
        }
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
        lfoDivSlider.getProperties().set   ("modId", pfx + "Div");
        lfoHzSlider.getProperties().set    ("modId", pfx + "Hz");
        lfoPhaseSlider.getProperties().set ("modId", pfx + "Phase");
    }
    custRangeAtt = std::make_unique<SliderAttachment> (processorRef.apvts, pfx + "Range", custRangeFader);
    custRangeFader.getProperties().set ("modId", pfx + "Range");
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
    // Remember where the title landed: clicking it plays the jingle (see mouseDown).
    {
        juce::Rectangle<float> tb;
        for (const auto& s : squares) tb = tb.isEmpty() ? s.rect : tb.getUnion (s.rect);
        titleClickArea = tb.isEmpty() ? juce::Rectangle<int>() : tb.getSmallestIntegerContainer();
    }

    // SPECTRUM / Essentials / Tuning macro-groups: the WHOLE box is drawn here - frame +
    // fill + title (in ONE colour) + the SHARED internal dividers between its sub-cells +
    // each cell's small inline uppercase label. Drawn in the editor paint so it sits UNDER
    // the child controls but is never covered by a child fill (and so no opaque
    // GroupComponent can end up on top of the controls it is supposed to contain).
    // interiorAlpha < 1 greys just the fill (the frame + title stay bright), used for
    // whichever engine box is currently being overridden by the other.
    // The frame alone, no title chip. Split out so a box whose contents already name
    // themselves can have the same outline without a caption repeating the obvious.
    auto macroFrame = [&] (juce::Rectangle<int> boxI, juce::Colour col, float interiorAlpha)
    {
        // Rounded top, angular bottom - and a SQUARE top-right when that edge is the shared
        // wall, so the wall reads as one straight line instead of two arcs pinching it.
        // macroFramePath is the only place that decides; layoutProbe() measures its result.
        const auto shape = macroFramePath (boxI);
        g.setColour (juce::Colour (0xff17171f).interpolatedWith (col, 0.05f * interiorAlpha));
        g.fillPath (shape);
        g.setColour (col.withAlpha (0.95f));
        g.strokePath (shape, juce::PathStrokeType (2.6f));
        return boxI.toFloat();
    };
    auto macroFrameTitle = [&] (juce::Rectangle<int> boxI, juce::Colour col, const juce::String& ttl,
                                float interiorAlpha)
    {
        const auto box = macroFrame (boxI, col, interiorAlpha);
        drawTitleChip (g, box, col, ttl); // colour chip, bounded to the text
    };
    // Every inline sub-label of every macro box, drawn from the ONE list layoutProbe()
    // also measures (see paintedSubLabels): the rect that is drawn and the rect that is
    // checked can then never drift apart.
    auto drawSubLabels = [&]
    {
        for (const auto& s : paintedSubLabels())
        {
            g.setColour (s.col);
            g.setFont (juce::Font (juce::FontOptions (s.fontHeight).withStyle ("Bold")));
            if (s.fitted)
                g.drawFittedText (s.text, s.cell, juce::Justification::centredLeft, 1, 0.8f);
            else
                g.drawText (s.text, s.cell, juce::Justification::centredLeft, false);
        }
    };

    //== SPECTRUM: one panel holding both engines and everything that shapes either =====
    if (! spectrumBounds.isEmpty())
    {
        // The container wears the SELECTED ENGINE's colour - frame, title chip and
        // interior tint - exactly as the Modulators box wears its selected tab's. It was
        // a neutral steel that read as white next to the coloured panels around it and
        // said nothing about which engine was making the sound.
        macroFrameTitle (spectrumBounds, spectrumPanelColour, tr ("SPECTRUM", "ESPECTRE"), 1.0f);

        //-- ONE BOX, COMPARTMENTS, ONE COLOUR --------------------------------------
        // The panel used to hold two more FRAMED boxes inside itself (the engine box and
        // a "both engines" box), each with its own outline, its own interior fill and -
        // worse - its own hue: mint for Decay/Morph/Range, magenta for Fractal, blue or
        // pink for the engine. Three frames inside a frame said "three independent
        // things"; they are three compartments of one spectrum. So: no inner frames at
        // all, only DIVIDER LINES, and every one of them (like every sub-label and every
        // control - see recolourSpectrumControls) in the SELECTED ENGINE's colour. There
        // is now exactly one hue in the panel, and changing engine changes all of it.
        //
        // Lines are drawn INSIDE the outer frame: its top line sits at box.y + 16 with a
        // 2.6 px stroke, so anything starting above ~17.5 pokes through the panel's edge.
        {
            g.setColour (spectrumPanelColour.withAlpha (0.5f));
            // ONE list, built by spectrumDividers(), which layoutProbe() also reports.
            // paint() drawing its own lines while the probe rebuilt them from a second
            // copy of the rule is exactly how a wall that stopped 23 px short of the
            // panel floor passed every check in the build.
            for (const auto& d : spectrumDividers())
                g.drawLine (d.second, 1.0f);
        }
    }

    // Essentials macro-group: one framed box, one colour, PHASE / GAIN / REFERENCE /
    // OCTAVE stacked with shared horizontal dividers + inline labels (like the ADSR
    // letters). Same treatment as the boxes inside SPECTRUM. (OCTAVE is the whole-synth
    // transpose only now - the spectrum's RANGE cut moved into SPECTRUM, where it can be
    // seen next to the other things that thin the partials.)
    if (! essentialsBounds.isEmpty())
    {
        macroFrameTitle (essentialsBounds, cEssentials, tr ("ESSENTIALS", "ESSENCIALS"), 1.0f);
        g.setColour (cEssentials.withAlpha (0.5f)); // shared horizontal dividers between the cells
        const float lx = (float) essentialsBounds.getX() + 4.0f;
        const float rx = (float) essentialsBounds.getRight() - 4.0f;
        for (auto* cell : { &ecGain, &ecRef, &ecOct })
            g.drawLine (lx, (float) cell->getY(), rx, (float) cell->getY(), 1.0f);
    }

    // Every inline sub-label of every macro box, from the one shared list.
    drawSubLabels();

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
        presetDirtyMark.setBounds (presetBox.getRight() - 15, presetBox.getY() - 3, 14, 14);
        r2.removeFromLeft (4);
        savePresetButton.setBounds (r2.removeFromLeft (46));
        // A third row, the full width of the cluster: the button's word is the whole
        // feature and it is the longer language that decides how wide it has to be, so
        // it gets the lot rather than a leftover slice (see the probe check in
        // RhythmVerify - "Obre el Laboratori de Presets" is 8 characters longer than the
        // English and clipped at 150 px).
        playgroundButton.setBounds (lc.getX(), presetBox.getBottom() + 3, 200, 20);
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

    // A left-hand caption sized to the WORDS in it, never to a pixel count that only
    // ever fitted English. A juce::Label whose cell is too narrow does not clip and does
    // not disappear - it CONDENSES to 70% - so it stays inside its bounds, overlaps
    // nothing, and every geometric check in the build passes while the word on screen is
    // visibly squashed ("Temps" in the Morph cell was at 91%). Never takes so much that
    // the control beside it is left with nothing.
    auto caption = [] (juce::Label& l, juce::Rectangle<int>& row, int nominal, int keepForControl = 24)
    {
        l.setJustificationType (juce::Justification::centredLeft);
        const int need = juce::GlyphArrangement::getStringWidthInt (l.getFont(), l.getText()) + 4;
        const int w    = juce::jmax (nominal, need);
        l.setBounds (row.removeFromLeft (juce::jmin (w, juce::jmax (0, row.getWidth() - keepForControl))));
    };

    //== Right column: ONE full-height Spectrum-to-Tuning box - mode controls on top,
    //   its result list (chords / dyad degrees / segment picker) INSIDE it below.
    //   The Tuning box is now a plain row-2 box (no more inverted L).
    {
        //== THE SHARED WALL ====================================================
        // SPECTRUM's right border, TUNING's right border and the Spectrum-vs-Tuning
        // column's LEFT border are ONE drawn line, not two lines side by side and not
        // three lines with gutters between them.
        //
        // Every box's border is a stroke whose centre sits kFrameLine inside its own
        // rectangle (see macroFrame / drawGroupComponentOutline), so "one line" means
        // the box rects must OVERLAP by 2*kFrameLine: rows 1-2 end 3 px right of the
        // wall, the column starts 3 px left of it, and both strokes then land on the
        // same x. The column paints after the editor (it is a child component), so the
        // single line that survives is its own - which is correct: the wall belongs to
        // the box that runs the full height of the window.
        //
        // Merely ABUTTING the rects (which is what "no gutter" first produced) still
        // drew two lines, pink then orange, back to back - a 7 px two-tone band that is
        // exactly the doubled border this was meant to remove.
        //
        // The old layout had a 6 px gutter PLUS both insets = 12 px of dead space
        // between two parallel lines. Reclaiming it widens rows 1-2 by 9 px and the
        // right column by 6, so nothing had to shrink to pay for it - which is why
        // nothing in either box needed re-fitting.
        const int wallX = area.getRight() - 208;
        // Box inset + the two stroke offsets. ROUNDED, not truncated: 2*1.3 truncates to
        // 2 and leaves the two strokes 0.6 px apart, which is a visible pink sliver down
        // the orange border - the doubled wall in miniature.
        const int lap   = 3 + juce::roundToInt (2.0f * kFrameLine);
        auto sp = area.withLeft (wallX - lap);
        area.setRight (wallX + 3);

        // ONE beating mode now, with two selectors inside it:
        //   SOURCE - from the current tuning (combinations of its own degrees) or
        //            from all possible tunings (a free search, loadable as a tuning)
        //   SCOPE  - rank by the beating against 0 only, or by every pair's beating
        const int  mode = specModeBox.getSelectedId();
        const bool beat = mode == 1;                       // "Find tunings based on beating"
        const bool adapt = mode == 2;                      // "Adapt spectrum to tuning..."
        const bool seg  = mode == 3;                       // From spectrum's segment
        const bool fromCurrent = tuneSourceBox.getSelectedId() != 2;
        const bool chord = beat && ! fromCurrent;          // the free chord search
        const bool dy    = beat && fromCurrent;            // combinations of this tuning

        gSpecScale.setBounds (sp.reduced (3, 2)); // same top/bottom lines as rows 1-2
        auto si = sp.reduced (3).reduced (8, 5); si.removeFromTop (20);
        specModeBox.setBounds (si.removeFromTop (34));     // two lines: full mode names fit
        si.removeFromTop (3);
        tuneSourceBox.setVisible (beat);
        // The SCOPE means the same thing in both directions - judge every note against the
        // root, or every pair - so the adaptation shares the control rather than growing a
        // second one that could disagree with it.
        tuneScopeBox.setVisible (beat || adapt);
        if (beat)
        {
            tuneSourceBox.setBounds (si.removeFromTop (22));
            si.removeFromTop (2);
        }
        if (adapt)
        {
            adaptTargetBox.setBounds (si.removeFromTop (22));
            si.removeFromTop (2);
        }
        if (beat || adapt)
        {
            // 24, not 30. It was sized for two lines of a name it no longer has ("0 vs all"
            // and "all vs all" are one short line in both languages), and in the adaptation
            // those six pixels are the difference between four adapted partials on screen
            // and five - the list is the leftover of this column and pays for every row.
            tuneScopeBox.setBounds (si.removeFromTop (24));
            si.removeFromTop (3);
        }
        si.removeFromTop (1);

        // Mode-gated visibility. BOTH beating sources now share ONE set of controls
        // - Notes, Min/Max, Period, smallest interval - so the two submenus differ
        // only in what they search, never in how they are operated. The old
        // dyad-only row (Find/Save/Steps/Period/min) is superseded by them.
        juce::Component* sethC[]  = { &dissBtn, &dissCountBox, &lblSteps, &periodEdit, &dissModeBox, &dissSaveBtn, &lblPeriod,
                                      &dissMinEdit, &dissMinLbl };
        juce::Component* segC[]   = { &segView, &segPeriodEdit, &segTopToggle, &segPeriodLbl, &segHelpLbl };
        juce::Component* beatC[]  = { &chordNotesBox, &chordMinMaxBox, &chordNotesLbl,
                                      &chordPerLbl, &chordPeriodEdit, &chordMinEdit, &chordMinLbl };
        juce::Component* adaptC[] = { &adaptTargetBox, &adaptPartialsBox, &adaptPartialsLbl,
                                      &adaptGoalBox, &adaptAmpsToggle, &adaptGoBtn, &adaptRtBtn,
                                      &lookaheadBtn, &adaptInfoLbl, &adaptView,
                                      &adaptRegLbl, &adaptRegStepsEdit, &adaptRegSpanLbl };
        for (auto* c : sethC)  c->setVisible (false);
        for (auto* c : segC)   c->setVisible (seg);
        for (auto* c : beatC)  c->setVisible (beat || adapt); // Period + smallest interval are shared
        for (auto* c : adaptC) c->setVisible (adapt);
        // Notes / Min-Max belong to the SEARCH only: the adaptation's count is its own
        // "Partials" box and its goal has a third option (a range) the search has no
        // meaning for.
        chordNotesBox.setVisible (beat); chordNotesLbl.setVisible (beat);
        chordMinMaxBox.setVisible (beat);
        adaptRangeLbl.setVisible (adapt && adaptGoalBox.getSelectedId() == 3);
        adaptLoEdit.setVisible   (adapt && adaptGoalBox.getSelectedId() == 3);
        adaptHiEdit.setVisible   (adapt && adaptGoalBox.getSelectedId() == 3);
        // "From current tuning" updates live: no Find button, and Save makes no sense.
        chordGoBtn.setVisible   (chord);
        chordRtBtn.setVisible   (chord);
        chordProgLbl.setVisible (chord);   // the adaptation's progress lives in its read-out
        nadBox.setVisible (false); // the note count is the shared "Notes" box now

        const bool fromChord = processorRef.getGeneratedSclName().startsWith ("chord-");
        chordViewBack.setVisible (chord && fromChord);
        chordViewFwd.setVisible  (chord && fromChord);

        if (seg)
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
        else if (beat || adapt) // the SAME stacked rows for the search and its inverse
        {
            // "Notes" spans 2..8 in the free search and 2..the tuning's degrees when
            // combining the current tuning: one control, always honest about what
            // it can produce.
            const int D    = juce::jmax (2, processorRef.getScaleDegrees());
            const int maxN = fromCurrent ? juce::jlimit (2, 12, D) : 8;
            if (maxN != lastNotesMax)
            {
                lastNotesMax = maxN;
                const int keep = chordNotesBox.getSelectedId();
                chordNotesBox.clear (juce::dontSendNotification);
                for (int n = 2; n <= maxN; ++n) chordNotesBox.addItem (juce::String (n), n);
                chordNotesBox.setSelectedId (juce::jlimit (2, maxN, keep < 2 ? 3 : keep),
                                             juce::dontSendNotification);
            }

            auto r1 = si.removeFromTop (22);
            if (beat)
            {
                chordNotesLbl.setJustificationType (juce::Justification::centredLeft);
                chordNotesLbl.setBounds (r1.removeFromLeft (44));
                compact (chordNotesBox, r1.removeFromLeft (48));
                r1.removeFromLeft (6);
                compact (chordMinMaxBox, r1);
            }
            else
            {
                adaptPartialsLbl.setJustificationType (juce::Justification::centredLeft);
                adaptPartialsLbl.setBounds (r1.removeFromLeft (52)); // "Partials"
                compact (adaptPartialsBox, r1.removeFromLeft (46));
                r1.removeFromLeft (6);
                compact (adaptGoalBox, r1);
            }
            si.removeFromTop (3);
            auto r2 = si.removeFromTop (22);
            chordPerLbl.setJustificationType (juce::Justification::centredLeft);
            chordPerLbl.setBounds (r2.removeFromLeft (44));
            chordPeriodEdit.setBounds (r2.removeFromLeft (44).reduced (0, 1));
            if (adapt)
            {
                r2.removeFromLeft (6);
                adaptAmpsToggle.setBounds (r2);      // "+ amplitudes"
            }
            // The result count is fixed at 16 - one control less to think about.
            chordCountLbl.setVisible (false); chordCountBox.setVisible (false);
            si.removeFromTop (3);
            auto r3 = si.removeFromTop (22);
            caption (chordMinLbl, r3, 112, 48); // "smallest interval (c)" / "interval mes petit (c)"
            chordMinEdit.setBounds (r3.removeFromLeft (juce::jmin (48, r3.getWidth())).reduced (0, 1));
            if (adapt && adaptGoalBox.getSelectedId() == 3)
            {
                si.removeFromTop (3);
                auto rr = si.removeFromTop (22);
                adaptRangeLbl.setJustificationType (juce::Justification::centredLeft);
                adaptRangeLbl.setBounds (rr.removeFromLeft (58)); // "between"
                adaptLoEdit.setBounds (rr.removeFromLeft (44).reduced (0, 1));
                rr.removeFromLeft (4);
                adaptHiEdit.setBounds (rr.removeFromLeft (44).reduced (0, 1));
            }
            if (adapt)
            {
                // HOW WIDE, and - on the line under it - FROM WHERE. On its own row because
                // the answer genuinely differs per register and the user must be able to
                // see, and change, what was optimised for rather than infer it from the
                // note they happened to be holding.
                si.removeFromTop (2);
                auto rg = si.removeFromTop (22);
                // ONE control on this row now: the caption and a three-digit count (32 px,
                // where a period number needed 25). The aggregate box that used to sit
                // beside it is gone - it could not change the result - and so are the two
                // lines of sentence under it that explained the choice; both are now one
                // paragraph in this field's own hover. The 36 px kept back is the field
                // plus its gap, so the caption may still grow into the rest of the row
                // rather than condensing to 70% in a language with a longer word.
                caption (adaptRegLbl, rg, 46, 36);
                adaptRegStepsEdit.setBounds (rg.removeFromLeft (32).reduced (0, 1));
                // WHAT N COMES TO, and WHICH NOTE it hangs off. One line at 10 pt, and it is
                // paid for out of the gaps around it rather than out of the result list:
                // without this line N is a magic number and the anchor is invisible, which
                // is the whole failure mode of a range that follows the hands.
                adaptRegSpanLbl.setBounds (si.removeFromTop (12));
            }
            if (chord)
            {
                si.removeFromTop (4);
                auto r4 = si.removeFromTop (24);
                // Find takes the left ~60%, REALTIME the rest: the follower is a mode, not
                // the primary action, and it must never be the button the hand lands on.
                chordGoBtn.setBounds (r4.removeFromLeft (r4.getWidth() * 3 / 5).reduced (1, 0));
                chordRtBtn.setBounds (r4.reduced (1, 0));
            }
            if (adapt)
            {
                si.removeFromTop (3);
                auto r4 = si.removeFromTop (24);
                adaptGoBtn.setBounds (r4.removeFromLeft (r4.getWidth() * 3 / 5).reduced (1, 0));
                adaptRtBtn.setBounds (r4.reduced (1, 0));
                // LOOKAHEAD on its own row UNDER the follower it belongs to: it is the
                // follower's latency trade and means nothing without it, so it must not
                // sit where it could be mistaken for a third action.
                si.removeFromTop (2);
                lookaheadBtn.setBounds (si.removeFromTop (22).reduced (1, 0));
                si.removeFromTop (1);
                // THE one read-out (see adaptReadoutText). 28 px is two lines at 10.5 pt,
                // and every sentence it can print is written to fit two - RhythmVerify
                // measures all five of them, in both languages, because only one is ever on
                // screen and the state that overflows is always the other one. There is no
                // second line of numbers under it any more and no separate progress row:
                // while a run is going, the progress IS this line.
                adaptInfoLbl.setBounds (si.removeFromTop (28));
            }
            if (chord && fromChord) // labelled view switch: all results vs the loaded chord's degrees
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
            // The chord search keeps its own progress line. The adaptation does NOT: its
            // read-out above carries the progress, and a second line that was blank most
            // of the time and said "realtime: 37 updates" the rest of it was one of the
            // two lines nobody could explain.
            if (! adapt)
            {
                si.removeFromTop (2);
                chordProgLbl.setBounds (si.removeFromTop (14)); // search progress only
            }
        }

        // The result list fills the REST of this same box.
        si.removeFromTop (4);
        const bool showDeg = (dy) || (chord && fromChord && chordDegView);
        // The SCOPE selector only decides how a combination is RANKED (against 0, or
        // over every pair); how many notes it has is the shared "Notes" box above.
        dissHeader.setVisible (false);
        dissView.setVisible (false);
        nadView.setVisible (showDeg);
        chordView.setVisible (chord && ! showDeg);
        if (adapt)
        {
            adaptView.setBounds (si);
            const int ah  = adaptList.contentHeight();
            const bool as = ah > si.getHeight();
            adaptList.setSize (as ? si.getWidth() - 10 : si.getWidth(), juce::jmax (si.getHeight(), ah));
        }
        else if (seg)
        {
            segView.setBounds (si);
            segList.numRows = processorRef.getTemplatePartialCount();
            const int sh  = segList.contentHeight();
            const bool ss = sh > si.getHeight();
            segList.setSize (ss ? si.getWidth() - 10 : si.getWidth(), juce::jmax (si.getHeight(), sh));
        }
        else if (showDeg)
        {
            const int D = juce::jmax (1, processorRef.getScaleDegrees());
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

    //== Row 1: the SPECTRUM macro-panel, across the whole width ==================
    // One panel, two nested boxes: [ENGINE (shared)] [BOTH ENGINES]. The two engines
    // are MUTUALLY EXCLUSIVE, so they share ONE box whose title row is the HARMONIC /
    // CUSTOM chip strip: the selected chip's controls are inside, the other engine's
    // are hidden (never reset - their parameters are untouched). Everything that
    // shapes WHICHEVER engine is running - Decay (+ the per-partial pencil), Fractal,
    // Morph, Range - is in the second box, outside the engine frame.
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        // Same 3 px horizontal inset as every other macro box, so all frames in both
        // rows share their left and right lines.
        spectrumBounds = row.reduced (3, 0);

        auto sp = spectrumBounds.reduced (5, 4);
        sp.removeFromTop (16); // clear the panel's frame line + title-chip band

        const int pW = sp.getWidth();
        // The shared engine box gets what the harmonic view (its busiest content:
        // Base + Harmonics + Prime) needs; the BOTH ENGINES box takes the rest.
        engineBoxBounds   = sp.removeFromLeft (pW * 46 / 100);
        sp.removeFromLeft (5);
        bothEnginesBounds = sp;

        const int lblH  = 15; // inline sub-label band (BASE / HARMONICS / DECAY / ...)
        const int chipH = 18; // the box's title band - the engine chips live in it

        // Which engine's controls the shared box shows = which engine SOUNDS
        // (engineInForce), the same rule that lights the chips: clicking a chip both
        // selects the view and makes that engine the one generating. Temper mode shows
        // the HARMONIC view - the harmonic engine is the one generating there. The one
        // exception is the empty-workspace pin (see customViewShown in the header).
        const bool customView = customViewShown();

        //-- the chip strip IS the box title (like the Modulators box's tab row) -----
        {
            auto eb = engineBoxBounds.reduced (5, 0);
            auto chips = eb.removeFromTop (chipH);
            for (int i = 0; i < 2; ++i)
            {
                engineChip[i].setBounds (chips.removeFromLeft (104).reduced (1, 1));
                chips.removeFromLeft (3);
            }
        }

        // View switching: only the selected engine's controls exist on screen. Hidden,
        // not dimmed - with one shared box a dimmed control would sit ON TOP of the
        // other view's controls, and the old grey-out story ("inert but visible") is
        // exactly the messaging this layout retires.
        juce::Component* harmView[] = { &baseNFader, &lblBaseN, &basePitchBox, &lblBasePitch,
                                        &harmonicsFader, &primeModeBox };
        juce::Component* custView[] = { &loadTimbreButton, &randomSpectrumBtn, &intervalEdit,
                                        &timbreStatusLabel, &timbreRepeatsFader, &lblTimbreRepeats };
        for (auto* c : harmView) c->setVisible (! customView);
        for (auto* c : custView) c->setVisible (customView);

        if (! customView)
        {
            //-- HARMONIC view: BASE | HARMONICS over PRIME, TEMPER along the bottom --
            auto hb = engineBoxBounds.reduced (5, 0);
            hb.removeFromTop (chipH); hb.removeFromBottom (3);
            // TEMPER is reachable from BOTH views (see the ctor comment): here it spans
            // the bottom of the box, under the cells.
            temperToggle.setBounds (hb.removeFromBottom (20));
            // Base is the wider column so the Base-pitch combo can show "Transposed
            // down" in full without clipping.
            hcBase      = hb.removeFromLeft (hb.getWidth() * 52 / 100);
            auto midCol = hb;
            hcHarm      = midCol.withHeight (midCol.getHeight() * 2 / 5);
            hcPrime     = midCol.withTrimmedTop (midCol.getHeight() * 2 / 5);
            {
                auto bi = hcBase.reduced (5, 3); bi.removeFromTop (lblH);
                auto left = bi.removeFromLeft (bi.getWidth() * 34 / 100);
                lblBaseN.setBounds (left.removeFromTop (13)); baseNFader.setBounds (left);
                bi.removeFromLeft (5);
                lblBasePitch.setBounds (bi.removeFromTop (13));
                compact (basePitchBox, bi.removeFromTop (24));
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
                    // Take the width from what is LEFT, not a flat 170: withSizeKeepingCentre
                    // grows a rect around its centre, so a 170 forced into a narrower cell
                    // spilled the fader out of the box and over its neighbour.
                    const int plW = juce::jmin (170, pi.getWidth());
                    primeLimitFader.setBounds (pi.removeFromLeft (plW).withSizeKeepingCentre (plW, 22));
                }
            }
        }
        else
        {
            //-- CUSTOM view: the whole box belongs to the custom engine now ----------
            // The prime controls are the harmonic view's; paint() reads empty hc* rects
            // as "draw no harmonic dividers or sub-labels".
            hcBase = hcHarm = hcPrime = {};
            primeLimitFader.setVisible (false); lblPrimeLimit.setVisible (false);
            for (int i = 0; i < 9; ++i) { primeToggle[i].setVisible (false); primeLbl[i].setVisible (false); }

            auto ci = engineBoxBounds.reduced (6, 0);
            ci.removeFromTop (chipH + 2); ci.removeFromBottom (4);
            auto brow = ci.removeFromTop (24);
            // TEMPER on the right of the top row (reachable from BOTH views).
            temperToggle.setBounds (brow.removeFromRight (juce::jmin (170, brow.getWidth() * 40 / 100)));
            brow.removeFromRight (6);
            // [Load / Remove][Random] - two buttons that never change in number, so the
            // row does not re-shape under the user's hand the way it did when the Off
            // pill appeared and disappeared with the loaded scale. RANDOM is always
            // there because it is the one control that can CREATE a spectrum out of
            // nothing, which is exactly the state where a workspace is emptiest.
            loadTimbreButton.setBounds (brow.removeFromLeft (brow.getWidth() * 1 / 2).reduced (2, 0));
            randomSpectrumBtn.setBounds (brow.reduced (2, 0));
            // The bottom rows first, then EVERYTHING left goes to the intervals box:
            // the custom view has far fewer controls than the harmonic one, and the
            // freed space is exactly what the typed spectrum needed - it was one 24 px
            // line squeezed into a fifth of the panel.
            auto repsRow = ci.removeFromBottom (22);
            ci.removeFromBottom (4);
            auto statusRow = ci.removeFromBottom (16);
            ci.removeFromBottom (4);
            ci.removeFromTop (5);
            intervalEditHome = ci.reduced (1, 0); // multi-line: type intervals, Enter builds
            intervalEdit.setBounds (intervalEditHome);
            timbreStatusLabel.setBounds (statusRow);
            caption (lblTimbreRepeats, repsRow, 38, 60);
            timbreRepeatsFader.setBounds (repsRow.removeFromLeft (juce::jmin (220, repsRow.getWidth())));
            // No cells and no divider here on purpose. This view had one horizontal line
            // between the Load/Remove row and the typed-ratios box: "hay una linea fina
            // horizontal en custom spectrum entre el boton remove y la caja de ratios de
            // abajo, es inecesaria y fea, quitala." The ratios box draws its own border
            // one pixel below where that line was, so it separated nothing and read as a
            // doubled edge.
        }

        //-- BOTH ENGINES: SLOPE | FRACTAL | MORPH over RANGE --------------------
        {
            auto bo = bothEnginesBounds.reduced (5, 0);
            bo.removeFromTop (chipH); bo.removeFromBottom (3);
            const int bW = bo.getWidth();
            beSlope   = bo.removeFromLeft (bW * 31 / 100);
            beFractal = bo.removeFromLeft (bW * 23 / 100);
            auto mcol = bo;
            beMorph   = mcol.withHeight (mcol.getHeight() * 55 / 100);
            beRange   = mcol.withTrimmedTop (mcol.getHeight() * 55 / 100);
            {
                auto si = beSlope.reduced (6, 3); si.removeFromTop (lblH);
                // The pencil and its ON/OFF pill take a narrow column on the RIGHT of the
                // cell, the pill directly BELOW the pencil (they are one feature and read as
                // a pair); the fader keeps the rest.
                auto pcol = si.removeFromRight (46);
                si.removeFromRight (4);
                slopeFader.setBounds (si);
                const int pencilH = juce::jmin (30, juce::jmax (20, pcol.getHeight() / 3));
                partialEditBtn.setBounds (pcol.removeFromTop (pencilH).withSizeKeepingCentre (pencilH, pencilH));
                pcol.removeFromTop (4);
                partialManualT.setBounds (pcol.removeFromTop (juce::jmin (22, pcol.getHeight())));
            }
            {
                auto fi = beFractal.reduced (6, 3); fi.removeFromTop (lblH);
                fractalFader.setBounds (fi);
            }
            {
                auto mi = beMorph.reduced (6, 3); mi.removeFromTop (lblH);
                morphToggle.setBounds (mi.removeFromTop (20));
                mi.removeFromTop (2);
                auto mr = mi.removeFromTop (juce::jmin (19, juce::jmax (0, mi.getHeight())));
                caption (lblMorphTime, mr, 34, 60);
                morphTimeFader.setBounds (mr);
            }
            {
                // RANGE: the spectrum's octave span, as the same [value][+][-] stepper it
                // had in Essentials. Its name is the painted OCTAVE RANGE sub-label now,
                // so the row's own "Spectrum" label was deleted rather than hidden - a
                // member hidden on every single layout pass is a member nothing can ever
                // show, and it kept a dead entry alive in the layout probe.
                auto ri = beRange.reduced (6, 3); ri.removeFromTop (lblH + 3);
                auto rr = ri.removeFromTop (juce::jmin (24, juce::jmax (0, ri.getHeight())));
                sprVal.setBounds (rr.removeFromLeft (40));
                sprUp.setBounds  (rr.removeFromLeft (24).reduced (0, 1));
                rr.removeFromLeft (4);
                sprDn.setBounds  (rr.removeFromLeft (24).reduced (0, 1));
            }
        }
    }

    //== Row 2: Essentials | Modulators | Tuning ==================================
    {
        // AIR BETWEEN THE ROWS. Rows 1 and 2 do NOT share a border - they are two panels
        // with open page between them - so the gap is 6 px of box rect (8.6 px between the
        // drawn lines) rather than the 4 it was. Taken HERE, out of row 2's top, rather
        // than by deepening either row's own inset: those insets are what puts row 1's top
        // and row 2's bottom on the same lines as the full-height column beside them, and
        // a "wider gap" that quietly unpicked the shared top and bottom edges would be
        // trading one seam for two.
        area.removeFromTop (2);
        // Takes ALL the remaining height (not another rowH slice) so its boxes end
        // on exactly the same line as the full-height right column: the frames are
        // drawn from the box rect, so every box in this row must share one rect.
        auto row = area.reduced (0, 2);
        const int W = row.getWidth();

        // Essentials macro (Phase / Gain / Reference / Octave) - one framed box like
        // the ones inside SPECTRUM: 4 stacked cells, shared horizontal dividers + inline
        // labels drawn in paint() (from the ec* cell rects stored here). It came DOWN
        // from row 1 to make room for the SPECTRUM panel, into the slot the Custom Engine
        // box left behind, and is a little wider than it was there.
        {
            essentialsBounds = row.removeFromLeft (W * 28 / 100).reduced (3, 0);
            // ...and the same air sideways. Essentials, Envelopes and Tuning share no
            // border with each other, so each pair gets 8 px of box rect (10.6 between the
            // drawn lines) instead of 6. Taken out of the row BETWEEN the boxes, never off
            // the outer 3 px inset: that inset is what lines the leftmost box up with the
            // SPECTRUM panel above it and the rightmost one up with the shared wall.
            row.removeFromLeft (2);
            auto e = essentialsBounds.reduced (5, 4);
            e.removeFromTop (20); // clear the "ESSENTIALS" title band
            // OCTAVE holds ONE stepper row now (RANGE moved into SPECTRUM), so it needs
            // much less than the half-box it used to take and the other three cells get
            // the difference - the Reference read-out finally has room to breathe.
            const int octH = juce::jmin (40, e.getHeight() / 3);
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
              caption (lblRefA, rr, 34, 70);
              lblRefHz.setBounds (rr.removeFromRight (22));
              refAEdit.setBounds (rr); }
            { auto c = ecOct.reduced (6, 1);   c.removeFromLeft (78);
              // Same shape as PHASE / GAIN / REFERENCE: the stepper sits RIGHT of the
              // painted OCTAVE label (see paintedSubLabels()), not below it - it was the
              // one cell in the box laid out the other way round.
              // [white value][+ green][- red]: value first, then + before -.
              auto rr = c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()));
              // No "Transpose" caption: the cell's own painted OCTAVE label already says
              // it, and the stepper only ever transposes. The freed width goes to the
              // controls (the caption member itself is gone, not merely hidden).
              octVal.setBounds (rr.removeFromLeft (40));
              octUp.setBounds  (rr.removeFromLeft (24).reduced (0, 1));
              rr.removeFromLeft (4);
              octDn.setBounds  (rr.removeFromLeft (24).reduced (0, 1)); }
        }

        // ONE shared envelope box with three colour-chip tabs (Volume / Pitch /
        // Filter): the selected chip is bright and recolours the frame; the others
        // stay dimmed but readable. Minimal fixed width - the faders adapt - and
        // everything saved goes to the Tuning box on the right.
        {
            auto eb = row.removeFromLeft (juce::jmin (262, W * 24 / 100));
            row.removeFromLeft (2);   // the same unshared-border air, Envelopes -> Tuning
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
            // MONOPHONIC GLIDE: the toggle takes a strip off the BOTTOM of the tab's body
            // before anything else is laid out (the three tabs each copy `in`, so trimming
            // it here trims it for all of them - which is why it is done only on PITCH),
            // and the glide-time fader joins the pitch amount as a second extra column.
            const bool pitchTab = envTab == 1;
            monoGlideT.setVisible (pitchTab);
            if (pitchTab)
            {
                auto strip = in.removeFromBottom (18);
                monoGlideT.setBounds (strip);
                in.removeFromBottom (2);
            }
            juce::Slider* setP[] = { &pEnvAmtFader, &glideFader, &pitchAdsr[0], &pitchAdsr[1], &pitchAdsr[2], &pitchAdsr[3] };
            juce::Label*  labP[] = { &lblPitchAmt, &lblGlide, &pitchAdsrLbl[0], &pitchAdsrLbl[1], &pitchAdsrLbl[2], &pitchAdsrLbl[3] };
            juce::Slider* setF[] = { &cutoffFader, &resFader, &fEnvAmtFader, &filtAdsr[0], &filtAdsr[1], &filtAdsr[2], &filtAdsr[3] };
            juce::Label*  labF[] = { &lblCutoff, &lblRes, &lblEnvAmt, &filtAdsrLbl[0], &filtAdsrLbl[1], &filtAdsrLbl[2], &filtAdsrLbl[3] };
            // The four A/D/S/R faders of each voice envelope are replaced by the same
            // curve editor the modulators use; the tab's OTHER controls (pitch amount,
            // cutoff, resonance, filter-env amount) keep their faders beside it.
            juce::Slider** sets[] = { setV, setP, setF };
            juce::Label**  labs[] = { labV, labP, labF };
            const int counts[]    = { 4, 6, 7 };
            const int extras[]    = { 0, 2, 3 };   // leading faders that are NOT the ADSR
            for (int t = 0; t < 3; ++t)
            {
                const bool on = t == envTab;
                auto area2 = in;
                for (int i = 0; i < counts[t]; ++i)
                {
                    const bool isExtra = i < extras[t];
                    if (! on || ! isExtra) { sets[t][i]->setVisible (false); labs[t][i]->setVisible (false); continue; }
                    auto c = area2.removeFromLeft (juce::jmax (34, in.getWidth() / 7));
                    sets[t][i]->setVisible (true);
                    labs[t][i]->setVisible (true);
                    labs[t][i]->setBounds (c.removeFromTop (13));
                    sets[t][i]->setBounds (c);
                }
                if (on)
                {
                    voiceEnv.setVisible (true);
                    voiceEnv.setBounds (area2.withTrimmedLeft (4));
                    voiceEnvTab = t;
                    voiceEnv.col = tabCols[t];
                    voiceEnv.refresh();
                }
            }
            if (envTab == 3) voiceEnv.setVisible (false);

            // CUSTOM tab: modulator selector + On + Unmap-all row, then the selected
            // modulator's own controls (envelope ADSR+Range, or the LFO set).
            const bool cust  = envTab == 3;
            const bool isEnv = modSel < 3;
            juce::Component* selRowC[] = { &modSelBox, &modOnT, &modUnmapBtn };
            for (auto* c : selRowC) c->setVisible (cust);
            // Drawn wave: the editor OPENS BY ITSELF, docked to the right of this box,
            // for as long as the selected LFO is on "Custom (draw)" - no button to
            // find, and it disappears when you leave that LFO or change its wave.
            const bool drawMode = cust && ! isEnv && lfoWaveBox.getSelectedId() == 6;
            lfoWaveEditBtn.setVisible (false);
            if (drawMode)
            {
                const int l = modSel - 3;
                if (! waveEditor.isVisible() || lastDrawLfo != l)
                {
                    lastDrawLfo = l;
                    waveEditor.fromText (processorRef.getLfoWavePoints (juce::jmax (0, l)));
                }
                auto box = eb;                       // the Modulators box
                const int w = juce::jmin (300, getWidth() - box.getRight() - 12);
                waveEditor.setBounds (box.getRight() + 6, box.getY(), juce::jmax (160, w),
                                      juce::jmax (120, box.getHeight()));
                waveEditor.setVisible (true);
                waveEditor.toFront (false);
            }
            else { waveEditor.setVisible (false); lastDrawLfo = -1; }
            for (int i = 0; i < 4; ++i)
            { custAdsr[i].setVisible (false); custAdsrLbl[i].setVisible (false); } // replaced by the curve editor
            envEditor.setVisible (cust && isEnv);
            if (envEditor.isVisible()) envEditor.refresh();
    if (voiceEnv.isVisible())  voiceEnv.refresh(); // (the 30 Hz timer keeps it live)
            custRangeFader.setVisible (cust);
            lblCustRange.setVisible (cust);
            juce::Component* lfoC[] = { &lfoWaveBox, &lfoRateModeBox, &lfoPhaseModeBox,
                                        &lfoPhaseSlider, &lblLfoPhase };
            for (auto* c : lfoC) c->setVisible (cust && ! isEnv);
            // Rate mode 1 (BPM / 2^n) and 2 (Fundamental / 2^n) are driven by the
            // DIVISOR; mode 3 (Free Hz) by the Hz knob. Only the one that actually
            // does something is shown - a knob that cannot affect the sound is worse
            // than no knob at all.
            const int  rateMode = lfoRateModeBox.getSelectedId();
            const bool showDiv  = cust && ! isEnv && rateMode != 3;
            const bool showHz   = cust && ! isEnv && rateMode == 3;
            lfoDivSlider.setVisible (showDiv); lblLfoDiv.setVisible (showDiv);
            lfoHzSlider.setVisible  (showHz);  lblLfoHz.setVisible  (showHz);
            if (cust)
            {
                // [modulator][On][Unmap] and, for a drawn LFO wave, [Draw] - which
                // gets a readable share of the row instead of whatever is left over.
                auto selRow = in.removeFromTop (20);
                const bool drawVis = lfoWaveEditBtn.isVisible();
                if (drawVis) lfoWaveEditBtn.setBounds (selRow.removeFromRight (56));
                if (drawVis) selRow.removeFromRight (4);
                const int nCell = 3;
                const int cw3 = juce::jmax (30, selRow.getWidth() / nCell - 3);
                modSelBox.setBounds (selRow.removeFromLeft (cw3));
                selRow.removeFromLeft (3);
                modOnT.setBounds (selRow.removeFromLeft (juce::jmin (46, cw3)));
                selRow.removeFromLeft (3);
                modUnmapBtn.setBounds (selRow);
                in.removeFromTop (3);
                if (isEnv)
                {
                    // Curve editor + the Range fader beside it.
                    auto a2 = in;
                    auto rangeCol = a2.removeFromRight (46);
                    envEditor.setBounds (a2.withTrimmedRight (4));
                    lblCustRange.setBounds (rangeCol.removeFromTop (13));
                    custRangeFader.setBounds (rangeCol);
                }
                else
                {
                    auto combos = in.removeFromTop (20);
                    lfoWaveBox.setBounds (combos.removeFromLeft (combos.getWidth() / 2).reduced (1, 0));
                    lfoRateModeBox.setBounds (combos.reduced (1, 0));
                    in.removeFromTop (2);
                    lfoPhaseModeBox.setBounds (in.removeFromTop (20).reduced (1, 0));
                    in.removeFromTop (2);
                    // Three columns, not four: whichever of Div/Hz applies, plus
                    // Phase and Range. The hidden one leaves no gap behind.
                    const int cw = in.getWidth() / 3;
                    auto a2 = in;
                    if (showDiv)
                    { auto c = a2.removeFromLeft (cw); lblLfoDiv.setBounds (c.removeFromTop (12)); lfoDivSlider.setBounds (c); }
                    if (showHz)
                    { auto c = a2.removeFromLeft (cw); lblLfoHz.setBounds (c.removeFromTop (12));  lfoHzSlider.setBounds (c); }
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
            // SHEPARD generator: the panel's bottom row, reserved FIRST so the pile of
            // optional rows above (its-spectrum / preset-tuning / spectrum-tuning) can
            // never squeeze it out - reserved last it would inherit the zero-height
            // leftovers exactly in the states with the most buttons showing. Its height
            // is PAID FOR by merging the old bend and Recent rows into one below, so
            // the bank row keeps the same worst case it had.
            auto shepRow = ti.removeFromBottom (22);
            ti.removeFromBottom (3);
            auto mrow = ti.removeFromTop (24);
            compact (tuningBox, mrow.removeFromLeft (mrow.getWidth() * 34 / 100));
            mrow.removeFromLeft (5);
            // Load / Save / + sit immediately to the RIGHT of the mode selector, each at
            // its OWN minimum width. They used to have a row to themselves underneath,
            // split into equal thirds of the whole panel, which made three buttons with
            // four characters of text on them absurdly wide and pushed the tuning bank
            // down. The word "tuning" is gone from the labels too - a tuning fork in
            // front of each one says the same thing in a fraction of the space.
            {
                loadSclButton.setVisible (true);
                saveTuningBtn.setVisible (true);
                tuningBankAddBtn.setVisible (true);
                clearSclButton.setVisible (false);  // Reset removed - the mode selector does it
                const juce::Font bf (juce::FontOptions (12.0f));
                const auto fit = [&] (juce::TextButton& b)
                {
                    const int fork = b.getProperties().contains ("fork") ? 13 : 0; // drawn glyph
                    const int w = juce::GlyphArrangement::getStringWidthInt (bf, b.getButtonText()) + 14 + fork;
                    b.setBounds (mrow.removeFromLeft (juce::jmin (w, juce::jmax (0, mrow.getWidth()))));
                    mrow.removeFromLeft (4);
                };
                fit (loadSclButton);
                fit (saveTuningBtn);
                fit (tuningBankAddBtn);
            }
            tuningNameLabel.setBounds (mrow); // shows the loaded scale's file name
            // Period of the active scale, in red, always visible. For an absolute 128-key
            // table it is Fibló's ESTIMATE, so the same row also carries the field that
            // overrides it (and a reset back to the estimate) - the readout alone would
            // state a guess as if the tuning had declared it.
            ti.removeFromTop (2);
            {
                auto prow = ti.removeFromTop (18);
                const bool isTable = processorRef.tuningIsAbsoluteTable();
                tablePeriodEdit.setVisible (isTable);
                tablePeriodResetBtn.setVisible (isTable && processorRef.tuningTablePeriodIsUserSet());
                if (isTable)
                {
                    if (tablePeriodResetBtn.isVisible())
                    {
                        tablePeriodResetBtn.setBounds (prow.removeFromRight (46).reduced (0, 1));
                        prow.removeFromRight (4);
                    }
                    tablePeriodEdit.setBounds (prow.removeFromRight (72).reduced (0, 1));
                    prow.removeFromRight (5);
                }
                tuningPeriodLbl.setBounds (prow);
            }
            // Load / Save / + to bank share ONE row: stacking them overflowed the box
            // and squeezed the tuning-bank controls to zero height (invisible).
            // "Load tuning" is ALWAYS available. Hiding it outside Scala mode left a
            // fresh instance - which now opens in MTS-ESP - with no way at all to load
            // a scale: there is no drag-and-drop and no menu route, and the only other
            // .scl chooser on screen loads a TIMBRE, which is the wrong thing entirely.
            // The gate was redundant anyway: loadScalaFile() switches to Scala itself.
            // (Load / Save / + are laid out beside the mode selector above; the row that
            // used to be here is gone, which gives the tuning bank its height back.)
            const int tmode = processorRef.getTuningMode();
            // Only for scales this synth wrote: they carry their own patch.
            const bool hasPatch = processorRef.activeSclHasPreset();
            loadItsSpectrumBtn.setVisible (hasPatch);
            if (hasPatch)
            {
                ti.removeFromTop (3);
                loadItsSpectrumBtn.setBounds (ti.removeFromTop (22).reduced (2, 0));
            }
            // Only when the preset just loaded carries a tuning that actually
            // differs from the one active now (recomputed live: loading it, or
            // changing the tuning some other way, makes the button disappear again) -
            // AND the preset itself has not drifted (a knob/automation/undo moved
            // something outside its own modulators): once it has drifted, "this
            // preset's tuning" is no longer an honest thing to offer.
            const bool hasPresetTuning = processorRef.hasPendingPresetTuning()
                                       && ! processorRef.presetHasDrifted();
            loadPresetTuningBtn.setVisible (hasPresetTuning);
            if (hasPresetTuning)
            {
                ti.removeFromTop (3);
                loadPresetTuningBtn.setBounds (ti.removeFromTop (22).reduced (2, 0));
            }
            // The custom spectrum's own tuning, whenever it HAS one (derived by Adapt,
            // or loaded from a scale file). Same idiom as the preset row above, but it
            // stays VISIBLE when that tuning is already active - disabled and saying so
            // (refreshSpectrumTuningButton) - because vanishing right after the click
            // would read as the button failing rather than the job being done.
            const bool hasProvTuning = processorRef.hasSpectrumProvenance();
            loadSpectrumTuningBtn.setVisible (hasProvTuning);
            if (hasProvTuning)
            {
                refreshSpectrumTuningButton();
                ti.removeFromTop (3);
                loadSpectrumTuningBtn.setBounds (ti.removeFromTop (22).reduced (2, 0));
            }
            // Pitch bend and Recent share ONE row now (bend left, Recent right): the
            // Shepard row at the bottom is paid for with exactly the row this merge
            // frees, so the bank underneath keeps the height it had. Bend is hidden
            // in MPE-input mode (the MPE zone fixes the per-note range at +-48, so
            // the choice is irrelevant there) - Recent then takes the whole row.
            const bool bendVis = tmode != 3;
            lblBend.setVisible (bendVis); bendBox.setVisible (bendVis);
            {
                ti.removeFromTop (3);
                auto bendRow = ti.removeFromTop (22);
                if (bendVis)
                {
                    lblBend.setJustificationType (juce::Justification::centredLeft);
                    lblBend.setBounds (bendRow.removeFromLeft (74));
                    compact (bendBox, bendRow.removeFromLeft (juce::jmin (120, bendRow.getWidth())));
                    bendRow.removeFromLeft (6);
                }
                // Recent is ONE button that opens a floating list of every file loaded;
                // the bank stays inline. Both stay reachable in every mode - picking one
                // of them IS how you get into Scala mode, so gating them on already
                // being there was a dead end. It also keeps "+ to bank" from ever
                // feeding a list you cannot see.
                recentOpenBtn.setBounds (bendRow.reduced (2, 0));
            }
            recentLabel.setVisible (false);
            for (int i = 0; i < 3; ++i) recentBtn[i].setVisible (false);
            const bool bankUi = true;
            recentOpenBtn.setVisible (bankUi);
            lblTuningBank.setVisible (bankUi); tuningBankBox.setVisible (bankUi);
            tuningBankLoadBtn.setVisible (bankUi);
            tuningBankClearBtn.setVisible (bankUi);
            if (bankUi)
            {
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
            // SHEPARD MODE (the row reserved off the bottom above):
            // [Shepard][Free-or-Sync][the rate control in that mode's dress][rise
            // interval, only when the tuning's period is not an octave].
            // ONE rate control, two dresses: the ms fader (Free) and the beats box
            // (Sync) occupy the same slot and exactly one is on screen - a knob that
            // does nothing in the current mode is worse than no knob at all (the
            // same rule as the LFO Div/Hz pair). The rise-interval box follows the
            // same rule one step further: with an octave-period tuning the two
            // choices are the SAME interval, so the box is not there at all.
            {
                // Each box is sized to the WORDS IT HOLDS, in the language actually on
                // screen - never to a pixel count that only ever fitted English. A
                // ComboBox does not clip visibly and does not resize: it draws as much
                // of the item as the cell holds, so a fixed 94 px slot showed the
                // renamed "Sync Project Tempo" as "Sync Project Te" while every bounds
                // and overlap check in the build stayed green. The widest ITEM is what
                // is measured, not the selected one, or the box would resize under the
                // user as they change modes.
                const int shepH = juce::jmin (24, shepRow.getHeight());
                auto needFor = [shepH] (const juce::ComboBox& c, int nominal)
                {
                    auto& lf = c.getLookAndFeel();
                    const auto f = lf.getComboBoxFont (const_cast<juce::ComboBox&> (c));
                    int w = 0;
                    for (int i = 0; i < c.getNumItems(); ++i)
                        w = juce::jmax (w, juce::GlyphArrangement::getStringWidthInt (f, c.getItemText (i)));
                    return juce::jmax (nominal, GalacticLnF::comboWidthForText (w, shepH));
                };

                auto sh = shepRow;
                shepOnT.setBounds (sh.removeFromLeft (86));
                sh.removeFromLeft (4);
                compact (shepModeBox, sh.removeFromLeft (juce::jmin (needFor (shepModeBox, 94), sh.getWidth())));
                sh.removeFromLeft (6);
                const bool riseChoice = shepard::periodDiffersFromOctave (processorRef.getTuningPeriodRatio());
                shepRiseBox.setVisible (riseChoice);
                if (riseChoice)
                {
                    compact (shepRiseBox, sh.removeFromRight (juce::jmin (needFor (shepRiseBox, 108), sh.getWidth())));
                    sh.removeFromRight (6);
                }
                const bool sync = shepModeBox.getSelectedId() == 2;
                shepTimeFader.setVisible (! sync);
                shepDivBox.setVisible (sync);
                // The beats box holds "1/16".."16": it never needed 86 px, and the room
                // it was hoarding is what pays for the rate mode's longer name.
                if (sync) compact (shepDivBox, sh.removeFromLeft (juce::jmin (needFor (shepDivBox, 60), sh.getWidth())));
                else      shepTimeFader.setBounds (sh);
            }
        }
    }

    // Floating "silent settings" explainer, centred over everything.
    warningOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (580, 200));
    warningOverlay.toFront (false);
    noticeOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (560, 220));
    recentOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (520, 380));
    if (recentOverlay.isVisible()) recentOverlay.toFront (false);
    rhythmScrim.setBounds (getLocalBounds());
    rhythmPanel.setBounds (getLocalBounds().withSizeKeepingCentre (820, 640));
    rhythmScrim.toFront (false);
    rhythmPanel.toFront (false);
}

void FractalAdditiveAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    // Clicking the title plays the jingle. It is the ONLY way it ever sounds - it
    // never plays on its own, so it can't surprise anyone mid-session.
    if (! titleClickArea.isEmpty() && titleClickArea.contains (e.getPosition()))
        processorRef.triggerJingle();
    grabKeyboardFocus();
}

// After switching to another plugin window (or app) and back, nothing in our
// hierarchy holds keyboard focus, so the computer keyboard can't play (and with no
// driving note the Spectrum-to-Rhythm pulses stop too). Reclaim focus when the host
// re-activates our window OR the mouse re-enters the plugin - unless the user is in a
// text field, whose typing must not be hijacked.
void FractalAdditiveAudioProcessorEditor::reclaimFocusUnlessTyping()
{
    if (! focusReclaimState().wouldGrab) return;   // see refocus() for what each test is for
    lastFocusReclaimMs = juce::Time::getMillisecondCounter();
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

// Which windows of ours count as "the player is here", for the QWERTY-as-MIDI mapping.
//
// The per-partial amplitude editor is a REAL top-level window, so opening it takes the
// keyboard focus off the editor and playing simply stopped - you could not hear the
// partial whose knob you were dragging, which is the one moment you most want to. Its
// own window is added here (and forwards its key events to keyPressed /
// keyStateChanged, see openPartialAmpWindow) so the same mapping keeps working there.
// isActiveWindow() and not hasKeyboardFocus(): nothing inside that window has to want
// focus for the window to be the one in front.
bool FractalAdditiveAudioProcessorEditor::playKeysLive() const
{
    if (hasKeyboardFocus (true)) return true;
    // BOTH floating windows count, or their key forwarding is a lie: the playground
    // forwards its key events exactly like the partial window (hearing the morph move
    // IS the feature), but while only partialWindow was listed here the poll decided
    // "the player left" 250 ms after the pad took focus and released every held note.
    const auto live = [] (const auto& w)
        { return w != nullptr && w->isVisible() && w->isActiveWindow(); };
    return live (partialWindow) || live (playgroundWindow);
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
    const bool focusedNow = playKeysLive() && ! typing;
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

    // The envelope curve follows its parameters live (automation, a modulator
    // driving them, the selector switching modulator, an undo...).
    if (envEditor.isVisible()) envEditor.refresh();

    refreshModWatch(); // live-modulation marker on every watched fader (see the header)
    refreshSlopeFreeze(); // SLOPE goes grey + "FROZEN" while manual amplitudes are on
    refreshShepardSyncState(); // beats box goes amber while Sync has no BPM to sync to
    // The amplitude window follows the spectrum: the template is rebuilt on a background
    // thread whenever Base/Fractal/primes/tuning move, and the stored edits can change
    // with no rebuild at all (an undo, a preset, a reload). Both are polled here rather
    // than by a second timer inside the window.
    if (partialWindow != nullptr && partialWindow->isVisible())
        partialPanel.syncIfNeeded();
    refreshPresetDirtyMark();
    // ...and WHOSE patch the controls are. Polled, not only pushed: the processor can end
    // a corner edit on its own (the playground disengaging, a corner cleared by
    // automation), and the "*" appears the moment any control moves.
    refreshCornerEditUi();
    // Keep the Spectrum-Builder box showing the typed spectrum that is actually
    // loaded - it can change under us (a preset, an undo, "load its original
    // spectrum"). Never while it has focus: that would overwrite what is being typed.
    {
        const auto src = processorRef.getTimbreName() == "typed" ? processorRef.getTimbreSource()
                                                                 : juce::String();
        if (src != lastSeenTimbreSource && ! intervalEdit.hasKeyboardFocus (false))
        {
            lastSeenTimbreSource = src;
            setIntervalText (src);
        }
    }
    refreshRefAText();       // keep the Reference-A box in sync with the parameter (automation)
    // While an MTS-ESP master is connected it OWNS the reference (see the processor's
    // timerCallback), which rewrites the parameter 60x a second - so anything typed
    // here was reverted within ~17 ms and silently lost. Show that it is not yours to
    // set right now instead of eating the value. Never while it has focus, or the
    // pending focus-lost commit would be dropped mid-edit.
    {
        const bool mtsOwnsRef = processorRef.getTuningMode() == (int) TuningManager::Mode::MtsEsp
                             && processorRef.tuningHasMaster();
        if (! refAEdit.hasKeyboardFocus (false) && refAEdit.isEnabled() == mtsOwnsRef)
        {
            refAEdit.setEnabled (! mtsOwnsRef);
            refAEdit.setAlpha (mtsOwnsRef ? 0.55f : 1.0f);
            refAEdit.setTooltip (mtsOwnsRef
                ? tr ("The connected *MTS-ESP master* is setting the reference right now, so this is what IT says.\nLeave MTS-ESP mode to set it by hand again - !your own value is kept! and comes straight back.",
                      "El *master MTS-ESP* connectat est\xc3\xa0 fixant la refer\xc3\xa8ncia ara mateix, aix\xc3\xb2 \xc3\xa9s el que diu ELL.\nSurt del mode MTS-ESP per posar-la a m\xc3\xa0 - !el teu valor es conserva! i torna tot seguit.")
                : tr ("Frequency of *C3* - transposes the whole tuning.",
                      "Freq\xc3\xbc\xc3\xa8ncia de *C3* - transposa tota l'afinaci\xc3\xb3."));
        }
    }

    // (Undo transaction grouping itself now lives in the PROCESSOR's timer, so host
    // automation with this window shut is still cut into ~1 s steps instead of piling
    // into one unbounded transaction - see FractalAdditiveAudioProcessor::timerCallback.)
    // This tick only keeps the tuning-bank combo in sync with auto-added entries.
    if (++undoTxnTick >= 30)
    {
        undoTxnTick = 0;
        int filled = 0;
        for (int i = 0; i < FractalAdditiveAudioProcessor::kBankSlots; ++i)
            if (processorRef.getTuningBankName (i).isNotEmpty()) ++filled;
        if (filled + 1 != tuningBankBox.getNumItems())
            refreshTuningBank();
    }

    // n-AD list: recompute when the selection or the scale/timbre changes; publish
    // results when the worker finishes (values coloured green -> red like the list).
    // THE GATE IS "IS THIS LIST ON SCREEN", NOT "WHICH SOURCE IS SELECTED".
    // It used to also require tuneSourceBox != 2 - i.e. it refused to recompute in the
    // FREE-SEARCH source. But that source is exactly where the "Tuning info" button
    // lives: load a chord from the search and the view flips to this same list, showing
    // the loaded chord's own degrees. With the recompute switched off there, the rows
    // left over from whatever scale was on screen before simply stayed - so a 5-note
    // chord loaded as the scale was captioned with 12-TET's answer: "en una pentatonica
    // me dice 7 y 11 como recomendaciones de acordes en ese tuning". nadView.isVisible()
    // is already exactly "one of the two views that show this list is up".
    if (nadView.isVisible() && specModeBox.getSelectedId() == 1)
    {
        // Live: any change to the tuning, the timbre or ANY of the four shared
        // controls recomputes on its own - "From current tuning" has no Find button.
        const bool   rootOnly = tuneScopeBox.getSelectedId() != 2;
        const bool   maxMode  = chordMinMaxBox.getSelectedId() == 2;
        const double minGap   = juce::jlimit (0.0, 600.0, chordMinEdit.getText().getDoubleValue());
        double periodRatio = 0.0; juce::String periodLine;
        if (! TuningManager::parsePeriodSpec (chordPeriodEdit.getText(), periodRatio, periodLine))
            periodRatio = 0.0;   // unreadable: the tuning's own period is used
        const auto opts = juce::String ((int) maxMode) + "|" + juce::String (periodRatio, 6)
                        + "|" + juce::String (minGap, 3);
        // Trigger off the TUNING's own shape, not off lastScaleSig. lastScaleSig is now
        // rate-limited (it gates the roughness sweep, which a modulated spectrum would
        // otherwise drive at timer rate), and hanging this on it meant a tuning change
        // could go unnoticed: the worker never republished, so the list AND the
        // equal-step flag below stayed frozen on whatever scale was loaded before -
        // showing 31-EDO's degrees, greyed as if every shape were valid from every
        // degree, while an unequal scale was actually playing.
        const int shapeD = processorRef.getScaleDegrees();
        double shapeSig = (double) shapeD * 7.0 + processorRef.getTuningMode() * 3.0;
        // The period is degree D itself, so it is covered by the loop below reaching it.
        for (int d = 1, n = juce::jmin (shapeD, 24); d <= n; ++d)
            shapeSig += processorRef.getDegreeRatio (d) * (double) (d + 3);
        // The TIMBRE is part of the signature too: the list ranks combinations by the
        // Sethares beating of the CURRENT spectrum, so a spectrum edit (a knob, a
        // per-partial amplitude edit - which never bumps the template generation, hence
        // the separate gains revision) must re-rank it, or it keeps showing the previous
        // timbre's numbers next to a Degrees list that has already moved on. A modulated
        // spectrum changes the generation on essentially every tick, so spectrum-driven
        // recomputes are rate-limited to ~1 Hz (the worker joins its predecessor on
        // every restart; timer-rate restarts would put a heavy cancel/join cycle on the
        // message thread). Tuning/selector changes stay immediate.
        double specSig = (double) processorRef.getTemplateGeneration() * 101.0
                       + (double) processorRef.getPartialGainsRevision() * 19.0;
        {
            const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
            if (specSig != lastNadSpecSig && nowMs - lastNadSpecMs >= 1000)
            {
                lastNadSpecMs  = nowMs;
                lastNadSpecSig = specSig;
            }
            specSig = lastNadSpecSig; // between refreshes the held value keeps the sig stable
        }
        shapeSig += specSig;

        if (std::abs (shapeSig - lastNadSig) > 1.0e-9 || chordNotesBox.getSelectedId() != lastNadSel
            || rootOnly != lastNadRootOnly || opts != lastNadOpts)
        {
            lastNadSig = shapeSig;
            lastNadSel = juce::jmax (2, chordNotesBox.getSelectedId());
            lastNadRootOnly = rootOnly;
            lastNadOpts = opts;
            processorRef.startNadCompute (lastNadSel, rootOnly, maxMode, periodRatio, minGap);
        }
        if (processorRef.getNadGeneration() != lastNadGen)
        {
            lastNadGen = processorRef.getNadGeneration();
            // A row's chips are degree INDICES into the scale the worker ran over. If the
            // tuning has moved on since, those indices name degrees the scale in force
            // does not have, and printing them is the whole complaint. Belt and braces
            // behind the gate above: the list shows nothing rather than something wrong,
            // and the pending recompute fills it a tick later.
            const bool sameScale = processorRef.getNadResultDegrees() == processorRef.getScaleDegrees();
            const auto res = sameScale ? processorRef.getNadResults()
                                       : std::vector<FractalAdditiveAudioProcessor::NadRow>();
            double lo = 1.0e30, hi = -1.0e30;
            for (const auto& r : res) { lo = juce::jmin (lo, r.value); hi = juce::jmax (hi, r.value); }
            // In an equal-step tuning every shape is valid from every degree, so the
            // list drops the key colours and shows offsets from "any".
            nadList.equalStep = processorRef.tuningIsEqualStep();
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
                // Keep previewNotes in step with what is REALLY sounding, in both
                // directions. It used to be wiped wholesale once the queue drained -
                // but a hover preview's last event per voice is a note-ON (it sustains
                // while you hover), so that wipe threw away the only record of notes
                // that were still gated on. stopChordPreview() then early-returned on
                // an empty list, which made mouse-exit, moving to another row AND the
                // editor's own destructor all no-ops: every hovered chord droned for
                // ever and they piled up. THE stuck-note bug.
                const std::pair<int, int> k { m.getChannel(), m.getNoteNumber() };
                if (m.isNoteOff())
                    previewNotes.erase (std::remove (previewNotes.begin(), previewNotes.end(), k),
                                        previewNotes.end());
                else if (m.isNoteOn()
                         && std::find (previewNotes.begin(), previewNotes.end(), k) == previewNotes.end())
                    previewNotes.push_back (k);
                previewQueue.erase (previewQueue.begin() + (long) i);
            }
            else ++i;
        }
    }

    // The two REALTIME buttons: their lit state, and the refusal notice when a link
    // switched itself off rather than close a feedback loop.
    paintRealtimeButtons();

    // MONOPHONIC GLIDE stands the note gate's own "Mono" down (see the processor's
    // noteGate.process call for why: the gate cuts the sounding note BEFORE the new one,
    // which turned every takeover back into a restrike and killed the glide silently).
    // The toggle greys out so the panel is not showing a lit switch that is not driving
    // anything; its tooltip carries the reason permanently.
    {
        const bool mg = processorRef.apvts.getRawParameterValue ("monoGlide")->load() > 0.5f;
        if (rhythmPanel.monoT.isEnabled() == mg)            // only on a real change
        {
            rhythmPanel.monoT.setEnabled (! mg);
            rhythmPanel.monoT.setAlpha (mg ? 0.45f : 1.0f);
        }
    }

    // Spectrum adaptation (mode 2): progress, the adapted partial list, and the honest
    // before/after read-out.
    if (specModeBox.getSelectedId() == 2)
    {
        const bool rt = processorRef.getRealtimeLink() != FractalAdditiveAudioProcessor::RtLink::Off;
        // The range hangs off the note being played, so the line that names the anchor has
        // to follow the hands - and the follower has to be told when it moves.
        syncAdaptRegisterSpan();
        // The button carries the price of the next press, and becomes Stop while a run is
        // going; the read-out under it says which of those is true right now.
        refreshAdaptButtonText();
        adaptInfoLbl.setText (adaptReadoutText(), juce::dontSendNotification);
        // The TOOLTIP at 4 Hz, not 30. It carries the raw numbers and the cost estimate,
        // so it has to follow the controls - but rebuilding a paragraph thirty times a
        // second to hand it to a component nobody is hovering is pure allocation, and this
        // timer already runs the whole window.
        if (const auto now = juce::Time::getMillisecondCounter(); now - lastAdaptTipMs > 250)
        {
            lastAdaptTipMs = now;
            adaptInfoLbl.setTooltip (adaptReadoutTip());
        }

        if (processorRef.getSpectrumAdaptGeneration() != lastAdaptGen)
        {
            lastAdaptGen = processorRef.getSpectrumAdaptGeneration();
            // A HAND-PRESSED Adapt is applied HERE, as one undo step. (A realtime link
            // applies its own results inside the processor's timer, which is what keeps it
            // working with this window shut - so this must not apply them a second time.)
            if (! rt)
            {
                juce::String aerr;
                if (! processorRef.applySpectrumAdapt (true, aerr) && aerr.isNotEmpty())
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        tr ("Adapt spectrum to tuning", "Adapta l'espectre a l'afinaci\xc3\xb3"), aerr, this);
            }
            const auto info = processorRef.getSpectrumAdaptResult();
            double aMax = 0.0;
            for (const double a : info.amps) aMax = juce::jmax (aMax, a);
            adaptList.rows.clear();
            for (size_t i = 0; i < info.ratios.size(); ++i)
            {
                AdaptList::Row row;
                // Source-honest: an adapted ratio is a SEARCHED value, so it only claims a
                // fraction when it really is one (justName's own rule), else cents.
                row.name  = justName (info.ratios[i]);
                row.level = aMax > 0.0 ? (float) (info.amps[i] / aMax) : 1.0f;
                adaptList.rows.push_back (std::move (row));
            }
            // "before" is the harmonic seed's beating, "after" the adapted spectrum's, both
            // on the search's own model; "now" is measured on the partials the synth is
            // REALLY producing. When the third number differs from the second, the patch has
            // something the model does not carry (Reps stacking the period, or a fractal
            // level self-convolving it) - and it says so rather than letting the difference
            // pass for the adaptation not working.
            const auto s = realtimeSettingsFromPanel();
            double f0 = 0.0;
            const auto alphas = adaptTargetBox.getSelectedId() == 2
                                    ? processorRef.heldChordAsTargetAlphas (f0)
                                    : processorRef.tuningAsTargetAlphas (f0);
            // The union of many notes at 64 analysis partials each is millions of pairs -
            // fine on a worker thread, not on this one. Above a chord's worth of notes the
            // "all vs all" measurement is left out rather than freezing the window for it.
            //
            // `alphas` is re-read HERE, after the search finished, so the notes may already
            // be released - and measureCurrentAgainstTarget answers 0.0 for an empty target.
            // Printed, that 0.0 read as "sounding now: 0.0", i.e. the patch has become
            // perfectly smooth, which is a lie about a measurement that was never taken.
            const bool measurable = ! alphas.empty() && (s.adapt.rootOnly || alphas.size() <= 8);
            // Measured over the SAME registers the finished run used - taken from the
            // result itself, not re-derived from the panel, because the user may have
            // dragged the range while the search was running and "sounding now" must sit
            // on the scale of the number printed beside it.
            dissonance::Registers doneRegs;
            doneRegs.f0 = info.registerF0;
            const double now = measurable
                                   ? processorRef.measureCurrentAgainstTarget (alphas, f0, s.adapt.rootOnly,
                                                                               doneRegs)
                                   : 0.0;
            // ONE SENTENCE, no raw Sethares numbers. "avg C2-C4: 769.9 -> 27.8 (96% less)"
            // is four quantities and a scale nobody was told: 769.9 of what? The only part
            // of it a musician can act on is the PERCENT, and the periods it was measured
            // over. Everything else moved into the tooltip below.
            //
            // The periods come from the RESULT's own registers, not from the fields: the
            // user may have retyped the range while the search was running, and a line that
            // claimed periods 1-7 for a run made over 2-4 would be a plain lie.
            const int D = registerDegreesPerPeriod();
            const juce::String span = juce::String (ScaleKeyboard::periodNumberForNote (
                                          nearestKeyForHz (info.regLoHz), D))
                                    + juce::String::fromUTF8 ("\xe2\x80\x93")
                                    + juce::String (ScaleKeyboard::periodNumberForNote (
                                          nearestKeyForHz (info.regHiHz), D));
            // The one caveat that changes what you HEAR: Reps stacks the whole adapted set
            // again one period up and a fractal level self-convolves it, and the search
            // saw neither. It used to be a bare second number ("sounding now: 31.4") that
            // meant nothing without the first, and it was the line that always clipped.
            const int   reps = (int) processorRef.apvts.getRawParameterValue ("timbreRepeats")->load();
            const float frac = processorRef.apvts.getRawParameterValue ("fractalLevel")->load();
            const bool  drift = measurable && info.after > 1.0e-12
                             && std::abs (now - info.after) > 0.05 * info.after;
            const int   pct = info.before > 1.0e-12
                                  ? (int) std::lround ((1.0 - info.after / info.before) * 100.0) : 0;
            adaptResultLine = adaptLineResult (info.before > 1.0e-12 ? std::abs (pct) : -1,
                                               pct >= 0, span,
                                               drift && (reps > 1 || frac > 1.001f));
            // ...and every number that used to be on screen, kept where it can be read on
            // demand instead of competing for a 192 px line.
            adaptResultTip = tr ("Last run: ", "\xc3\x9altima cerca: ")
                           + tr ("average", "mitjana")
                           + " " + span + ", "
                           + tr ("beating ", "batec ") + juce::String (info.before, 1)
                           + " -> " + juce::String (info.after, 1)
                           + tr (" (Sethares roughness; lower = smoother).",
                                 " (aspror de Sethares; m\xc3\xa9s baix = m\xc3\xa9s suau).")
                           + (drift ? tr (" Really sounding now: ", " Sonant ara de veritat: ")
                                      + juce::String (now, 1) + "."
                                    : juce::String());
            adaptInfoLbl.setText (adaptReadoutText(), juce::dontSendNotification);
            adaptInfoLbl.setTooltip (adaptReadoutTip());
            resized();
            adaptList.repaint();
        }
    }

    // Chord search: progress read-out while running; rebuild the list on completion.
    if (specModeBox.getSelectedId() == 1)
    {
        chordProgLbl.setText (processorRef.isChordSearchRunning()
                                  ? tr ("Searching... ", "Cercant... ")
                                    + juce::String ((int) std::lround (processorRef.getChordSearchProgress() * 100.0f)) + "%"
                                  : chordRtOn ? tr ("realtime: ", "temps real: ")
                                                + juce::String (processorRef.getRealtimeApplies())
                                                + tr (" retunes", " reafinacions")
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

            // Say WHY a search came back empty or short instead of just blanking the
            // list: with n notes the minimum interval needs n gaps inside the period,
            // so a large setting can make the request impossible.
            const int req = processorRef.getChordRequested(), got = processorRef.getChordFound();
            if (req > 0 && got < req)
            {
                const juce::String gap ((int) std::lround (processorRef.getChordMinGap()));
                noticeOverlay.title = got == 0
                    ? tr ("No chord fits that smallest interval", "Cap acord no cap amb aquest interval m\xc3\xadnim")
                    : tr ("Fewer chords than requested", "Menys acords dels demanats");
                noticeOverlay.body = got == 0
                    ? tr ("Every note pair must be at least ", "Cada parella de notes ha d'estar a ") + gap
                      + tr (" cents apart, and that many gaps do not fit inside the period. Lower 'smallest interval', use fewer notes, or widen the period.",
                            " cents com a m\xc3\xadnim, i tants espais no caben dins el per\xc3\xado" "de. Abaixa 'interval m\xc3\xa9s petit', posa menys notes o eixampla el per\xc3\xado" "de.")
                    : tr ("Only ", "Nom\xc3\xa9s ") + juce::String (got) + tr (" of the ", " dels ") + juce::String (req)
                      + tr (" chords you asked for satisfy a smallest interval of ", " acords demanats compleixen l'interval m\xc3\xadnim de ")
                      + gap + tr (" cents.", " cents.");
                noticeOverlay.setVisible (true);
                noticeOverlay.toFront (false);
                noticeOverlay.repaint();
            }
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
    // (DAW-tempo sync itself now lives in the PROCESSOR's timer, so it keeps working
    // with this window closed - see FractalAdditiveAudioProcessor::timerCallback.)
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
        // Empty template = the Harmonics + prime rules (intersected) left nothing.
        meter = tr ("0 partials \xe2\x80\x94 the Harmonics / Prime filters leave nothing to sound",
                    "0 parcials \xe2\x80\x94 els filtres d'Harm\xc3\xb2nics / de primers no deixen sonar res");
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

    // Say what is loaded even while the engine is OFF - switching it off no longer
    // discards the scale, so the box must not look empty. And say when the scale is ON
    // but BYPASSED: in temper mode this line used to read "Custom: <name>" in the box's
    // own bright pink while the spectrum came from the harmonic engine, which is the one
    // place the UI outright claimed the wrong engine was sounding.
    using E = FractalAdditiveAudioProcessor::EngineInForce;
    const auto est = processorRef.engineInForce();
    juce::String tms;
    // The panel's own colour, not a fixed pink: with ONE box in ONE engine colour this
    // line is the only thing left inside it that used to hold a hue of its own. The
    // BYPASSED case keeps the amber below, because that is a warning, not a compartment.
    juce::Colour tmsCol = spectrumPanelColour;
    if (est == E::Tempered && processorRef.isTimbreActive())
    {
        tms = tr ("Bypassed - ", "De banda - ") + processorRef.getTimbreName()
            + tr (" (TEMPERED)", " (TEMPERAT)");
        tmsCol = cOverride;
    }
    else if (processorRef.isTimbreActive())
        tms = "Custom: " + processorRef.getTimbreName() + "  (" + juce::String (processorRef.getTimbreNoteCount())
            + tr (" notes/period)", " notes/per\xc3\xadode)");
    else if (processorRef.timbreLoaded())
        tms = tr ("Off - ", "Apagat - ") + processorRef.getTimbreName()
            + tr (" still loaded", " segueix carregat");
    else
        tms = tr ("Custom engine off", "Motor propi apagat");
    timbreStatusLabel.setText (tms, juce::dontSendNotification);
    timbreStatusLabel.setColour (juce::Label::textColourId, tmsCol);

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
    // away from plain 12-TET, the Custom-Engine On/Off only when a scale is loaded. Relayout
    // so the Load button expands to fill the freed space. The tuning MODE is packed
    // in too: Recent/Tuning-bank/Pitch-bend visibility depends on it.
    const bool resetVis = processorRef.getTuningMode() != 0 || processorRef.tuningHasScala();
    const bool offVis   = processorRef.timbreLoaded();
    // The table-period field and its reset only exist in table mode, and the reset only
    // once the user has typed a period - both are laid out in resized(), so both belong in
    // this signature or the field appears only after some unrelated relayout.
    // ...and the spectrum-provenance button's two states: whether it exists at all
    // (a row appears/disappears in the TUNING stack, so it must relayout) and whether
    // its tuning is the active one (word + enabled flip without any other bit moving -
    // e.g. the user loads a different scale while an adapted spectrum sits loaded).
    const int  packed   = (resetVis ? 1 : 0) | (offVis ? 2 : 0) | (processorRef.getTuningMode() << 2)
                        | (processorRef.tuningIsAbsoluteTable()      ? 1 << 4 : 0)
                        | (processorRef.tuningTablePeriodIsUserSet() ? 1 << 5 : 0)
                        | (processorRef.hasSpectrumProvenance()      ? 1 << 6 : 0)
                        | (processorRef.spectrumProvenanceIsActive() ? 1 << 7 : 0)
                        // ...and whether the Shepard's rise-interval box is a real
                        // choice at all. It exists only while the active tuning's
                        // period is not an octave, and resized() is what puts it on
                        // screen - so it belongs HERE or the box would appear only at
                        // the next unrelated relayout after a Bohlen-Pierce load.
                        | (shepard::periodDiffersFromOctave (
                               processorRef.getTuningPeriodRatio())  ? 1 << 8 : 0);
    if (packed != lastResetOffState) { lastResetOffState = packed; resized(); }
    refreshSpectrumTuningButton(); // cheap (sig-guarded): keeps word/enabled honest per tick

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
    if (processorRef.tuningIsAbsoluteTable())
    {
        // An absolute table declares no period, so the readout must not claim one: it says
        // whose period this is (Fibló's estimate, or the user's) and how far the file
        // strays from it. Silence about the deviation would make a guess look measured.
        const bool user = processorRef.tuningTablePeriodIsUserSet();
        juce::String t = tr ("Period  ", "Per\xc3\xado" "de  ");
        t << (user ? tr ("yours", "el teu")
                   : (processorRef.tuningTablePeriodWasFound() ? tr ("estimated", "estimat")
                                                              : tr ("none found", "cap trobat")))
          << "  \xc2\xb1" << juce::String (processorRef.tuningTablePeriodDevCents(), 1)
          << juce::String::fromUTF8 ("\xc2\xa2") << "  ("
          << juce::String (processorRef.getScaleDegrees()) << tr (" keys)", " tecles)");
        tuningPeriodLbl.setText (t, juce::dontSendNotification);
        // Never fight the user for the field while they are typing in it.
        if (! tablePeriodEdit.hasKeyboardFocus (true))
            tablePeriodEdit.setText (processorRef.tuningTablePeriodSpec(), juce::dontSendNotification);
    }
    else
    {
        const int D = juce::jmax (1, processorRef.getScaleDegrees());
        auto per = degreeLabel (processorRef, D);
        if (per.endsWith ("/1")) per = per.dropLastCharacters (2); // "2/1" reads better as "2"
        tuningPeriodLbl.setText (tr ("Period  ", "Per\xc3\xado" "de  ") + per, juce::dontSendNotification);
    }

    // The tuning-mode box is wired by hand (12ED2 is not in the list), so it has to be
    // pulled back into step with the parameter here - after a preset load, host
    // automation, or an undo. A project saved on 12ED2 restores the mode faithfully and
    // simply shows nothing selected, which is honest: it is not one of the choices.
    {
        const int want = processorRef.getTuningMode() + 1;
        if (tuningBox.getSelectedId() != want)
            tuningBox.setSelectedId (want > 1 ? want : 0, juce::dontSendNotification);
    }

    // Which engine is in force can change from anywhere - host automation, an undo, a
    // preset, a .scl load - so the panel re-dresses itself from the processor here rather
    // than trusting whoever caused it. The overridden box is DIMMED, never reset: forcing
    // the harmonic controls back to their defaults (as this once did) silently destroyed
    // the user's settings the moment a .scl was loaded, and made the sound depend on the
    // editor being open.
    if (lastEngineShown != (int) processorRef.engineInForce() * 2
                           + (processorRef.timbreLoaded() ? 1 : 0))
    {
        // applyEngineState() re-dresses the Load/Remove button itself: "is a spectrum
        // loaded" is half of the very state this comparison is watching, so the button's
        // word arrives with the rest of the panel instead of one tick behind it.
        applyEngineState();
    }
}
