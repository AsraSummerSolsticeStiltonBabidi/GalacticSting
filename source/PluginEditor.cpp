#include "PluginEditor.h"
#include "BinaryData.h"
#include "FloatingWindowOwner.h" // keeps the floating windows above the editor (Win32 ownership)
#include "SnapshotRig.h"         // the editor photographs itself when asked to (--snapshot)
#include "GalacticPalette.h"
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

    // THE TITLE'S OWN GEOMETRY, asked once and answered for both callers. paint() draws
    // the squares from it and resized() centres the credit line under it. Two hand-copies
    // of these six lines is exactly how the credit drifts off the title the first time
    // the free zone moves - the class of mistake the whole of fgfit exists to prevent.
    inline juce::String titleWord()
    { return juce::String::fromUTF8 ("Fibl\xc3\xb3 Gal\xc3\xa0" "ctic"); }

    struct TitleBand
    {
        float side, total, x;                            // square edge, row width, left edge
        float centreX() const { return x + total * 0.5f; }
        float bottom()  const { return 8.0f + side; }    // cy + side/2, with cy = 8 + side/2
    };

    inline TitleBand titleBandFor (int windowWidth)
    {
        const auto title = titleWord();
        int squaresN = 0, spaces = 0;
        for (int i = 0; i < title.length(); ++i) (title[i] == ' ' ? spaces : squaresN)++;
        TitleBand t;
        t.side = 52.0f;
        const float gap = 4.0f;
        t.total = (float) squaresN * (t.side + gap) + (float) spaces * (t.side * 0.5f);
        const float freeLeft  = 214.0f;                       // clears Playground/Menu/Presets/Save
        const float freeRight = (float) windowWidth - 186.0f; // clears the Spectrum-to-Rhythm button
        const float freeW     = juce::jmax (120.0f, freeRight - freeLeft);
        if (t.total > freeW) { t.side *= freeW / t.total; t.total = freeW; }
        t.x = juce::jmax (((float) windowWidth - t.total) * 0.5f, freeLeft);
        if (t.x + t.total > freeRight) t.x = freeRight - t.total;
        return t;
    }

    // The microtonal accidental face (notes.otf by Sintel, from tessera - MIT; see
    // assets/fonts/NOTATION-LICENSES.txt). Loaded once, like the title face.
    //
    // It is not a decoration. In this font the accidentals ride on ordinary ASCII
    // letters, so the ups-and-downs name for C sharp is the two characters "Cc" and any
    // other typeface draws it as the literal letters C and c - a WRONG NOTE NAME, not a
    // missing-glyph box, and therefore invisible to every check that only inspects
    // rectangles. If this ever fails to load, the keyboard keeps its degree numbers.
    const juce::Typeface::Ptr& notesTypeface()
    {
        static juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (
            BinaryData::notes_otf, (size_t) BinaryData::notes_otfSize);
        return tf;
    }

    // SECTION COLOURS - every one of them from galactic::palette (source/GalacticPalette.h),
    // which is where they are DECIDED and where a test can read them.
    //
    // They used to be hand-picked hues right here, in an anonymous namespace nothing
    // could measure, "deliberately spread around the hue wheel" by eye. Spread by eye is
    // how the TUNING panel (0xb27bff) and the CUSTOM modulator tab (0xff9a7bff) ended up
    // 3.8 units of OKLab apart - two panels no one can tell apart, with every check in
    // the build green - and how the least legible of them sat at 6.2:1 against the page.
    //
    // The set is now the answer to a maximin problem (tools/PaletteSearch.cpp): the
    // SMALLEST perceptual distance between any two of them is as large as it can be
    // while each stays legible, in gamut, and a real colour rather than a near-white.
    // The worst pair is 15.2 apart instead of 3.8, and nothing here is a free choice any
    // more - to change one, change it in the palette and re-run the search.
    const juce::Colour cHarmonic  { 0xff000000u | galactic::palette::kHarmonic };
    const juce::Colour cSlope     { 0xff000000u | galactic::palette::kSlope };
    const juce::Colour cFractal   { 0xff000000u | galactic::palette::kFractal };
    const juce::Colour cEssentials{ 0xff000000u | galactic::palette::kEssentials };
    const juce::Colour cFilter    { 0xff000000u | galactic::palette::kFilter };
    const juce::Colour cEnv       { 0xff000000u | galactic::palette::kEnv };
    const juce::Colour cPitch     { 0xff000000u | galactic::palette::kPitch };
    const juce::Colour cTuning    { 0xff000000u | galactic::palette::kTuning };
    const juce::Colour cTimbre    { 0xff000000u | galactic::palette::kTimbre };
    const juce::Colour cCustomMod { 0xff000000u | galactic::palette::kCustomMod };
    const juce::Colour cScaleDiss { 0xff000000u | galactic::palette::kScaleDiss };
    // ...and the one that is NOT chosen: Spectrum-vs-Tuning is the MIXTURE of the two
    // panels it sits between, computed from them (rule (a) - see GalacticPalette.h). It
    // was a hard-coded orange, which said nothing about what that column is for; now it
    // cannot stop being a mixture, because there is no hex to edit.
    const juce::Colour cSpecScale { 0xff000000u | galactic::palette::specScale() };
    // SEMANTIC colours, deliberately outside the separation rule: these mean go, stop
    // and "something else is driving this", and a colour chosen to win a distance
    // contest would stop meaning it.
    const juce::Colour cOverride  { 0xffffb020 }; // amber - "this is being overridden" (as FROZEN)
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

    // ONE DECIMAL POLICY FOR EVERY FADER READ-OUT.
    //
    // The SPECTRUM panel alone showed four different precisions at once - "1.000" (amp
    // decay), "1.00" (fractal), "0.3000" (transition time), "0.000" (phase) - with "0.00"
    // (gain) in ESSENTIALS beside it and "0.500" (a modulator Range) in MODULATORS. Six
    // ways of writing the same kind of number in one window, and not one of them was a
    // decision: juce::AudioParameterFloat derives its decimals from the parameter's own
    // INTERVAL (0.001 -> three, 0.0001 -> four), so the read-out was reporting the
    // automation grid rather than the value, and two faders that happen to step
    // differently printed differently for no reason a user can see.
    //
    // Capped, never extended: a fader whose step already gives it fewer digits keeps them
    // (the glide time stays "120.0 ms", shepTime stays whole milliseconds), so this adds a
    // ceiling and never a trailing zero. What is left on screen is two policies - whole
    // numbers where the step is whole, and at most two decimals everywhere else.
    //
    // It is a DISPLAY cap and nothing else: the parameter, the automation lane, the
    // modulation and the saved state are all untouched, and the fader still moves in its
    // own steps. (The one place two decimals would have been a lie is the REFERENCE field
    // in ESSENTIALS, which is a text box the user types into and which is READ BACK on
    // focus loss - rounding 261.6255653 to 261.626 there would retune the instrument by
    // 0.0006 Hz the next time the field lost focus. It is deliberately not touched here;
    // it is not a fader read-out.)
    inline constexpr int kFaderMaxDecimals = 2;

    // Re-render a bare decimal number with at most `dp` decimals. Anything that is NOT a
    // bare number - a note name, "Even-All 50%", "7th harmonic", a unit word - is somebody
    // else's wording and comes back untouched, so this can never mangle a fader that
    // writes its own text.
    juce::String capDecimals (const juce::String& text, int dp)
    {
        const auto s = text.trim();
        if (s.isEmpty()) return text;
        int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        if (i >= s.length()) return text;
        int dot = -1;
        for (int k = i; k < s.length(); ++k)
        {
            const auto c = s[k];
            if (c == '.') { if (dot >= 0) return text; dot = k; }
            else if (! juce::CharacterFunctions::isDigit (c)) return text;
        }
        if (dot < 0 || s.length() - dot - 1 <= dp) return text;   // already short enough
        return juce::String (s.getDoubleValue(), dp);             // rounds, never truncates
    }

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

        // fgfit (see PluginEditor.h) is the ONE place these numbers live: this painter,
        // the resized() that sizes these buttons to their words, and the text sweep that
        // checks them all read from it. A font stated twice is a truncation waiting for
        // a translation - it was, in the tuning row.
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        { return fgfit::textButtonFont (buttonHeight); }

        // The CLOSED box's read-out. Only a small-item box (see fgfit::kSmallComboName)
        // overrides anything: every other combo keeps LookAndFeel_V4's font exactly, so
        // this cannot move a single control that did not ask for it. Routed through fgfit
        // so the text sweep, which measures whatever getComboBoxFont returns, measures the
        // face the item is really drawn in. The OPEN menu is untouched - a mode you are
        // choosing from a list wants to be read easily, and only the panel needs the
        // chosen name to sit under its title.
        juce::Font getComboBoxFont (juce::ComboBox& box) override
        {
            if (box.getName() == fgfit::kSmallComboName)
                return fgfit::comboItemFont (box, box.getHeight());
            return juce::LookAndFeel_V4::getComboBoxFont (box);
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

            // A STEPPER'S + AND - ARE DRAWN, NOT TYPED.
            //
            // As glyphs they are not a pair at all: in this face the hyphen is a ~5 px bar
            // about 1 px thick sitting on the x-height, while the plus is a ~10 px cross of
            // two ~2 px strokes on the maths axis. Side by side in two 20 px circles - the
            // OCTAVE pair in ESSENTIALS, OCTAVE RANGE in SPECTRUM, and the rhythm panel's
            // PULSE RATE and Octave (all) - the minus reads as optically half the weight of
            // the plus and 5 px higher, so one half of a symmetrical control looks disabled.
            // No font choice fixes that; the two characters are simply not designed as a
            // set. Two rectangles are: same length, same pen, same centre, and the plus is
            // the minus with one more bar across it.
            //
            // Sized from the circle GalacticLnF::drawButtonBackground really fills (the
            // SHORTER side of the button), so a stepper laid out in a rect that is not
            // square still gets a glyph centred in its own circle rather than in the rect.
            if (b.getName() == "round")
            {
                const auto t = b.getButtonText();
                if (t == "+" || t == "-")
                {
                    const auto r  = b.getLocalBounds().toFloat();
                    const float d = juce::jmin (r.getWidth(), r.getHeight());
                    const float arm = std::round (d * 0.46f);                    // bar length
                    const float pen = juce::jmax (1.6f, std::round (d * 0.14f)); // bar weight
                    const float cx = r.getCentreX(), cy = r.getCentreY();
                    g.fillRect (cx - arm * 0.5f, cy - pen * 0.5f, arm, pen);
                    if (t == "+") g.fillRect (cx - pen * 0.5f, cy - arm * 0.5f, pen, arm);
                    return;
                }
            }

            auto area = b.getLocalBounds().reduced (4, 0);
            // A button flagged "fork" gets a real tuning fork drawn at its left edge.
            // There is no tuning-fork character in any font we can rely on - the nearest
            // candidates (Psi, the pitchfork sign) all have THREE prongs, which reads as a
            // trident, not a fork. So it is drawn: two prongs, a U-bend and a stem.
            if (b.getProperties().contains ("fork"))
            {
                const float fh = juce::jmin (13.0f, (float) b.getHeight() - 4.0f);
                // (the width taken here is textButtonForkWidth minus the 2 px gap below)
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
            // EVERY OTHER TICK BOX IS THE SAME BOX, WITH THE SAME GAP TO ITS WORD.
            //
            // LookAndFeel_V4 derives the box from the BUTTON'S HEIGHT
            // (jmin (15, h * 0.75) * 1.1) and starts the text at roundToInt (that) + 10,
            // so a 215 x 106 block of one PRIME CONTROL panel drew three different tick
            // boxes - 15x15 for the nine prime toggles, 18x18 for "Temper to tuning", and
            // 18x16 for "On" (not even square, because the rounding of a fractional height
            // lands the outline between pixels) - with 6 px after one word and 9 after
            // another. Nothing was wrong with any single control; they simply had three
            // different heights, and V4 turned that into three different SIZES.
            //
            // A tick box is not a scaled part of its row: it is a fixed 16 px square glyph
            // that says on or off, and the word beside it always sits the same distance
            // away. fgfit::kTickBox / kTickGap / kTickX are those three numbers, and
            // fgfit::toggleRoom / toggleWidth are computed FROM them - so the sweep that
            // checks a toggle's word fits and the resized() that sizes it to its word both
            // measure the geometry this function really draws.
            //
            // ...AND A DISABLED ONE LOOKS DISABLED. drawTickBox is handed an isEnabled flag
            // and uses it only for the BORDER - the tick itself is filled in full
            // tickColourId whatever the state - so the nine prime boxes sat at full olive
            // saturation under a greyed "Whitelist" the moment that panel's switch went
            // off, which is the one state where they are not acting on the sound at all.
            // Caught by reading the rig's own photographs; no gate could see it, because
            // nothing overflowed and nothing was clipped - it was simply the wrong colour.
            //
            // A transparency layer rather than a hand-copy of the tick path: it costs one
            // small offscreen buffer per disabled box per repaint (these are 16x16) and it
            // covers the box, the word and anything added here later. 0.45 is the same
            // order as the 0.5 the text already used on its own.
            const auto paintToggle = [&]
            {
                const float box = (float) fgfit::kTickBox;
                drawTickBox (g, b, (float) fgfit::kTickX, ((float) b.getHeight() - box) * 0.5f,
                             box, box, b.getToggleState(), b.isEnabled(), over, down);
                g.setColour (b.findColour (juce::ToggleButton::textColourId));
                g.setFont (fgfit::toggleFont (b.getHeight()));
                g.drawFittedText (b.getButtonText(),
                                  b.getLocalBounds()
                                      .withTrimmedLeft (fgfit::kTickX + fgfit::kTickBox + fgfit::kTickGap)
                                      .withTrimmedRight (2),
                                  juce::Justification::centredLeft, 10);
            };
            if (! b.isEnabled())
            {
                g.beginTransparencyLayer (0.45f);
                paintToggle();
                g.endTransparencyLayer();
            }
            else paintToggle();
        }

        // A SCROLLBAR THAT BELONGS TO THIS WINDOW.
        //
        // There was no drawScrollbar here at all, so the five result lists inside the
        // salmon Spectrum-vs-Tuning column - and the per-partial window's grid - drew
        // LookAndFeel_V4's default thumb, which is its colour scheme's `highlightedFill`:
        // a #42a2c8 cyan that appears nowhere else in this product's chrome. On the zebra
        // rows of a salmon panel it read as an unstyled system control that had escaped
        // the theme, and it had no track at all, so it floated on the stripes with nothing
        // to say how far the list ran.
        //
        // Neutral on purpose, and not one more identity colour: this is a mechanism, not a
        // section. It is the same 0x14ffffff wash the zebra rows themselves are drawn in,
        // one step up for the thumb, so the bar reads as part of the LIST rather than as a
        // thing sitting on top of it - and it stays out of the way of every panel hue it
        // has to appear in, which is what stopped the cyan from working.
        void drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int w, int h,
                            bool vertical, int thumbStart, int thumbSize,
                            bool over, bool down) override
        {
            juce::ignoreUnused (bar);
            const auto full = juce::Rectangle<int> (x, y, w, h).toFloat();
            const float rad = (vertical ? full.getWidth() : full.getHeight()) * 0.5f;
            // The track: the whole travel, so the bar says how much list there is even
            // when the thumb is short.
            g.setColour (juce::Colour (0x14ffffff));
            g.fillRoundedRectangle (full.reduced (vertical ? 2.0f : 0.0f, vertical ? 0.0f : 2.0f), rad);
            if (thumbSize <= 0) return;
            auto thumb = vertical ? juce::Rectangle<float> (full.getX(), (float) thumbStart,
                                                            full.getWidth(), (float) thumbSize)
                                  : juce::Rectangle<float> ((float) thumbStart, full.getY(),
                                                            (float) thumbSize, full.getHeight());
            thumb = thumb.reduced (vertical ? 1.5f : 1.0f, vertical ? 1.0f : 1.5f);
            g.setColour (juce::Colour (down ? 0xffc4ccda : (over ? 0xffa2a8b8 : 0xff70768a)));
            g.fillRoundedRectangle (thumb, juce::jmin (rad, thumb.getWidth() * 0.5f,
                                                       thumb.getHeight() * 0.5f));
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
        //
        // ...AND THREE RULES THAT NEED NO MARKUP AT ALL, because the strings already
        // follow them and nothing was reading them. "QUE TENGAN FORMATO DE VERDAD
        // (ESPACIADO, COLOR, NEGRITA, SUBRAYADO) EN VEZ DE UN PARRAFAZO":
        //     - a FIRST line that reads like a title - all capitals, or ending in a
        //       colon - is drawn bold, brighter, and UNDERLINED. Dozens of these strings
        //       open with one and every one of them was body text;
        //     - a BLANK line (the paragraph break, used in the longest hovers) becomes a
        //       HALF line of air instead of a whole empty one, so paragraphs separate
        //       without the card growing a hole;
        //     - a (parenthetical) inside otherwise unmarked text is dimmed. There are
        //       hundreds; they are asides by construction, and at full white they
        //       compete with the sentence they qualify.
        // The heading test is on the WHOLE line and refuses anything ending in . ! ?,
        // because capitals are also used INSIDE sentences here for emphasis - "How a
        // combination is SCORED." is a sentence, not a title - and it needs a letter in
        // it, or "1200c" would be promoted for having no lower case to lose.
        //
        // headingLines is an out-param rather than part of the return value because the
        // underline cannot be expressed in an AttributedString at all: its Attribute is
        // {range, font, colour} and nothing more, in JUCE 8.0.14. It is stroked from the
        // TextLayout's own line geometry in drawTooltip, the only place with a Graphics.
        // Measurement (getTooltipBounds) passes nullptr and is unchanged - and because
        // both paths call THIS function, the card still sizes itself to what is painted.
        static juce::TextLayout layoutFor (const juce::String& text, float maxW,
                                           int* headingLines = nullptr)
        {
            const juce::Font plain (juce::FontOptions (13.5f));
            const juce::Font bold  (juce::FontOptions (13.5f).withStyle ("Bold"));
            const juce::Font gapF  (juce::FontOptions (6.5f));   // the half-line of air
            const juce::Colour cPlain  { juce::Colours::white };
            const juce::Colour cTerm   { 0xffffc95c }; // amber - concepts
            const juce::Colour cValue  { 0xff7fd4ff }; // blue  - literals you can type
            const juce::Colour cWarn   { 0xffff8a7a }; // red   - consequences
            const juce::Colour cSource { 0xffff9a3c }; // orange - "from the partial"
            const juce::Colour cHead   { 0xffffffff }; // the title line
            const juce::Colour cAside  { 0xffa2a8b8 }; // (parentheticals)

            if (headingLines != nullptr) *headingLines = 0;

            juce::AttributedString as;
            juce::StringArray lines;
            lines.addLines (text);

            const auto readsAsHeading = [] (const juce::String& l)
            {
                const auto t = l.trim();
                if (t.isEmpty()) return false;
                if (juce::String (".!?").containsChar (t.getLastCharacter())) return false;
                bool hasLetter = false;
                for (int k = 0; k < t.length(); ++k)
                    if (juce::CharacterFunctions::isLetter (t[k])) { hasLetter = true; break; }
                if (! hasLetter) return false;
                return t.endsWithChar (':') || t == t.toUpperCase();
            };

            for (int i = 0; i < lines.size(); ++i)
            {
                const auto& line = lines[i];
                const auto tail = (i < lines.size() - 1 ? juce::String ("\n") : juce::String());
                if (isSourceLine (line)) { as.append (line + tail, plain, cSource); continue; }
                if (line.trim().isEmpty() && tail.isNotEmpty())
                { as.append (tail, gapF, cPlain); continue; }
                if (i == 0 && lines.size() > 1 && readsAsHeading (line))
                {
                    as.append (line + tail, bold, cHead);
                    if (headingLines != nullptr) *headingLines = 1;
                    continue;
                }

                // Walk the line, splitting on the three delimiters. An unmatched
                // delimiter is treated as ordinary text, so a stray * never eats
                // the rest of a tooltip.
                // A delimiter only OPENS a span when it starts a word: it must not be
                // wedged against a letter or digit on its left, and must not be followed
                // by a space. Without that rule "keeps 14 (2*7) and 25 (5*5)" parsed as a
                // highlighted "7) and 25 (5" - which is exactly where the meaningless
                // coloured runs in the prime tooltip came from. Arithmetic in a tooltip
                // is ordinary text and has to stay that way.
                const auto opensHere = [&line] (int k)
                {
                    if (k + 1 >= line.length()) return false;
                    const auto prev = k > 0 ? line[k - 1] : (juce::juce_wchar) ' ';
                    return ! juce::CharacterFunctions::isLetterOrDigit (prev)
                        && ! juce::CharacterFunctions::isWhitespace (line[k + 1]);
                };

                // Unmarked text, with its parentheses dimmed. Only balanced pairs on the
                // same line, and only OUTSIDE a marked span - a colour somebody chose
                // deliberately is never overridden by this.
                const auto plainRun = [&] (const juce::String& s)
                {
                    int p = 0;
                    while (p < s.length())
                    {
                        const int open = s.indexOfChar (p, '(');
                        const int shut = open < 0 ? -1 : s.indexOfChar (open + 1, ')');
                        if (open < 0 || shut < 0) { as.append (s.substring (p), plain, cPlain); break; }
                        if (open > p) as.append (s.substring (p, open), plain, cPlain);
                        as.append (s.substring (open, shut + 1), plain, cAside);
                        p = shut + 1;
                    }
                };

                int pos = 0;
                while (pos < line.length())
                {
                    int at = -1; juce::juce_wchar delim = 0;
                    for (int k = pos; k < line.length(); ++k)
                    {
                        const auto ch = line[k];
                        if ((ch == '*' || ch == '`' || ch == '!') && opensHere (k))
                        { at = k; delim = ch; break; }
                    }

                    if (at < 0) { plainRun (line.substring (pos)); break; }
                    const int close = line.indexOfChar (at + 1, delim);
                    if (close < 0) { plainRun (line.substring (pos)); break; }

                    if (at > pos) plainRun (line.substring (pos, at));
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
            // The heading's rule, stroked by hand. juce::AttributedString carries no
            // underline attribute in JUCE 8, so the only way to get one is to draw it
            // from the laid-out line's own geometry - and TextLayout::Line gives exactly
            // what that needs: the baseline origin, the descent, and the line's real
            // horizontal extent. Drawn INSIDE the descent, so it costs the card no
            // height and getTooltipBounds needs no adjustment. Same precedent as the
            // fraction bar under a ratio on the keyboard: the rule is exactly as long as
            // the glyphs, never as long as the box.
            const auto area = body.reduced (9.0f, 5.0f);
            int heads = 0;
            const auto tl = layoutFor (text, (float) width - 18.0f, &heads);
            tl.draw (g, area);
            for (int i = 0; i < juce::jmin (heads, tl.getNumLines()); ++i)
            {
                const auto& ln = tl.getLine (i);
                const auto xs  = ln.getLineBoundsX();
                const float y  = area.getY() + ln.lineOrigin.y + ln.descent * 0.45f;
                g.setColour (juce::Colours::white.withAlpha (0.75f));
                g.fillRect (area.getX() + xs.getStart(), y, xs.getLength(), 1.0f);
            }
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

            // Chevron on the right. THESE THREE NUMBERS ARE WHERE fgfit::comboArrowGutter
            // comes from - left ink edge = width - h*0.55 - h*0.15 - half the 1.6 px cap -
            // so moving any of them without moving that function puts the item's last
            // glyph back under the arrow, which is exactly the defect it was written for.
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

        // Keep the selected-item text clear of the rounded left end. The usable width is
        // fgfit::comboTextWidth - the ONE place it lives, shared with the resized() that
        // sizes a box to its words and with the probe that checks it fits (see the
        // comment there for what a second copy of this formula cost).
        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            const int inset = box.getHeight() / 3;
            label.setBounds (inset, 1, fgfit::comboTextWidth (box.getWidth(), box.getHeight()), box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
            // AN ITEM THAT DOES NOT FIT IS SHORTENED, NEVER SQUEEZED.
            //
            // juce::Label leaves minimumHorizontalScale at 0, which drawFittedText reads as
            // Font::getDefaultMinimumHorizontalScaleFactor() (0.7) and spends BEFORE it
            // considers cutting anything: the glyphs are stretched to 70% of their width
            // and the string is drawn in full, narrower than the same face two boxes away.
            // Photographed in the Preset Playground at every window size: a corner's
            // preset name needed 404 px and was drawn condensed into 174, while the pad's
            // own corner read-out - 25 px above it, same string, same font - ellipsised.
            // Two policies on one string, side by side.
            //
            // 1.0f is the whole fix: GlyphArrangement::fitLineIntoSpace skips
            // stretchRangeOfGlyphs entirely at that scale and goes straight to dropping
            // glyphs and appending an ellipsis, which is this project's rule ("nothing is
            // silently squashed") and what the neighbour already did. Set for EVERY combo,
            // not just the preset pickers: every other box in this window is sized by
            // fgfit::snugComboWidth to hold its own widest item, so this can only ever
            // fire where the box was ALREADY too small - and there it is the difference
            // between a word you cannot read and one that says it was cut.
            label.setMinimumHorizontalScale (1.0f);
            // ...and let getLabelFont keep it. drawLabel does not use the font set here:
            // it asks getLabelFont, which puts a 12.5 px BOLD floor under everything. For
            // a small-item box that floor is the whole thing being opted out of, so the
            // label is renamed into the existing "small" escape hatch. Done HERE because
            // this is the one place that owns the box label's font.
            if (box.getName() == fgfit::kSmallComboName) label.setName ("small");
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
    // A SMALL-ITEM BOX: its item is a PHRASE ("Transposed down", "Transposada avall")
    // read under the painted "Pitch" caption right above it, which is exactly what the
    // hatch is for - the caption is what the eye finds, the item only has to be legible.
    // Measured: this box's longest item is what the rig's legibility walk names as the
    // binding control in BOTH languages, so the type size it asks for is the type size
    // the whole window's minimum width is standing on.
    basePitchBox.setName (fgfit::kSmallComboName);
    makeLabel (lblBaseN, cHarmonic); makeLabel (lblBasePitch, cHarmonic);
    lblBasePitch.setJustificationType (juce::Justification::centredLeft); // top-LEFT of its combo, like the others

    // TRANSITION: ONE question - how does a spectrum change LAND? - asked once.
    //
    // "haz que morph y dry/wet sean dos opciones dentro de un mismo desplegable en el
    //  submenu transition, asi economizamos espacio, que las dos a la vez no tienen
    //  sentido, haz que la default sea drywet"
    //
    // Morph and Dry/Wet used to be two cells, each with its own ON/OFF pill, its own
    // painted name and its own Time fader - six controls answering one question, plus a
    // greyed-out cell and an amber MORPH ON tag whose whole job was to explain that the
    // second answer could not act while the first one did. They are ONE crossfade machine
    // per voice (see AdditiveVoice.h and bf0f768), so exactly one of them can ever act,
    // and a dropdown is the shape of a question with one answer.
    //
    // THE PARAMETERS DO NOT CHANGE, and that is the point. "morph" and "dryWet" are still
    // the two booleans they always were: the DSP reads them unchanged, every automation
    // lane already drawn on them still moves them, every project ever saved still restores
    // them, and the preset playground still blends them. This box is a VIEW of that pair,
    // not a new parameter laid over it - which is also why it is the honest view: the
    // pair has four states and only THREE sounds, because with Morph on the crossfade
    // cannot act at all. Off / Morph / Dry/Wet is that set of three, exactly.
    //
    //   both off  -> Off       a spectrum change lands instantly (the 50 ms template fade)
    //   morph     -> Morph     every partial GLIDES to its new place
    //   dryWet    -> Dry/Wet   the old spectrum fades out while the new one fades in
    //   both on   -> Morph     what a project with both flags set already SOUNDS like
    //
    // OFF IS AN ITEM, not "turn the Time fader down". The Time fader cannot express it:
    // its range bottoms out at 1 ms and the crossfade clamps itself to a 5 ms floor
    // (below that the phase hand-over cannot be made click-free), so there is no fader
    // position that means "no transition". It is also a state a player chooses on purpose
    // - it is what every build before bf0f768 did - and a state you can only reach by
    // failing to find it is a state that has been removed.
    transitionBox.setName (fgfit::kSmallComboName);   // its item reads BELOW the TRANSITION title
    transitionBox.setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (transitionBox);
    // No ComboBoxAttachment: this box does not own a parameter, it shows two. Items carry
    // FIXED ids (1/2/3) rather than positions, so applyLanguage can re-word them without
    // the selection sliding onto a different mode.
    transitionBox.onChange = [this]
    {
        const int id = transitionBox.getSelectedId();
        if (id <= 0) return;              // mid-relabel: getSelectedId reads 0, see applyLanguage
        setTransitionParams (id == 2, id == 3);
        refocus();
    };
    // The right-click "Map to" menu, kept from the two pills this replaced: the mode is
    // still modulable, and the one it routes is the one the box is currently showing -
    // a modulator on "morph" gates Morph, one on "dryWet" gates the crossfade.
    addMapListener (transitionBox, "morph");

    // ONE Time fader, for whichever mode is selected - but still the two PARAMETERS, so
    // each mode keeps the time the user set for it and neither lane is disturbed. The two
    // sliders share one rectangle and exactly one of them is ever visible; `resized()`
    // lays out only that one, and the layout probe records only what is on screen.
    configFader (morphTimeFader, "morphTime", morphTimeAtt, cSlope, "Morph transition time in seconds: how long partials take to glide to the new spectrum.");
    morphTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    morphTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);
    configFader (dryWetTimeFader, "dryWetTime", dryWetTimeAtt, cSlope,
                 "Dry/wet transition time in seconds: how long the old spectrum takes to fade out while the new one fades in.");
    dryWetTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    dryWetTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);
    makeLabel (lblTransTime, cSlope);

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
    // THE NUMBER UNDER BASE SAYS WHAT IT IS. It used to read "1", "2", "7" - a bare
    // number under a bare "N", which tells a reader nothing about what it selects. It is
    // the harmonic the spectrum is built from, so it says so: "1st harmonic", "7th
    // harmonic", and in Catalan the noun first with the ordinal on the number.
    //
    // Through tr(), not through juce::String(const char*): the Catalan needs an accent,
    // and a raw literal is read as Latin-1 and comes out "harmA-2nic". The mojibake gate
    // caught exactly that, 22 times.
    baseNFader.textFromValueFunction = [this] (double v)
    {
        const int n = juce::jmax (1, (int) std::lround (v));
        if (catalan)
        {
            // The ordinal letters are not translations - they are Catalan spelling. Wrapping
            // each one in a translation pair just to get the ACCENT decoded invented three
            // pairs whose two halves were identical, and the source audit rightly called
            // them out. Only the two pieces that actually carry an accent are decoded, and
            // this comment deliberately avoids spelling the call out: the audit greps the
            // SOURCE, so an example in a comment is a finding.
            const char* plain = n == 1 ? "r" : n == 2 ? "n" : n == 3 ? "r" : n == 4 ? "t" : nullptr;
            return tr ("harmonic ", "harm\xc3\xb2nic ") + juce::String (n)
                 + (plain != nullptr ? juce::String (plain)
                                     : juce::String::fromUTF8 ("\xc3\xa8"));
        }
        const int t = n % 10, h = n % 100;
        const char* suf = (t == 1 && h != 11) ? "st"
                        : (t == 2 && h != 12) ? "nd"
                        : (t == 3 && h != 13) ? "rd" : "th";
        return juce::String (n) + suf + " harmonic";
    };
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
    // Essentials macro (Gain / Reference / Octave) - one framed box drawn
    // in paint() like the Harmonic Engine, so no per-cell GroupComponents here.
    // PHASE, the fader. 0 = Aligned, 1 = Scattered, and every position between them is
    // a real sound now rather than a rounding to one of the two - which is the whole
    // request. The end words live in the tooltip and in the value read-out, because the
    // sub-section's painted title already says PHASE and a second one would not fit.
    //
    // cSlope, NOT cEssentials: the fader lives in the SPECTRUM panel now, in the same
    // column as SLOPE / MORPH / DRY-WET, and the hue is what tells a player which
    // compartment a control belongs to. Left on the Essentials hue it would read as a
    // stray from the box below - the one thing the move was meant to stop.
    configFader (phaseFader, "phaseMode", phaseAtt, cSlope,
                 "0 = Aligned (tonal pulse, never clips), 1 = Scattered (louder, noisier). Anywhere between is a real blend. "
                 "Under a held note it re-phases over the Morph / Dry-Wet transition - with both off, at the next note.");
    phaseFader.setSliderStyle (juce::Slider::LinearHorizontal);
    phaseFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 38, 14);
    // There is no glide box beside the fader any more: how a mid-note move reaches
    // sounding notes, and how long it takes, both come from the SPECTRUM panel's
    // Morph / Dry-Wet transition now (see the phaseGlide note in createParameterLayout).
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
    // NO TOOLTIP HERE. The two stepper hovers used to be built as plain English
    // juce::Strings on the next two lines and handed to this lambda, which meant they
    // were written ONCE, in the constructor, and applyLanguage() never touched them
    // again: they stayed English for the rest of the editor's life in both languages.
    // That is exactly cause (a) of commit 2b51e2f ("strings set once in the constructor")
    // and the same fix applies - the words live in applyLanguage() now (search octTip
    // there), which is the only place a language switch can reach.
    // A STEPPER PAIR IS ONE CONTROL, AND IT WEARS ITS PANEL'S COLOUR.
    //
    // "-" used to be cReset (red) and "+" cLoad (green) - the semantic pair that means
    // DESTROY and LOAD everywhere else in this window. Stepping a transpose by one octave
    // is neither: it is the most ordinary, most reversible edit there is, and painting it
    // in the two loudest colours the product owns put the same red on the octave minus
    // that is on the X which deletes a bank slot. Red then meant five different things at
    // once (destructive, off, decrement, un-map, and a neutral read-out) and therefore
    // meant nothing.
    //
    // Both halves now take the colour of the panel they sit in, at the same alpha, so the
    // pair reads as one control belonging to one box - and the direction is carried by the
    // glyph, which is what the glyph is for. `col` is passed in rather than assumed:
    // OCTAVE lives in ESSENTIALS and OCTAVE RANGE in SPECTRUM, and a stepper wearing the
    // wrong panel's hue is exactly the kind of drift this parameter prevents.
    auto configStep = [&] (juce::TextButton& b, const char* txt, const char* id, int delta,
                           juce::Colour col)
    {
        b.setButtonText (txt);
        b.setName ("round"); // circular body + drawn glyph (see GalacticLnF)
        b.setColour (juce::TextButton::buttonColourId, col.withAlpha (0.45f));
        b.setMouseClickGrabsKeyboardFocus (false);
        b.onClick = [stepParam, id, delta] { stepParam (id, delta); };
        addAndMakeVisible (b);
    };
    configStep (octDn, "-", "octaveShift", -1, cEssentials);
    configStep (octUp, "+", "octaveShift",  1, cEssentials);
    // The SPECTRUM panel re-colours itself with the selected engine (see
    // recolourSpectrumControls), so this pair is seeded with the "both engines" hue and
    // repainted from there like every other control in that panel.
    configStep (sprDn, "-", "spectrumRange", -1, cSlope);
    configStep (sprUp, "+", "spectrumRange",  1, cSlope);
    for (auto* v : { &octVal, &sprVal })
    {
        v->setJustificationType (juce::Justification::centred);
        v->setColour (juce::Label::textColourId, juce::Colours::white);
        v->setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
        addAndMakeVisible (*v);
    }
    // (octVal's and sprVal's hovers are written by applyLanguage() too - same reason.)

    // THE PRIME CONTROL SWITCH. It sits on the mode row, to the right of Limit/Whitelist,
    // in space that row was already wasting. Same colours as the nine tick boxes it
    // governs, because it is the same control at a higher level.
    primeOnToggle.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffb9c0d0));
    primeOnToggle.setColour (juce::ToggleButton::tickColourId, cHarmonic);
    primeOnToggle.setColour (juce::ToggleButton::tickDisabledColourId, cHarmonic.withAlpha (0.4f));
    primeOnToggle.setMouseClickGrabsKeyboardFocus (false);
    primeOnToggle.onClick = [this] { refocus(); resized(); };
    addAndMakeVisible (primeOnToggle);
    primeOnAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "primeOn", primeOnToggle);
    addMapListener (primeOnToggle, "primeOn");
    configCombo (primeModeBox,  "primeMode",  primeModeAtt,  cHarmonic, "Limit = drop harmonics with a prime factor above the value.  Whitelist = only the ticked primes.");
    // Prime LIMIT as a FADER (so it can be driven by the modulators). The value is
    // the index into {Off, 2, 3, 5, 7, 11, 13, 17, 19, 23}, shown by name.
    configFader (primeLimitFader, "primeLimit", primeLimitFaderAtt, cHarmonic,
                 "Largest prime factor allowed (Limit mode). Right-click to map a modulator onto it.");
    primeLimitFader.setSliderStyle (juce::Slider::LinearHorizontal);
    primeLimitFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 15);
    primeLimitFader.setSliderSnapsToMousePosition (false);
    primeLimitFader.normalSensitivity = 300;
    primeLimitFader.textFromValueFunction = [this] (double v)
    {
        // The only WORD in this read-out; the other nine entries are prime numbers. It
        // was a bare const char* and so was the one label on the fader that never
        // changed language - and "No" is the very word its own tooltip uses for this
        // position ("No = sense filtre de primers").
        const juce::String names[] = { tr ("Off", "No"), "23", "19", "17", "13", "11", "7", "5", "3", "2" };
        const int lo = juce::jlimit (0, 9, (int) std::floor (v));
        const double t = v - (double) lo;
        if (t < 0.01) return names[lo];
        return names[lo] + juce::String::fromUTF8 ("\xe2\x86\x92")
             + names[juce::jmin (9, lo + 1)]
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
        // A 2 px border each side is right for a caption sitting in open panel space and
        // wrong for these: nine of them share one cell width, so 4 px of border is 4 px
        // the DIGITS do not get, and "11" was the string the whole window's legibility
        // measurement stopped on - needing 14.6 px in the 14.0 the border left it. The
        // cells are adjacent and each holds one short token under its own tick, so 1 px
        // is enough to keep two neighbours from touching.
        primeLbl[i].setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
        // ...and out of the 12.5 px BOLD floor GalacticLnF::getLabelFont puts under every
        // Label. That floor is right for a caption in open space and wrong for nine
        // numbers sharing one cell width: "17" needs 14.6 px at 12.5 Bold and the cell is
        // often 16, which is what the whole window's legibility measurement kept stopping
        // on. Named "small" (the opt-out the LnF already documents), resized() then picks
        // the size from the width the cell really has.
        primeLbl[i].setName ("small");
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
        const juce::Colour & cCustom = cCustomMod; // the CUSTOM modulator identity, from the palette
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
        makeLabel (lblLfoPhase, cCustom); lblLfoPhase.setText ("Phase", juce::dontSendNotification);

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
        lfoWaveBox.addItem ("Square", 5);      lfoWaveBox.addItem ("Custom", 6);
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
            // SMALL-ITEM BOXES. These three hold PHRASES - "Fundamental / 2^n",
            // "Restart each note" - not one-word modes, and at LookAndFeel_V4's default
            // (16 px, which getLabelFont then draws BOLD) the rate mode needed 129 px in
            // the 108 it had and was drawn shrunk. The hatch exists for exactly this: a
            // box whose item is a sentence sitting under a painted sub-label, where the
            // sub-label is what the eye reads first and the item only has to be legible.
            // All three together, not the one that overflowed, or the row would carry two
            // different type sizes side by side.
            cb->setName (fgfit::kSmallComboName);
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
    tuningNameLabel.setBorderSize (juce::BorderSize<int> (0, 2, 0, 2)); // one inset - see tuningPeriodLbl
    // Period of the active scale, in the TUNING PANEL'S OWN COLOUR, clearly labelled.
    //
    // IT WAS RED, AND RED WAS SAYING NOTHING. This read-out is not a warning, not an
    // off-state and not a button: it is a neutral fact about the loaded scale, printed
    // ALWAYS - "Period 2" in the shipped state, "Period 3" for a tritave, "Period 1 (no
    // repeat)" for a scale that declares none. A colour that is on screen in every single
    // state carries no information by definition, and this one was the same red as the X
    // that deletes a bank slot, the Reset buttons and the OFF pills. Four jobs for one
    // hue teaches the eye to stop reading it, which costs the red where it MATTERS.
    //
    // cTuning is what the rest of this panel is already wearing (its border, its title
    // chip, its captions, the scale name directly above this line), so the period now
    // reads as one more field of the TUNING box instead of as an alarm inside it. Red is
    // left to mean exactly one thing in this window: this control destroys something.
    tuningPeriodLbl.setJustificationType (juce::Justification::centredLeft);
    tuningPeriodLbl.setColour (juce::Label::textColourId, cTuning.brighter (0.15f));
    tuningPeriodLbl.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
    // The same 2 px inline-caption border every makeLabel() caption in this panel keeps
    // (see makeLabel), instead of juce::Label's default 5. Four stacked text items in this
    // box started their ink at 537 / 536 / 533 / 533 px because the two read-outs kept the
    // default border and the captions did not - a 3 px stagger down one column with every
    // component rect on the same x. One inset, so the column is a column.
    tuningPeriodLbl.setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));
    addAndMakeVisible (tuningPeriodLbl);
    addAndMakeVisible (tuningNameLabel);

    // Absolute 128-key table: the period is Fibló's own ESTIMATE and the user may set it.
    // It cannot move a pitch (the table fixes all 128) - it only answers the features that
    // need a period: the keyboard colouring, the snapping grid, the chord search and the
    // dissonance lists. Applied on Return, so a half-typed "3/" is never read as a period.
    tablePeriodEdit.setJustification (juce::Justification::centred);
    tablePeriodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    // Outlined in the panel's own colour, like every other field in it - it was the same
    // red as the period read-out beside it, and for the same wrong reason (see
    // tuningPeriodLbl above): typing a period is not a destructive act.
    tablePeriodEdit.setColour (juce::TextEditor::outlineColourId, cTuning.withAlpha (0.7f));
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
    // (tooltip in applyLanguage - a tooltip set here would keep the launch language)
    tuningBankBox.onChange = [this] { processorRef.setTuningBankSlotParam (tuningBankBox.getSelectedId() - 1); refocus(); };
    addAndMakeVisible (tuningBankBox);
    tuningBankLoadBtn.setColour (juce::TextButton::buttonColourId, cLoad);
    tuningBankLoadBtn.setMouseClickGrabsKeyboardFocus (false);
    // (tooltip in applyLanguage)
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
    // (tooltip in applyLanguage)
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
        // NOTE NAMES. Ticked when the preference is on. When it is on but this tuning
        // cannot be named, the item SAYS WHY on its own line rather than silently doing
        // nothing - "solo para tunings regulares y para JI" means most scales get numbers,
        // and an option that looks broken is worse than one that explains itself.
        {
            const auto why = noteNamesReason();
            juce::String label = tr ("Note names on the keyboard",
                                     "Noms de nota al teclat");
            if (why.isNotEmpty())
                label += " - " + tr ("numbers: ", "n\xc3\xbameros: ") + why;
            m.addItem (3, label, true, noteNamesOn);
            m.addItem (4, tr ("Note names: credits", "Noms de nota: cr\xc3\xa8" "dits"));
        }
        // WHERE THE PRESETS ACTUALLY ARE. The Save dialog names the folder once and then
        // it is gone; every other way of finding it means guessing at a path under
        // Documents. This opens it in the file manager - and creates it first, because
        // before the first save it does not exist and Explorer would answer a nonexistent
        // path with an error box instead of a folder.
        m.addItem (5, tr ("Open preset folder", "Obre la carpeta de presets"));
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
                    // ...AND EVERY MODULATOR ROUTING, which is not a parameter and so
                    // survived every "reset all parameters" there has ever been.
                    //
                    // "AL DARLE A RESET ALL PARAMETERS EL PITCH SE MUEVE COMO SI TUVIESE
                    // ENVOLVENTE DE PITCH, Y POR MUCHO QUE CAMBIE LA ENVOLVENTE DE PITCH
                    // NO PUEDO QUITARSELO ... ES POR QUE A PESAR DE RESET ALL PARAMETERS
                    // SE QUEDO EL LFO PUESTO."
                    //
                    // Exactly that: an LFO mapped onto pitch keeps modulating after the
                    // reset, and nothing in the pitch ENVELOPE can stop it, because it is
                    // not the envelope doing it. The map lives in ModEngine, outside the
                    // APVTS, so setting every parameter to its default never touched it.
                    // Init means init.
                    for (int m = 0; m < ModEngine::kMods; ++m)
                        processorRef.getModEngine().unmapAll (m);
                    // The per-partial pencil edits are the same kind of state: not
                    // parameters, and a reset that leaves them behind hands the user a
                    // "default" patch with somebody else's amplitudes in it.
                    processorRef.setPartialGains ({}, false);
                    rhythmPanel.refreshSlots();
                    rhythmPanel.list.repaint();
                }
                else if (r == 2)
                {
                    catalan = ! catalan;
                    applyLanguage();
                }
                else if (r == 3)
                {
                    setNoteNamesForTests (! noteNamesOn);   // the one path, menu and check
                }
                else if (r == 4)
                {
                    // The credit, where someone using the feature can actually find it.
                    // Names, not just licences: the notation, the algorithm and the font
                    // are three different people's work.
                    juce::NativeMessageBox::showMessageBoxAsync (
                        juce::MessageBoxIconType::InfoIcon,
                        tr ("Note names: credits", "Noms de nota: cr\xc3\xa8" "dits"),
                        juce::String::fromUTF8 (
                            catalan
                              ? "Notaci\xc3\xb3 d'ups and downs, de Kite Giedraitis.\n\n"
                                "Algorisme de grafia adaptat de \"Notation\", de Richie Greene "
                                "(llic\xc3\xa8ncia MIT) - github.com/richiegreene/notation\n\n"
                                "Glifs d'alteraci\xc3\xb3: notes.otf, de Sintel, del projecte tessera "
                                "(llic\xc3\xa8ncia MIT) - github.com/Sin-tel/tessera\n\n"
                                "La notaci\xc3\xb3 HEJI per a entonaci\xc3\xb3 justa (Marc Sabat i Thomas "
                                "Nicholson, amb Wolfgang von Schweinitz) encara NO hi \xc3\xa9s: els seus "
                                "glifs SMuFL nom\xc3\xa9s existeixen aqu\xc3\xad en format WOFF2, que JUCE no pot "
                                "carregar, i dibuixar-los donaria quadres buits.\n\n"
                                "Textos complets de les llic\xc3\xa8ncies: assets/fonts/NOTATION-LICENSES.txt"
                              : "Ups and downs notation, devised by Kite Giedraitis.\n\n"
                                "Spelling algorithm ported from \"Notation\" by Richie Greene "
                                "(MIT licence) - github.com/richiegreene/notation\n\n"
                                "Accidental glyphs: notes.otf by Sintel, from tessera "
                                "(MIT licence) - github.com/Sin-tel/tessera\n\n"
                                "HEJI, the just-intonation notation by Marc Sabat and Thomas "
                                "Nicholson with Wolfgang von Schweinitz, is NOT here yet: its "
                                "SMuFL glyphs exist here only as WOFF2, which JUCE cannot load, "
                                "and drawing them would give empty boxes.\n\n"
                                "Full licence texts: assets/fonts/NOTATION-LICENSES.txt"),
                        this);
                }
                else if (r == 5)
                {
                    // createDirectory() on a folder that already exists is a no-op that
                    // returns ok, so there is no "does it exist" branch to get wrong -
                    // and it is the SAME getPresetFolder() the save and load paths use,
                    // so a test rig that redirected the folder opens the redirected one
                    // rather than the user's.
                    const auto dir = FractalAdditiveAudioProcessor::getPresetFolder();
                    dir.createDirectory();
                    dir.revealToUser();
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

    // Spectrum -> rhythm MIDI panel (top-bar button toggles it), IN THE COLOUR OF THE
    // PANEL IT OPENS.
    //
    // It was 0xfff2f2f2 - "WHITE: maximum contrast against the dark bar" - and that was
    // the problem, not the intention. On a 0x0c0c12 page it was the only near-white
    // element in the entire window, so it was the first thing the eye landed on, ahead of
    // the title, the preset selector and every panel: maximum contrast spent on a
    // SECONDARY view toggle. Nothing else in this product is painted white, either, so it
    // read as an unstyled system control that had escaped the theme.
    //
    // 0xff5cb8ff is the exact colour the panel's own SPECTRUM VS RHYTHM heading is drawn
    // in (see RhythmPanel::heading), which is the only relationship this button has to
    // express: press it and that heading appears. Still light enough for
    // GalacticLnF::drawButtonText to put BLACK letters on it (perceived brightness ~0.66,
    // well over the 0.52 threshold), so it loses none of its legibility - only its
    // undeserved first place in the reading order.
    rhythmBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5cb8ff));
    rhythmBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    rhythmBtn.setMouseClickGrabsKeyboardFocus (false);
    // (tooltip in applyLanguage)
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
    // (label + tooltip in applyLanguage: "Steps" was set here once and stayed English
    //  through every later language switch - it is one of the strings the en/ca drawn-text
    //  assertion in SnapshotRig.cpp was added to catch.)
    // Modulatable, like every other fader: right-click to route a modulator.
    addMapListener (rhythmPanel.velSlopeS, "rhythmVelSlope");
    addMapListener (rhythmPanel.octAllVal, "rhythmOctAll");     // Octave (all pulses)
    // (tooltip in applyLanguage)
    rhythmMpeAtt = std::make_unique<ButtonAttachment>   (processorRef.apvts, "rhythmMpe",  rhythmPanel.mpeT);
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
    // (tooltip in applyLanguage)
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
    // A SMALL-ITEM BOX: "±48 st (MPE)" needs 98 px of the 92 its row gives it, and the row
    // cannot spare the difference - widening it made the tuning panel's ladder drop a row
    // at the floor, which is a whole control lost to gain six pixels. The smaller face
    // costs no width at all, and this box sits under a "Pitch bend" caption that already
    // says what it is.
    bendBox.setName (fgfit::kSmallComboName);
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
    // THE DIRECTION ARROW. One press flips a rising Shepard into a falling one.
    //
    // The glyph carries the whole meaning, so it is the same in both languages and only
    // the hover needs translating - which is also why this is a button and not a two-item
    // dropdown: a dropdown would cost the row about 90 px to say what an arrow says in 24,
    // on the tightest row in the tuning panel.
    //
    // setClickingTogglesState so a plain ButtonAttachment drives it, exactly like the
    // Shepard switch beside it; onStateChange (not onClick) keeps the glyph honest when
    // the parameter moves from somewhere else - host automation, a preset load, an undo.
    shepDirBtn.setClickingTogglesState (true);
    shepDirBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
    shepDirBtn.setColour (juce::TextButton::buttonOnColourId, cTuning.withAlpha (0.85f));
    shepDirBtn.setColour (juce::TextButton::textColourOffId, cTuning.brighter (0.35f));
    shepDirBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    shepDirBtn.setMouseClickGrabsKeyboardFocus (false);
    shepDirBtn.onStateChange = [this]
    {
        shepDirBtn.setButtonText (shepDirBtn.getToggleState()
                                      ? juce::String::fromUTF8 ("\xe2\x86\x93")   // down
                                      : juce::String::fromUTF8 ("\xe2\x86\x91")); // up
    };
    shepDirBtn.onStateChange();
    shepDirAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "shepDown", shepDirBtn);
    addAndMakeVisible (shepDirBtn);
    configCombo (shepModeBox, "shepMode", shepModeAtt, cTuning, {});
    // SMALL-ITEM BOXES, for the same reason the LFO row's three are: these hold PHRASES
    // ("Sync Project Tempo", "Bend de periode"), not one-word modes, and the row's
    // meaning is already carried by the SHEPARD toggle beside them.
    //
    // Tried and REVERTED one commit ago, on a measurement that was wrong: the walk went
    // width first and reported 820x680, which read as 60 px of width bought with 130 of
    // height. Measured both ways round it is 850x550 - a smaller window than the 880x550
    // it replaces, in both dimensions at once. The change was always right; the ruler was
    // one-sided.
    shepModeBox.setName (fgfit::kSmallComboName);
    shepRiseBox.setName (fgfit::kSmallComboName);
    // Switching mode swaps which rate control is on screen, so it must re-lay the
    // row NOW - the attachment only moves the parameter, never the layout.
    shepModeBox.onChange = [this] { resized(); repaint(); refocus(); };
    configCombo (shepDivBox, "shepDiv", shepDivAtt, cTuning, {});
    // ...and it RE-LAYS OUT when the division changes, because its width is reserved from
    // what the SELECTED item needs. Without this the reservation is only true at the
    // moment the row was laid out: pick "1/16" at a narrow window and the box keeps the
    // width "4" was given. configCombo's own handler is kept - it is the focus return.
    shepDivBox.onChange = [this] { resized(); refocus(); };
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
    // ...and IMMEDIATELY after it, before a single control of this column: the section
    // frame has to sit ABOVE the group's interior fill and BELOW everything clickable,
    // and in JUCE that is decided by the order these two lines run in. See SpecSections.
    specSections.col = cSpecScale;
    addChildComponent (specSections);   // shown by resized(), per mode
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
    specModeBox.addItem ("Find tunings from spectrum's segment", 3);
    tuneSourceBox.addItem ("From current tuning", 1);
    // "all tunings", not "all possible tunings": see applyLanguage() for why the word
    // this box could not afford is the one that was adding nothing.
    tuneSourceBox.addItem ("From all tunings", 2);
    tuneSourceBox.setSelectedId (1, juce::dontSendNotification);
    tuneScopeBox.addItem ("0 vs all", 1);
    // Sentence case, like every other item and caption in this column. It used to be
    // "all vs all", and beside "0 vs all" - which opens on a digit and so reads as
    // capitalised whatever you do - that lower-case "a" was the only lower-case opening
    // letter in the whole box. See applyLanguage() for the pair this is translated to.
    tuneScopeBox.addItem ("All vs all", 2);
    // (its hover is written by applyLanguage(); set once here it stayed English for ever)
    tuneScopeBox.setSelectedId (2, juce::dontSendNotification); // "All vs all" by default
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
    // A SMALL-ITEM BOX, and this one is why the hatch has to be measured rather than
    // assumed. Its three items are SENTENCES ("Adapt spectrum to tuning based on
    // beating"), it is 192x34, and that height was chosen so they take TWO lines. At the
    // default face they take one and get cut - the shipped 1200x650 read "Find tunings
    // based on beati..." - because juce::Label gives its painter one line per font height
    // and this font is too tall for two. Dropped to the small face they fit two lines of
    // 154 px, which is what the box was built for.
    specModeBox.setName (fgfit::kSmallComboName);
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
    // SMALL-ITEM BOXES, for the same reason as the mode box above them: their items are
    // PHRASES, not one-word modes, and they sit in a column whose painted titles already
    // say what they are for. At the default face "To the chord held now" needs 165 px of
    // the 164 the column gives and "De totes les afinacions" needs 167 - one and three
    // pixels, which is a squeeze nobody can see and every check now reports honestly.
    tuneSourceBox.setName (fgfit::kSmallComboName);
    adaptTargetBox.setName (fgfit::kSmallComboName);
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
    // --- The REGISTER BAND the adaptation is optimised over --------------------
    // ONE number: N PERIODS of the loaded tuning, counted BOTH WAYS from the note being
    // played (see the block comment at registerDegreesPerPeriod). A period is the unit the
    // tuning itself is built from, so the number reads as a musical width in every scale,
    // and an anchor the hands move makes the band follow the register actually being played
    // instead of a pair of fixed pitches.
    {
        auto* ed = &adaptRegPeriodsEdit;
        ed->setJustification (juce::Justification::centred);
        ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
        ed->setColour (juce::TextEditor::outlineColourId, cSpecScale.withAlpha (0.7f));
        ed->setIndents (4, 2);
        // A WIDTH IN PERIODS: up to four characters, a decimal point allowed (a period is a
        // coarse unit - 1902 cents in Bohlen-Pierce - so half of one is a real setting), a
        // comma accepted because that is the decimal separator on screen in Catalan, and no
        // sign at all. The width runs both ways by construction, so a negative number is
        // not a direction, it is a typo.
        ed->setInputRestrictions (4, "0123456789.,");
        ed->setSelectAllWhenFocused (true);
        ed->onReturnKey = [this] { commitAdaptRegisterPeriods(); refocus(); };
        ed->onFocusLost = [this] { commitAdaptRegisterPeriods(); };
        addChildComponent (*ed);
    }
    makeLabel (adaptPartialsLbl, cSpecScale); adaptPartialsLbl.setVisible (false);
    makeLabel (adaptRangeLbl,    cSpecScale); adaptRangeLbl.setVisible (false);
    makeLabel (adaptRegLbl,      cSpecScale); adaptRegLbl.setVisible (false);
    // A size smaller than the other captions, kept from when three controls shared this row
    // (the caption, the field and an aggregate box that has since gone): the caption still
    // has to leave the field its full width in Catalan, and RhythmVerify measures that.
    adaptRegLbl.setFont (juce::Font (juce::FontOptions (10.0f).withStyle ("Bold")));
    // HOW FAR N REACHES EACH WAY, and WHICH NOTE it is counted from - one line, under the
    // field. Without it the two ends are invisible: a band that hangs off a note nobody
    // named is a control the user cannot aim, and one shortened by the end of the keyboard
    // would look symmetric while the search used something else.
    adaptRegSpanLbl.setJustificationType (juce::Justification::topLeft);
    adaptRegSpanLbl.setColour (juce::Label::textColourId, cSpecScale.withAlpha (0.8f));
    // 9 pt, a point and a half under its neighbours: this line carries the widest numbers
    // the panel can produce - a one-degree-per-period scale reaches an anchor of "-10:0",
    // the width takes its two-sided form at the ends of the keyboard ("-7.5/+8.0"), the two
    // band edges are five digits each, and an absolute .tun adds its "(no period)" marker on
    // top of all of that. At 10 pt the Catalan form overflowed by 8 px and at 9.5 by 7 (both
    // measured by the layout probe, which is the only thing in this build that can see it:
    // a Label answers an overflow by CONDENSING to 70%, which is unreadable and looks
    // intentional). Every one of those forms is a readoutVariant below, so this size is
    // checked and not hoped for.
    adaptRegSpanLbl.setFont (juce::Font (juce::FontOptions (9.0f)));
    adaptRegSpanLbl.setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
    adaptRegSpanLbl.setMinimumHorizontalScale (1.0f); // it must FIT, not condense
    addChildComponent (adaptRegSpanLbl);
    // The default width, written DIRECTLY - not through syncAdaptRegisterSpan, which pushes
    // the panel's settings to the processor. Doing that here ran syncRealtimeLink() from
    // inside the constructor, before specModeBox and adaptRtOn had been restored, so
    // reopening the window while a REALTIME link was running computed "no link wanted" and
    // switched the user's running follower off. The timer picks it up on its first tick,
    // when the panel really does know what it is showing.
    adaptRegPeriodsEdit.setText (juce::String (registerPeriodsEffective(), 1), juce::dontSendNotification);
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
    // FIVE, not three ("que por defecto sean 5 notas"). Three notes is a triad and the
    // search is for TUNINGS: five is where the result starts being a scale you can play
    // rather than one chord, and it is still well inside what the search does quickly.
    chordNotesBox.setSelectedId (5, juce::dontSendNotification);
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
        // (its hover is written by applyLanguage(): a correct tr() pair set ONLY here
        //  would be frozen in the constructor's language for the editor's whole life)
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
    // (tooltip in applyLanguage)
    addChildComponent (nadBox); // kept for state; the shared "Notes" box drives it now
    nadView.setViewedComponent (&nadList, false);
    nadView.setScrollBarsShown (true, false);
    addChildComponent (nadView);
    // (tip in applyLanguage)

    segList.proc      = &processorRef;
    segList.ratioName = [] (double r) { return justName (r); };
    segList.onChange  = [this] { segList.repaint(); updateScaleDissonance(); refocus(); };
    segView.setViewedComponent (&segList, false);
    segView.setScrollBarsShown (true, false);
    addAndMakeVisible (segView);
    makeLabel (segPeriodLbl, cSpecScale);
    // EVERY LEFT-HAND CAPTION IN THE SPECTRUM-vs-TUNING COLUMN, JUSTIFIED ONCE.
    //
    // makeLabel's default is CENTRED, and these seven are captions: each names the
    // control immediately to its right, so a centred one floats away from what it names
    // by however much its cell exceeds its word - which differs per language. Four of
    // them were re-justified inside resized() instead, on every layout pass, and the
    // other three only got it because they happen to go through the `caption` helper.
    // That is one decision written in three different places, and it drifted: the two
    // rows that did NOT set it were the two that had never been touched since.
    //
    // Set here, where the labels are made, it is a property of the label - so a row that
    // stops using `caption` cannot silently lose it, and `caption` setting it again is a
    // no-op rather than the only thing holding it up.
    for (auto* l : { &chordNotesLbl, &chordPerLbl, &chordMinLbl, &segPeriodLbl,
                     &adaptPartialsLbl, &adaptRangeLbl, &adaptRegLbl })
        l->setJustificationType (juce::Justification::centredLeft);
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
    // ...and the painter has to KEEP that 10.5, which it only does through the "small"
    // escape hatch: GalacticLnF::getLabelFont puts a 12.5 px BOLD floor under every other
    // label, and drawLabel asks IT, not the font set here. At 12.5 px only ONE line fits
    // this 26 px box, so a two-line hint was drawn as one line squashed to 70% - the
    // silent condensation a juce::Label does instead of clipping. This is the same class
    // of hint as the play-keys line, which is what the hatch was made for.
    segHelpLbl.setName ("small");
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
    // box you type the Hz into (with "Hz" shown to its right), not a fader.
    refAEdit.setJustification (juce::Justification::centred);
    refAEdit.setFont (juce::Font (juce::FontOptions (13.0f))); // fits the slim cell, no clipped descenders
    refAEdit.setInputRestrictions (11, "0123456789."); // fits C3's unrounded 261.6255653
    refAEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    refAEdit.setColour (juce::TextEditor::outlineColourId, cEssentials.withAlpha (0.7f));
    refAEdit.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    refAEdit.setIndents (8, 2); // clear the rounded (pill) ends
    refAEdit.setSelectAllWhenFocused (true);
    // ...and the first value through the ONE formatter (see refreshRefAText), rather than
    // a second copy of the format string here. The copy is how this field came to show a
    // number that was not the value: it wrote all seven decimals unconditionally, and a
    // juce::TextEditor does not clip a string it cannot hold - it SCROLLS, with the caret
    // parked at the end after setText. At 690x550 "261.6255653" was drawn as "55653": no
    // ellipsis, no cut glyph, just a plausible and entirely wrong 55653 Hz.
    refreshRefAText();
    refAEdit.onReturnKey  = [this] { commitRefA(); refocus(); };
    refAEdit.onFocusLost  = [this] { commitRefA(); };
    refAEdit.onEscapeKey  = [this] { refreshRefAText(); refocus(); };
    addAndMakeVisible (refAEdit);
    makeLabel (lblRefA, cEssentials);
    lblRefHz.setText ("Hz", juce::dontSendNotification); // the SI capital - see ScaleKeyboard
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
    // Note names (off unless the user turned them on in a previous session - the
    // preference lives in the processor's global settings file, not in patch state).
    noteNamesOn = processorRef.getKeyboardNoteNames();
    refreshDegreeNames();
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
    // The TRANSITION cell, before the first paint and NOT left to the first timer tick.
    // configFader made BOTH Time faders visible and they share ONE rectangle, so until
    // this runs the panel is two sliders stacked on each other - which is exactly what
    // RhythmVerify's "no two controls overlap" check caught, in an editor built by a
    // verifier that has no message loop to tick a timer with. After setSize, because it
    // re-lays the cell it just changed.
    applyTransitionMode();
    startTimerHz (30); // also the reconcile rate for stuck/ghost computer-keyboard notes
    // From here on a language switch may re-run the live read-out pass itself (see the
    // tail of applyLanguage). Not before: the applyLanguage() above runs ahead of
    // setSize() and applyEngineState(), and a timer tick there would read a half-built
    // editor and, worse, set lastEngineShown so applyEngineState() would then skip.
    editorConstructed = true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });

    // THE ONE CALL SITE OF THE SELF-SNAPSHOT RIG (source/SnapshotRig.h).
    //
    // juce_add_plugin writes the Standalone's main(), so there is no entry point of ours
    // to put this in; the command line is reachable from here through JUCEApplicationBase.
    // On an ordinary launch this returns after one enum-less test and one StringArray
    // lookup, before touching anything at all: in a plugin wrapper isStandaloneApp() is
    // false and it leaves immediately, and in a standalone with no --snapshot flag it
    // leaves on the next line. Nothing below it runs, nothing is allocated by it, and no
    // state of this editor is read or written - a normal launch behaves exactly as it did
    // before this line existed. It is last in the constructor for the same reason: by the
    // time it can do anything, this editor is fully built.
    fgsnap::runIfRequested();
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
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr (fgdlg::loadScale.en, fgdlg::loadScale.ca), err, this);
    }
    refocus();
}

namespace
{
    // Ask every Slider under `root` to re-render its value box. See the call site at the
    // end of applyLanguage(): a Slider only refreshes that text when its VALUE moves, so
    // a read-out that is a WORD survives a language switch untouched.
    void updateSliderValueTexts (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* s = dynamic_cast<juce::Slider*> (child)) s->updateText();
            updateSliderValueTexts (*child);
        }
    }

    // ...and hand every Slider's tooltip back to the sub-components JUCE gave a COPY of
    // it to. Same shape of defect as the line above, same sweep, and it was found the
    // same way - by a standing check, once tooltips were finally included in one.
    //
    // juce_Slider.cpp:606 does `valueBox->setTooltip (owner.getTooltip())` ONCE, while it
    // is building the text box, and Slider::setTooltip does not propagate afterwards. So
    // every fader in this window whose hover applyLanguage() re-writes kept its
    // CONSTRUCTION-TIME English on the little numeric box beside it, in both languages,
    // for ever: SLOPE, FRACTAL, HARMONICS, the prime limit, PHASE, GAIN, the transition
    // times. Hovering the number said one thing and hovering the fader two pixels away
    // said another. The inc/dec buttons of an IncDecButtons slider get the same copy at
    // juce_Slider.cpp:637, so they are re-pointed here too.
    void pushSliderTooltipsIntoSubComponents (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* s = dynamic_cast<juce::Slider*> (child))
            {
                const auto tip = s->getTooltip();
                // The children of a Slider are JUCE's own - the value box and, in
                // IncDecButtons style, the two arrows. Giving them the slider's tooltip
                // is exactly what JUCE did at construction; this only makes it happen
                // again, now, in the language that is actually on screen.
                for (auto* sub : s->getChildren())
                    if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (sub))
                        t->setTooltip (tip);
            }
            pushSliderTooltipsIntoSubComponents (*child);
        }
    }
}

//==============================================================================
// NOTE NAMES ON THE KEYBOARD.
//
// The keyboard asks for a name per degree through a std::function it may call once per
// key per repaint, so the SPELLING IS CACHED HERE and the function is an array lookup.
// Spelling on demand would rebuild the whole name vector - one std::string per degree -
// dozens of times a second on the message thread, for an answer that only changes when
// the tuning does.
//
// The cache is keyed on the EDO itself (0 = "this tuning is not an equal division of the
// octave"), so it is rebuilt exactly when the answer can have changed: a new .scl, an
// MTS master appearing, the mode switching. `namesOff` is remembered separately from an
// empty cache because "the user turned them off" and "this tuning has no names" are
// different states and the menu has to tell them apart.
void FractalAdditiveAudioProcessorEditor::refreshDegreeNames()
{
    const int edo = noteNamesOn ? processorRef.getEqualDivisionsOfOctave() : 0;
    if (edo == degreeNameEdo)
        return;   // same tuning, same answer - and this runs from resized()

    degreeNameEdo = edo;
    degreeNameCache.clear();
    for (const auto& s : notenames::edoNames (edo))
        degreeNameCache.push_back (juce::String (s));   // ASCII by construction

    // No names to give? Hand the keyboard nothing, and it keeps its numbers. The face
    // may also be null if the embedded font failed to load - same outcome, no boxes.
    if (degreeNameCache.empty() || notesTypeface() == nullptr)
    {
        keyboard.setDegreeNamer (nullptr, {});
        return;
    }
    keyboard.setDegreeNamer (
        [this] (int d) -> juce::String
        {
            // The cache belongs to ONE tuning. If the scale changed under it and nothing
            // has refreshed us yet, handing out its names is not a MISSING label, it is a
            // WRONG one - 12-EDO's letters printed over a seven-note just scale, which no
            // rectangle check can see. The guard is a comparison of two integers, so it is
            // affordable on the per-key path the keyboard calls this from: the cache only
            // ever holds an EDO's worth of names, so a degree count that no longer matches
            // it means the cache is stale and the keys must go back to numbers.
            return (degreeNameEdo > 0
                    && (int) degreeNameCache.size() == degreeNameEdo
                    && processorRef.getScaleDegrees() == degreeNameEdo
                    && juce::isPositiveAndBelow (d, (int) degreeNameCache.size()))
                     ? degreeNameCache[(size_t) d] : juce::String();
        },
        notesTypeface());
}

// Is EVERY degree of this scale a genuine ratio - the "y para JI" half of the request?
// Asked by SOURCE, not by arithmetic: getDegreeIsRational reports what the .scl actually
// wrote, so a degree spelled "3/2" is just and a degree spelled "701.955" is not, however
// close the two sound. A cents approximation of a just scale is a tempered scale, and
// telling its player "this is just intonation" would be a confident lie.
bool FractalAdditiveAudioProcessorEditor::scaleIsJustIntonation() const
{
    const int D = processorRef.getScaleDegrees();
    if (D < 2) return false;
    long long p = 1, q = 1;
    // Degree 0 is the unison in every tuning, so it proves nothing and is skipped; the
    // top index D is the period, which must be just too - a just scale inside a stretched
    // octave is not a JI scale.
    for (int d = 1; d <= D; ++d)
        if (! processorRef.getDegreeIsRational (d, p, q))
            return false;
    return true;
}

// Why the keyboard is showing NUMBERS while the option is on - in words, in the user's
// language. An option that silently does nothing is the thing the task forbade: when
// neither ups-and-downs nor JI applies, the toggle has to SAY so.
juce::String FractalAdditiveAudioProcessorEditor::noteNamesReason() const
{
    if (! noteNamesOn) return {};

    // Built with the language flag directly rather than through tr(): every one of these
    // carries a NUMBER, and tr() takes literals. The Catalan is written with explicit
    // UTF-8 escapes, like every other accented string in this file.
    const int edo = processorRef.getEqualDivisionsOfOctave();
    if (edo == 0)
    {
        // THE OTHER HALF OF THE REQUEST, named out loud. "solo para tunings regulares y
        // para JI" asked for two notations, and this build ships one. A just scale
        // answering only "not an equal division of the octave" would be true and useless:
        // it would read as "your scale is wrong" when what is missing is the FONT. See
        // section 4 of assets/fonts/NOTATION-LICENSES.txt for why HEJI is not here.
        if (scaleIsJustIntonation())
            return catalan
                     ? juce::String::fromUTF8 (
                           "l'entonaci\xc3\xb3 justa es nota amb HEJI, i aquesta versi\xc3\xb3 no en porta els glifs")
                     : juce::String (
                           "just intonation is notated with HEJI, whose glyphs are not in this build");
        return catalan ? juce::String::fromUTF8 ("aquesta afinaci\xc3\xb3 no divideix l'octava en parts iguals")
                       : juce::String ("this tuning is not an equal division of the octave");
    }
    if (edo > notenames::kMaxEdo)
        return catalan ? juce::String::fromUTF8 ("m\xc3\xa9s de ") + juce::String (notenames::kMaxEdo)
                             + juce::String::fromUTF8 (" graus per octava")
                       : juce::String ("more than ") + juce::String (notenames::kMaxEdo)
                             + juce::String (" degrees per octave");
    if (degreeNameCache.empty())
        return catalan ? juce::String (edo) + juce::String::fromUTF8 (
                             " EDO no t\xc3\xa9 cap grafia que doni un nom diferent a cada grau")
                       : juce::String (edo) + juce::String (
                             "-EDO has no spelling that gives every degree a different name");
    if (! keyboard.namesFitTheKeys())
        return catalan ? juce::String::fromUTF8 ("els noms de ") + juce::String (edo)
                             + juce::String::fromUTF8 (" EDO no caben en una tecla")
                       : juce::String ("the names of ") + juce::String (edo)
                             + juce::String ("-EDO are too wide for a key");
    return {};
}

void FractalAdditiveAudioProcessorEditor::setNoteNamesForSnapshot (bool on)
{
    // No preference write - see the declaration. degreeNameEdo is reset so the re-spell
    // is unconditional: the cache is keyed on the EDO alone, and a scenario that loads a
    // scale and then asks for names is exactly the sequence where "same number, nothing
    // to do" would photograph the previous tuning's keys.
    noteNamesOn   = on;
    degreeNameEdo = -1;
    refreshDegreeNames();
    keyboard.repaint();
}

void FractalAdditiveAudioProcessorEditor::setNoteNamesForTests (bool on)
{
    noteNamesOn = on;
    processorRef.setKeyboardNoteNames (on);   // persisted immediately, globally
    refreshDegreeNames();
    keyboard.repaint();
}

FractalAdditiveAudioProcessorEditor::NoteNameProbe
FractalAdditiveAudioProcessorEditor::noteNameProbeForTests() const
{
    NoteNameProbe p;
    p.optionOn     = noteNamesOn;
    p.edo          = processorRef.getEqualDivisionsOfOctave();
    p.namedCount   = (int) degreeNameCache.size();
    p.showing      = keyboard.showingNames();
    p.fits         = keyboard.namesFitTheKeys();
    p.widest       = keyboard.widestDegreeText();
    p.reason       = noteNamesReason();
    // At the NARROWEST key the keyboard ever draws, not the one this window produced.
    // The NAME height, even when numbers are winning: "9.9 pt, needs 8.5" is what tells
    // a reader whether a tuning missed the fit by a hair or by a mile.
    p.fontAtMinKey = keyboard.nameFontAtMinKey();
    p.face         = notesTypeface();
    return p;
}

FractalAdditiveAudioProcessorEditor::NoteNameProbe
FractalAdditiveAudioProcessorEditor::noteNameProbeInLanguage (bool useCatalan)
{
    const bool was = catalan;
    catalan = useCatalan;
    applyLanguage();
    resized();
    auto p = noteNameProbeForTests();
    catalan = was;
    applyLanguage();
    resized();
    return p;
}

std::vector<ScaleKeyboard::PeriodTag>
FractalAdditiveAudioProcessorEditor::keyboardPeriodTagsInLanguage (bool useCatalan)
{
    const bool was = catalan;
    catalan = useCatalan;
    applyLanguage();
    resized();
    auto t = keyboard.periodTagProbe();
    catalan = was;
    applyLanguage();
    resized();
    return t;
}

void FractalAdditiveAudioProcessorEditor::applyLanguage()
{
    using juce::dontSendNotification;
    langButton.setButtonText (catalan ? "ENG" : "CAT");
    // The credit line. It was set once in the constructor and never again, so it was the
    // one string in the title bar that could not change language.
    vibeLabel.setText (tr ("Vibe coded by", "Vibe coded per"), dontSendNotification);
    menuButton.setButtonText (tr ("Menu", "Men\xc3\xba"));
    rhythmBtn.setButtonText (tr ("Spectrum vs Rhythm", "Espectre vs ritme"));
    partialEditBtn.setTooltip (tr (
        "Draw the spectrum by hand: a resizable window with *one knob per partial* of the current sound. Every partial is there, however many - nothing is hidden or capped.\nThe switch below is the same one as inside the window; while it is on, !SLOPE is frozen!.",
        "Dibuixa l'espectre a m\xc3\xa0: una finestra redimensionable amb *un control per parcial* del so actual. Hi ha tots els parcials, siguin quants siguin - res amagat ni limitat.\nL'interruptor de sota \xc3\xa9s el mateix que el de dins la finestra; mentre estigui actiu, !el PENDENT queda congelat!."));
    partialManualT.setTooltip (tr (
        "Manual per-partial amplitudes. The SAME switch as the one inside the pencil's window. Off by default, but your first drag on a partial knob switches it on for you (otherwise that drag would be stored and never heard); while it is on, !SLOPE is frozen! where it stood and the amplitudes come from the knobs.",
        "Amplituds manuals per parcial. \xc3\x89s EL MATEIX interruptor que el de dins la finestra del llapis. Apagat per defecte, per\xc3\xb2 el teu primer arrossegament d'un parcial l'activa (si no, aquell arrossegament es desaria i no sonaria mai); mentre estigui actiu, !el PENDENT queda congelat! on era i les amplituds venen dels controls."));
    if (partialWindow != nullptr)
        partialWindow->setName (tr ("Fibl\xc3\xb3 - partial amplitudes", "Fibl\xc3\xb3 - amplituds dels parcials"));
    partialPanel.applyLanguage();
    lastTransitionShown = -1;     // ...and the TRANSITION cell, so the re-worded item is re-selected
    lastManualFrozenShown = -1;   // re-apply the slope fader's tooltip in the new language
    lastEngineShown       = -1;   // and re-light the engine chips with the new words
    rhythmPanel.heading.setText (tr ("SPECTRUM VS RHYTHM", "ESPECTRE VS RITME"), dontSendNotification);
    rhythmPanel.outLbl.setText  (tr ("MIDI out (to DAW)", "Sortida MIDI (al DAW)"), dontSendNotification);
    rhythmPanel.selfLbl.setText (juce::String::fromUTF8 (catalan ? "Sona al Fibl\xc3\xb3" : "Play in Fibl\xc3\xb3"), dontSendNotification);
    // SIX CONTROLS THAT EXPLAINED NOTHING. Found by asking the probe which components it
    // records WITHOUT a Tooltip row beside them: seven of seventy-nine, and one of them
    // is the box that decides what the whole Spectrum-vs-Tuning panel does. The two
    // symbols in the rhythm panel are the worst of it - "x2" and "/2" name an operation
    // without naming what it operates on - and "X" is a glyph with no word at all.
    // (Menu is the seventh and is left alone: the word IS the explanation.)
    // ...AND THE FOUR IN THE MODULATORS PANEL, which the same sweep found in the custom
    // view: the box that says WHICH modulator you are editing, and the three that shape
    // an LFO. A phrase like "Fundamental / 2^n" names a rule without saying what it does.
    lblLfoPhase.setText (tr ("Phase", "Fase"), dontSendNotification);
    modSelBox.setTooltip (tr (
        "WHICH modulator these controls edit. Env 1-3 are envelopes (they follow a note); "
        "LFO 1-3 run continuously. Each keeps its own targets and settings.",
        "QUIN modulador editen aquests controls. Env 1-3 s\xc3\xb3n envolupants (segueixen una "
        "nota); LFO 1-3 van cont\xc3\xad" "nuament. Cadascun mant\xc3\xa9 els seus destins."));
    lfoWaveBox.setTooltip (tr (
        "The LFO's shape. 'Custom' opens an editor beside this box where you draw "
        "the wave yourself, point by point.",
        "La forma de l'LFO. 'Pr\xc3\xb2pia' obre un editor al costat d'aquesta "
        "caixa on dibuixes l'ona tu mateix, punt a punt."));
    lfoRateModeBox.setTooltip (tr (
        "WHAT SETS THE SPEED. BPM / 2^n locks to the project tempo, halving or doubling. "
        "Fundamental / 2^n locks to the played note's own frequency, so the LFO stays in "
        "step with the pitch. Free Hz ignores both.",
        "QU\xc3\x88 FIXA LA VELOCITAT. BPM / 2^n es lliga al tempo del projecte, dividint o "
        "doblant. Fundamental / 2^n es lliga a la freq\xc3\xbc\xc3\xa8ncia de la nota tocada, "
        "o sigui que l'LFO va al pas del to. Free Hz no fa cas de cap dels dos."));
    lfoPhaseModeBox.setTooltip (tr (
        "WHERE THE WAVE STARTS. 'Restart each note' begins every note at the same point of "
        "the wave; 'Project transport' leaves it running with the song, so two notes a bar "
        "apart hear different parts of it.",
        "ON COMEN\xc3\x87" "A L'ONA. 'Reinicia a cada nota' comen\xc3\xa7" "a cada nota al "
        "mateix punt de l'ona; 'Transport del projecte' la deixa c\xc3\xb3rrer amb la "
        "can\xc3\xa7\xc3\xb3, o sigui que dues notes separades per un comp\xc3\xa0s en senten "
        "trossos diferents."));
    specModeBox.setTooltip (tr (
        "WHAT THIS PANEL DOES. Find = search for tunings whose notes beat least (or most) "
        "against the spectrum you have. Adapt = keep the tuning and move the SPECTRUM's "
        "partials to it instead. From spectrum's segment = build a scale out of the "
        "partials themselves.",
        "QU\xc3\x88 FA AQUEST PANELL. Troba = cerca afinacions les notes de les quals baten "
        "menys (o m\xc3\xa9s) contra l'espectre que tens. Adapta = mant\xc3\xa9 l'afinaci\xc3\xb3 "
        "i mou els parcials de l'ESPECTRE cap a ella. Del segment de l'espectre = construeix "
        "una escala amb els mateixos parcials."));
    tuneSourceBox.setTooltip (tr (
        "WHICH NOTES the search listens to: the tuning loaded now, every tuning in the bank, "
        "or the chord you are holding.",
        "QUINES NOTES escolta la cerca: l'afinaci\xc3\xb3 carregada ara, totes les del banc, "
        "o l'acord que estiguis prement."));
    tuningBankAddBtn.setTooltip (tr (
        "Put the tuning loaded now into the bank, so 'From all tunings' can search it and "
        "the Bank box can reload it later.",
        "Posa l'afinaci\xc3\xb3 carregada ara al banc, perqu\xc3\xa8 'De totes les afinacions' "
        "la pugui cercar i la caixa Banc la pugui tornar a carregar."));
    rhythmPanel.tempoX2.setTooltip (tr (
        "Send the DAW a tempo twice as fast for the same pulse - use it when the transport "
        "locks to half the speed you meant.",
        "Envia al DAW un tempo el doble de r\xc3\xa0pid per al mateix pols - fes-ho servir quan "
        "el transport es lliga a la meitat de la velocitat que volies."));
    rhythmPanel.tempoD2.setTooltip (tr (
        "Send the DAW a tempo half as fast for the same pulse - use it when the transport "
        "locks to twice the speed you meant.",
        "Envia al DAW un tempo la meitat de r\xc3\xa0pid per al mateix pols - fes-ho servir quan "
        "el transport es lliga al doble de la velocitat que volies."));
    rhythmPanel.closeB.setTooltip (tr ("Close this panel.", "Tanca aquest panell."));
    rhythmPanel.droneT.setButtonText (tr ("Note under pulses", "Nota sota els polsos"));
    rhythmPanel.droneT.setTooltip (tr (
        "With \"Play in Fibló\" on, ALSO sound the held note normally underneath the pulses. Off = only the rhythm sounds.",
        "Amb \"Sona al Fibl\xc3\xb3\" actiu, fes sonar TAMB\xc3\x89 la nota mantinguda normal sota els polsos. Desactivat = nom\xc3\xa9s sona el ritme."));
    rhythmPanel.feedbackT.setButtonText (tr ("Fractal feedback", "Feedback fractal"));
    rhythmPanel.feedbackT.setTooltip (tr (
        "Play in Fibló only: every pulse spawns the WHOLE set of routed trains one level down, anchored at ITS pitch (rates and intervals rebuilt from it), with its velocity faded by the slope and its gate inherited. Those pulses spawn again -> a self-similar fractal rhythm that always dies out (never a runaway).",
        "Nom\xc3\xa9s amb Sona al Fibl\xc3\xb3: cada pols genera TOT el conjunt de trens encaminats un nivell avall, ancorat al SEU to (velocitats i intervals reconstru\xc3\xafts des d'ell), amb la seva velocity esva\xc3\xaf" "da pel pendent i el seu gate heretat. Aquells polsos tornen a generar -> un ritme fractal autosimilar que sempre s'apaga (mai descontrolat)."));
    rhythmPanel.feedbackLbl.setText (tr ("Decay", "Deca\xc3\xafment"), dontSendNotification);
    rhythmPanel.feedbackS.setTooltip (tr (
        "How much of each repeat's velocity carries to the next loop. 50% = halve each time (short tail); higher = a longer, louder tail.",
        "Quanta velocity de cada repetici\xc3\xb3 passa al seg\xc3\xbc" "ent bucle. 50% = meitat cada cop (cua curta); m\xc3\xa9s alt = cua m\xc3\xa9s llarga i forta."));
    rhythmPanel.selfT.setTooltip (tr (
        "This synth plays the rhythm itself: the played note becomes a trigger and only the pulses sound. On by default; it engages once you route a partial. No feedback loop: pulses never spawn pulses.",
        "Aquest sinte toca el ritme ell mateix: la nota tocada esdev\xc3\xa9 un activador i nom\xc3\xa9s sonen els polsos. Actiu per defecte; s'activa quan encamines un parcial. Sense bucle: els polsos mai generen polsos."));
    // EVERY CONTROL IN A ROUTED ROW SAYS WHAT IT DOES. There are five of them per train
    // and not one had a tooltip: the pitch box, the two ±12 buttons, the velocity fader,
    // the gate fader and the delete. They are the controls a user touches MOST in this
    // panel and the only ones it never explained - and nothing had noticed, because no
    // picture had ever contained a routed row (scenario 53 is the first).
    //
    // Set per row, in the language currently on screen, alongside every other string the
    // panel localises.
    for (int i = 0; i < RhythmEngine::kSlots; ++i)
    {
        rhythmPanel.pitchE[i].setTooltip (tr (
            "WHAT THIS TRAIN PLAYS. A number of scale steps from the played note "
            "(\"7step\" up, \"-7step\" down), or a ratio (\"3/2\"), or a note name. "
            "Steps follow the loaded tuning; a ratio does not.",
            "QU\xc3\x88 TOCA AQUEST TREN. Un nombre de graus de l'escala des de la nota "
            "tocada (\"7step\" amunt, \"-7step\" avall), o una ra\xc3\xb3 (\"3/2\"), o un nom "
            "de nota. Els graus segueixen l'afinaci\xc3\xb3 carregada; una ra\xc3\xb3 no."));
        rhythmPanel.s12M[i].setTooltip (tr ("An octave down, without retyping the box.",
                                            "Una octava avall, sense reescriure la caixa."));
        rhythmPanel.s12P[i].setTooltip (tr ("An octave up, without retyping the box.",
                                            "Una octava amunt, sense reescriure la caixa."));
        rhythmPanel.velS[i].setTooltip (tr (
            "MIDI velocity of this train's pulses (1-127). The knob over the VEL column "
            "tilts every train at once.",
            "Velocitat MIDI dels polsos d'aquest tren (1-127). El comandament sobre la "
            "columna VEL els inclina tots alhora."));
        rhythmPanel.durS[i].setTooltip (tr (
            "How long each pulse of this train is held, in milliseconds - the gate. Short "
            "is a click, long is a note.",
            "Quant dura cada pols d'aquest tren, en mil\xc2\xb7lisegons - la porta. Curt \xc3\xa9s "
            "un clic, llarg \xc3\xa9s una nota."));
        rhythmPanel.delB[i].setTooltip (tr ("Stop routing this partial.",
                                            "Deixa d'encaminar aquest parcial."));
    }
    rhythmPanel.velHead.setText (tr ("VEL", "VEL"), dontSendNotification);
    rhythmPanel.velHead.setTooltip (tr ("MIDI velocity of every pulse this train sends (1-127).",
                                        "Velocity MIDI de cada pols que envia aquest tren (1-127)."));
    rhythmPanel.octLbl.setText (tr ("Octave (all)", "Octava (tots)"), dontSendNotification);
    for (auto* b : { &rhythmPanel.octAllUp, &rhythmPanel.octAllDn })
        b->setTooltip (tr ("Shift ALL pulses together by whole octaves (-4..+4). Ratio pitches move by exact 2/1s; steps pitches by whole periods of the scale.",
                           "Despla\xc3\xa7""a TOTS els polsos alhora per octaves senceres (-4..+4). Els tons en ratio es mouen per 2/1 exactes; els de passos per per\xc3\xado" "des sencers de l'escala."));
    // Short enough for the 236 px column it sits over. The long form - "of the current
    // patch" - was 28 px wider than the column in English and 43 in Catalan, so it was
    // drawn condensed; it lives in the hover now, where it has all the room it wants.
    rhythmPanel.listHead.setText (tr ("Partials (click to route)",
                                      "Parcials (clica per encaminar)"), dontSendNotification);
    rhythmPanel.listHead.setTooltip (tr ("The partials of the patch loaded right now. Click one to route it "
                                         "to a pulse train.",
                                         "Els parcials del patch carregat ara mateix. Clica'n un per "
                                         "encaminar-lo a un tren de polsos."));
    rhythmPanel.pickHint.setText (juce::String::fromUTF8 (catalan
                                      ? "\xe2\x9f\xb5 Clica un parcial per encaminar-lo"
                                      : "\xe2\x9f\xb5 Click a partial to route it"), dontSendNotification);
    rhythmPanel.onT.setButtonText (tr ("On", "Actiu"));
    // THE ONE GROUP THAT HAD NO NAME. secPlay existed - it was constructed, coloured,
    // given a font and laid out at the top of its box - and no line ever put a word in
    // it, so the first frame in the panel was a bordered rectangle with a blank caption
    // strip while the four below it were captioned. That is why the audit reported "a
    // bordered box with NO CAPTION AT ALL": not a missing frame, a missing string. It is
    // invisible from inside the code (an empty Label draws nothing and reports no fault)
    // and obvious in a photograph, which is what the photographs are for.
    rhythmPanel.secPlay.setText  (tr ("PLAYBACK", "REPRODUCCI\xc3\x93"), dontSendNotification);
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
        "Monophonic: a new note stops the one sounding. Off = polyphonic.\nThis one CUTS the old note (a restrike at the new pitch). \"Monophonic glide\" on the PITCH tab BENDS it to the new one instead, and while that is on this greys out, because cutting the note first is exactly what stops it gliding.",
        "Monof\xc3\xb2nic: una nota nova atura la que sona. Desactivat = polif\xc3\xb2nic.\nAquest TALLA la nota antiga (un atac nou a la nova al\xc3\xa7""ada). El \"Glide monof\xc3\xb2nic\" de la pestanya PITCH la DOBLEGA fins a la nova, i mentre aquell estigui actiu aquest queda desactivat, perqu\xc3\xa8 tallar la nota primer \xc3\xa9s justament el que impedeix que llisqui."));
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
    // The column header over the pitch boxes, 165 px wide: the full Catalan sentence
    // measured 167 and the English 157 against 155 of usable width, so both were drawn
    // condensed. Shortened here and stated in full in the hover below.
    rhythmPanel.noteHead.setText (tr ("Steps from the note", "Passos des de la nota"),
                                  dontSendNotification);
    rhythmPanel.noteHead.setTooltip (tr (
        "Each train sends the PLAYED note shifted by this many scale steps: \"7st\" = 7 up, \"7st-\" = 7 down. Never a fixed note. With MPE out, ratios (\"3/2\") and cents (\"702c\") are also FROM the played note.",
        "Cada tren envia la nota TOCADA despla\xc3\xa7""ada aquests passos d'escala: \"7st\" = 7 amunt, \"7st-\" = 7 avall. Mai una nota fixa. Amb MPE out, els ratios (\"3/2\") i cents (\"702c\") tamb\xc3\xa9 s\xc3\xb3n DES DE la nota tocada."));
    // GATE stays GATE in both languages: it is the universal name of the MIDI note
    // length on every sequencer, Catalan ones included. Written as an explicit tr() pair
    // rather than a bare literal so the en/ca drawn-text assertion can tell a DECISION
    // from an oversight - a raw literal here would look exactly like the "Steps" bug.
    rhythmPanel.durHead.setText (tr ("GATE", "GATE"), dontSendNotification);
    rhythmPanel.durHead.setTooltip (tr (
        "GATE: how long each pulse's MIDI note lasts (5-500 ms). It is NOT latency - pulse timing stays sample-locked; this only sets where the note-off lands.",
        "GATE: quant dura la nota MIDI de cada pols (5-500 ms). NO \xc3\xa9s lat\xc3\xa8ncia - els polsos queden clavats a la mostra; nom\xc3\xa9s fixa on cau el note-off."));
    // Help popup text (the "?" button). fromUTF8 directly: tr()'s English branch
    // does not decode UTF-8 accents.
    rhythmPanel.helpTitle = tr (fgdlg::rhythmHelp.en, fgdlg::rhythmHelp.ca);
    rhythmPanel.helpText = juce::String::fromUTF8 (catalan
        ? "ENCAMINAMENT (qualsevol DAW):\nEn una altra pista d'instrument, tria com a ENTRADA MIDI la SORTIDA MIDI d'aquest plugin (no l'entrada de la pista del Fibl\xc3\xb3!) i activa el monitoratge d'entrada.\nA Ableton: MIDI From -> pista del Fibl\xc3\xb3 -> 2n selector \"Fibl\xc3\xb3 Gal\xc3\xa0" "ctic\" -> Monitor In.\n\nTEMPO DEL DAW (CC20):\n. Ableton (f\xc3\xa0" "cil): arrossega el dispositiu Max for Live \"Fibl\xc3\xb3 Tempo Sync\" (a User Library > Presets > MIDI Effects) a una pista MIDI que rebi la sortida del Fibl\xc3\xb3, i activa Sync de tempo. Sense mapejar res: el tempo ix el pols 1.\n. Altres DAW (Reaper, FL...): instal\xc2\xb7la loopMIDI (gratu\xc3\xaft), crea un port; envia la sortida MIDI del Fibl\xc3\xb3 a aquest port; al DAW, mapeja el tempo a l'entrada loopMIDI (rang 20-147) i activa Sync de tempo.\n\nLAT\xc3\x88NCIA: els polsos surten clavats a la mostra; el DAW pot afegir un buffer al MIDI entre pistes - fes servir un buffer petit."
        : "ROUTING (any DAW):\nOn another instrument track, choose this plugin's MIDI OUTPUT as the track's MIDI INPUT (not the Fibl\xc3\xb3 track's own input!) and enable input monitoring.\nIn Ableton: MIDI From -> the Fibl\xc3\xb3 track -> 2nd selector \"Fibl\xc3\xb3 Gal\xc3\xa0" "ctic\" -> Monitor In.\n\nDAW TEMPO (CC20):\n. Ableton (easy): drag the Max for Live device \"Fibl\xc3\xb3 Tempo Sync\" (in User Library > Presets > MIDI Effects) onto a MIDI track that receives the Fibl\xc3\xb3's MIDI output, and turn Tempo sync on. No mapping needed - the tempo follows pulse 1.\n. Other DAWs (Reaper, FL...): install loopMIDI (free), create a port; send the Fibl\xc3\xb3's MIDI output to that port; in the DAW, MIDI-map the tempo to the loopMIDI input (range 20-147) and turn Tempo sync on.\n\nLATENCY: pulses leave sample-locked; the DAW may add one buffer to inter-track MIDI - use a small buffer size.");
    rhythmPanel.mpeT.setButtonText ("MPE out");
    rhythmPanel.mpeT.setTooltip (tr (
        "Each pulse train gets its own MIDI channel with per-note pitch bend, and the pitch boxes also accept cents (\"1987.4c\") or ratios (\"3\", \"3/2\") - sent RETUNED so an MPE-capable synth plays the exact pitch (assumes the receiver's bend range is +-48, the MPE default).",
        "Cada tren de polsos t\xc3\xa9 el seu canal MIDI amb pitch bend per nota, i les caselles de to tamb\xc3\xa9 accepten cents (\"1987.4c\") o ratios (\"3\", \"3/2\") - s'envien REAFINATS perqu\xc3\xa8 un sinte amb MPE toqui el to exacte (assumeix rang de bend +-48 al receptor, el defecte MPE)."));
    rhythmPanel.modeBox.setTooltip (tr (
        "Lowest note: only the lowest held note drives the pulses. All notes: every held note drives its own copy of the trains, transposed by its interval from the lowest.",
        "Nota m\xc3\xa9s greu: nom\xc3\xa9s la nota m\xc3\xa9s greu mant\xc3\xa9 els polsos. Totes les notes: cada nota mantinguda porta la seva c\xc3\xb2pia dels trens, transposada pel seu interval respecte a la m\xc3\xa9s greu."));
    rhythmPanel.tempoT.setButtonText (tr ("Tempo sync (CC20)", "Sync de tempo (CC20)"));
    rhythmPanel.tempoT.setTooltip (tr (
        "The DAW tempo FOLLOWS pulse 1 (x2 / /2 to scale). NEEDS a one-time setup: in Ableton, drop the free \"Fibló Tempo Sync\" Max for Live device on a MIDI track that receives this plugin's MIDI output. In other DAWs, MIDI-map the tempo to CC20 with a 20-147 BPM range. Full steps: the \"How to route\" button.",
        "El tempo del DAW SEGUEIX el pols 1 (x2 / /2 per escalar). CAL una configuraci\xc3\xb3 \xc3\xbanica: a Ableton, posa el dispositiu Max for Live gratu\xc3\xaft \"Fibl\xc3\xb3 Tempo Sync\" en una pista MIDI que rebi la sortida MIDI del plugin. En altres DAW, mapeja el tempo al CC20 amb rang 20-147 BPM. Passos complets: el bot\xc3\xb3 \"How to route\"."));
    for (int i = 0; i < RhythmEngine::kSlots; ++i)
    {
        rhythmPanel.pitchE[i].setTooltip (tr (
            "Scale steps FROM the played note: \"7st\" = 7 up, \"7st-\" = 7 down (never a fixed note). With MPE out on, also cents (\"1987.4c\") or a ratio (\"3\", \"3/2\") above the played note, sent retuned via per-note bend.",
            "Passos d'escala DES DE la nota tocada: \"7st\" = 7 amunt, \"7st-\" = 7 avall (mai una nota fixa). Amb MPE out actiu, tamb\xc3\xa9 cents (\"1987.4c\") o un ratio (\"3\", \"3/2\") sobre la nota tocada, enviat reafinat amb bend per nota."));
        rhythmPanel.durS[i].setTooltip (tr (
            "GATE: how long each pulse's MIDI note lasts (5-500 ms). It is NOT latency - pulse timing stays sample-locked; this only sets where the note-off lands.",
            "GATE: quant dura la nota MIDI de cada pols (5-500 ms). NO \xc3\xa9s lat\xc3\xa8ncia - els polsos queden clavats a la mostra; nom\xc3\xa9s fixa on cau el note-off."));
    }
    rhythmPanel.onT.setTooltip (tr (
        "MIDI OUT: send the rhythm pulses out of the plugin to another DAW track. Off by default, and independent of \"Play in Fibló\".",
        "SORTIDA MIDI: envia els polsos de ritme fora del plugin cap a una altra pista del DAW. Desactivat per defecte, i independent de \"Sona al Fibl\xc3\xb3\"."));
    langButton.setTooltip (tr ("Switch the interface language to Catal\xc3\xa0.",
                               "Torna la interf\xc3\xad" "cie a l'angl\xc3\xa8s."));

    gFilter.setText        (tr ("Filter", "Filtre"));
    gAmpEnv.setText        (tr ("Modulators", "Moduladors")); // shared chip-tabbed box
    gPitchEnv.setText      (tr ("Pitch Envelope", "Envolupant to"));
    // THE MODULATOR TAB STRIP, and the one rule this window applies to a panel word:
    // translate it unless the Catalan word IS the English one. AMP is the same
    // abbreviation in both (amplitud), so it stays; PITCH and FILTER are not, so they
    // move; and CUSTOM becomes PROPI here for the same reason the engine chip below
    // already says PROPI - one English word printing as two different Catalan words in
    // two chip rows of the same window is the inconsistency this pass exists to end.
    envTabB[0].setButtonText (tr ("AMP",    "AMP"));
    envTabB[1].setButtonText (tr ("PITCH",  "TO"));
    envTabB[2].setButtonText (tr ("FILTER", "FILTRE"));
    envTabB[3].setButtonText (tr ("CUSTOM", "PROPI"));
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
    modOnT.setTooltip (tr ("This modulator on/off (off restores its targets to their own values).",
                           "Aquest modulador on/off (off retorna els seus dest\xc3\xadns al seu valor propi)."));
    // THE CUSTOM TAB'S OWN FADERS, and the pitch envelope's amount. Every one of these
    // was handed a bare English literal by configFader in the constructor and never
    // re-set, so the whole CUSTOM modulator panel explained itself in English however
    // the window was set - the last corner of the editor still doing cause (a). Found by
    // the "translated?" gate the moment tooltips were let into it.
    pEnvAmtFader.setTooltip (tr (
        "How far the pitch envelope bends the pitch (semitones, \xc2\xb1).",
        "Quant desafina la nota l'envolupant de to (semitons, \xc2\xb1)."));
    for (auto* a : { &custAdsr[0], &custAdsr[1], &custAdsr[2], &custAdsr[3] })
        // Same wording as the volume / filter envelopes six lines further down: the four
        // stage names are the terms of art this window already uses in Catalan, and one
        // envelope spelling them differently from the other three would be the
        // inconsistency, not the translation.
        a->setTooltip (tr ("Custom envelope: Attack, Decay, Sustain (level), Release. Times in seconds.",
                           "Envolupant pr\xc3\xb2pia: Atac, Decay, Sustain (nivell), Release. Temps en segons."));
    custRangeFader.setTooltip (tr (
        "How far this modulator MOVES its mapped parameters (\xc2\xb1 of their full range), riding on each knob's own value. Note: parameters that REBUILD the spectrum (Base, Harmonics, Decay, Fractal, Prime, Range, Reps) follow the modulator at the 60 Hz rebuild rate and glide when Morph is on - everything else follows per audio block.",
        "Quant MOU aquest modulador els par\xc3\xa0metres que t\xc3\xa9 assignats (\xc2\xb1 del seu rang complet), cavalcant sobre el valor propi de cada control. Nota: els par\xc3\xa0metres que RECONSTRUEIXEN l'espectre (Base, Harm\xc3\xb2nics, Caiguda, Fractal, Primer, Rang, Reps) segueixen el modulador al ritme de reconstrucci\xc3\xb3 de 60 Hz i llisquen amb el Morphing actiu - la resta segueix bloc a bloc."));
    lfoDivSlider.setTooltip (tr (
        "Rate divider: halves the base rate per step (base = project BPM or the lowest held note's frequency).",
        "Divisor de velocitat: parteix per la meitat la velocitat base a cada pas (la base \xc3\xa9s el tempo del DAW o la freq\xc3\xbc\xc3\xa8ncia de la nota m\xc3\xa9s greu que sona)."));
    lfoHzSlider.setTooltip (tr ("Free LFO rate in Hz.", "Velocitat lliure de l'LFO, en Hz."));
    lfoPhaseSlider.setTooltip (tr (
        "Phase offset: where in the wave the oscillation starts.",
        "Despla\xc3\xa7""ament de fase: en quin punt de l'ona comen\xc3\xa7""a l'oscil\xc2\xb7laci\xc3\xb3."));
    temperToggle.setButtonText (tr ("Temper to tuning", "Tempera a l'afinaci\xc3\xb3"));
    temperToggle.setTooltip (tr (
        "ON: the HARMONIC ENGINE keeps generating partials, each snapped to the nearest pitch of the ACTIVE TUNING in the Tuning box (degree x period^k) and following it live - both engines work TOGETHER and the Custom Engine's own scale is bypassed. In MPE input mode the host hands the plugin no scale, so the grid there is plain 12-TET. OFF: no snapping (the Custom Engine's .scl, if loaded and ON, is the spectrum by itself).",
        "ON: el MOTOR HARM\xc3\x92NIC segueix generant parcials, cadascun ajustat al to m\xc3\xa9s proper de l'AFINACI\xc3\x93 ACTIVA de la caixa Tuning (grau x per\xc3\xado" "de^k) i seguint-la en viu - els dos motors treballen JUNTS i l'escala del Motor propi queda de banda. En mode d'entrada MPE l'amfitri\xc3\xb3 no d\xc3\xb3na cap escala al plugin, aix\xc3\xad que all\xc3\xa0 la graella \xc3\xa9s 12-TET. OFF: sense ajust (l'.scl del Motor propi, si est\xc3\xa0 carregat i actiu, \xc3\xa9s l'espectre per si mateix)."));
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
    // "Disson\xc3\xa0ncia", with its accent: every other Catalan use of the word in this file
    // carries it, and this one - a GROUP TITLE, drawn upper-cased in a chip - was the one
    // that did not. Neither language gate can see it: it is spelled the same either way
    // to a byte-compare, and a missing accent is not a missing translation.
    gScaleDiss.setText     (tr ("Scale Dissonance", "Disson\xc3\xa0ncia de l'escala"));
    segPeriodLbl.setText   (tr ("Period", "Per\xc3\xado" "de"), juce::dontSendNotification);
    segTopToggle.setButtonText (tr ("= top", "= sup"));
    segTopToggle.setTooltip (tr ("Use the highest selected partial as the period (instead of the typed value).",
                                 "Fes servir el parcial seleccionat m\xc3\xa9s agut com a per\xc3\xado" "de (en lloc del valor escrit)."));
    // Short enough for the two lines the box really has (192 px at 10.5 px, so ~380 px of
    // room): the old sentence needed 529 and was drawn condensed. "Lowest = tonic" says
    // what "Lowest selected = tonic" said - only selected partials are in the scale.
    segHelpLbl.setText (tr ("Click a partial to add it; Shift-click a range. Lowest = tonic (C3).",
                            "Clica un parcial per afegir-lo; Shift-clic un rang. El m\xc3\xa9s greu = t\xc3\xb2nica (C3)."),
                        juce::dontSendNotification);
    segList.tip = tr ("Each partial of the current spectrum. Click to add/remove it as a scale degree (live). Shift-click selects the span to the last click. The lowest selected is the tonic (C3); all selected share one colour.",
                      "Cada parcial de l'espectre actual. Clica per afegir/treure'l com a grau de l'escala (en viu). Shift-clic selecciona el tram fins a l'\xc3\xbaltim clic. El m\xc3\xa9s greu seleccionat \xc3\xa9s la t\xc3\xb2nica (C3); tots els seleccionats comparteixen color.");

    lblBaseN.setText     ("N", dontSendNotification);
    lblBasePitch.setText (tr ("Pitch", "To"), dontSendNotification);
    lblPrimeLimit.setText(tr ("Limit", "L\xc3\xadmit"), dontSendNotification);
    lblCutoff.setText    (tr ("Cut", "Tall"), dontSendNotification);
    // NOT "Res" in Catalan: beside "Tall" (cut) it reads as the Catalan word for
    // NOTHING, which is the opposite of what a resonance control does.
    lblRes.setText       (tr ("Res", "Ress."), dontSendNotification);
    lblEnvAmt.setText    (tr ("Env", "Env"), dontSendNotification);
    lblPitchAmt.setText  (tr ("St", "St"), dontSendNotification);
    lblGlide.setText     (tr ("Glide", "Glide"), dontSendNotification);
    monoGlideT.setButtonText (tr ("Monophonic glide", "Glide monof\xc3\xb2nic"));
    monoGlideT.setTooltip (tr (
        "ON, the synth plays ONE note at a time - it cannot make chords - and a new key does not start a new voice: it TAKES OVER the sounding one and BENDS its pitch there over the Glide time, through the tuning's own frequencies (per-note bend included), never through 12-TET semitones.\nLAST-NOTE priority: the newest key wins, so a second note always moves the pitch.\nLegato does NOT retrigger: while a key is held, the takeover keeps the envelopes, the modulators and a running Shepard exactly where they are. Lift every key and the next note starts fresh, at its own pitch, with everything retriggered.\nRelease the key you are on while another is still down and the pitch glides BACK to the newest key still held.\nIn playground MIX mode every corner engine is mono too, on the same note stream - \"Mix Voices per Corner\" reads 1 and waits while this is on.",
        "Actiu, el sinte toca UNA nota alhora - no pot fer acords - i una tecla nova no engega cap veu nova: PREN la que sona i li DOBLEGA l'al\xc3\xa7""ada fins all\xc3\xa0 en el temps de Glide, per les freq\xc3\xbc\xc3\xa8ncies de la mateixa afinaci\xc3\xb3 (amb el bend per nota inclos), mai per semitons de 12-TET.\nPrioritat a l'\xc3\x9aLTIMA nota: la tecla m\xc3\xa9s nova guanya, o sigui que una segona nota sempre mou l'al\xc3\xa7""ada.\nEl legato NO redispara: mentre mantens una tecla, la presa de rel\xc3\xa8u deixa les envolupants, els moduladors i un Shepard en marxa exactament on eren. Deixa anar totes les tecles i la nota seg\xc3\xbc" "ent comen\xc3\xa7""a de nou, a la seva al\xc3\xa7""ada i amb tot redisparat.\nDeixa anar la tecla on ets mentre en mantens una altra i l'al\xc3\xa7""ada torna lliscant a la tecla m\xc3\xa9s nova que encara mantens.\nEn mode MIX del playground cada motor de cantonada tamb\xc3\xa9 \xc3\xa9s mono, sobre el mateix flux de notes - \"Veus per cantonada\" marca 1 i espera mentre aix\xc3\xb2 estigui actiu."));
    glideFader.setTooltip (tr (
        "How long the pitch takes to travel from the previous note to the new one, in milliseconds. It is a TIME, not a rate: two octaves take exactly as long as a semitone. The fader is logarithmic (1 ms to 5 s, half its travel below 100 ms). Only does anything while Monophonic glide is on.",
        "Quant triga l'al\xc3\xa7""ada a anar de la nota anterior a la nova, en millisegons. \xc3\x89s un TEMPS, no una velocitat: dues octaves triguen exactament el mateix que un semit\xc3\xb2. El fader \xc3\xa9s logar\xc3\xadtmic (1 ms a 5 s, mitja cursa per sota de 100 ms). Nom\xc3\xa9s fa res amb el Glide monof\xc3\xb2nic actiu."));
    lblTransTime.setText (tr ("Time", "Temps"), dontSendNotification);
    // THE TRANSITION ITEMS, re-worded in place. The ids are fixed (1 Off, 2 Morph,
    // 3 Dry/Wet) and the selection is put back explicitly, because changeItemText leaves
    // the box's displayed label disagreeing with its selected item for as long as it
    // takes to finish - and getSelectedId() reads 0 through exactly that window. A
    // language switch that dropped the selection would be the whole cell going blank
    // (RhythmVerify's combo sweep asserts no box loses its selection across two
    // round-trips of both languages, which is what caught that class of bug before).
    {
        const int keep = transitionBox.getSelectedId();
        transitionBox.clear (dontSendNotification);
        transitionBox.addItem (tr ("Off",     "Cap"),          1);
        transitionBox.addItem (tr ("Morph",   "Morphing"),     2);
        transitionBox.addItem (tr ("Dry/Wet", "Sec/Humit"),    3);
        transitionBox.setSelectedId (keep > 0 ? keep : 3, dontSendNotification);
    }
    // ONE tooltip where there were two, because there is one control. It must name the
    // SCOPE - a transition rides on every spectrum change from any source, and users read
    // it as a Base-only effect because Base is where they first hear it - and it must say
    // what each of the three items does, including Off, which is the one that has no
    // Time and no visible sign that it is doing its job.
    transitionBox.setTooltip (tr (
        "How a spectrum CHANGE lands - for ANY reason it changes: Base, Harmonics, Prime, Fractal, Range, switching engine, loading or typing a scale, an adapted spectrum from Spectrum vs Tuning, a modulator moving any of those.\n*Off* - the change lands instantly.\n*Morph* - every partial GLIDES to its place in the new spectrum, over the Time beside this. A per-partial pitch bend: mid-glide you hear frequencies that are in neither spectrum.\n*Dry/Wet* - the spectrum you were hearing FADES OUT while the new one FADES IN, over the Time beside this. Nothing moves in frequency at any point; you hear both standing spectra at once, and the two are mixed so the loudness does not dip through the middle.\nOne at a time, because a voice has ONE crossfade between two banks of oscillators and both of these are that same machine under different rules.",
        "Com cau un CANVI d'espectre - pel motiu que sigui: Base, Harm\xc3\xb2nics, Primers, Fractal, Abast, canviar de motor, carregar o escriure una escala, un espectre adaptat d'Espectre vs afinaci\xc3\xb3, un modulador que mogui qualsevol d'aquests.\n*Cap* - el canvi cau a l'instant.\n*Morphing* - cada parcial LLISCA al seu lloc del nou espectre, durant el Temps del costat. \xc3\x89s un pitch bend per parcial: a mig lliscament sents freq\xc3\xbc\xc3\xa8ncies que no s\xc3\xb3n de cap dels dos espectres.\n*Sec/Humit* - l'espectre que senties s'ESVAEIX mentre el nou APAREIX, durant el Temps del costat. No es mou res de freq\xc3\xbc\xc3\xa8ncia en cap moment; sents els dos espectres quiets alhora, barrejats de manera que el volum no caigui pel mig.\nNom\xc3\xa9s un alhora, perqu\xc3\xa8 una veu t\xc3\xa9 UNA sola fosa creuada entre dos bancs d'oscilladors i tots dos s\xc3\xb3n aquella mateixa m\xc3\xa0quina sota regles diferents."));
    morphTimeFader.setTooltip (tr (
        "How long the glide takes, in seconds. It applies to EVERY spectrum change, whatever caused it (Base, Harmonics, Prime, Fractal, engine switch, an adapted spectrum, a modulator...) - not just Base.\nMorph and Dry/Wet keep their own Time, so switching between them brings each one's back.",
        "Quant dura el lliscament, en segons. S'aplica a QUALSEVOL canvi d'espectre, el causi el que el causi (Base, Harm\xc3\xb2nics, Primers, Fractal, canvi de motor, un espectre adaptat, un modulador...) - no nom\xc3\xa9s Base.\nEl Morphing i el Sec/Humit tenen cadascun el seu Temps, o sigui que canviar d'un a l'altre torna a treure el que \xc3\xa9s seu."));
    dryWetTimeFader.setTooltip (tr (
        "How long the crossfade takes, in seconds. Same range and feel as Morph Time, and it applies to EVERY spectrum change whatever caused it. Below about 5 ms the phase hand-over cannot be made click-free, so 5 ms is the floor the transition actually uses.\nMorph and Dry/Wet keep their own Time, so switching between them brings each one's back. With the transition Off this fader is greyed: there is no transition for it to time.",
        "Quant dura la fosa creuada, en segons. El mateix abast i tacte que el Temps de Morphing, i s'aplica a QUALSEVOL canvi d'espectre, el causi el que el causi. Per sota d'uns 5 ms el relleu de fase no es pot fer sense clic, i 5 ms \xc3\xa9s el m\xc3\xadnim que la transici\xc3\xb3 fa servir de debo.\nEl Morphing i el Sec/Humit tenen cadascun el seu Temps, o sigui que canviar d'un a l'altre torna a treure el que \xc3\xa9s seu. Amb la transici\xc3\xb3 a Cap aquest fader queda apagat: no hi ha cap transici\xc3\xb3 a cronometrar."));
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
        "Sethares dissonance: how ROUGH each scale step sounds with the CURRENT timbre. It recomputes live from the synth's actual partials, so editing the timbre (N, fractal, decay, primes, .scl...) changes these values in real time. Lower = more consonant; the rows are ordered most consonant first.",
        "Dissonancia de Sethares: com d'ASPRE sona cada pas de l'escala amb el timbre ACTUAL. Es recalcula en viu a partir dels parcials reals del sinte, aix\xc3\xad que editar el timbre (N, fractal, caiguda, primers, .scl...) canvia aquests valors en temps real. M\xc3\xa9s baix = m\xc3\xa9s consonant; les files van de m\xc3\xa9s consonant primer.");
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);
    recentOpenBtn.setButtonText (tr ("Recent...", "Recents..."));
    recentOpenBtn.setTooltip (tr ("Every scale file loaded in this plugin, newest first. Click one to load it.",
                                  "Tots els fitxers d'""escala carregats en aquest plugin, els més recents primer. Clica'n un per carregar-lo."));
    recentOverlay.title = tr ("Recent scales", "Escales recents");

    // These two used to BOTH read "Load .scl" in adjacent boxes, and clicking the
    // wrong one greys out the whole Harmonic Engine (it loads a TIMBRE), which reads
    // as the plugin breaking. They say what they load now.
    loadSclButton.setButtonText    (tr ("Load", "Obrir"));
    loadSclButton.getProperties().set ("fork", true);
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
    playgroundButton.setButtonText (tr ("Preset Playground", "Laboratori de Presets"));
    playgroundButton.setTooltip (tr (
        "Put up to four presets on the corners of a rectangle and drag a point between them: everything you hear morphs, including !across engines! (a custom scale on one corner and the harmonic engine on another blend their compiled PARTIALS, because their knobs have nothing in common to average).\nThe point is two automatable parameters, so a move recorded in the DAW plays back with the window shut.\n!The tuning is never touched! - mode, scale, reference A, octave and bend range all stay exactly as you left them.",
        "Posa fins a quatre presets a les cantonades d'un rectangle i arrossega un punt entre ells: tot el que sents es transforma, !fins i tot entre motors! (una escala pr\xc3\xb2pia a una cantonada i el motor harm\xc3\xb2nic a una altra barregen els seus PARCIALS compilats, perqu\xc3\xa8 els seus controls no tenen res en com\xc3\xba per promitjar).\nEl punt s\xc3\xb3n dos par\xc3\xa0metres automatitzables, o sigui que un moviment gravat al DAW es reprodueix amb la finestra tancada.\n!L'afinaci\xc3\xb3 no es toca mai! - mode, escala, refer\xc3\xa8ncia A, octava i rang de bend queden exactament com els tenies."));
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
    // The old tooltip promised "Aligned: never clips" as a property of the whole
    // control. That stops being true at any position past 0, because the peak-capped
    // normalisation law is only fully in force at the very bottom of the travel - so
    // the promise is now attached to the END where it holds, not to the control.
    phaseFader.setTooltip (tr (
        "Starting phase of the partials, as a continuous fade. 0 = Aligned: all peak together, forming a periodic pulse - tonal, never clips, and with dense spectra the pulse itself is heard as a low difference tone. 1 = Scattered: pseudo-random phases - smoother, louder, no pulse. In between, every partial moves the shortest way from one to the other and the level follows.",
        "Fase inicial dels parcials, com un fos continu. 0 = Alineat: tots fan el pic alhora i formen un pols peri\xc3\xb2""dic - tonal, mai clipa, i amb espectres densos el pols s'escolta com un to diferencial greu. 1 = Dispers: fases pseudoaleat\xc3\xb2ries - m\xc3\xa9s suau, m\xc3\xa9s fort, sense pols. Enmig, cada parcial va pel cam\xc3\xad m\xc3\xa9s curt d'un a l'altre i el volum el segueix."));
    // HONEST ABOUT WHAT THIS BUILD DOES, IN THE ONE PLACE THE USER READS.
    //
    // The glide dropdown is gone, so its sentence has to live here or nowhere - and the
    // sentence still has to be TRUE. Live re-phasing is NOT implemented: this build
    // changes a held note's LEVEL immediately (the normalisation law is blended by the
    // same control) and its PHASES only from the next note-on. So this names where the
    // mode and the duration WILL come from - the SPECTRUM panel's transition, which is
    // now the single place that answers "how is a spectrum change made smooth" - without
    // claiming a re-phasing that would not happen. A tooltip promising a glide the
    // oscillators never perform is worse than no tooltip: the player trusts it on stage.
    //
    // APPENDED rather than retyped, so the continuous-fade paragraph above stays the one
    // copy of itself; two hand-kept halves of one tooltip drift, and only one of them
    // would get corrected.
    phaseFader.setTooltip (phaseFader.getTooltip() + tr (
        "\nWHILE NOTES ARE SOUNDING: a Phase move changes their LEVEL at once, and their PHASES only from the next note on - this build does not re-phase a held note. When live re-phasing lands it will take BOTH its mode and its duration from the SPECTRUM panel's TRANSITION, because re-phasing IS a spectrum change. Transition Morph: by pitchbend, over its Time. Transition Dry/Wet: by crossfade, over its Time. Transition Off: no live re-phasing at all.",
        "\nMENTRE SONEN LES NOTES: moure la Fase els canvia el VOLUM a l'instant, i les FASES nom\xc3\xa9s a partir de la nota seg\xc3\xbc""ent - aquesta versi\xc3\xb3 no refasa una nota que ja sona. Quan arribi el refasat en directe, en prendr\xc3\xa0 el mode I la durada de la transici\xc3\xb3 del plaf\xc3\xb3 ESPECTRE, perqu\xc3\xa8 refasar \xc3\x89S un canvi d'espectre. Transici\xc3\xb3 a Morphing: per pitchbend, durant el seu Temps. Transici\xc3\xb3 a Sec/Humit: per fosa creuada, durant el seu Temps. Transici\xc3\xb3 a Cap: cap refasat en directe."));
    gainFader.setTooltip (tr ("Final output volume, in dB.", "Volum final de sortida, en dB."));
    // THE TWO STEPPER ROWS' HOVERS. They were built in the constructor as plain English
    // juce::Strings and never re-set, so OCTAVE's and RANGE's only explanation stayed
    // English in Catalan - and neither the fit sweep nor the "translated?" check could
    // see it, because both only ever looked at DRAWN text and a hover is not drawn.
    // Each is set on all three components of its row (- , value, +) so the hover is the
    // same wherever the pointer lands, in one place rather than three.
    {
        const auto octTip = tr (
            "Octave transpose: shift the WHOLE synth by whole octaves (\xc2\xb1""5).",
            "Transposici\xc3\xb3 d'octava: despla\xc3\xa7""a TOT el sintetitzador per octaves senceres (\xc2\xb1""5).");
        const auto sprTip = tr (
            "Spectrum's octaves: keep only partials within this many octaves above the played note. 10 = no cut (full spectrum).",
            "Octaves de l'espectre: mant\xc3\xa9 nom\xc3\xa9s els parcials dins d'aquest nombre d'octaves per sobre de la nota tocada. 10 = sense tall (espectre sencer).");
        for (auto* b : { &octDn, &octUp }) b->setTooltip (octTip);
        for (auto* b : { &sprDn, &sprUp }) b->setTooltip (sprTip);
        octVal.setTooltip (octTip);
        sprVal.setTooltip (sprTip);
    }
    // The switch says ON because that is the state you are asking for when you click it,
    // and the tooltip carries the part that is not obvious: OFF is not "limit 0" or "all
    // primes ticked", it is no filter at all.
    primeOnToggle.setButtonText (tr ("On", "Actiu"));
    primeOnToggle.setTooltip (tr (
        "Turns the whole prime filter on and off. OFF passes every harmonic, whichever mode and ticks are showing - it is not the same as ticking all nine primes, because a harmonic like 29 has a prime factor no tick can allow.",
        "Activa i desactiva tot el filtre de primers. Desactivat deixa passar tots els harm\xc3\xb2nics, sigui quin sigui el mode i les caselles - no \xc3\xa9s el mateix que marcar els nou primers, perqu\xc3\xa8 un harm\xc3\xb2nic com el 29 t\xc3\xa9 un factor primer que cap casella pot permetre."));
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
        "Low-pass cutoff as an INTERVAL above the played note, in cents (1200 = one octave, 12000 = fully open). It TRACKS the note, so the same partials are cut at every pitch - a partial at ratio r sits at 1200·log2(r) cents (e.g. harmonic 2 = 1200c, harmonic 3 = 1902c).",
        "Tall del passa-baixos com a INTERVAL per sobre de la nota tocada, en cents (1200 = una octava, 12000 = tot obert). SEGUEIX la nota, aix\xc3\xad que talla els mateixos parcials a qualsevol al\xc3\xa7""ada - un parcial de ratio r \xc3\xa9s a 1200·log2(r) cents (p. ex. harm\xc3\xb2nic 2 = 1200c, harm\xc3\xb2nic 3 = 1902c)."));
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
    for (int i = 0; i < 3; ++i)
        recentBtn[i].setTooltip (tr ("Reload this recently used file as the scale.",
                                     "Torna a carregar aquest fitxer recent com a escala."));
    bendBox.setTooltip (tr (
        "Per-note pitch-bend range. +-48 st is the MPE standard and is REQUIRED for Ableton Live 12's tuning systems and the Microtuner device (enable MPE on this plugin in Live). Use +-2 for a classic keyboard pitch wheel.",
        "Rang del pitch bend per nota. +-48 st \xc3\xa9s l'est\xc3\xa0ndard MPE i \xc3\x89S NECESSARI per als sistemes d'afinaci\xc3\xb3 d'Ableton Live 12 i el Microtuner (activa MPE al plugin dins de Live). Fes servir +-2 per a la roda d'un teclat cl\xc3\xa0ssic."));
    //-- SHEPARD MODE (bottom row of the Tuning box) ---------------------------
    shepOnT.setButtonText ("Shepard");
    // (its hover is written by refreshShepardRiseState() below - it carries the
    //  rise-interval state, which depends on the TUNING and so cannot live here)
    shepModeBox.setTooltip (tr (
        "How the rise time is set. *Free*: the fader beside gives the time to rise one interval, in ms. *Sync* (the default): rising one interval lasts the chosen number of host beats. Switching mode (or moving the rate) GLIDES the speed - a sounding Shepard never jumps.",
        "Com es fixa el temps de pujada. *Lliure*: el fader del costat d\xc3\xb3na el temps de pujar un interval, en ms. *Sinc* (per defecte): pujar un interval dura els beats triats del tempo del DAW. Canviar de mode (o moure la velocitat) fa LLISCAR la marxa - un Shepard que sona mai salta."));
    shepTimeFader.setTooltip (tr (
        "Time to rise ONE interval, 100 ms to 60 s (log fader). Changes glide - a sounding Shepard never jumps.",
        "Temps de pujar UN interval, de 100 ms a 60 s (fader logar\xc3\xadtmic). Els canvis llisquen - un Shepard que sona mai salta."));
    shepRiseBox.setTooltip (tr (
        "What ONE rise step is. *Octave*: 2/1, the default in every tuning - an octave is what the ear hears as the same note again, so the illusion works whatever scale is loaded. *Period*: the active tuning's own repeat interval (3/1 under Bohlen-Pierce), which makes the rise land on the scale's own steps instead. This control only appears when the two differ.",
        "Qu\xc3\xa8 \xc3\xa9s UN pas de pujada. *Octava*: 2/1, el valor per defecte en qualsevol afinaci\xc3\xb3 - una octava \xc3\xa9s el que l'orella sent com la mateixa nota un altre cop, aix\xc3\xad la il\xc2\xb7lusi\xc3\xb3 funciona amb l'escala que sigui. *Per\xc3\xad" "ode*: l'interval de repetici\xc3\xb3 de l'afinaci\xc3\xb3 activa (3/1 amb Bohlen-Pierce), que fa que la pujada caigui sobre els graus de l'escala. Aquest control nom\xc3\xa9s apareix quan els dos difereixen."));
    // The beats box's tooltip carries the no-BPM state and is written by
    // refreshShepardSyncState(); resetting the memo makes it re-render in this language.
    lastShepNoBpm = -1;
    refreshShepardSyncState();
    // ...and the SHEPARD switch's carries the rise-interval state (see
    // refreshShepardRiseState), for the same reason and by the same rule.
    lastShepRiseState = -1;
    refreshShepardRiseState();
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
    // Phase has NOTHING to localise here any more. Its two end words live on the fader
    // (its tooltip and its read-out), and the glide choice that used to need a localised
    // two-item box is gone entirely - the mode comes from the SPECTRUM panel's Morph /
    // Dry-Wet transition, whose own words are already translated where they are drawn.
    // That also retires the "Bend" compromise: the box was 67 px wide, "Pitchbend"
    // measured 65 px in RhythmVerify's drawn-string sweep, and the English word had to be
    // shortened to keep it off the fader. No box, no compromise.
    localise (primeModeBox, { tr ("Limit", "L\xc3\xadmit"), tr ("Whitelist", "Llista") });
    // THROUGH tr(), even though the three items are the same word in both languages.
    // Written as raw const char* they went through juce::String(const char*), which reads
    // each byte as a code point, and U+00B1 shipped as "Â±48 st (MPE)" - the most-repeated
    // visual defect in the product (visible in every one of the 42 main-editor snapshots).
    // tr() is the one place that decodes UTF-8, so "same in both languages" is spelled as
    // an identical pair rather than as an escape from the decoder.
    localise (bendBox,      { tr ("\xc2\xb1""2 st",        "\xc2\xb1""2 st"),
                              tr ("\xc2\xb1""12 st",       "\xc2\xb1""12 st"),
                              tr ("\xc2\xb1""48 st (MPE)", "\xc2\xb1""48 st (MPE)") });
    // "Sync (beats)" said nothing about WHOSE beats: the user read it as a beat
    // subdivision of the rise rather than a lock to the host clock. Name the thing it
    // actually locks to. (Only the TEXT changes - changeItemText keeps the item's
    // POSITION, which is the whole contract with the ComboBoxAttachment.)
    // "SYNC" IS THE WORD THIS ROW CANNOT AFFORD, so it goes - in both languages, the
    // same trade the source selector made. It was the single control holding the
    // fully-legible size at 820 px wide, needing 101.0 (en) / 101.9 (ca) of the 99.0
    // the row can give it, and three ways of finding it more room are already measured
    // and rejected. Against "Free (ms)" beside it, "Project Tempo" says which clock the
    // rate comes from and the syncing is what CHOOSING it means. The parameter's own
    // choice strings are untouched: those are what the host writes into automation.
    localise (shepModeBox,  { tr ("Free (ms)", "Lliure (ms)"),
                              tr ("Project Tempo", "Tempo projecte") });
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
                                tr ("Square", "Quadrada"), tr ("Custom", "Pr\xc3\xb2pia"),
                                tr ("Random", "Aleat\xc3\xb2ria") });
    localise (lfoRateModeBox, { tr ("BPM / 2^n", "BPM / 2^n"),
                                tr ("Fundamental / 2^n", "Fonamental / 2^n"),
                                tr ("Free Hz", "Hz lliures") });
    localise (lfoPhaseModeBox,{ tr ("Restart each note", "Reinicia a cada nota"),
                                tr ("Project transport", "Transport del projecte") });
    modUnmapBtn.setButtonText  (tr ("Unmap", "Desassigna"));
    rhythmPanel.helpB.setButtonText (tr ("How to route", "Com encaminar"));
    // The unit on every routed row's steps field. Set here rather than at the call site
    // because stepsText is static - see the comment on stepWord.
    rhythmPanel.stepWord = tr ("step", "pas");
    rhythmPanel.refreshSlots();
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
        // THE TUNING MODE BOX'S OWN ITEMS, which come straight from the parameter's
        // StringArray and were never translated at all. Three of the four are the same
        // word in Catalan - 12ED2 is a name, MTS-ESP a standard, Scala a file format -
        // and the fourth is two English words that stayed English. Nothing found it
        // because it is the WIDEST item, so it is never the one a picture shows: every
        // shot has "Scala (.scl)" selected.
        //
        // changeItemText, not localise, because the attachment owns the selection and
        // re-adding the items would move it. The PARAMETER's strings are untouched: those
        // are what the host writes into automation lanes.
        const int keepTuning = tuningBox.getSelectedId();
        tuningBox.changeItemText (1, tr ("12ED2", "12ED2"));
        tuningBox.changeItemText (2, tr ("MTS-ESP", "MTS-ESP"));
        tuningBox.changeItemText (3, tr ("Scala (.scl)", "Scala (.scl)"));
        // THE CATALAN GLOSS IS SHORTER THAN THE ENGLISH ONE ON PURPOSE, and this is the
        // one string in the window where that is a LAYOUT decision rather than a
        // translation one. This box is sized to its WIDEST item, this is that item, and
        // it shares the TUNING panel's 24 px mode row with three fork buttons that each
        // measure their own word. Measured at the shipped 1200x650, where that row is
        // 447 px wide (the row's own comment already states the four controls want ~466
        // of it):
        //
        //   english   box 174 + 5 + Load 57 + 4 + Save 58 + 4 + the bank word 107 = 409
        //   catalan   box 215 + 5 + Obrir 60 + 4 + Desa 58 + 4 + the bank word 134 = 480
        //
        // The Catalan gloss is 41 px wider than the English one and the Catalan bank word
        // 27 px wider, so the Catalan row was 33 px over and the LAST control on it paid:
        // the button that puts the playing tuning into the bank measured 134 against the
        // 101 it was left, hid itself by this row's stated rule - a word that cannot be
        // read is hidden, never smeared - and so never got bounds at all. It is also not
        // reachable any other way: it is the only route into the bank that does not go
        // through a file. A FEATURE THAT EXISTS IN ONE LANGUAGE AND NOT THE OTHER is not
        // an outcome that rule may produce, so the row has to fit.
        //
        // And nothing saw it, by construction: layoutProbe and the text sweep both skip
        // invisible children, so hiding a control REMOVES its measurements and every
        // bounds, overlap and fit check stays green. Only the word survived, in
        // buttonTexts - a word present with no control under it, which is exactly the
        // fingerprint a human found by looking at the rendered picture.
        //
        // Dropping the gloss to the one word that names the mechanism takes the box to
        // 122, so the row asks 151 + 5 + 260 = 416 of 447 - 31 px spare against English's
        // 38, with all four controls laid out in both languages from about a 1090 px
        // window up. The mechanism is not lost: this box's own hover says the host sends
        // the tuning as per-note bend, in both languages, and "bend" is the word an MPE
        // user already has for it. English is not touched by a pixel.
        //
        // TRIED AND REJECTED, both measured:
        //   - sizing the box to its SELECTED item and reserving all three buttons. It
        //     does fix the row, and it moves the rig's smallest fully legible size from
        //     780x550 to about 1190x550: the text sweep measures every combo against its
        //     WIDEST item, this box is already the control that walk names as binding for
        //     the whole window, and it starts clipping below about 174 px.
        //   - shortening the bank button's word instead. That word is the FEATURE'S NAME;
        //     a lone plus sign was replaced by a sentence precisely because it named
        //     neither what it added nor where, and there is a standing check that it
        //     still says so.
        //   - wrapping the three buttons onto a row of their own, which resized() already
        //     does when it can. It cannot here: the panel has 112 px under the mode row
        //     and the wrap wants 123, and the 27 px would come off the tuning BANK row -
        //     trading the panel's whole bank UI for the one button that reaches it.
        tuningBox.changeItemText (4, tr ("MPE (bend input)", "MPE (bend)"));
        if (keepTuning > 0) tuningBox.setSelectedId (keepTuning, juce::dontSendNotification);
    }
    {
        const int keepMode = specModeBox.getSelectedId(); // BEFORE the texts move - see localise
        specModeBox.changeItemText (1, tr ("Find tunings based on beating", "Troba afinacions pel batec"));
        specModeBox.changeItemText (2, tr ("Adapt spectrum to tuning based on beating",
                                           "Adapta l'espectre a l'afinaci\xc3\xb3 pel batec"));
        specModeBox.changeItemText (3, tr ("Find tunings from spectrum's segment",
                                         "Troba afinacions del segment de l'espectre"));
        specModeBox.setSelectedId (keepMode, juce::dontSendNotification);
    }
    // THE SOURCE, IN THE WORDS THE BOX CAN AFFORD.
    //
    // This box is already the full width of the Spectrum-vs-Tuning column, and at the
    // SHIPPED 1200x650 "De totes les afinacions possibles" needed 213.96 px of the 174 it
    // has - it drew 39.96 px past its own right edge, and a ComboBox neither clips
    // visibly nor resizes, so what a Catalan player actually read was a sentence that
    // stopped mid-word with nothing on screen saying so. There is no width to take: the
    // column is 208 px at every window size from the 870x540 floor to the shipped size,
    // and the previous pass measured what happens when it is widened there (rows 1-2
    // start clipping instead).
    //
    // So the SENTENCE gives way, and the word it gives up is the one that was carrying
    // nothing. The pair reads "from current tuning" against "from all tunings": the
    // contrast that matters is THIS tuning against ALL of them, and "possible" was only
    // ever restating "all". Both languages lose exactly the same word, so the two still
    // say the same thing - and the English was not comfortable either, at 165.03 px of
    // 174 with nine pixels to spare in a box whose items get re-worded on every language
    // switch.
    localise (tuneSourceBox, { tr ("From current tuning", "De l'afinaci\xc3\xb3 actual"),
                               tr ("From all tunings", "De totes les afinacions") });
    localise (tuneScopeBox,  { tr ("0 vs all", "0 vs tot"), tr ("All vs all", "Tot vs tot") });
    // TWO BOXES THAT WERE NEVER ON THIS LIST AT ALL. Both are populated with English at
    // construction, like every other box here, and both were missing their line - so they
    // stayed English in the Catalan window. "Min" was on screen in 41 of the rig's Catalan
    // pictures and the translated gate did not object, because it compares the two
    // languages and these were identical in both. The dissonance one is worse: its items
    // are not on screen in ANY photographed state, so nothing could have found it by
    // looking. Same words as adaptGoalBox above, same accents.
    localise (chordMinMaxBox, { tr ("Min", "M\xc3\xadn"), tr ("Max", "M\xc3\xa0x") });
    localise (dissModeBox,    { tr ("Minima", "M\xc3\xadnims"), tr ("Maxima", "M\xc3\xa0xims") });
    // Its ITEMS were already localised (line above) but its HOVER was not: it was set
    // once in the constructor as an English literal, so the two-word items changed
    // language and the sentence that says what they MEAN never did. The items alone do
    // not carry the meaning - "0 vs tot" is only intelligible from this hover - so this
    // is the half that mattered.
    tuneScopeBox.setTooltip (tr (
        "How a combination is SCORED.\n"
        "*0 vs all* - each note is judged only by its beating against the root (0).\n"
        "*All vs all* - every pair of notes in the combination counts.",
        "Com es PUNTUA una combinaci\xc3\xb3.\n"
        "*0 vs tot* - cada nota es jutja nom\xc3\xa9s pel seu batec contra la fonamental (0).\n"
        "*Tot vs tot* - hi compta cada parella de notes de la combinaci\xc3\xb3."));
    lastNadD = 0; // force the n-ad selector's items to rebuild in the new language

    // Chord-search mode texts.
    chordNotesLbl.setText (tr ("Notes", "Notes"), dontSendNotification);
    chordPerLbl.setText   (tr ("Period", "Per\xc3\xado" "de"), dontSendNotification);
    // Capitalised like "Notes" and "Period" above them: these three are the SAME kind of
    // thing - a caption over a value in one stack - and two of them opened on a capital
    // while the third did not. The extra letter costs nothing: a capital S is 1 px wider
    // than a lower-case one at this face and the caption is sized to its own words.
    dissMinLbl.setText    (tr ("Smallest interval (c)", "Interval m\xc3\xa9s petit (c)"), dontSendNotification);
    chordMinLbl.setText   (tr ("Smallest interval (c)", "Interval m\xc3\xa9s petit (c)"), dontSendNotification);
    // The two minimum-interval boxes. Their tr() pair was correct and sat in the
    // constructor, where applyLanguage() could never reach it - the four-word caption
    // above each box changed language and the sentence explaining it did not.
    for (auto* e : { &dissMinEdit, &chordMinEdit })
        e->setTooltip (tr ("Minimum interval between generated notes, in cents (also to the tonic and the period).",
                           "Interval m\xc3\xadnim entre notes generades, en cents (tamb\xc3\xa9 respecte a la t\xc3\xb2nica i el per\xc3\xado" "de)."));
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
        "How many notes each combination has (the root 1/1 is included). From the current tuning it stops at the number of degrees the scale has; from all tunings it goes to 8. More notes = a heavier search.",
        "Quantes notes t\xc3\xa9 cada combinaci\xc3\xb3 (l'arrel 1/1 hi \xc3\xa9s inclosa). Des de l'afinaci\xc3\xb3 actual arriba fins als graus que t\xc3\xa9 l'escala; des de totes les afinacions arriba a 8. M\xc3\xa9s notes = cerca m\xc3\xa9s pesada."));
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
    chordList.rowTipLoad  = tr ("*Double-click to load*", "*Doble clic per carregar*");
    chordList.rowTipValue = tr ("total dissonance:", "disson\xc3\xa0ncia total:");

    //== "Adapt spectrum to tuning based on beating" + the two REALTIME buttons =====
    localise (adaptTargetBox, { tr ("To the loaded tuning", "A l'afinaci\xc3\xb3 carregada"),
                                tr ("To the chord held now", "A l'acord que sona ara") });
    localise (adaptGoalBox,   { tr ("Min", "M\xc3\xadn"), tr ("Max", "M\xc3\xa0x"), tr ("Range", "Rang") });
    adaptPartialsLbl.setText (tr ("Partials", "Parcials"), dontSendNotification);
    adaptRangeLbl.setText    (tr ("Between", "Entre"), dontSendNotification);
    // "Periods", bare: the DIRECTIONS are in the line underneath, which has room to print
    // them as numbers ("+-1.0 per around 3:0 = 2:0..4:0"). A caption that tried to say
    // "Periods each way" would need more than this row can spare, and juce::Label answers
    // that by CONDENSING to 70% rather than by clipping - which looks deliberate and reads
    // as a different font (RhythmVerify measured exactly that on the word this replaces).
    adaptRegLbl.setText      (tr ("Periods", "Per\xc3\xado" "des"), dontSendNotification);
    refreshAdaptButtonText();
    // THE TICK IS THE "+".
    //
    // "+ amplituds" needed 75.11 px of the 62 its cell left it and drew 13.11 px past the
    // toggle at the SHIPPED 1200x650, and again at the 870x540 floor - a ToggleButton
    // does not clip its word, it just draws it over whatever is to its right.
    //
    // Nowhere in this column can hold it. The content is 192 px wide at EVERY window size
    // the product ships (the column is a flat 208 from the floor to 1200x650), a
    // ToggleButton spends 28 px of its width on the tick box and its gap before a letter
    // is drawn, and the widest slot any row here can free is 102 px - two pixels short of
    // the 104 the Catalan wanted. A row of its own is the one thing that would fit it and
    // it is the one thing this panel cannot pay for: the adapted-partial list is the
    // LEFTOVER of this column, RhythmVerify holds it to three unscrolled rows, and in the
    // goal-Range shape it is already down to 56 px of the 54 that bar demands.
    //
    // So the LABEL gives way, and what it gives up is the "+" - which the tick box was
    // already saying. A checkbox reading "amplitudes" is a checkbox that turns amplitudes
    // on; the "+" was a second copy of the tick, in the row's most expensive pixels. Both
    // languages drop it, so the two still say the same thing, and the noun each keeps is
    // its own real word rather than an abbreviation shared out of desperation.
    adaptAmpsToggle.setButtonText (tr ("Amps", "Amplituds"));
    // SENTENCE CASE, NOT SHOUTING. These two were the only ALL-CAPS words among the
    // column's controls - every caption, item and button around them is sentence case -
    // so the caps read as emphasis this pair has not earned over the action beside it.
    // The hierarchy inside the pair is carried by ORDER and COLOUR (see actionPair), and
    // neither of those changes here.
    //
    // It is also the one capitalisation change that BUYS room rather than spending it.
    // actionPair measures the follower's word and gives the action what is left, and the
    // comment there records what the all-caps pair cost: "REALTIME" wanted 79 px of the
    // 76 the old 2/5 split gave it and "TEMPS REAL" wanted 96. Lower-case letters are
    // narrower than capitals in this face, so both words shrink and the action - whose
    // text GROWS when a run would take more than a second - gets the difference.
    chordRtBtn.setButtonText (tr ("Realtime", "Temps real"));
    adaptRtBtn.setButtonText (tr ("Realtime", "Temps real"));
    adaptTargetBox.setTooltip (tr (
        "WHICH NOTES the spectrum is adapted to.\n*The loaded tuning* - its degrees.\n*The chord held now* - literally the notes under your fingers, at the pitch they are really sounding.\nONE note counts as a target too: with nothing else sounding, what is minimised is the partials' beating against EACH OTHER, at that note's own pitch.\nREALTIME always follows the held chord, so this box shows that and waits while the link is on.",
        "QUINES NOTES fan d'objectiu.\n*L'afinaci\xc3\xb3 carregada* - els seus graus.\n*L'acord que sona ara* - literalment les notes sota els dits, a l'al\xc3\xa7""ada real.\nUNA sola nota tamb\xc3\xa9 val com a objectiu: si no sona res m\xc3\xa9s, el que es minimitza \xc3\xa9s el batec dels parcials ENTRE ELLS, a l'al\xc3\xa7""ada d'aquella nota.\nTEMPS REAL sempre segueix l'acord que mantens, per aix\xc3\xb2 aquesta casella ho mostra i espera mentre el v\xc3\xadncle est\xc3\xa0 actiu."));
    adaptPartialsBox.setTooltip (tr (
        "How many partials the adapted spectrum has. They are all kept: the search MOVES them and never merges or removes one - each keeps at least the smallest interval below from its neighbours.",
        "Quants parcials t\xc3\xa9 l'espectre adaptat. Es conserven tots: la cerca els MOU i mai no en fusiona ni n'elimina cap - cada un guarda com a m\xc3\xadnim l'interval m\xc3\xa9s petit de sota respecte als ve\xc3\xafns."));
    adaptGoalBox.setTooltip (tr (
        "The TARGET. Min = the partials go where these notes beat least. Max = where they beat most. Range = hit a beating value between the two numbers you type (the read-out below shows the scale those numbers are on).",
        "L'OBJECTIU. M\xc3\xadn = els parcials van on aquestes notes baten menys. M\xc3\xa0x = on baten m\xc3\xa9s. Rang = arriba a un valor de batec entre els dos n\xc3\xbameros que escriguis (el marcador de sota mostra l'escala d'aquests n\xc3\xbameros)."));
    adaptRegPeriodsEdit.setTooltip (tr (
        "HOW WIDE a band of registers the spectrum is optimised for, counted in PERIODS of the loaded tuning and reaching that far BOTH ABOVE AND BELOW the note you are playing. 1.0 means one period down and one period up.\nThe anchor is the lowest note you are HOLDING; with nothing down it is the last note you played, and before you have played anything it is the reference key. The line below names it as period:degree - \"3:0\" is that reference key - and prints the two keys the band really reaches, so a band cut short by the end of the keyboard shows as \"-0.4/+1.0\" instead of pretending to be symmetric.\nWhy PERIODS and not steps or octaves: a period is the unit the tuning itself is built from, so one number is the same musical width in every scale (1200 cents in 12-EDO, 1902 in Bohlen-Pierce) and it never has to be re-read when you change tuning. Most of this instrument's tunings have no octave, and a count of scale steps is exact but unreadable: 62 is two periods in 31-EDO and sixty-two in a one-degree scale.\nWhy BOTH WAYS: Sethares' critical band is a width in Hz, not in cents, so the answer really is different up and down the keyboard - the interval that beats worst is about 434 cents low and only 61 cents four octaves up. Measured, one period below the played note costs 1.46x a spectrum optimised there, against 1.27x once the band straddles the note.\nThe default is one period each way, and that is measured rather than guessed: across a symmetric band the coverage average barely separates the widths at all - a 2.4% spread - so the default is decided by what does separate them. It costs the register you are really playing almost nothing, and it is the WIDEST band the REALTIME follower samples exactly: one register per period, three points, which is the follower's own cap. 0 is legal and means the played register alone.\nWITH AN ABSOLUTE .tun TABLE there is no scale behind the pitches and therefore no period; the band is then built on the period Fibló measured for the file, and the line below says so and by how many cents the file strays from it. If the file has nothing ascending to measure at all, that line says so too - it is never presented as a fact.\nHOW THE BAND IS COMBINED: every register in it counts EQUALLY - the spectrum is optimised for the AVERAGE beating across the whole span, so a wider band trades a little at the note under your fingers for the registers around it. That is not a setting: there used to be a *worst case* choice beside this field and it could not change the result. The objective is evaluated at one point per period, capped at 7 for the Adapt button and 3 for the REALTIME follower (the cost is exactly linear in that count). Nothing is degraded silently - the panel prints how many points each path really uses.",
        "L'AMPLADA de la banda de registres per als quals s'optimitza l'espectre, comptada en PER\xc3\x8dODES de l'afinaci\xc3\xb3 carregada i que arriba fins all\xc3\xa0 tant PER SOBRE com PER SOTA de la nota que toques. 1,0 vol dir un per\xc3\xado" "de avall i un amunt.\nL'\xc3\xa0ncora \xc3\xa9s la nota m\xc3\xa9s greu que MANTENS; si no en mantens cap, l'\xc3\xbaltima que has tocat, i abans de tocar res, la tecla de refer\xc3\xa8ncia. La l\xc3\xadnia de sota l'anomena com a per\xc3\xado" "de:grau - \"3:0\" \xc3\xa9s aquella tecla de refer\xc3\xa8ncia - i escriu les dues tecles a les quals arriba de debo, de manera que una banda escap\xc3\xa7""ada pel final del teclat es veu com a \"-0,4/+1,0\" en comptes de fer veure que \xc3\xa9s sim\xc3\xa8trica.\nPer qu\xc3\xa8 PER\xc3\x8dODES i no passos ni octaves: el per\xc3\xado" "de \xc3\xa9s la unitat amb qu\xc3\xa8 est\xc3\xa0 constru\xc3\xaf" "da l'afinaci\xc3\xb3 mateixa, o sigui que un sol n\xc3\xbamero \xc3\xa9s la mateixa amplada musical a qualsevol escala (1200 cents en 12-EDO, 1902 en Bohlen-Pierce) i no s'ha de rellegir mai en canviar d'afinaci\xc3\xb3. La majoria d'afinacions d'aquest instrument no tenen octava, i un compte de passos \xc3\xa9s exacte per\xc3\xb2 illegible: 62 s\xc3\xb3n dos per\xc3\xado" "des en 31-EDO i seixanta-dos en una escala d'un sol grau.\nPer qu\xc3\xa8 CAP A TOTES DUES BANDES: la banda cr\xc3\xadtica de Sethares \xc3\xa9s una amplada en Hz, no en cents, i per aix\xc3\xb2 la resposta \xc3\xa9s realment diferent amunt i avall del teclat - l'interval que m\xc3\xa9s bat \xc3\xa9s d'uns 434 cents als greus i nom\xc3\xa9s 61 quatre octaves m\xc3\xa9s amunt. Mesurat, un per\xc3\xado" "de per sota de la nota tocada costa 1,46x respecte d'un espectre optimitzat all\xc3\xa0, contra 1,27x quan la banda encavalca la nota.\nPer defecte \xc3\xa9s un per\xc3\xado" "de a cada banda, i aix\xc3\xb2 est\xc3\xa0 mesurat, no suposat: amb una banda sim\xc3\xa8trica la mitjana de cobertura gaireb\xc3\xa9 no separa les amplades - un 2,4% de diferencia - de manera que el valor per defecte el decideix el que s\xc3\xad que les separa. Costa gaireb\xc3\xa9 res al registre que est\xc3\xa0s tocant de debo, i \xc3\xa9s la banda M\xc3\x89S AMPLA que el seguidor en TEMPS REAL mostreja exactament: un registre per per\xc3\xado" "de, tres punts, que \xc3\xa9s el seu propi m\xc3\xa0xim. 0 \xc3\xa9s legal i vol dir nom\xc3\xa9s el registre tocat.\nAMB UNA TAULA .tun ABSOLUTA no hi ha cap escala darrere de les al\xc3\xa7""ades i per tant no hi ha per\xc3\xado" "de; la banda es construeix llavors sobre el per\xc3\xado" "de que Fibl\xc3\xb3 ha mesurat per al fitxer, i la l\xc3\xadnia de sota ho diu i tamb\xc3\xa9 quants cents se n'aparta el fitxer. Si el fitxer no t\xc3\xa9 res ascendent per mesurar, aquella l\xc3\xadnia tamb\xc3\xa9 ho diu - mai no es presenta com un fet.\nCOM ES COMBINA LA BANDA: tots els seus registres compten IGUAL - l'espectre s'optimitza per a la MITJANA del batec de tot el tram, de manera que una banda m\xc3\xa9s ampla cedeix una mica a la nota que tens sota els dits per guanyar als registres del voltant. Aix\xc3\xb2 no \xc3\xa9s cap opci\xc3\xb3: al costat d'aquest camp hi havia un *pitjor cas* i no podia canviar el resultat. L'objectiu s'avalua a un punt per per\xc3\xado" "de, amb un m\xc3\xa0xim de 7 per al bot\xc3\xb3 Adapta i de 3 per al seguidor en TEMPS REAL (el cost \xc3\xa9s exactament lineal amb aquest nombre). No es degrada res en silenci - el panell escriu quants punts fa servir de debo cada cam\xc3\xad."));
    // The SAME hover on the caption and on the line under the field: the sentence that
    // explains the aggregate now lives here (the box that used to say it in words is gone),
    // and a user reaching for "what happened to the average/worst choice" is as likely to
    // hover the words as the number.
    adaptRegSpanLbl.setTooltip (adaptRegPeriodsEdit.getTooltip());
    adaptRegLbl.setTooltip     (adaptRegPeriodsEdit.getTooltip());
    adaptAmpsToggle.setTooltip (tr (
        "Also shape each partial's AMPLITUDE, not only where it sits. The amplitudes are written as per-partial edits (the pencil window's own values), bounded and never muted - a silenced partial would be a deleted one.",
        "Modela tamb\xc3\xa9 l'AMPLITUD de cada parcial, no nom\xc3\xa9s la posici\xc3\xb3. Les amplituds s'escriuen com a edicions per parcial (els mateixos valors de la finestra del llapis), acotades i mai silenciades - un parcial silenciat seria un parcial esborrat."));
    adaptGoBtn.setTooltip (tr (
        "Adapt the spectrum once, now. It is written into the Custom Engine (partial ratios) plus the per-partial amplitudes, as ONE undo step.\nA one-shot calculation, so it is allowed to take time: when it will take more than a moment the button says roughly how long, and pressing it again while it runs STOPS it. Nothing is refused for being big except a request estimated past ten minutes, which is stated as a number rather than a verdict. You can keep playing while it works.",
        "Adapta l'espectre un cop, ara. S'escriu al Motor Propi (raons dels parcials) m\xc3\xa9s les amplituds per parcial, com UN sol pas d'anul\xc2\xb7laci\xc3\xb3.\nUn c\xc3\xa0lcul D'UN SOL COP, i per tant t\xc3\xa9 dret a trigar: quan trigui m\xc3\xa9s d'un instant, el bot\xc3\xb3 diu aproximadament quant, i tornar-lo a pr\xc3\xa9mer mentre treballa l'ATURA. No es rebutja res per ser gran, tret d'una petici\xc3\xb3 estimada en m\xc3\xa9s de deu minuts, i aix\xc3\xb2 es diu amb un n\xc3\xbamero, no amb un veredicte. Pots seguir tocant mentre treballa."));
    chordRtBtn.setTooltip (tr (
        "REALTIME: the tuning follows the TOP result on its own, every time the spectrum changes (a knob, a modulator, a preset). At most three searches a second, never two at once, and many changes while it is busy collapse into one, so a sweeping modulator cannot pile them up. A full search takes about 0.3 s with a rich spectrum (0.1 s when the spectrum's tables can be reused), so each retune lands as its own search finishes. Realtime updates do NOT fill the tuning bank: the tuning you had joins it ONCE when you switch this on, and switching it off leaves ONE Ctrl+Z back to it.",
        "TEMPS REAL: l'afinaci\xc3\xb3 segueix sola el PRIMER resultat cada cop que canvia l'espectre (un potenci\xc3\xb2metre, un modulador, un preset). Com a m\xc3\xa0xim tres cerques per segon, mai dues alhora, i molts canvis mentre treballa es fusionen en un, aix\xc3\xad un modulador escombrant no els pot acumular. Una cerca completa triga uns 0,3 s amb un espectre ric (0,1 s quan es poden reaprofitar les taules de l'espectre), per aix\xc3\xb2 cada reafinaci\xc3\xb3 arriba quan acaba la seva cerca. Les actualitzacions en temps real NO omplen el banc: l'afinaci\xc3\xb3 que tenies hi entra UN sol cop en activar-ho, i en desactivar-ho queda UN Ctrl+Z per recuperar-la."));
    adaptRtBtn.setTooltip (tr (
        "REALTIME: the spectrum follows WHAT YOU PLAY - it re-adapts to the chord under your fingers, at the pitch it is really sounding, every time that chord changes. A SINGLE note works: with nothing else sounding the target is the partials' beating against each other, so a lead or a bass line still gets its own smoothest (or roughest) timbre, and a different one in each register. Hold nothing and it does nothing; hold the same chord and it stays put. Throttled to about four adaptations a second and coalesced. (The loaded tuning is a target for the Adapt button, not for a follower: it would just keep writing the same scale into the spectrum.) Switching it off leaves ONE Ctrl+Z back to the spectrum you had before.",
        "TEMPS REAL: l'espectre segueix el QUE TOQUES - es torna a adaptar a l'acord que tens sota els dits, a l'al\xc3\xa7""ada real, cada cop que aquell acord canvia. UNA sola nota ja funciona: si no sona res m\xc3\xa9s, l'objectiu \xc3\xa9s el batec dels parcials entre ells, de manera que una melodia o un baix tamb\xc3\xa9 tenen el seu timbre m\xc3\xa9s llis (o m\xc3\xa9s aspre), i un de diferent a cada registre. Si no mantens res, no fa res; si mantens el mateix acord, es queda quiet. Limitat a ~quatre adaptacions per segon i fusionat. (L'afinaci\xc3\xb3 carregada \xc3\xa9s un objectiu per al bot\xc3\xb3 Adapta, no per a un seguidor: nom\xc3\xa9s escriuria la mateixa escala a l'espectre.) En desactivar-ho queda UN Ctrl+Z per recuperar l'espectre anterior."));
    lookaheadBtn.setTooltip (tr (
        "LOOKAHEAD: the spectrum is ready BEFORE the note sounds, for written MIDI played back by the DAW.\nA plugin cannot see future MIDI - so this makes the plugin ARRIVE LATE on purpose. It reports 250 ms of latency, the host's delay compensation shifts everything back into place, and the incoming MIDI is buffered here by exactly that much. The adaptive search runs on each note as it ARRIVES, so on playback the partials change sample-aligned with the note instead of ~50-200 ms into it.\nWHERE 250 ms COMES FROM (all measured): up to 100 ms of the follower's own throttle before the search may start, one 60 Hz poll to notice the notes, the search itself (11 ms at 8 partials, 33 at 12, ~104-137 at 16), one more poll to apply it and the 50 ms de-click fade.\nSO IT DOES NOT FIT EVERY SETTING: beside the budget the button prints how long the LAST chord change really took on your machine, and marks it LATE when it did not fit. If you see LATE, use fewer partials.\nPLAYING LIVE FEELS THOSE 250 ms: this is for composing, not for performing. The plugin's rhythm MIDI OUT is delayed by the same amount (it is generated from the same shifted stream, which is what keeps the pulses locked to the audio) and hosts differ in whether they compensate a plugin's MIDI output.\nIt only costs latency while a REALTIME spectrum follower is actually running; otherwise it waits and reports zero. A loop or a locator jump flushes what is in flight, so no stale note is replayed.",
        "ANTICIPACI\xc3\x93: l'espectre est\xc3\xa0 llest ABANS que soni la nota, per a MIDI escrit i reprodu\xc3\xaft pel DAW.\nUn plugin no pot veure el MIDI futur - o sigui que aix\xc3\xb2 fa que el plugin ARRIBI TARD expressament. Declara 250 ms de lat\xc3\xa8ncia, la compensaci\xc3\xb3 de retard de l'amfitri\xc3\xb3 ho torna tot al seu lloc, i el MIDI d'entrada es reserva aqu\xc3\xad exactament aquest temps. La cerca adaptativa s'executa sobre cada nota quan ARRIBA, o sigui que en reproducci\xc3\xb3 els parcials canvien clavats a la mostra amb la nota, en comptes de ~50-200 ms m\xc3\xa9s tard.\nD'ON SURTEN ELS 250 ms (tot mesurat): fins a 100 ms del limitador del seguidor abans que la cerca pugui comen\xc3\xa7""ar, un sondeig de 60 Hz per adonar-se'n, la cerca mateixa (11 ms amb 8 parcials, 33 amb 12, ~104-137 amb 16), un altre sondeig per aplicar-la i els 50 ms d'esvaniment anti-clic.\nAIX\xc3\x92 VOL DIR QUE NO ENCAIXA A TOTES LES CONFIGURACIONS: al costat del pressupost el bot\xc3\xb3 escriu quant ha trigat de debo el darrer canvi d'acord a la teva m\xc3\xa0quina, i el marca TARD quan no hi ha cabut. Si veus TARD, fes servir menys parcials.\nTOCAR EN VIU NOTA aquests 250 ms: aix\xc3\xb2 \xc3\xa9s per compondre, no per tocar. La sortida MIDI de ritme del plugin es retarda igual (es genera del mateix flux despla\xc3\xa7""at, que \xc3\xa9s el que mant\xc3\xa9 els polsos clavats a l'\xc3\xa0udio) i els amfitrions difereixen en si compensen la sortida MIDI d'un plugin.\nNom\xc3\xa9s costa lat\xc3\xa8ncia mentre un seguidor d'espectre en TEMPS REAL est\xc3\xa0 realment en marxa; si no, espera i declara zero. Un bucle o un salt de localitzador buida el que hi ha en vol, o sigui que no es repeteix cap nota vella."));
    adaptList.tip = tr ("The adapted spectrum: every partial's interval from the played note, and the amplitude the search gave it. All of them are here - the search moves partials, it never drops them.",
                        "L'espectre adaptat: l'interval de cada parcial respecte a la nota tocada i l'amplitud que li ha donat la cerca. Hi s\xc3\xb3n tots - la cerca mou parcials, no en descarta cap.");
    adaptList.emptyText = tr ("Press Adapt to move the partials to where these notes beat least (or most).",
                              "Prem Adapta per moure els parcials on aquestes notes baten menys (o m\xc3\xa9s).");

    // MOVED HERE OUT OF THE CONSTRUCTOR. Every one of these was written exactly once, at
    // construction, so it kept whatever language the editor was BUILT in for the rest of
    // its life: the rhythm panel's "Steps" caption never became "Passos", and eight
    // tooltips never changed language at all. A string set in the constructor is a string
    // that cannot be translated - this is the whole of cause (a).
    //
    // BEFORE the sub-panel relayout below, not after: the rhythm panel sizes this caption
    // from its own text, so a caption rewritten after the layout is measured against the
    // other language's width.
    rhythmPanel.stepsAllLbl.setText (tr ("Steps", "Passos"), dontSendNotification);
    rhythmPanel.stepsAllS.setTooltip (tr (
        "Shifts EVERY step-pitched pulse by this many scale steps from the played note (JI/ratio pulses have no step, so they are unaffected). Right-click to map a modulator.",
        "Despla\xc3\xa7""a TOTS els polsos amb to per passos aquests passos d'escala des de la nota tocada (els polsos JI/ra\xc3\xb3 no en tenen, i no els afecta). Clic dret per assignar-hi un modulador."));
    rhythmPanel.velSlopeS.setTooltip (tr (
        "Velocity SLOPE across the spectrum. 0 = flat (every pulse keeps its own velocity). To the right the HIGH partials start losing velocity; to the left the low ones. At the end of its travel the ramp is vertical and no pulse has any velocity left. Right-click to map a modulator.",
        "PENDENT de velocitat sobre l'espectre. 0 = plana (cada pols mant\xc3\xa9 la seva). Cap a la dreta els parcials AGUTS comencen a perdre velocitat; cap a l'esquerra els greus. Al final del recorregut la pendent \xc3\xa9s vertical i cap pols conserva velocitat. Clic dret per assignar-hi un modulador."));
    rhythmBtn.setTooltip (tr (
        "Turns partials of the current patch into MIDI pulse trains - exact polyrhythms derived from the spectrum's frequency ratios, sample-locked to the sound - sent out of this plugin's MIDI OUTPUT to any other track/instrument. Click to open the panel.",
        "Converteix parcials del patch actual en trens de polsos MIDI - poliritmes exactes derivats de les relacions de freq\xc3\xbc\xc3\xa8ncia de l'espectre, clavats al so mostra a mostra - que surten per la SORTIDA MIDI del plugin cap a qualsevol altra pista/instrument. Clica per obrir el panell."));
    tuningBankBox.setTooltip (tr (
        "Bank of scales: pick a slot to make it the active tuning. This selector is a PARAMETER, so a host can automate switching between the loaded scales.",
        "Banc d'escales: tria una ranura per fer-la l'afinaci\xc3\xb3 activa. Aquest selector \xc3\xa9s un PAR\xc3\x80METRE, aix\xc3\xad que un host pot automatitzar el canvi entre les escales carregades."));
    tuningBankLoadBtn.setTooltip (tr ("Load a .tun / .scl / .ascl into the selected bank slot.",
                                      "Carrega un .tun / .scl / .ascl a la ranura seleccionada del banc."));
    tuningBankClearBtn.setTooltip (tr ("Remove the selected tuning from the bank.",
                                       "Treu l'afinaci\xc3\xb3 seleccionada del banc."));
    // Rebuilt, not just re-tooltipped: its first item is the translated "- none -" /
    // "- cap -", and the only thing that ever rebuilt this list was the timer, once every
    // 30 ticks and only when the NUMBER of items changed - which a language switch never
    // does. So the one word in it stayed English for ever.
    refreshTuningBank();
    presetDirtyMark.setTooltip (tr (
        "*Changed since loaded.*\nSome parameter no longer matches this preset - moved by something other than one of its OWN modulators (a knob, host automation, an undo).\n!Save stores it as a NEW preset! instead of overwriting the original.",
        "*Canviat des de la c\xc3\xa0rrega.*\nAlg\xc3\xban par\xc3\xa0metre ja no coincideix amb aquest preset - l'ha mogut alguna cosa que no \xc3\xa9s un dels seus PROPIS moduladors (un bot\xc3\xb3, automatitzaci\xc3\xb3, un desfer).\n!Desar el guarda com un preset NOU! en lloc de sobreescriure l'original."));
    nadBox.setTooltip (tr (
        "What the list below shows: Degrees = each scale step against the tonic. Dyads, triads, tetrads... = EVERY combination of that many scale notes, ranked by total beating (all pairs counted, not just against 0).",
        "Qu\xc3\xa8 mostra la llista: Graus = cada pas contra la t\xc3\xb2nica. D\xc3\xad""ades, tr\xc3\xad""ades, t\xc3\xa8trades... = TOTES les combinacions d'aquest nombre de notes, ordenades pel batec total (tots els parells, no nom\xc3\xa9s contra el 0)."));
    nadList.tip = tr ("Combinations of scale degrees with the LEAST total beating first (colour = the value's height on the beating range).",
                      "Combinacions de graus de l'escala amb MENYS batec total primer (color = al\xc3\xa7""ada del valor dins el rang de batec).");
    // NEVER TRANSLATED AT ALL - cause (b). The rhythm panel's mode box was filled with two
    // English literals in RhythmPanel::init() and had only its TOOLTIP localised, so the
    // words the user reads stayed English while the explanation of them was in Catalan.
    localise (rhythmPanel.modeBox, { tr ("Lowest note", "Nota m\xc3\xa9s greu"),
                                     tr ("All notes",   "Totes les notes") });

    // THE SUB-PANELS MUST BE LAID OUT AGAIN, NOT ONLY RE-WORDED.
    // Component::setBounds only calls resized() when the SIZE changes, and these three
    // panels are always given the same size - so each was laid out exactly once, in
    // whatever language the editor was built in, and every later language switch put
    // Catalan strings into boxes measured for English. That is why the rhythm panel's
    // captions were the last things in the window still drawn condensed: they size
    // themselves from their own text (fgfit), and nothing ever asked them to.
    rhythmPanel.resized();
    playgroundPanel.resized();
    partialPanel.resized();

    // EVERY SLIDER'S VALUE BOX RE-RENDERS.
    //
    // juce::Slider caches whatever its textFromValueFunction returned and only asks for
    // it again when the VALUE moves. Most of ours print numbers, but two print WORDS -
    // the harmonics fader ("Even/All/Odd/Prime" -> "Parells/Tots/Senars/Primers") and the
    // prime-limit fader ("Off") - and those kept the old language's word on screen until
    // the user happened to drag them. Swept over the whole tree rather than named one by
    // one, because the next fader with a worded read-out must not have to remember this.
    updateSliderValueTexts (*this);
    updateSliderValueTexts (playgroundPanel);   // not children of this editor: they live
    updateSliderValueTexts (partialPanel);      // in their own floating windows

    // The degrees list's own status line ("this scale states no period...") is written
    // by rebuildScaleDissList, which the timer only reaches when the SCALE changes - and
    // a language switch is not a scale change. Rebuilt from the analysis already in hand,
    // so nothing is recomputed and the sentence arrives in this language.
    rebuildScaleDissList();

    // ...AND EVERY SLIDER'S VALUE BOX GETS THIS LANGUAGE'S HOVER (see the sweep's own
    // comment: JUCE copied the English one into it once, at construction).
    pushSliderTooltipsIntoSubComponents (*this);
    pushSliderTooltipsIntoSubComponents (playgroundPanel);
    pushSliderTooltipsIntoSubComponents (partialPanel);

    // ...AND THE LIVE READ-OUTS ARE RE-RUN, NOT LEFT TO THE NEXT TIMER TICK.
    //
    // A whole class of strings in this window is written by the 30 Hz timer, not by this
    // function: the "N partials audible - M in the full timbre" line under the panels, the
    // Custom-engine status, the adaptation read-out, the register band, the LOOKAHEAD
    // button, the tuning-bank combo's "- none -". Every one of them is a correct tr()
    // pair; none of them was in the new language when this function returned, and the
    // rhythm panel's read-outs (its "(hold a note to pulse)" line, the "- missing" suffix
    // on a routed slot, the BPM read-out) are never rewritten by the timer at all while
    // the panel is shut, so those stayed English for ever.
    //
    // refreshSlots/refreshLive first because the timer does not touch a hidden panel;
    // then one timer pass for everything else. Guarded both ways: not before the
    // constructor has finished (the first applyLanguage() runs before setSize() and
    // applyEngineState(), and a timer tick there would read a half-built editor), and
    // never reentrantly.
    if (editorConstructed && ! inLanguageRefresh)
    {
        const juce::ScopedValueSetter<bool> guard (inLanguageRefresh, true);
        rhythmPanel.refreshSlots();
        rhythmPanel.refreshLive();
        timerCallback();
    }
    // EVERY FADER'S VALUE BOX IS AS WIDE AS THE VALUE IT SHOWS.
    //
    // Done here, and not in configFader, because here every piece is finally in place:
    // the attachment's text function, the suffix the caller sets afterwards, and the
    // language, which can change a unit word. Slider::resized() clamps the box to the
    // slider's width, so this is a ceiling and not a promise - but a box that asks for
    // less than its own text needs can never be honest, whatever the column does.
    //
    // 46 stays the floor, so nothing that fitted before gets narrower, and the style is
    // only re-set when the number actually changes: setTextBoxStyle rebuilds the label.
    for (auto* s : configuredFaders)
    {
        const int want = juce::jmax (46, fgfit::faderValueWidth (*s) + 2);
        if (s->getTextBoxWidth() != want)
            s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, want, 15);
    }
    // The one word the KEYBOARD paints for itself, in the language on screen. It used to
    // be a literal inside ScaleKeyboard::paint and so was the only user-visible string in
    // the window that never changed language.
    keyboard.setReferenceWord (tr ("REFERENCE", "REFER\xc3\x88NCIA"));
    // The three info rows above the keys, named on the left. "cents" really is the same
    // word in Catalan - the unit is spelled that way in both - so it is declared to the
    // audits rather than translated into itself; the other two are not.
    keyboard.setRowCaptions (tr ("steps", "passos"), tr ("cents", "cents"),
                             tr ("beating", "batec"));
    // The self-painting components' own EN/CA hook. They draw their words straight onto a
    // Graphics, so nothing else in this file can put those words in the right language -
    // and, until d8aeeaf, nothing could see that they were in the wrong one.
    {
        auto hook = [this] (const char* en, const char* ca) { return tr (en, ca); };
        waveEditor.trF        = hook;
        recentOverlay.trF     = hook;
        recentOverlay.list.trF = hook;
        dissHeader.trF        = hook;
    }
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

namespace
{
    // A readable name for one component in the walk: its own name if it has one, else
    // the class, so the report says WHERE the clipped string is instead of "label 47".
    juce::String probeNameFor (const juce::Component& c)
    {
        if (c.getName().isNotEmpty()) return c.getName();
        juce::String cls (typeid (c).name());
        // MSVC gives "class juce::Label"; keep the last word, drop the namespaces.
        cls = cls.fromLastOccurrenceOf (" ", false, false)
                 .fromLastOccurrenceOf ("::", false, false);
        return cls;
    }

    // How much room a control really leaves for its text, and in which font - from
    // fgfit, the same functions the layout sizes these controls with, so the check and
    // the painter cannot drift apart. Returns false for a Button that paints its own
    // graphics (the pencil), whose button TEXT is only its name and never reaches the
    // glass at all.
    bool buttonTextRoom (const juce::Button& b, juce::Font& fontOut, float& availOut, int& maxLines)
    {
        maxLines = 1;
        if (const auto* t = dynamic_cast<const juce::ToggleButton*> (&b))
        {
            if (t->getName() == "onled")   // the green/red pill: "ON" / "OFF", centred
            {
                fontOut  = juce::Font (juce::FontOptions ((float) b.getHeight() * 0.62f).withStyle ("Bold"));
                availOut = (float) b.getWidth();
                return true;
            }
            fontOut  = fgfit::toggleFont (b.getHeight());
            availOut = fgfit::toggleRoom (b.getWidth(), b.getHeight());
            return true;
        }
        if (dynamic_cast<const juce::TextButton*> (&b) != nullptr)
        {
            fontOut  = fgfit::textButtonFont (b.getHeight());
            availOut = fgfit::textButtonRoom (b.getWidth(), b.getHeight(),
                                              b.getProperties().contains ("fork"));
            maxLines = (float) b.getHeight() >= fontOut.getHeight() * 1.9f ? 2 : 1;
            return true;
        }
        return false;
    }

    // Word-wrap `text` in `font` at `width` and report the height it takes. The same
    // measurement drawFittedText makes before it starts condensing.
    float wrappedHeight (const juce::String& text, const juce::Font& font, float width,
                         juce::Justification just, float& widthOut)
    {
        juce::AttributedString as;
        as.setText (text);
        as.setFont (font);
        as.setJustification (just);
        as.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout (as, width);
        widthOut = tl.getWidth();
        return tl.getHeight();
    }
}

void FractalAdditiveAudioProcessorEditor::walkTextFits (
    const juce::Component& parent, const juce::String& path,
    std::vector<TextFit>& out) const
{
    int index = 0;
    for (auto* c : parent.getChildren())
    {
        const int myIndex = index++;
        if (c == nullptr || ! c->isVisible() || c->getWidth() <= 0) continue;
        // Most of these components have no name, so the index makes the path unique and
        // stable - and the report prints the TEXT and the rect too, which is what a hand
        // actually needs to find one.
        const auto here = path + "/" + probeNameFor (*c) + "#" + juce::String (myIndex);

        // THE HOVER, recorded as a row of its own.
        //
        // A tooltip is a user-visible string that is NEVER DRAWN into a box of ours, so
        // until this line it was invisible to every standing check in the build: the fit
        // sweep only walks drawn text, and the "translated?" gate compares exactly what
        // the fit sweep collected. That is why three of them (tuneScopeBox's, and the
        // octave / range steppers') sat in the constructor as bare English literals for
        // months with both rigs green - the same defect class commit 2b51e2f fixed for
        // eight others, which only a person happening to hover in Catalan could find.
        //
        // `need` and `avail` stay 0 on purpose: a TooltipWindow sizes ITSELF to the
        // string, so there is no box to overflow and over() is 0 - the fit sweep sees
        // these rows and correctly has nothing to say about them. What DOES read them is
        // the pair of language gates in SnapshotRig.cpp, which is the whole point.
        if (auto* tc = dynamic_cast<juce::TooltipClient*> (c))
        {
            const auto tip = tc->getTooltip();
            if (tip.isNotEmpty())
            {
                TextFit f;
                f.path = here; f.kind = "Tooltip"; f.text = tip;
                f.bounds = getLocalArea (c, c->getLocalBounds());
                out.push_back (f);
            }
    // ...and the same for every other component that paints its own words. Each entry
    // carries no geometry: it is here for the language checks, not for a fit measurement
    // it has not made. Keep this list in step with the trF hooks in applyLanguage - a
    // string that has a hook but no entry here is one the gate still cannot see.
    {
        const auto painted = [this, &out] (const char* path, juce::String text,
                                           const juce::Component& c)
        {
            TextFit f;
            f.path = path; f.kind = "Painted"; f.text = std::move (text);
            f.bounds = getLocalArea (&c, c.getLocalBounds());
            out.push_back (f);
        };
        painted ("waveEditor/hint",
                 tr ("LFO wave  -  drag points, double-click adds, right-click deletes,"
                     " drag a line's middle to curve it",
                     "Ona de l'LFO  -  arrossega els punts, doble clic n'afegeix,"
                     " clic dret n'esborra, arrossega el mig d'una l\xc3\xadnia per corbar-la"),
                 waveEditor);
        painted ("recentOverlay/empty",
                 tr ("nothing loaded yet", "encara no s'ha carregat res"), recentOverlay);
        painted ("dissHeader/step",    tr ("Step", "Grau"),      dissHeader);
        painted ("dissHeader/beating", tr ("beating", "batec"),  dissHeader);
        // THE TWO FLOATING EXPLAINERS, which nothing has ever measured. They paint their
        // own title and body, so walkTextFits cannot reach them, and they are only on
        // screen in states no scenario reaches - the two ways a string escapes every gate
        // in this build at once.
        //
        // The BODY is the one at risk: drawFittedText is given 8 lines and NO horizontal
        // squash, so a Catalan sentence that needs a ninth is not simply taller - it is
        // SHRUNK to fit or cut. Measured here with the same geometry paint() uses, and
        // the two overlays do NOT share it: the warning band is 26 px at 18/14 pt, the
        // notice band 24 px at 17/13.5. A probe that measures the wrong font measures
        // nothing, so both numbers are passed in rather than assumed equal.
        //
        // availH is the EIGHT-LINE cap, not the box: the box is 140 px tall and eight
        // 14 px lines are 112, so a body wrapping to nine lines would still "fit" the
        // box while paint() quietly shrank it. The question asked is paint's own.
        const auto overlayText = [this, &out] (const char* path, const juce::String& text,
                                               const juce::Component& c, bool isBody,
                                               int bandH, float titlePt, float bodyPt)
        {
            if (text.isEmpty()) return;
            auto in = c.getLocalBounds().reduced (22, 14);
            const juce::Font f (juce::FontOptions (isBody ? bodyPt : titlePt)
                                    .withStyle (isBody ? "" : "Bold"));
            auto band = in.removeFromTop (bandH);
            if (isBody) { in.removeFromTop (6); }
            TextFit t;
            t.path = path; t.kind = "Painted"; t.text = text;
            t.bounds = getLocalArea (&c, c.getLocalBounds());
            t.need   = juce::GlyphArrangement::getStringWidth (f, text);
            t.avail  = (float) (isBody ? in.getWidth() : band.getWidth());
            t.availH = isBody ? juce::jmin ((float) in.getHeight(), 8.0f * f.getHeight())
                              : (float) band.getHeight();
            if (isBody)
            {
                t.wraps = true;
                t.needH = wrappedHeight (text, f, t.avail, juce::Justification::topLeft, t.need);
            }
            out.push_back (t);
        };
        overlayText ("warningOverlay/title", warningOverlay.title, warningOverlay, false, 26, 18.0f, 14.0f);
        overlayText ("warningOverlay/body",  warningOverlay.body,  warningOverlay, true,  26, 18.0f, 14.0f);
        overlayText ("noticeOverlay/title",  noticeOverlay.title,  noticeOverlay,  false, 24, 17.0f, 13.5f);
        overlayText ("noticeOverlay/body",   noticeOverlay.body,   noticeOverlay,  true,  24, 17.0f, 13.5f);
        // THE NOTICE THE RIG CANNOT STAGE. "Fewer degrees than requested" only appears
        // after a dissonance search returns short, which a scenario cannot arrange
        // without running the search itself - so the longest sentence in the plugin has
        // never been measured. Asked here at its WORST CASE instead, which is stronger
        // than photographing one instance: the Notes box offers 1..24, so 24 and 23 are
        // the widest numbers the sentence can ever carry.
        overlayText ("noticeOverlay/worstBody", fewerDegreesBody (24, 23),
                     noticeOverlay, true, 24, 17.0f, 13.5f);
    }
        }

        // A string that fits on ONE line needs no wrapping test; one that does not may
        // still be fine if its box is tall enough for the extra lines the painter is
        // allowed to use. Both cases go through here so there is one rule, not three.
        auto record = [this, c, &out] (const juce::String& path_, const char* kind,
                                       const juce::String& text, const juce::Font& font,
                                       float avail, float availH, int maxLines,
                                       juce::Justification just)
        {
            TextFit f;
            f.path = path_; f.kind = kind; f.text = text;
            f.bounds = getLocalArea (c, c->getLocalBounds());
            f.need   = juce::GlyphArrangement::getStringWidth (font, text);
            f.avail  = avail;
            f.availH = availH;
            if (f.need > f.avail && maxLines > 1)
            {
                f.wraps = true;
                f.needH = wrappedHeight (text, font, avail, just, f.need);
                // Wrapped: the question is now whether the LINES fit, and never more
                // than the painter is allowed to draw.
                f.availH = juce::jmin (availH, font.getHeight() * (float) maxLines * 1.15f);
            }
            out.push_back (f);
        };

        if (const auto* box = dynamic_cast<const juce::ComboBox*> (c))
        {
            const auto txt = box->getText();
            if (txt.isNotEmpty())
            {
                // MEASURED OFF THE LABEL THE BOX REALLY DRAWS IN, through the same three
                // fgfit helpers the layout probe's combo() uses - one body, so the two
                // can never again disagree about the same box.
                //
                // What this used to be: getComboBoxFont's LIGHTER face measured against
                // fgfit::comboTextWidth, with the Label's own {1,5,1,5} border not taken
                // off and the arrow gutter understated by h*0.20 + 0.8 px (see
                // fgfit::comboArrowGutter). Three misses, all of them running towards "it
                // fits", which is why three photographed collisions between an item and
                // its own chevron - "Find tunings based on beating" at 1200x650 in both
                // languages, "Whitelist" at 690x550 - came back from this sweep as ZERO
                // fit problems. avail now subtracts the very gutter positionComboBoxText
                // reserves, because it IS the rectangle positionComboBoxText produced.
                const auto  font = fgfit::comboDrawnFont (*box);
                const float w    = (float) fgfit::comboDrawnRoom (*box);
                const float h    = (float) fgfit::comboDrawnRoomH (*box);
                // The label inside a ComboBox is a juce::Label, so a TALL box really does
                // get a second line - that is how the mode selector shows its full names.
                const int lines = juce::jmax (1, (int) (h / font.getHeight()));
                record (here, "ComboBox", txt, font, w, h, lines, juce::Justification::centredLeft);
                // ...AND THE WIDEST ITEM IT HOLDS, which is a different question and the
                // one that keeps finding defects. A closed ComboBox draws only the item
                // that is CHOSEN, so every picture measures whatever happened to be
                // selected when it was taken - the Shepard beats box sat at "4" in every
                // shot while "1/16" needed 31 px of the 28 it had, and the rig called it
                // fine. Asked here for every box at once instead of one scenario at a
                // time: whatever the user picks, the box must be able to show it.
                //
                // Reported as its own row so a box that is right about its selection and
                // wrong about its widest item names which of the two it failed.
                // ...EXCEPT where the items are the USER'S OWN DATA. A preset picker is
                // filled with whatever the user called their patches, and one of them here
                // is 64 characters long and needs 502 px: no box can be asked to hold a
                // name whose length nobody controls, and asking flagged four boxes in
                // every one of the 98 scenarios. RefreshingComboBox is exactly the set
                // that refills itself from disk, which is the same thing said in code.
                if (dynamic_cast<const RefreshingComboBox*> (box) == nullptr)
                {
                    juce::String widest;
                    float widestW = 0.0f;
                    for (int i = 0; i < box->getNumItems(); ++i)
                    {
                        const auto item = box->getItemText (i);
                        const float iw = juce::GlyphArrangement::getStringWidth (font, item);
                        if (iw > widestW) { widestW = iw; widest = item; }
                    }
                    if (widest.isNotEmpty() && widest != txt)
                        record (here + "#widest", "ComboBox", widest, font, w, h, lines,
                                juce::Justification::centredLeft);
                }
            }
            continue;   // its label is the box's own; do not measure it twice
        }
        // A SINGLE-LINE TEXT FIELD, which nothing in this build had ever measured.
        //
        // This sweep walked Labels, Buttons and ComboBoxes, and each of those fails
        // VISIBLY: a Label condenses, a button's word is cut, a combo draws what fits.
        // A juce::TextEditor does none of those - it SCROLLS. setText leaves the caret at
        // the end of the string (cursorWasAtEnd is true on an empty editor, so the very
        // first setText scrolls to the end), and a value wider than the field is then
        // truncated from the LEFT with no ellipsis and no clipped glyph. That is not a
        // cut number, it is a DIFFERENT number: the reference-pitch field displayed
        // "55653" for 261.6255653 Hz at 690x550, which reads as a perfectly plausible
        // 55653 Hz. Nothing was out of bounds, nothing overflowed, nothing went red.
        //
        // SINGLE-LINE ONLY. A multi-line editor (the custom engine's typed ratios) is a
        // scrolling document by design and holds however much the user types; asking it
        // to fit its content would report a problem on every keystroke and mean nothing.
        // The room is TextEditor::getMaximumTextWidth's own formula (see
        // fgfit::textEditorRoom), so this measures the width the text is really given.
        if (const auto* te = dynamic_cast<const juce::TextEditor*> (c))
        {
            const auto txt = te->getText();
            if (txt.isNotEmpty() && ! te->isMultiLine())
                record (here, "TextEditor", txt, te->getFont(),
                        (float) fgfit::textEditorRoom (*te),
                        (float) te->getBorder().subtractedFrom (te->getLocalBounds()).getHeight(),
                        1, juce::Justification::centred);
            continue;   // its viewport and text holder are plumbing, not our text
        }
        if (const auto* b = dynamic_cast<const juce::Button*> (c))
        {
            const auto txt = b->getButtonText();
            juce::Font font; float avail = 0.0f; int lines = 1;
            if (txt.isNotEmpty() && buttonTextRoom (*b, font, avail, lines))
                record (here, dynamic_cast<const juce::ToggleButton*> (b) != nullptr ? "ToggleButton" : "Button",
                        txt, font, avail, (float) b->getHeight(), lines, juce::Justification::centred);
            // A TICK BOX IS A CONTROL EVEN WITH NO WORD BESIDE IT.
            //
            // Everything above this line is a question about TEXT, so a ToggleButton whose
            // label lives in a separate juce::Label - the nine prime checkboxes - was read
            // as an empty string and skipped by every check in the build. At 690x550 they
            // are 6 px wide against a 20.5 px tick box that drawToggleButton starts at
            // x = 4: what reaches the glass is a 2 px sliver, and the row that decides
            // which primes the whole spectrum is allowed cannot be read or aimed at.
            //
            // Its own kind, not "ToggleButton": the no-tooltip gate keys on the kinds that
            // name controls, and this row is a SECOND measurement of a control that may
            // already have one - the same reason "#widest" rows are skipped there. `need`
            // is the tick's own right edge and `avail` the button's width, so over() reads
            // as "this many pixels of tick box are drawn outside the button".
            //
            // The "onled" pill has no tick at all (GalacticLnF draws it as a coloured
            // capsule with ON / OFF centred in the whole button) so it is not asked.
            if (const auto* t = dynamic_cast<const juce::ToggleButton*> (b);
                t != nullptr && t->getName() != "onled")
            {
                TextFit f;
                f.path = here + "#tick"; f.kind = "TickBox"; f.text = txt;
                f.bounds = getLocalArea (c, c->getLocalBounds());
                f.need   = (float) fgfit::toggleTickWidth (t->getHeight());
                f.avail  = (float) t->getWidth();
                f.availH = (float) t->getHeight();
                out.push_back (f);
            }
            continue;
        }
        if (const auto* l = dynamic_cast<const juce::Label*> (c))
        {
            const auto txt = l->getText();
            if (txt.isNotEmpty())
            {
                const auto area = l->getBorderSize().subtractedFrom (l->getLocalBounds());
                const auto font = l->getLookAndFeel().getLabelFont (const_cast<juce::Label&> (*l));
                const int lines = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
                record (here, "Label", txt, font, (float) area.getWidth(), (float) area.getHeight(),
                        lines, l->getJustificationType());
            }
            // No `continue`: a Slider's value box IS a Label and lives under the slider,
            // so the recursion below still has to reach it.
        }
        walkTextFits (*c, here, out);
    }
}

std::vector<FractalAdditiveAudioProcessorEditor::TextFit>
FractalAdditiveAudioProcessorEditor::textFitProbeForTests (bool useCatalan)
{
    const bool was = catalan;
    catalan = useCatalan;
    // applyLanguage() and a relayout, for the same reason layoutProbeInLanguage does it:
    // a Label's text was written once by applyLanguage, so probing on the flag alone
    // would measure this language's geometry against the other language's strings.
    applyLanguage();
    if (getWidth() > 0) resized();

    std::vector<TextFit> out;
    walkTextFits (*this, {}, out);

    // THE STRINGS A COMPONENT PAINTS FOR ITSELF, which walkTextFits cannot reach: it
    // walks juce::Components, and a word drawn straight onto a Graphics has no component
    // to find. That is not a technicality - it is how the keyboard's "REFERENCE" stayed
    // English through every run of the translated gate, in a window where every other
    // string changes language.
    //
    // It has a MEASUREMENT now, where before it went in with none. Its band is
    // jmax (keyWidth, kReferenceBandMinWidth), so the constant is the width the word is
    // guaranteed to get: a word that fits it fits at every key width, and one that does
    // not is clipped at the narrowest. That matters here more than anywhere - the word is
    // drawn with drawText (useEllipsis = false), the one call in this file that cuts a
    // string without leaving a mark on it.
    {
        TextFit f;
        f.path = "keyboard/reference"; f.kind = "Painted";
        f.text = keyboard.getReferenceWord();
        f.bounds = getLocalArea (&keyboard, keyboard.getLocalBounds());
        const juce::Font rf (juce::FontOptions (ScaleKeyboard::kReferenceFontHeight)
                                 .withStyle ("Bold"));
        f.need  = juce::GlyphArrangement::getStringWidth (rf, f.text);
        f.avail = ScaleKeyboard::kReferenceBandMinWidth;
        out.push_back (f);
    }

    // THE KEYBOARD'S THREE ROW CAPTIONS, painted onto its own Graphics exactly like
    // REFERENCE above, and only on screen while a key is HOVERED - which is both of this
    // build's ways of hiding a string at once. No snapshot scenario hovers a key, so the
    // pictures will never contain them: this registration is the ONLY thing that makes
    // them visible to the language gate, and it is deliberately NOT conditional on
    // hoveredNote, or the gate would see them in neither language and be satisfied.
    //
    // Measured against the gutter they are guaranteed, less its 3 px inset each side, so
    // a Catalan caption that would be drawn condensed fails the fit sweep here instead of
    // looking slightly wrong on somebody's screen.
    {
        const char* paths[3] = { "keyboard/rowSteps", "keyboard/rowCents", "keyboard/rowBeating" };
        const juce::String texts[3] = { tr ("steps", "passos"), tr ("cents", "cents"),
                                        tr ("beating", "batec") };
        const juce::Font cf (juce::FontOptions (ScaleKeyboard::kCaptionFontHeight).withStyle ("Bold"));
        for (int i = 0; i < 3; ++i)
        {
            TextFit f;
            f.path = paths[i]; f.kind = "Painted"; f.text = texts[i];
            f.bounds = getLocalArea (&keyboard, keyboard.getLocalBounds());
            f.need  = juce::GlyphArrangement::getStringWidth (cf, f.text);
            f.avail = (float) (ScaleKeyboard::kCaptionW - 6);
            out.push_back (f);
        }
    }

    // THE ONE WORD IN THE PENCIL WINDOW'S CELLS, which is only ON SCREEN when a partial
    // is muted - a state no scenario reaches - and is painted straight into the cell, so
    // walkTextFits could not reach it either. Both of this build's ways of hiding a
    // string at once, which is why it sat there in English. Declared here so the
    // translated gate sees it whether or not anything is muted.
    //
    // Its band is the cell's bottom 12 px, and the cell is never narrower than kCellW,
    // so kCellW - 4 is the width the word is guaranteed at any window size.
    {
        TextFit f;
        f.path = "partialAmpGrid/mute"; f.kind = "Painted";
        f.text = tr ("MUTE", "MUT");
        f.bounds = getLocalArea (&partialPanel, partialPanel.getLocalBounds());
        const juce::Font mf (juce::FontOptions (10.5f).withStyle ("Bold"));
        f.need  = juce::GlyphArrangement::getStringWidth (mf, f.text);
        f.avail = (float) (PartialAmpGrid::kCellW - 4);
        out.push_back (f);
    }

    // THE BLEND PAD'S OWN FOUR LABELS, which nothing could see. The pad paints them
    // straight onto its Graphics, so walkTextFits has no component to find, and until
    // scenario 52 no picture even contained one - the only playground scenario showed the
    // panel EMPTY, which is the one state where none of its text is real.
    //
    // What they carry is the corner letter, the preset's name and the blend percentage,
    // and the percentage is the number the whole pad exists to show. Measured against the
    // box paint() actually used, so a name that grows can never push it out again.
    {
        const auto& pad = playgroundPanel.pad;
        for (int i = 0; i < 4; ++i)
        {
            if (pad.lastLabel[i].isEmpty() || pad.lastLabelBox[i].isEmpty()) continue;
            TextFit f;
            f.path = "playgroundPad/corner" + juce::String::charToString ((juce::juce_wchar) ('A' + i));
            f.kind = "Painted";
            f.text = pad.lastLabel[i];
            f.bounds = pad.lastLabelBox[i];
            const juce::Font lf (juce::FontOptions (12.0f).withStyle ("Bold"));
            f.need  = juce::GlyphArrangement::getStringWidth (lf, f.text);
            f.avail = (float) pad.lastLabelBox[i].getWidth();
            f.availH = (float) pad.lastLabelBox[i].getHeight();
            out.push_back (f);
        }
    }

    // THE NATIVE DIALOG TITLES, read from the SAME table the call sites use (fgdlg), so
    // the gate cannot go stale by holding a copy of a string that has since changed.
    // With no geometry: the OS sizes its own title bar, so there is no width to measure
    // and this makes no claim to have measured one.
    {
        int i = 0;
        for (const auto& d : fgdlg::all)
        {
            TextFit f;
            f.path = "dialogTitle#" + juce::String (i++); f.kind = "Painted";
            f.text = tr (d.en, d.ca);
            out.push_back (f);
        }
    }

    // THE PANELS THAT ARE NORMALLY SHUT. The user's own example - "not in this
    // spectrum" - lives in the Spectrum-to-Rhythm panel, which is invisible until a
    // button is pressed, so a sweep of what happens to be on screen would never have
    // seen it. Opened here, walked, and put straight back.
    {
        const bool rhythmWas = rhythmPanel.isVisible();
        rhythmPanel.numRows = processorRef.getTemplatePartialCount();
        rhythmPanel.list.updateContent();
        rhythmPanel.refreshSlots();
        rhythmPanel.refreshLive();
        rhythmPanel.setVisible (true);
        if (getWidth() > 0) resized();
        walkTextFits (rhythmPanel, "rhythm", out);
        rhythmPanel.setVisible (rhythmWas);
    }
    // The two FLOATING panels are not children of this editor at all (they live in their
    // own windows), so they have to be walked by name. Given the size their windows open
    // at, so the measurement is the one the user sees.
    {
        playgroundPanel.applyLanguage();
        playgroundPanel.setSize (560, 520);
        playgroundPanel.refresh (true);
        walkTextFits (playgroundPanel, "playground", out);

        partialPanel.applyLanguage();
        partialPanel.setSize (760, 520);
        partialPanel.resized();
        walkTextFits (partialPanel, "partials", out);
    }

    catalan = was;
    applyLanguage();
    if (getWidth() > 0) resized();
    return out;
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
    else if (name == "dryWetTime") s = &dryWetTimeFader;
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

FractalAdditiveAudioProcessorEditor::IntervalBoxProbe
FractalAdditiveAudioProcessorEditor::typeIntervalsProbeForTests (const juce::String& typed,
                                                                 int backspaces, bool tickTimer)
{
    IntervalBoxProbe r;
    auto* peer = getPeer();   // present only when the caller put us on an off-screen peer

    auto press = [this, peer] (const juce::KeyPress& k)
    {
        if (peer != nullptr) peer->handleKeyPress (k);
        else                 intervalEdit.keyPressed (k);
    };
    // AFTER EVERY KEYSTROKE, the two things that really follow one in the plugin: the
    // POSTED onTextChange (delivered by hand - postCommandMessage never arrives without a
    // message loop, and a probe that trusted it would be testing nothing), and, if asked,
    // one tick of this editor's own 30 Hz timer.
    auto afterKey = [this, tickTimer]
    {
        recolourIntervalTokens();
        if (tickTimer) timerCallback();
    };

    setIntervalText ({});   // start from an empty box, through the one seeding helper
    if (peer != nullptr) intervalEdit.grabKeyboardFocus();

    // ONE KeyPress PER CHARACTER, not setText: a probe that assigns the text cannot see a
    // box that has stopped accepting keys, and "typing does nothing" is the report.
    for (auto p = typed.getCharPointer(); ! p.isEmpty(); ++p)
    {
        press (juce::KeyPress (0, juce::ModifierKeys(), *p));
        afterKey();
    }
    for (int i = 0; i < backspaces; ++i)
    {
        press (juce::KeyPress (juce::KeyPress::backspaceKey));
        afterKey();
    }

    r.boxText  = intervalEdit.getText();
    r.visible  = intervalEdit.isVisible();
    r.enabled  = intervalEdit.isEnabled();
    r.readOnly = intervalEdit.isReadOnly();
    r.bounds   = intervalEdit.getBounds();
    r.hadFocus = intervalEdit.hasKeyboardFocus (false);

    // RETURN. keyPressed() alone only reaches TextEditor::returnPressed(), which is a
    // postCommandMessage, so the handler is delivered by hand for the same reason.
    press (juce::KeyPress (juce::KeyPress::returnKey));
    if (intervalEdit.onReturnKey != nullptr) intervalEdit.onReturnKey();
    if (tickTimer) { timerCallback(); timerCallback(); }

    r.timbreSource = processorRef.getTimbreSource();
    r.timbreNotes  = processorRef.getTimbreNoteCount();
    return r;
}

FractalAdditiveAudioProcessorEditor::IntervalDraftProbe
FractalAdditiveAudioProcessorEditor::intervalDraftProbeForTests (const juce::String& edit)
{
    IntervalDraftProbe r;
    timerCallback();                       // let the re-seed put the loaded spectrum in
    r.seeded = intervalEdit.getText();

    if (auto* peer = getPeer()) { juce::ignoreUnused (peer); intervalEdit.grabKeyboardFocus(); }
    intervalEdit.setCaretPosition (intervalEdit.getTotalNumChars());
    for (auto p = edit.getCharPointer(); ! p.isEmpty(); ++p)
    {
        intervalEdit.keyPressed (juce::KeyPress (0, juce::ModifierKeys(), *p));
        recolourIntervalTokens();
    }
    r.afterEdit = intervalEdit.getText();

    // WHAT refocus() DOES, and it is called by nearly every control in this window: the
    // editor takes the keyboard back. No click on the box is undone by this; the draft
    // is simply no longer the focused component's.
    grabKeyboardFocus();
    timerCallback();
    r.afterTick = intervalEdit.getText();
    r.survived  = r.afterTick == r.afterEdit;
    return r;
}

FractalAdditiveAudioProcessorEditor::PencilUndoProbe
FractalAdditiveAudioProcessorEditor::pencilGestureUndoProbeForTests (int cell, float dB, int ticksMidDrag)
{
    PencilUndoProbe r;
    partialPanel.resync();                       // the knobs must match the live spectrum
    r.cells = partialPanel.grid.model.count();
    if (! partialPanel.grid.model.valid (cell)) return r;

    r.stepsBefore = processorRef.undoStackDepthForTests();

    // MOUSE DOWN. The real handler: it takes the undo-gesture hold, arms manual mode
    // (which opens the transaction) and captures the before-state.
    if (partialPanel.grid.onGestureStart) partialPanel.grid.onGestureStart();
    r.armed = partialPanel.armedManualThisGesture;

    // THE PARAMETER FLIP REACHES THE UNDO STACK, and it reaches it DURING the drag.
    // In the product that is APVTS's own 20-500 ms timer writing the value into the
    // ValueTree through the undo manager; a verifier has no message loop, so the same
    // flush is asked for by hand. Without this the flip would land at some arbitrary
    // later point and the probe would be measuring a timeline no user ever sees - and
    // the mid-drag cut, which is the whole question, would have nothing to separate.
    processorRef.apvts.copyState();
    r.stepsAfterFlush = processorRef.undoStackDepthForTests();

    // DRAG. One frame is enough: the defect is about WHEN the transaction boundary
    // falls, not about how many frames are inside it.
    partialPanel.grid.model.cells[(size_t) cell].dB = dB;
    if (partialPanel.grid.onEdited) partialPanel.grid.onEdited();

    // ...AND THE DRAG TAKES TIME. Each tick is one turn of the processor's 60 Hz timer,
    // so 60 of them is the ~1 s mark where the undo cut used to fall inside the gesture.
    for (int i = 0; i < ticksMidDrag; ++i) processorRef.pollParametersForTests();
    r.stepsAfterTicks = processorRef.undoStackDepthForTests();

    // MOUSE UP: commit, and release the hold.
    if (partialPanel.grid.onGestureEnd) partialPanel.grid.onGestureEnd();
    r.stepsAfter = processorRef.undoStackDepthForTests();

    // ONE Ctrl+Z. The claim under test is not "the stack is tidy" - it is that a single
    // undo puts the user back where they started, rather than leaving manual mode armed
    // over a spectrum nobody drew.
    r.undid = processorRef.performUndo();
    r.manualAfterUndo = processorRef.apvts.getRawParameterValue ("partialManual")->load() > 0.5f;
    r.editsAfterUndo  = (int) processorRef.getPartialGains().size();
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
                auto why = FractalAdditiveAudioProcessor::PresetWriteRefusal::None;
                int blockedCorner = -1;
                const auto stored = safe->processorRef.savePreset (name, why, blockedCorner);
                if (stored.isNotEmpty())
                {
                    safe->refreshPresetList();
                    safe->presetBox.setText (stored, juce::dontSendNotification);
                    safe->refreshPresetDirtyMark(); // the fresh save matches itself - no drift
                }
                else
                {
                    // THE REFUSAL IS SPOKEN, and it names the corner. A save the
                    // fixed-point rule turns down - this name is on a playground corner
                    // and the controls are showing the pad's BLEND, not that preset -
                    // must say so in those words: a silent no-op, or a generic "try
                    // another name", would leave the user believing the corner they were
                    // listening to had been saved, which is the same kind of lie as the
                    // drain itself (case #88). Any other failure keeps the old wording.
                    const auto letter = juce::String::charToString (
                        (juce::juce_wchar) ('A' + juce::jmax (0, blockedCorner)));
                    const auto msg =
                        why == FractalAdditiveAudioProcessor::PresetWriteRefusal::CornerFixedPoint
                          ? safe->tr ("\"%N%\" is on playground corner %C%, and these controls are "
                                      "showing the pad's blend, not that preset. Saving over it would "
                                      "make the corner a blend of itself, drifting a little further "
                                      "every time you reload the project. Save under a different name, "
                                      "or use that corner's own Edit button.",
                                      "\"%N%\" \xc3\xa9s a la cantonada %C% del laboratori, i aquests "
                                      "controls mostren la barreja del tauler, no pas aquell preset. "
                                      "Desar-hi a sobre convertiria la cantonada en una barreja d'ella "
                                      "mateixa, i s'allunyaria una mica m\xc3\xa9s cada cop que "
                                      "obr\xc3\xadssiu el projecte. Deseu-lo amb un altre nom, o feu "
                                      "servir el bot\xc3\xb3 Edit d'aquella cantonada.")
                                .replace ("%N%", name).replace ("%C%", letter)
                          : safe->tr ("Could not save the preset. Try a different name.",
                                      "No s'ha pogut desar el preset. Prova un altre nom.");
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        safe->tr ("Save preset", "Desar preset"), msg, safe.getComponent());
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

// N as it stands: what the user typed, or - until they type - the measured default
// (see adaptRegPeriods and dissonance::kAdaptRegisterDefaultPeriods).
double FractalAdditiveAudioProcessorEditor::registerPeriodsEffective() const
{
    return adaptRegPeriods >= 0.0 ? adaptRegPeriods : dissonance::kAdaptRegisterDefaultPeriods;
}

// ...and how much of that width the 128-key board really has BELOW and ABOVE the anchor.
// Both directions have their own answer now, and near either end of the keyboard they are
// different numbers - which is exactly why the read-out has a two-sided form. Read back OUT
// of the frequencies the search is really given (registerBandHz), never recomputed from the
// typed number, so a clamp the processor applies cannot fail to appear here.
double FractalAdditiveAudioProcessorEditor::registerPeriodsFrom (double endHz) const
{
    const double a = processorRef.getNoteFrequency (processorRef.registerAnchorKey());
    const double perC = 1200.0 * std::log2 (juce::jmax (1.0001, processorRef.registerPeriodInfo().ratio));
    if (! (a > 0.0) || ! (endHz > 0.0) || ! std::isfinite (endHz)) return 0.0;
    return std::abs (1200.0 * std::log2 (endHz / a)) / perC;
}

double FractalAdditiveAudioProcessorEditor::registerPeriodsFittingDown() const
{
    double lo = 0.0, hi = 0.0;
    processorRef.registerBandHz (registerPeriodsEffective(), lo, hi);
    return registerPeriodsFrom (lo);
}

double FractalAdditiveAudioProcessorEditor::registerPeriodsFittingUp() const
{
    double lo = 0.0, hi = 0.0;
    processorRef.registerBandHz (registerPeriodsEffective(), lo, hi);
    return registerPeriodsFrom (hi);
}

// IS THE PERIOD A FACT? For a .scl or an MTS master, yes - the tuning states it, and the
// band is exact. For an absolute 128-key .tun there is no scale behind the pitches at all,
// so the number the band is built on is either Fibló's own estimate for the file (with the
// worst deviation it measured) or, when the file has nothing ascending to measure, the
// 2/1-over-12-keys fallback that nobody supplied. A band whose unit is a guess must SAY it
// is a guess: "silently wrong" is the one outcome this instrument does not allow itself.
juce::String FractalAdditiveAudioProcessorEditor::registerPeriodQualifier() const
{
    const auto pi = processorRef.registerPeriodInfo();
    using Src = FractalAdditiveAudioProcessor::RegisterPeriodSource;
    if (pi.source == Src::Scale) return {};
    return registerPeriodQualifierText (pi.source == Src::TableEstimate);
}

// ...as a function of which of the two it is, so the layout probe can measure the WIDER of
// them (the one nobody sees until they load a .tun with nothing ascending in it). Deliberately
// only a flag and not the deviation in cents: the number was 7 px too wide for this line at
// its worst case, and it is already printed in full where it belongs - the Tuning panel's own
// period field, which is where the user changes it. This line's job is to stop the width
// LOOKING like a fact when the tuning has no period of its own.
juce::String FractalAdditiveAudioProcessorEditor::registerPeriodQualifierText (bool estimated) const
{
    if (estimated) return " (est.)";
    // "per." on the Catalan side, matching the abbreviation the same line already uses for
    // the unit: written out, "cap per\xc3\xado" "de" is 14 characters and put this line 6 px past
    // its label at 9 pt (the layout probe measured it), and shrinking the whole line again
    // for one marker would cost every other form its legibility.
    return catalan ? juce::String (" (cap per.)") : juce::String (" (no period)");
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

// WHAT N MEANS RIGHT NOW, in one line: the note the band is counted from, how far it
// reaches in each direction, and the two keys those reaches land on. Without it N is a
// magic number and the anchor is invisible, which is the whole failure mode of a band that
// follows the hands.
//
// TWO FORMS, and the second one is the reason the first is not enough. Normally the band is
// symmetric and the line reads "+-1.0 per"; at either end of the keyboard one side is
// genuinely shorter than N, and then the line reads "-0.4/+1.0 per" instead. A control that
// showed N while the search used four steps below would be the panel lying about its own
// settings - and unlike the upward-only form this one can be cut at EITHER end.
// ...as a function of its own numbers, so the layout probe can measure BOTH forms at their
// widest. Only one is ever on screen, and the one that overflows is always the other.
juce::String FractalAdditiveAudioProcessorEditor::adaptRegSpanLine (const juce::String& anchor,
                                                                    double periods,
                                                                    double fitDown, double fitUp,
                                                                    const juce::String& loHz,
                                                                    const juce::String& hiHz,
                                                                    const juce::String& qualifier) const
{
    // Built by hand rather than through tr(): the numbers are runtime values, so this is
    // the one place where the two languages are assembled instead of chosen. Both forms
    // still exist in both languages - the layout probe measures all four.
    const bool full = std::abs (fitDown - periods) < 0.05 && std::abs (fitUp - periods) < 0.05;
    const juce::String width = full
        ? "+-" + juce::String (periods, 1)
        : "-" + juce::String (fitDown, 1) + "/+" + juce::String (fitUp, 1);
    // The two ends are the FREQUENCIES the search is really given, not key names. A band a
    // period and a half wide does not land on a key at all - that was the whole reason the
    // ends stopped being resolved through the key ladder - so naming a key there would be
    // naming the wrong pitch, and this way the line prints the search's own two numbers.
    //
    // ANCHOR FIRST, then the width, then the ends, with no connecting words: the line
    // carries four numbers plus a note name at 9.5 pt, and the English "N per around 3:0"
    // was 7 px past the label in Catalan, where "al voltant de" is three words. Reordered
    // it needs no preposition at all, so the two languages differ only in the decimal
    // comma - and the layout probe measures both anyway.
    const juce::String body = anchor + " " + (catalan ? width.replaceCharacter ('.', ',') : width)
                            + " per = " + loHz + ".." + hiHz + " Hz";
    return body + qualifier;
}

// One rule for both ends: a decimal below 100 Hz (where a cent is worth less than a tenth
// of a hertz and an integer would round two different bands to the same number) and none
// above it (where the decimal is noise and costs the line width it does not have).
juce::String FractalAdditiveAudioProcessorEditor::registerHzText (double hz) const
{
    if (! (hz > 0.0) || ! std::isfinite (hz)) return "-";
    const auto s = hz < 100.0 ? juce::String (hz, 1) : juce::String ((int) std::lround (hz));
    return catalan ? s.replaceCharacter ('.', ',') : s;
}

juce::String FractalAdditiveAudioProcessorEditor::adaptRegSpanText() const
{
    double lo = 0.0, hi = 0.0;
    processorRef.registerBandHz (registerPeriodsEffective(), lo, hi);
    return adaptRegSpanLine (registerKeyText (processorRef.registerAnchorKey()),
                             registerPeriodsEffective(),
                             registerPeriodsFrom (lo), registerPeriodsFrom (hi),
                             registerHzText (lo), registerHzText (hi),
                             registerPeriodQualifier());
}

// The anchor MOVES - it is the note being played - so the line under the field has to be
// rebuilt as the hands move, and the follower has to hear about it. Written only when the
// text really changed: this runs at timer rate, and re-setting a Label's text repaints it.
void FractalAdditiveAudioProcessorEditor::syncAdaptRegisterSpan()
{
    const auto want = adaptRegSpanText();
    const auto n = juce::String (registerPeriodsEffective(), 1);
    // Never with sendNotification and never when it already reads that, so a field the user
    // is typing in is not yanked out from under the caret. (Unlike the step count this
    // replaces, N is now in the tuning's own units, so a scale change never rewrites it -
    // this only re-states the number after a commit or a restore.)
    if (adaptRegPeriodsEdit.getText() != n && ! adaptRegPeriodsEdit.hasKeyboardFocus (true))
        adaptRegPeriodsEdit.setText (n, juce::dontSendNotification);
    if (want == adaptRegSpanLbl.getText()) return;
    adaptRegSpanLbl.setText (want, juce::dontSendNotification);
    syncRealtimeLink();   // the follower's settings carry the range, so it must hear this
}

// The user typed a width, in PERIODS. Unreadable text snaps back to the number in force
// instead of being taken as "no width" - which the processor would read as the default and
// quietly undo what they were trying to type. A typed number is theirs from then on, and
// needs no protection from a tuning change: periods are the tuning's own units, so "1.5"
// means one and a half periods of whatever is loaded, before the swap and after it.
void FractalAdditiveAudioProcessorEditor::commitAdaptRegisterPeriods()
{
    const auto s = adaptRegPeriodsEdit.getText().trim();
    if (s.isNotEmpty() && s.containsOnly ("0123456789.,"))
        // 0 IS legal and means "just the note I am playing" - one register, the old
        // single-pitch objective, chosen on purpose rather than arrived at by accident.
        // The ceiling is kAdaptRegisterMaxPeriods, which is past the keyboard in every
        // tuning this instrument loads, so the clamp is a guard against a typo pricing a
        // run in hours and never the thing that decides a musical question.
        adaptRegPeriods = juce::jlimit (0.0, dissonance::kAdaptRegisterMaxPeriods,
                                        s.replaceCharacter (',', '.').getDoubleValue());
    adaptRegPeriodsEdit.setText (juce::String (registerPeriodsEffective(), 1), juce::dontSendNotification);
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
    // THE REGISTER BAND, as the PERIOD COUNT the field shows. Not as two frequencies: the
    // band hangs off the note being played, and Hz resolved here would freeze the anchor
    // at whatever was under the fingers when this ran - which, with the window shut, is for
    // ever. The processor turns anchor +- N periods into Hz through the active tuning (see
    // adaptRegistersFor), which is also where the octave shift, the reference A and a .tun
    // table's anchor live, so nothing here has to know about any of them.
    s.adapt.regPeriods = registerPeriodsEffective();
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
    r.periods   = registerPeriodsEffective();
    r.anchorKey = processorRef.registerAnchorKey();
    processorRef.registerBandHz (r.periods, r.loHz, r.hiHz);
    r.fitDown   = registerPeriodsFrom (r.loHz);
    r.fitUp     = registerPeriodsFrom (r.hiHz);
    r.periodsText = adaptRegPeriodsEdit.getText();
    r.spanText    = adaptRegSpanText();
    return r;
}

void FractalAdditiveAudioProcessorEditor::setAdaptRegisterPeriodsForTests (const juce::String& n)
{
    adaptRegPeriodsEdit.setText (n, juce::dontSendNotification);
    commitAdaptRegisterPeriods();   // exactly what Return and focus-lost do
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

// How many notes a searched combination has - the shared "Notes" box. A test that
// reads the degree list has to be able to set this: the list is combinations of N
// scale degrees, so N against a scale SMALLER than N is legitimately empty, and a
// check that cannot tell "empty because there is nothing to find" from "empty
// because it broke" is not a check.
void FractalAdditiveAudioProcessorEditor::setChordNotesForTests (int n)
{
    chordNotesBox.setSelectedId (juce::jlimit (2, 8, n), juce::sendNotificationSync);
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
    // that quietly stopped being early and never said so. "late" is the whole point of
    // the read-out; it is not decoration.
    //
    // SENTENCE CASE, for the same reason as the two Realtime buttons above and with the
    // same benefit: this button sits directly under the follower it belongs to, on the
    // column's own full width, and it is the longest string in the stack - "ANTICIPACIO
    // 250 ms | darrer 310 ms TARD" in caps is the widest thing this 192 px column is ever
    // asked to draw. Lower-case letters are narrower, so the state that has the most to
    // say is the state that gains the most room to say it. The WARNING still reads as one:
    // it is the only word in the line that is not a number or a unit.
    juce::String txt;
    if (active)
    {
        const double budMs = (double) processorRef.getLookaheadSamples() * 1000.0
                           / juce::jmax (1.0, processorRef.getSampleRate());
        txt = tr ("Lookahead ", "Anticipaci\xc3\xb3 ") + juce::String ((int) std::lround (budMs)) + " ms";
        if (const double land = processorRef.getLastRealtimeLandingMs(); land >= 0.0)
            txt << tr (" | last ", " | darrer ") << juce::String ((int) std::lround (land)) << " ms"
                << (land <= budMs ? juce::String() : tr (" late", " tard"));
    }
    else if (armed) txt = tr ("Lookahead (idle)", "Anticipaci\xc3\xb3 (en espera)");
    else            txt = tr ("Lookahead", "Anticipaci\xc3\xb3");
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
    // THE MISSING PERIOD ROW, SAID OUT LOUD. A descending .scl and a degenerate 1/1
    // scale state no ascending period, so there is no "degree 0, one period up" to rank
    // and the list stops one row early (computeActiveScaleDissonance). Without this line
    // the user would just see a shorter list and no reason for it.
    degreeList.noteText = processorRef.tuningStatesPeriod()
                            ? juce::String()
                            : tr ("this scale states no period, so there is no period row",
                                  "aquesta escala no declara cap per\xc3\xado" "de, o sigui que no hi ha fila de per\xc3\xado" "de");
    // Re-size for the Viewport (scrolls when the list is taller than the box).
    const int visH = dissView.getHeight();
    const int ch   = degreeList.contentHeight();
    const bool scroll = ch > visH && visH > 0;
    degreeList.setSize (scroll ? dissView.getWidth() - 10 : dissView.getWidth(),
                        juce::jmax (visH, ch));
    degreeList.repaint();
}

// THE "FEWER DEGREES" BODY, in ONE place. It used to be assembled inline where it was
// shown, in a state the rig cannot photograph without running a whole dissonance search,
// so the longest sentence in the plugin was never measured against the eight lines
// drawFittedText gives it. Built here instead, so the probe can ask the question with
// the widest numbers the Notes box can produce - it offers 1..24, so two digits each -
// without a second copy of the words to drift from this one.
juce::String FractalAdditiveAudioProcessorEditor::fewerDegreesBody (int requested, int found) const
{
    return tr ("You asked for ", "N'has demanat ")
        + juce::String (requested)
        + tr (" but this timbre's dissonance curve only has ", " per\xc3\xb2 la corba de dissonancia d'aquest timbre nom\xc3\xa9s t\xc3\xa9 ")
        + juce::String (found)
        + tr (" usable extremes in the chosen period. A curve only forms a notch where partials interact; sparse spectra (few partials, e.g. a near-sine) form few notches - a pure sine forms none. The scale was generated with all the extremes that exist. Add partials (higher N, Fractal, flatter Decay) or widen the period for more.",
              " extrems usables dins del per\xc3\xado" "de triat. La corba nom\xc3\xa9s fa un clot on els parcials interactuen; els espectres escassos (pocs parcials) en fan pocs - un sinus pur no en fa cap. L'escala s'ha generat amb tots els extrems existents. Afegeix parcials (N m\xc3\xa9s alta, Fractal, Caiguda m\xc3\xa9s plana) o eixampla el per\xc3\xado" "de per obtenir-ne m\xc3\xa9s.");
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
        noticeOverlay.body  = fewerDegreesBody (processorRef.getDissRequested(),
                                                 processorRef.getDissFound());
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
        tr (fgdlg::saveGen.en, fgdlg::saveGen.ca),
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
                        tr (fgdlg::saveScale.en, fgdlg::saveScale.ca), err, this);
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
    // The switch first: with PRIME CONTROL off there is no prime filter acting, so a
    // diagnosis that blames the whitelist or the limit is telling the user to go and
    // change a control that is not doing anything. Read through the same gate the audio
    // path uses (see prepareSettings) and the branches below stay true by construction.
    const bool pon       = ap.getRawParameterValue ("primeOn")->load() > 0.5f;
    const bool wl        = pon && ap.getRawParameterValue ("primeMode")->load() > 0.5f;
    const int  limitIdx  = pon ? (int) ap.getRawParameterValue ("primeLimit")->load() : 0;
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
    else if (! pon)
        why = tr ("Prime Control is OFF, so nothing is being filtered by primes - the Harmonics setting alone is removing every harmonic of the current Base N.\n\nFix: set Harmonics to 'All', or change Base N.",
                  "El Control de primers est\xc3\xa0 desactivat, o sigui que els primers no filtren res - \xc3\xa9s nom\xc3\xa9s el filtre d'Harm\xc3\xb2nics el que treu tots els harm\xc3\xb2nics de la Base N actual.\n\nSoluci\xc3\xb3: posa Harm\xc3\xb2nics en 'Tots', o canvia la Base N.");
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
        // SOFTWARE, not the default native image type. On Windows a JUCE 8 native image
        // is a Direct2D bitmap, and this throwaway one is never read back on purpose -
        // its Graphics context is destroyed, which is where Direct2D's own
        // createPersistentBackup runs. Under the self-snapshot rig (which builds one
        // instrument per scenario and calls this in a loop) that readback died in
        // juce::readFromDirect2DBitmap, taking the whole run with it. A plain CPU bitmap
        // has no device to lose, and this image's only job was ever to make paint() run.
        juce::Image img (juce::SoftwareImageType().create (juce::Image::ARGB,
                                                           juce::jmax (1, w), juce::jmax (1, h), true));
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

// THE TRANSITION BOX IS A VIEW OF TWO BOOLEANS, and this is the half that reads them.
// It runs off the 30 Hz timer, so the box follows the parameters however they move - a
// host automation lane, a modulator, a preset, a playground corner, an undo - not just
// the user's own click on it.
//
// FOUR STATES, THREE SOUNDS. Both features are the same crossfade between two banks of
// oscillators inside a voice (see AdditiveVoice.h and bf0f768), so exactly one can act
// and MORPH WINS: a patch carrying both flags SOUNDS like Morph, so Morph is what the
// box shows. That is not a decision made here, it is the voice's rule reported honestly -
// and it is why an old project with both flags set opens on the right item without any
// migration. Nothing is written back: leaving the flags as the user's own state means a
// lane that raises and lowers "dryWet" under a held Morph still means exactly what it
// meant, instead of being silently rewritten by the panel it is being watched on.
void FractalAdditiveAudioProcessorEditor::applyTransitionMode()
{
    const bool morphOn = processorRef.apvts.getRawParameterValue ("morph")->load()  > 0.5f;
    const bool dwOn    = processorRef.apvts.getRawParameterValue ("dryWet")->load() > 0.5f;
    const int  item    = morphOn ? 2 : (dwOn ? 3 : 1);
    if (item == lastTransitionShown) return;    // timer rate: everything below repaints
    lastTransitionShown = item;
    transitionBox.setSelectedId (item, juce::dontSendNotification);

    // WHICH Time fader is on screen. Morph shows its own; Dry/Wet and Off both show the
    // crossfade's. Off shows it because Dry/Wet is the mode the box defaults to, so the
    // greyed number is the time that will apply the moment the user leaves Off - and
    // because the alternative, remembering the last live mode, is a piece of UI-only
    // state that a reopened project cannot restore and that would put a different number
    // under the same patch depending on what had been clicked first.
    const bool showMorph = (item == 2);
    morphTimeFader.setVisible (showMorph);
    dryWetTimeFader.setVisible (! showMorph);

    // GREYED WHEN THERE IS NOTHING TO TIME. With the transition Off no fade of any kind
    // runs, so a live fader here would be the panel offering to time something that is
    // not happening - the same failure the AMP DECAY / FROZEN pair exists to prevent, and
    // answered the same way.
    morphTimeFader.setEnabled (item != 1);
    dryWetTimeFader.setEnabled (item != 1);
    applyTransitionFaderColour();
    resized();                     // the fader that just appeared has to be given the row
    repaint (bothEnginesBounds);
}

// THE ONE WRITER of the two Time faders' colour, for the reason applySlopeFaderColour is
// the one writer of SLOPE's: they have a second state (the transition Off, where they can
// time nothing) and a control whose colour is set from two places ends up wearing
// whichever of them ran last.
void FractalAdditiveAudioProcessorEditor::applyTransitionFaderColour()
{
    const bool off = processorRef.apvts.getRawParameterValue ("morph")->load()  <= 0.5f
                  && processorRef.apvts.getRawParameterValue ("dryWet")->load() <= 0.5f;
    const auto col = off ? juce::Colour (0xff5a6070) : spectrumPanelColour;
    for (juce::Slider* s : { &morphTimeFader, &dryWetTimeFader })
    {
        s->setColour (juce::Slider::thumbColourId, col);
        s->setColour (juce::Slider::trackColourId, col.withAlpha (0.55f));
    }
}

// The other half: the box's own gesture, written back to the two parameters. One gesture
// each, because a host recording this sees two lanes move and both have to be a complete
// begin/end pair or the DAW keeps writing after the mouse is up.
void FractalAdditiveAudioProcessorEditor::setTransitionParams (bool morphOn, bool dryWetOn)
{
    auto put = [this] (const char* id, bool on)
    {
        if (auto* p = processorRef.apvts.getParameter (id))
        {
            const float v = on ? 1.0f : 0.0f;
            if (juce::approximatelyEqual (p->getValue(), v)) return;   // no empty gestures
            p->beginChangeGesture();
            p->setValueNotifyingHost (v);
            p->endChangeGesture();
        }
    };
    put ("morph",  morphOn);
    put ("dryWet", dryWetOn);
    applyTransitionMode();   // the box drew itself; the fader and the greying follow now
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
    //
    // WHERE IT OPENS. It used to be juce::File(), which means "wherever the OS last
    // left this process" - in practice the host's own folder, or the last place ANY
    // dialog in the session went, which for Fibló is the preset folder. A synth that
    // ships six tunings inside itself and then opens its tuning dialog somewhere else
    // is hiding them. So: the bundled folder on a first run (getTuningChooserFolder
    // extracts it on the way past), and after that the folder the user last actually
    // loaded a tuning FROM - see the callback.
    fileChooser = std::make_unique<juce::FileChooser> (tr (fgdlg::openTuning.en, fgdlg::openTuning.ca),
                                                       processorRef.getTuningChooserFolder(),
                                                       "*.tun;*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.loadScalaFile (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr (fgdlg::loadScale.en, fgdlg::loadScale.ca), err, this);
                else
                    // REMEMBERED ONLY ON A SUCCESSFUL LOAD. The user asked for the
                    // folder they LOAD from ("si el usuario carga desde otra carpeta"),
                    // and browsing into a folder to click a file that turns out to be
                    // unreadable is not loading from it - remembering that one would
                    // send them back to a dead end next time.
                    processorRef.setTuningChooserFolder (fc.getResult().getParentDirectory());
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::openTuningBankChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> (tr (fgdlg::openBank.en, fgdlg::openBank.ca),
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
    fileChooser = std::make_unique<juce::FileChooser> (tr (fgdlg::openTimbre.en, fgdlg::openTimbre.ca),
                                                       juce::File(), "*.tun;*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.loadTimbreFile (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr (fgdlg::loadTimbre.en, fgdlg::loadTimbre.ca), err, this);
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
        ? tr ("Delete the loaded custom spectrum and hand the sound back to the harmonic engine. One Ctrl+Z brings it back. To keep the spectrum but stop it sounding, click the HARMONIC chip instead.",
              "Esborra l'espectre propi carregat i torna el so al motor harm\xc3\xb2nic. Un Ctrl+Z el recupera. Si el vols conservar per\xc3\xb2 que no soni, clica el xip HARM\xc3\x92NIC.")
        : tr ("Load a .tun/.scl/.ascl whose intervals become the PARTIALS of the sound - the CUSTOM chip lights and this engine replaces the harmonic one. Decay, Fractal, Range and Reps still shape it.",
              "Carrega un .tun/.scl/.ascl els intervals del qual passen a ser els PARCIALS del so - s'enc\xc3\xa9n el xip PROPI i aquest motor substitueix l'harm\xc3\xb2nic. Caiguda, Fractal, Abast i Reps encara hi actuen."));
    randomSpectrumBtn.setButtonText (tr ("Random", "Atzar"));
    randomSpectrumBtn.setTooltip (tr (
        "A new random spectrum, different every press: 5 to 12 partials per period, spread evenly in PITCH (not in Hz) over the reach your Octave Range and Reps already set, never closer than 35 cents so no two of them collapse into one beating partial. The period stays a whole number of octaves, so Reps stacks it cleanly instead of smearing. Automatable: a 0-to-1 step drawn in the host fires it exactly like this click, with the window shut. One Ctrl+Z brings the previous spectrum back.",
        "Un espectre nou a l'atzar, diferent a cada pulsaci\xc3\xb3: de 5 a 12 parcials per per\xc3\xado" "de, repartits uniformement en TO (no en Hz) dins l'abast que ja fixen l'Abast d'octaves i les Reps, i mai a menys de 35 cents perqu\xc3\xa8 cap parell no es fongui en un sol parcial batent. El per\xc3\xado" "de es queda en un nombre enter d'octaves, aix\xc3\xad que les Reps l'apilen net en comptes d'esborronar-lo. Automatitzable: un escal\xc3\xb3 de 0 a 1 dibuixat a l'amfitri\xc3\xb3 el dispara igual que aquest clic, amb la finestra tancada. Un Ctrl+Z recupera l'espectre anterior."));
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

// THE RISE INTERVAL, AND WHY THE CHOICE MAY NOT BE THERE.
//
// The rise-interval box appears only when the tuning's period differs from the octave -
// otherwise there is nothing to pick between. That gate used to be asked of
// periodRatio(), which answers 2.0 when the tuning states NO ascending period at all
// (a descending .scl, a degenerate 1/1 scale): the box vanished, the rise ran at 2/1,
// and the reason - "your scale has no period for me to offer you" - existed nowhere on
// screen. The tuning's own OFF button says why it is off; this is the same courtesy for
// a control that is not there at all.
//
// It owns the whole SHEPARD hover for the same reason refreshShepardSyncState owns the
// beats box's: a sentence assembled from two places is a sentence that drifts.
void FractalAdditiveAudioProcessorEditor::refreshShepardRiseState()
{
    // statedPeriodRatio(), never the display fallback - the three states have to be told
    // apart by the FACT, and the fallback collapses two of them into one.
    const int state = ! processorRef.tuningStatesPeriod() ? 2
                    : shepardRiseChoiceExists()           ? 1 : 0;
    if (state == lastShepRiseState) return;
    lastShepRiseState = state;

    juce::String tip = tr (
        "The key you press becomes the root of a tone that rises for ever without ever getting higher - built from THIS synth's spectrum, not from sine waves. The voice sounds its whole spectrum at up to 8 transpositions AT ONCE, one rise interval apart, anchored on the pressed key's own tuned pitch, all climbing together under a FIXED spectral window whose edges reach exactly zero, so copies leave the top and enter underneath in silence - no click, nothing to hide. Everything else about the note is untouched - the three envelopes, the filter, the modulators, morph, manual amplitudes - and letting go of the key releases it like any other note. Latched PER NOTE: flipping this changes the notes you play NEXT, never the ones already sounding. The bounds are stated, never silent: at most 8 copies (the band narrows, centred, rather than losing one) and up to 1638 partials each; a spectrum dense enough to need more lifts the window's bottom edge instead of dropping a single partial.",
        "La tecla que prems es converteix en l'arrel d'un to que puja sense parar sense arribar mai m\xc3\xa9s amunt - fet amb l'espectre D'AQUEST sinte, no amb sinusoides. La veu fa sonar tot el seu espectre en fins a 8 transposicions ALHORA, separades un interval de pujada, ancorades al to real de la tecla premuda i pujant totes juntes sota una finestra espectral FIXA amb vores que arriben exactament a zero, o sigui que les c\xc3\xb2pies surten per dalt i entren per sota en silenci - sense clics ni res a amagar. La resta de la nota no canvia gens - les tres envolupants, el filtre, els moduladors, el morph, les amplituds manuals - i deixar anar la tecla l'allibera com qualsevol nota. Es fixa PER NOTA: canviar-ho afecta les notes que toquis DESPR\xc3\x88S, mai les que ja sonen. Els l\xc3\xadmits es diuen, mai s'amaguen: com a molt 8 c\xc3\xb2pies (la banda s'estreny, centrada, en comptes de perdre'n cap) i fins a 1638 parcials cadascuna; un espectre prou dens puja la vora inferior de la finestra en comptes de perdre ni un sol parcial.");
    if (state == 2)
        tip += tr ("\n!This scale states no period! (it descends, or its period is 1/1), so there is no scale period to rise by and the rise-interval choice is not offered: one step is an octave, 2/1. Nothing about your tuning is changed - the pitches are still the file's own.",
                   "\n!Aquesta escala no declara cap per\xc3\xado" "de! (baixa, o el seu per\xc3\xado" "de \xc3\xa9s 1/1), o sigui que no hi ha per\xc3\xado" "de d'escala per pujar i no s'ofereix l'opci\xc3\xb3 d'interval de pujada: un pas \xc3\xa9s una octava, 2/1. La teva afinaci\xc3\xb3 no canvia gens - els tons segueixen sent els del fitxer.");
    shepOnT.setTooltip (tip);
    // The arrow's own hover. Its glyph says WHICH way; this says what the other way is
    // for, and that the switch is safe to hit while a note is sounding.
    shepDirBtn.setTooltip (tr (
        "*Which way the tone goes.* Up is the endless rise; down is the same illusion falling for ever - the classic demonstration is a pair of them. Press it while a note is sounding and the stack decelerates through zero and comes back the other way over about one buffer, so there is nothing to hear but the turn.",
        "*Cap on va el to.* Amunt \xc3\xa9s la pujada infinita; avall \xc3\xa9s la mateixa il\xc2\xb7lusi\xc3\xb3 baixant per sempre - la demostraci\xc3\xb3 cl\xc3\xa0ssica \xc3\xa9s una parella. Prem-lo amb una nota sonant i la pila frena fins a zero i torna cap a l'altre cant\xc3\xb3 en aproximadament un buffer, o sigui que nom\xc3\xa9s se sent el gir."));
}

// ONE PANEL, ONE COLOUR. Every control inside SPECTRUM takes the selected engine's hue,
// so nothing on screen is left claiming the old nested boxes still exist: the panel used
// to be three framed boxes in three different colours (blue harmonic / pink custom /
// mint "both engines" with a magenta Fractal), and after the frames became plain divider
// lines those leftover hues were the only thing still drawing the boundaries.
//
// THE OCTAVE RANGE STEPPERS ARE IN THE LIST NOW, and this comment used to be the reason
// they were not: "green = up, red = down ... those colours mean an ACTION". They did not.
// Green and red mean LOAD and DESTROY everywhere else in this window, and a stepper that
// adds one octave to a range does neither - it is the most reversible edit the panel has.
// Wearing the two loudest colours the product owns, it put the delete-a-bank-slot red on
// an increment, which is how red came to carry five unrelated jobs at once. The pair now
// takes the panel hue like everything else here and lets its GLYPH carry the direction
// (see configStep, and GalacticLnF's drawn +/-, which finally weigh the same).
//
// Still deliberately NOT recoloured: the Load/Remove button (green loads, red DELETES a
// loaded spectrum). That one really is destructive, which is the one job red keeps.
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
                             (juce::Slider*) &fractalFader })
        fader (*s);
    // slopeFader is deliberately NOT in that list: it has a SECOND state (frozen by manual
    // per-partial amplitudes) and exactly one function is allowed to write its colour.
    // See applySlopeFaderColour - two writers is what turned it green.
    applySlopeFaderColour();
    // The two TRANSITION Time faders are out of the list for exactly the same reason and
    // were the same accident waiting to happen: they too have a second, greyed state (the
    // transition Off), and while they sat in the list an engine switch repainted the grey
    // away and the enablement's own before/after guard then refused to put it back.
    applyTransitionFaderColour();
    for (juce::ComboBox* b : { &basePitchBox, &primeModeBox, &transitionBox }) combo (*b);
    for (auto* l : { &lblBaseN, &lblBasePitch, &lblPrimeLimit, &lblTimbreRepeats, &lblTransTime })
        label (*l);
    for (int i = 0; i < 9; ++i) { tick (primeToggle[i]); label (primeLbl[i]); }
    tick (primeOnToggle); // the same panel, one level up
    // TEMPER's tick used to be forced to the harmonic blue "because that is the engine it
    // hands the spectrum to" - which was the whole point when the two engines were two
    // boxes, and is now just one control refusing to join the panel it lives in.
    tick (temperToggle);
    // OCTAVE RANGE's stepper pair - one control, one colour, the panel's own.
    for (auto* b : { &sprDn, &sprUp })
        b->setColour (juce::TextButton::buttonColourId, col.withAlpha (0.45f));
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
    // canTemper rides in the same signature the timer watches, so LOADING A TUNING with
    // no usable period re-dresses the toggle even when temper is off and the engine in
    // force does not change. Without it the control would keep offering a snap it cannot
    // perform until something else happened to redraw the panel.
    const bool canTemper = processorRef.temperIsPossible();
    lastEngineShown = ((int) st * 2 + (loaded ? 1 : 0)) * 2 + (canTemper ? 1 : 0);

    // TEMPER, REFUSED OUT LOUD. Temper snaps partials onto "degree x period^k", so with a
    // tuning that states no ascending period (a descending .scl, a degenerate period-1
    // scale) there is no grid to snap to. The synth used to substitute 2/1 and temper the
    // spectrum to an octave the user never wrote, invisibly. It now does nothing at all -
    // and this is where it SAYS so, on the control itself, so the refusal is never
    // something the user has to deduce from the sound.
    temperToggle.setEnabled (canTemper);
    if (canTemper)
    {
        // BOTH the word and the tooltip go back. Setting only the word left a control
        // that worked while still explaining, on hover, that it was unavailable - the
        // stale half of a two-part message is its own lie.
        temperToggle.setButtonText (tr ("Temper to tuning", "Tempera a l'afinaci\xc3\xb3"));
        temperToggle.setTooltip (tr (
            "ON: the HARMONIC ENGINE keeps generating partials and each one is snapped to the nearest pitch of the ACTIVE TUNING loaded in the Tuning box (degree x period^k), following it live - both engines work TOGETHER and the Custom Engine's own scale is bypassed. In MPE input mode the host does not hand the plugin a scale, so the grid there is plain 12-TET. OFF: no snapping (the Custom Engine's .scl, if loaded and ON, is the spectrum by itself).",
            "ON: el MOTOR HARM\xc3\x92NIC segueix generant parcials i cadascun s'ajusta al to m\xc3\xa9s proper de l'AFINACI\xc3\x93 ACTIVA carregada a la caixa Tuning (grau x per\xc3\xado" "de^k), seguint-la en viu - els dos motors treballen JUNTS i l'escala del Motor propi queda de banda. En mode d'entrada MPE l'amfitri\xc3\xb3 no d\xc3\xb3na cap escala al plugin, aix\xc3\xad que all\xc3\xa0 la graella \xc3\xa9s 12-TET. OFF: sense ajust (l'.scl del Motor propi, si est\xc3\xa0 carregat i actiu, \xc3\xa9s l'espectre per si mateix)."));
    }
    else
    {
        // THE WORD STAYS SHORT, AND THE TOOLTIP CARRIES THE REASON.
        //
        // This branch used to append " - no period" to the button's own text. That row
        // sizes the toggle to the words it holds and HIDES it when they do not fit - the
        // right rule - so the longer label made the control vanish outright, at every
        // size including 1200x650. The message that explains why a feature is unavailable
        // was exactly the message that could not fit, and the user got neither the
        // explanation nor the control. Found by scenario 43, which is the first picture
        // ever taken of a tuning that states no period.
        //
        // So the suffix goes, the same trade the source selector and the Shepard row
        // made. The toggle is already disabled here (setEnabled (canTemper) above), and
        // the tooltip below says the whole of it.
        temperToggle.setButtonText (tr ("Temper to tuning", "Tempera a l'afinaci\xc3\xb3"));
        temperToggle.setTooltip (tr (
            "UNAVAILABLE: this tuning states no period that ascends (a descending scale, or a period of 1/1), so there is no \"degree x period^k\" grid to snap partials onto. Fibl\xc3\xb3 will NOT invent an octave here - the harmonic engine keeps running untempered and your tuning is left exactly as you wrote it. Load a tuning whose last interval rises above 1/1 to use this.",
            "NO DISPONIBLE: aquesta afinaci\xc3\xb3 no declara cap per\xc3\xado" "de que pugi (una escala descendent, o un per\xc3\xado" "de d'1/1), aix\xc3\xad que no hi ha cap graella \"grau x per\xc3\xado" "de^k\" on ajustar els parcials. Fibl\xc3\xb3 NO s'inventar\xc3\xa0 cap octava aqu\xc3\xad - el motor harm\xc3\xb2nic segueix sonant sense temperar i la teva afinaci\xc3\xb3 queda exactament com l'has escrita. Carrega una afinaci\xc3\xb3 amb l'\xc3\xbaltim interval per sobre d'1/1 per fer-lo servir."));
    }

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
            add ("eng.prime", tr ("PRIME CONTROL", "CONTROL DE PRIMERS"), band (hcPrime.translated (0, 5)), cPanel);
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
            // ONE TITLE where MORPH and DRY/WET were two, and no warning tag under it.
            // That tag ("MORPH ON", beside a greyed DRY/WET) existed to explain why one of
            // two controls could not act; with one dropdown there is no second control to
            // explain away, and the state it warned about - both flags on at once - is not
            // reachable from this panel any more.
            add ("both.transition", tr ("TRANSITION", "TRANSICI\xc3\x93"), band (beTrans), cPanel);
            add ("both.range",   tr ("OCTAVE RANGE", "ABAST OCTAVES"), band (beRange.translated (0, 3)), cPanel);
            // PHASE shares OCTAVE RANGE's row, to its right, and is titled in the same
            // style as every other sub-section here - that title IS its name, which is why
            // the fader carries no label component of its own.
            add ("both.phase",   tr ("PHASE", "FASE"),                 band (bePhase.translated (0, 3)), cPanel);
        }
    }
    if (! essentialsBounds.isEmpty())
    {
        // Essentials' labels sit LEFT of their control (vertically centred), in the same
        // band resized() reserves for them (6 px in, 78 wide, minus a 2 px gap) - or
        // ABOVE it, in the cell's top 13 px, when resized() could not afford the side
        // band and stacked the cell instead (see essStacked). One flag, read by the two
        // places that have to agree about where the word goes.
        auto left = [this] (juce::Rectangle<int> c)
        {
            return essStacked ? c.reduced (6, 1).withHeight (13)
                              : c.reduced (6, 0).withWidth (76);
        };
        add ("ess.gain",  tr ("GAIN", "GUANY"),                    left (ecGain),  cEssentials);
        add ("ess.ref",   tr ("REFERENCE", "REFER\xc3\x88NCIA"),   left (ecRef),   cEssentials);
        // OCTAVE reads like the three rows above it now: label LEFT of the stepper,
        // not above it - it was the one cell in the box laid out the other way round.
        add ("ess.oct",   tr ("OCTAVE", "OCTAVA"),                 left (ecOct),   cEssentials);
    }
    // EVERY PAINTED SUB-LABEL IS SHRUNK UNTIL ITS WORD FITS, before anyone draws or
    // measures it.
    //
    // A painted label has no component to squash: paint() calls drawText with
    // useEllipsis false, so a word wider than its cell is simply CUT at the edge, with
    // nothing on screen and nothing in any bounds check to say so. "OCTAVE RANGE" needs
    // 83 px in the 79 it has at the floor, "ABAST OCTAVES" 88, "CAIGUDA AMP" 77 in 72 -
    // each of them losing its last letters in one language only.
    //
    // Done HERE rather than in the painter because layoutProbe() measures this same list:
    // a font chosen at paint time would be a font the rig never sees, and the check that
    // is supposed to catch clipping would go on measuring a size nothing is drawn in.
    //
    // 9.0 is the floor. Below that the word is not smaller, it is unreadable, so a cell
    // that still cannot hold its title at 9 px is a LAYOUT problem and stays reported as
    // one - the rig keeps saying so rather than this quietly making the type invisible.
    for (auto& s : v)
    {
        const auto widthAt = [&s] (float h)
        {
            return (float) juce::GlyphArrangement::getStringWidthInt (
                       juce::Font (juce::FontOptions (h).withStyle ("Bold")), s.text);
        };
        while (s.fontHeight > 9.0f && widthAt (s.fontHeight) > (float) s.cell.getWidth())
            s.fontHeight -= 0.25f;
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
        vert  ("fractal|transition", (float) beTrans.getX());
        // From the UNION: OCTAVE RANGE and PHASE are two sub-sections on ONE row, so the
        // line above them must span the whole column. Drawn from beRange alone it would
        // stop where the stepper's share ends and leave the fader hanging off an
        // unfinished divider.
        horiz ("transition|range", beRange.getUnion (bePhase));
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
            // ...in the font the PAINTER really uses, which is not the one the Label was
            // given: GalacticLnF::getLabelFont puts a 12.5 px floor under it and draws it
            // BOLD. Measured with l->getFont() this check read a lighter, sometimes
            // smaller face than the screen shows, so it under-reported - it certified as
            // fitting exactly the labels that condense in front of the user, which is the
            // hand-copied-formula mistake the rest of fgfit exists to make impossible.
            p.componentLabels.push_back ({ name, l->getText(), l->getBounds(),
                (float) juce::GlyphArrangement::getStringWidthInt (fgfit::labelFont (*l), l->getText()) });
        // ...and a BUTTON carries a word that has to fit the box it was given, measured
        // against the room the painter really leaves it (see LayoutProbe::buttonFits).
        // Recorded through the SAME `add` every control goes through, so a button that
        // is laid out is a button that is measured - a separate hand-kept list is how
        // the one control a change actually narrowed ends up being the one not checked.
        const int bh = c.getHeight(), bw = c.getWidth();
        if (const auto* tb = dynamic_cast<const juce::TextButton*> (&c);
            tb != nullptr && tb->getButtonText().isNotEmpty())
        {
            // A STEPPER IS MEASURED AGAINST ITS GLYPH, BECAUSE ITS GLYPH IS NOT ITS TEXT.
            //
            // GalacticLnF::drawButtonText draws a "round" +/- as two rectangles sized from
            // the button's own circle - it never lays out the character at all - so
            // measuring the string "-" in the button font measures something nothing
            // draws. That mismatch was not academic: the rig's width floor was pinned at
            // 690 px by "/round#19 (Button) \"-\" has negative room: -1.0 wide", i.e. by a
            // hyphen the painter had stopped using.
            //
            // The check stays, against what is really drawn: the bar is 0.46 of the
            // circle's diameter, centred, so what it needs of the BUTTON is that plus the
            // painter's 4 px a side. A stepper squeezed to nothing still fails; one that
            // has its circle passes, which is the truth.
            const bool stepper = tb->getName() == "round"
                              && (tb->getButtonText() == "+" || tb->getButtonText() == "-");
            if (stepper)
                p.buttonFits.push_back ({ name, tb->getButtonText(),
                    std::round ((float) juce::jmin (bw, bh) * 0.46f), juce::jmax (0, bw - 8) });
            else
                p.buttonFits.push_back ({ name, tb->getButtonText(),
                    (float) juce::GlyphArrangement::getStringWidth (fgfit::textButtonFont (bh), tb->getButtonText()),
                    (int) std::floor (fgfit::textButtonRoom (bw, bh, tb->getProperties().contains ("fork"))) });
        }
        else if (const auto* tg = dynamic_cast<const juce::ToggleButton*> (&c); tg != nullptr)
        {
            // The "onled" pill prints ON / OFF itself, centred in the whole button, and
            // has no juce::Button text at all - measured as an empty string it would
            // "fit" any width, including a width too small for the word it draws.
            if (tg->getName() == "onled")
                p.buttonFits.push_back ({ name, "OFF",
                    (float) juce::GlyphArrangement::getStringWidth (fgfit::onLedFont (bh), "OFF"), bw });
            else if (tg->getButtonText().isNotEmpty())
                p.buttonFits.push_back ({ name, tg->getButtonText(),
                    (float) juce::GlyphArrangement::getStringWidth (fgfit::toggleFont (bh), tg->getButtonText()),
                    (int) std::floor (fgfit::toggleRoom (bw, bh)) });
        }
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
    add ("harm.primeOn",     primeOnToggle);
    add ("harm.primeLimitLbl", lblPrimeLimit);
    add ("harm.prime0",      primeToggle[0]);  add ("harm.prime8",    primeToggle[8]);
    add ("harm.prime0Lbl",   primeLbl[0]);     add ("harm.prime8Lbl", primeLbl[8]);

    add ("cust.load",        loadTimbreButton); add ("cust.random",   randomSpectrumBtn);
    add ("cust.intervals",   intervalEdit);     add ("cust.status",   timbreStatusLabel);
    add ("cust.reps",        timbreRepeatsFader); add ("cust.repsLbl", lblTimbreRepeats);
    add ("cust.temper",      temperToggle); // laid out in BOTH views of the engine box

    // The two CHIP ROWS, so a build can see that they are still one size. They are the
    // same control doing the same job (see fgfit::chip) and they were 104 x 16 against
    // 58 x 19 - a difference nothing here could measure, because neither row was probed.
    add ("eng.chipHarmonic", engineChip[0]);   add ("eng.chipCustom", engineChip[1]);
    add ("mod.tabAmp",       envTabB[0]);      add ("mod.tabPitch",   envTabB[1]);
    add ("mod.tabFilter",    envTabB[2]);      add ("mod.tabCustom",  envTabB[3]);

    add ("both.slope",       slopeFader);      add ("both.pencil",    partialEditBtn);
    add ("both.manual",      partialManualT);  add ("both.fractal",   fractalFader);
    add ("both.transition",  transitionBox);
    // Both Time faders go through `add`, whose visibility gate records exactly the one
    // that is on screen - the same way the Shepard row's shepTime/shepDiv pair is probed.
    add ("both.morphTime",   morphTimeFader);
    add ("both.dryWetTime",  dryWetTimeFader); add ("both.transLbl", lblTransTime);
    add ("both.rangeVal",    sprVal);          add ("both.rangeUp",   sprUp);
    add ("both.rangeDn",     sprDn);

    add ("both.phase",       phaseFader);
    add ("ess.gain",         gainFader);
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
    add ("tun.shepRise",     shepRiseBox);   add ("tun.shepDir",    shepDirBtn);

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
    add ("spec.regLbl",      adaptRegLbl);     add ("spec.regPer",    adaptRegPeriodsEdit);
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
        // ...and the register line in ALL THREE forms, at their widest numbers: the anchor
        // and the two end keys a one-degree-per-period scale can reach ("-10:0"), the
        // widest width the field accepts, the "-A/+B" wording that only appears at the ends
        // of the keyboard, and the .tun qualifier that only appears under an absolute
        // table. Each of the last two is longer than the plain form and each is one a
        // player never sees until the day they land on it.
        const juce::String wideKey ("-10:0"), wideLo ("12.3"), wideHi ("20000");
        const juce::String wideDev = registerPeriodQualifierText (false);   // the longer of the two
        p.readoutVariants.push_back (measureWrapped ("regSpan.plain",   adaptRegSpanLbl,
                                                     adaptRegSpanLine (wideKey, 8.0, 8.0, 8.0,
                                                                       wideLo, wideHi, {})));
        p.readoutVariants.push_back (measureWrapped ("regSpan.clamped", adaptRegSpanLbl,
                                                     adaptRegSpanLine (wideKey, 8.0, 7.5, 8.0,
                                                                       wideLo, wideHi, {})));
        p.readoutVariants.push_back (measureWrapped ("regSpan.table",   adaptRegSpanLbl,
                                                     adaptRegSpanLine (wideKey, 8.0, 7.5, 8.0,
                                                                       wideLo, wideHi, wideDev)));
    }
    add ("spec.segPerLbl",   segPeriodLbl);    add ("spec.segPeriod", segPeriodEdit);
    add ("spec.segTop",      segTopToggle);

    // The sub-CELLS. Empty ones are skipped: the harmonic cells are deliberately empty
    // in the custom view (paint() reads that as "draw no harmonic dividers").
    auto cell = [&p] (const char* name, const juce::Rectangle<int>& r)
    { if (! r.isEmpty()) p.cells.push_back ({ name, r }); };
    cell ("eng.base",    hcBase);    cell ("eng.harm",    hcHarm);   cell ("eng.prime", hcPrime);
    cell ("both.slope",  beSlope);   cell ("both.fractal", beFractal);
    cell ("both.transition", beTrans);
    cell ("both.range",  beRange);   cell ("both.phase",  bePhase);
    cell ("ess.gain",    ecGain);
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
            // MEASURED THE WAY THE ITEM IS ACTUALLY DRAWN, which is not the way this
            // probe used to measure it. Four things stand between the box's width and
            // the room the word gets, and the old measurement missed three of them:
            //   - positionComboBoxText leaves the label fgfit::comboTextWidth (chevron
            //     and left inset off) - the one part it did have;
            //   - juce::Label keeps its own {1,5,1,5} border off that, so 10 more px go;
            //   - GalacticLnF::getLabelFont draws the item BOLD (12.5 px floor), while
            //     this probe measured it in getComboBoxFont's lighter face;
            //   - drawLabel word-wraps into however many WHOLE lines the height allows,
            //     which is why the 34 px mode box legitimately holds a long sentence.
            // Both of the misses ran the same way - towards "it fits" - so the probe was
            // quietly certifying boxes ~10 px narrower than the word needs.
            // MEASURED OFF THE LABEL THE BOX REALLY DRAWS IN, not off a model of it.
            //
            // A ComboBox draws its item through a child juce::Label, so that Label's own
            // bounds and its own LookAndFeel font ARE the answer - they cannot drift from
            // the painter the way a reconstruction can, and reconstructions here have now
            // been wrong in both directions. The first modelled the room with a constant
            // and the layout with addFittedText, which SHRINKS THE FONT until the text
            // fits: `need` could never exceed `avail`, so the check could not fail, and
            // the SPECTRUM mode box shipped reading "Find tunings based on beati..." at
            // 1200x650 while the probe reported it needing 153.86 of the 154 it had. The
            // second measured the string honestly but kept the modelled room, and reported
            // "+-48 st (MPE)" as truncated when it is drawn in full.
            // ...and it is the SAME three lines the fit sweep (walkTextFits) uses, because
            // they are the same question and they were being answered twice. Lifted into
            // fgfit so the correction above cannot be made here and forgotten there - it
            // was: this probe took the label's border off and the sweep did not, so the
            // sweep certified an item that touched its own chevron.
            const auto  f      = fgfit::comboDrawnFont (c);
            const int   availW = fgfit::comboDrawnRoom (c);
            juce::String widest; float need = 0.0f;
            // HOW MANY LINES THE PAINTER IS ALLOWED, from the label's own area and font -
            // the same jmax(1, height/fontHeight) juce::Label::paint uses. Most of these
            // boxes get one; the 34 px SPECTRUM mode box was BUILT to get two, and the
            // whole point of measuring it here is to say whether it really does.
            const float fh    = f.getHeight();
            const int   availH2 = fgfit::comboDrawnRoomH (c);
            const int   rows  = juce::jmax (1, (int) ((float) availH2 / juce::jmax (1.0f, fh)));
            for (int i = 0; i < c.getNumItems(); ++i)
            {
                const auto  t    = c.getItemText (i);
                const float oneW = juce::GlyphArrangement::getStringWidth (f, t);
                // WRAPPED BY WORDS, COUNTED HONESTLY. Not addFittedText, which is what this
                // check used to call: it lays the text out exactly as the painter does
                // INCLUDING shrinking the font until it fits, so the measurement made
                // itself true and the check could never fail. Greedy fill at the real
                // width instead, then ask whether the lines fit the rows on offer.
                int   lines = 1;
                float lineW = 0.0f;
                if (rows > 1 && oneW > (float) availW)
                {
                    auto words = juce::StringArray::fromTokens (t, " ", "");
                    for (const auto& w : words)
                    {
                        const float ww = juce::GlyphArrangement::getStringWidth (f, w + " ");
                        if (lineW > 0.0f && lineW + ww > (float) availW) { ++lines; lineW = ww; }
                        else lineW += ww;
                    }
                }
                // What it NEEDS of the width: nothing more than the box has when the lines
                // it takes fit the rows it is given; the whole single-line width when they
                // do not, because that is the width it would have had to have.
                const float w = (rows > 1 && lines <= rows) ? 0.0f : oneW;
                if (w > need) { need = w; widest = t; }
            }
            p.comboTexts.push_back ({ name, widest, need, availW });
        }
    };
    combo ("basePitch",   basePitchBox);
    combo ("transition",  transitionBox);
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
            s.col, s.fitted });

    // The words that ARE the feature (see LayoutProbe::buttonTexts).
    p.buttonTexts.push_back ({ "cust.load",  loadTimbreButton.getButtonText() });
    p.buttonTexts.push_back ({ "tun.toBank", tuningBankAddBtn.getButtonText() });
    p.buttonTexts.push_back ({ "top.playground", playgroundButton.getButtonText() });
    // The word the KEYBOARD paints over its reference key. It is here because the
    // translated-check walks this probe: a component that paints its own text is
    // otherwise invisible to it, which is how this one stayed English.
    p.buttonTexts.push_back ({ "keys.reference", keyboard.getReferenceWord() });
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
    p.regPeriodsTip     = const_cast<juce::TextEditor&> (adaptRegPeriodsEdit).getTooltip();
    p.dividers          = spectrumDividers(); // the very lines paint() strokes
    // The keyboard, and the keys really on it (see LayoutProbe::keyboardBox).
    p.keyboardBox  = keyboard.getBounds();
    p.keysDrawn    = keyboard.drawnKeysSpan().getSmallestIntegerContainer()
                       + keyboard.getBounds().getPosition();
    p.keysInWindow = keyboard.windowKeyCount();
    p.keysPlayable = keyboard.drawnKeyCount();
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

// THE REFERENCE PITCH, AT THE PRECISION THE FIELD CAN ACTUALLY SHOW.
//
// A juce::TextEditor is the one text control in this window that neither clips nor
// condenses when its string is too wide: it scrolls, and TextEditor::setText leaves the
// caret at the END (cursorWasAtEnd is true for an empty editor, so this is true from the
// very first call). The field is 8 px indented a side and 13 pt, so at 690x550 it holds
// ~46 px of text while "261.6255653" needs ~65 - and what the user read was "55653".
// Not a truncated number with a visible cut: a DIFFERENT number, well-formed, plausible,
// and off by a factor of 213. It is the one failure mode in this file where the wrong
// pixels look exactly like right ones.
//
// So the decimals are dropped one at a time until the number fits, and the INTEGER PART
// IS NEVER DROPPED - 261.6255653 -> 261.626 -> 261.63 -> 262, and 262 even in a field too
// narrow for it, where the (now existing) TextEditor row in walkTextFits reports it as a
// fit problem instead of the field quietly inventing a value. Left-truncation is not one
// of the options at any width.
//
// Measured in the field's OWN font against TextEditor::getMaximumTextWidth's own formula
// (fgfit::textEditorRoom), not against the box: the 8 px indents and the 2 px the caret
// keeps at the right edge are 18 of the ~64 px this cell has at the floor.
juce::String FractalAdditiveAudioProcessorEditor::refATextForRoom (double c3, int room) const
{
    // trimCharactersAtEnd only where there IS a point to trim to: at 0 decimals the string
    // is "262" and stripping "0" from its end would make it "26".
    const auto trim = [] (juce::String s)
    { return s.containsChar ('.') ? s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".") : s; };
    const auto font = refAEdit.getFont();
    juce::String s;
    for (int dp = 7; dp >= 0; --dp)   // 7: C3 at A=440 is 261.6255653 exactly
    {
        s = trim (juce::String (c3, dp));
        if (juce::GlyphArrangement::getStringWidth (font, s) <= (float) room) break;
    }
    return s;
}

void FractalAdditiveAudioProcessorEditor::refreshRefAText()
{
    const double a  = (double) processorRef.apvts.getRawParameterValue ("refA")->load();
    const auto txt = refATextForRoom (a * kC3overA, fgfit::textEditorRoom (refAEdit));
    if (refAEdit.getText() != txt && ! refAEdit.hasKeyboardFocus (false))
    {
        refAEdit.setText (txt, juce::dontSendNotification);
        // ...and the caret HOME. setText scrolls to wherever the caret lands, and the
        // whole defect above was that landing at the end scrolls the significant digits
        // off the left of a field that never says so. With the value now sized to the
        // room this is belt and braces - and it is the line that makes "never truncated
        // from the left" true for any string this field is ever handed.
        refAEdit.setCaretPosition (0);
    }
}

void FractalAdditiveAudioProcessorEditor::makeLabel (juce::Label& l, juce::Colour c)
{
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, c.brighter (0.15f));
    l.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
    // JUCE reserves 5 px of every Label for a border nothing here draws, which on a
    // 22 px caption is nearly half its width - and drawLabel then condenses the word to
    // fit what is left. These captions are inline text in a panel, not framed fields:
    // 2 px is all the breathing room they need.
    l.setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));
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
    // ...and the ONE decimal policy, wrapped around whatever the attachment just
    // installed (see kFaderMaxDecimals / capDecimals above for why).
    //
    // WRAPPED, not replaced, and that is the whole safety of it. The attachment's function
    // is the parameter's own getText, and juce::Slider's numDecimalPlaces is derived from
    // the range the attachment set - so the cap is read LAZILY, inside the lambda, and a
    // caller that later says setNumDecimalPlacesToDisplay (0) still gets 0 (shepTime does,
    // and until now that call did nothing at all: the attachment's textFromValueFunction
    // wins over numDecimalPlaces, so its 0 was silently ignored). A caller that installs
    // its OWN textFromValueFunction after this line - baseN, harmonics, primeLimit - still
    // wins outright, because it simply overwrites this one.
    if (s.textFromValueFunction != nullptr)
        s.textFromValueFunction = [prev = std::move (s.textFromValueFunction), &s] (double v)
        { return capDecimals (prev (v), juce::jmin (kFaderMaxDecimals, s.getNumDecimalPlacesToDisplay())); };
    // Remembered so the value boxes can be sized ONCE, later, when every fader's text is
    // final: the suffix (" ms", " c", " %") is set by the caller AFTER this function
    // returns, and the parameter's own text function arrives with the attachment on the
    // line above. Measured here, "120.0 ms" measures as "120.0" - which is exactly how
    // the glide fader ended up with a 46 px box it could not fill honestly.
    configuredFaders.push_back (&s);
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
    //
    // The six lines that decide side/total/x now live in titleBandFor(), above, because
    // resized() has to place the credit under this row and a second hand-copy of them is
    // how the two drift apart.
    const auto title  = titleWord();
    const int  L      = title.length();
    const int  period = juce::jmax (1, L - 1);
    const auto band   = titleBandFor (getWidth());
    const float side  = band.side;
    const float gap   = 4.0f;
    const float margin = side * 0.10f;
    const juce::Font glyphFont = titleTypeface() != nullptr
        ? juce::Font (juce::FontOptions (titleTypeface()).withHeight (64.0f))
        : juce::Font (juce::FontOptions (64.0f).withStyle ("Bold"));

    struct Sq { juce::Rectangle<float> rect; juce::Colour col; juce::Path glyph; juce::Colour glyphCol; };
    std::vector<Sq> squares;
    {
        // Centred on the FULL window width and clamped to the free zone - decided in
        // titleBandFor(), which resized() reads too so the credit lands under it.
        float x = band.x;
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
        // From ecRef down: a divider is drawn at each cell's TOP, so the FIRST cell must
        // not be in this list or the line lands on the title band. That used to be
        // ecPhase; with PHASE moved to the SPECTRUM panel it is ecGain.
        for (auto* cell : { &ecRef, &ecOct })
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
    // Re-spell the keys if the tuning moved. The timer does this too, but the timer is
    // not running in the offline verifiers - and, more to the point, a stale name is a
    // WRONG name, so the two cheapest moments to notice are both worth taking. It costs
    // one integer comparison when nothing changed (see refreshDegreeNames).
    refreshDegreeNames();

    auto full = getLocalBounds();
    auto titleBar = full.removeFromTop (78); // matches paint()'s titleArea
    langButton.setVisible (false); // language now lives inside the Menu popup
    {
        // Left cluster, two rows: PRESET PLAYGROUND at the very top-left with Menu after
        // it, then [Presets][Save] - Save directly BELOW Menu, the preset selector
        // directly LEFT of Save.
        //
        // "PON 'VIBE CODED BY ASRA3' CENTRADO DEBAJO DEL TITULO, Y QUE 'PRESET
        // PLAYGROUND' OCUPE SU SITIO." The credit sat in the corner and the playground
        // button had a private third row under the preset selector, which is what pushed
        // the left cluster to 77 of the band's 78 px. Swapping them costs nothing and
        // frees that row outright: the credit needs no row of its own down here, because
        // under the title there is already empty band.
        auto lc = titleBar.removeFromLeft (208).withTrimmedLeft (8);
        auto r1 = lc.removeFromTop (30).withTrimmedTop (8);
        // 150 x 20. The HEIGHT is the one it has always had, and it is the height the
        // verifier measures its word at - jmin (15, h * 0.6) - so keeping 20 keeps the
        // measurement that says "Laboratori de Presets" fits. 20 and not the row's 22 on
        // purpose: at 22 its bottom edge lands exactly on presetDirtyMark's top edge.
        playgroundButton.setBounds (r1.removeFromLeft (150).withHeight (20));
        r1.removeFromLeft (4);
        menuButton.setBounds (r1.removeFromLeft (46).withHeight (20));
        auto r2 = lc.withTrimmedTop (2).withHeight (22);
        presetBox.setBounds (r2.removeFromLeft (150));
        presetDirtyMark.setBounds (presetBox.getRight() - 15, presetBox.getY() - 3, 14, 14);
        r2.removeFromLeft (4);
        savePresetButton.setBounds (r2.removeFromLeft (46));
    }
    // Spectrum-to-Rhythm: top right, high-contrast - and on the SAME LINE as the
    // playground button at the other end of the bar ("que Spectrum vs Rhythm quede
    // alineado con Preset Playground"). It takes its y and its height FROM that button
    // rather than centring itself in the band, so the two cannot drift apart: aligning
    // two buttons by writing the same number twice is aligning them until somebody
    // changes one of the numbers.
    //
    // ...AND IT ENDS ON THE WINDOW'S RIGHT WALL, not 4 px past it. `titleBar` is the full
    // window width, so removeFromRight (178) + withSizeKeepingCentre (168) put its right
    // edge on getWidth() - 5, while every panel below it stops at getWidth() - 9: the page
    // margin (`area` = getLocalBounds().reduced (6, 4)) plus the 3 px each macro box keeps
    // inside it (see gSpecScale's `sp.reduced (3, 2)`). One button breaking a wall that
    // eight boxes hold is exactly the kind of near-miss that reads as sloppiness rather
    // than as a decision, so the number is DERIVED from the same two insets instead of
    // being a third hand-picked margin.
    const int kPanelRight = getWidth() - 6 - 3;
    rhythmBtn.setBounds (juce::Rectangle<int> (kPanelRight - 168, playgroundButton.getY(),
                                               168, playgroundButton.getHeight()));
    titleBar.removeFromRight (178);   // the band the title must not run into (see titleBandFor)

    // The credit, centred UNDER the title, from the same geometry paint() draws the
    // squares with. Sized to its own two words: the Label is capped so the link beside
    // it keeps room for "Asra3", exactly as it was in the corner. "Vibe coded per" is
    // 4 px wider than "Vibe coded by" and was drawn condensed the moment this line
    // started translating, which is why the floor and the cap are both measured.
    {
        const auto tband = titleBandFor (getWidth());
        const int labelW = juce::jlimit (92, 100, fgfit::labelWidth (vibeLabel));
        const int linkW  = (int) std::ceil (juce::GlyphArrangement::getStringWidth (
                               juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")),
                               vibeLink.getButtonText())) + 6;
        const int creditH = 16;
        auto credit = juce::Rectangle<int> (0, 0, labelW + 2 + linkW, creditH)
                          .withCentre ({ juce::roundToInt (tband.centreX()),
                                         juce::roundToInt (tband.bottom()) + 1 + creditH / 2 });
        vibeLabel.setJustificationType (juce::Justification::centredRight);
        vibeLabel.setBounds (credit.removeFromLeft (labelW));
        credit.removeFromLeft (2);
        vibeLink.setBounds (credit);
    }
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
        // fgfit::labelWidth, not a hand-rolled measurement: a juce::Label does NOT draw
        // into its whole width (getBorderSize is taken off first, 5 px a side by
        // default) and GalacticLnF draws it in a BOLD face with a 12.5 px floor. Sizing
        // to l.getFont()'s width plus 4 got both of those wrong at once, so "Period",
        // "Per\xc3\xado" "de" and "smallest interval (c)" were all quietly condensed.
        const int w = juce::jmax (nominal, fgfit::labelWidth (l));
        // THE LABEL'S OWN NEED OUTRANKS THE CONTROL'S NOMINAL RESERVE.
        //
        // `keepForControl` is what the control beside this label would LIKE. It used to be
        // absolute, so a narrow row squeezed the caption instead: "C3 =" needs 27 px of
        // text room and was handed 25 - and being a juce::Label it did not clip, it
        // condensed, which is the failure with no visible symptom. It was the control the
        // rig's legibility walk named as binding the whole window's minimum width.
        //
        // So the allowance is at least what the reserve leaves, and may rise to what the
        // label actually needs - never past leaving the control 24 px, which is the least
        // an editor or a read-out can be and still be one. The control is elastic; a word
        // is not.
        const int soft = juce::jmax (0, row.getWidth() - keepForControl);
        const int hard = juce::jmax (0, row.getWidth() - 24);
        const int give = juce::jmax (soft, juce::jmin (fgfit::labelWidth (l), hard));
        l.setBounds (row.removeFromLeft (juce::jmin (w, give)));
    };

    // [ THE ACTION ][ REALTIME ] - the pair the Spectrum-vs-Tuning column ends both of its
    // search modes with, laid out from the WORDS on the two buttons instead of from a
    // fixed 3/5 - 2/5 split of the row.
    //
    // The split was fitted to English and to English alone: "REALTIME" needs 79 px of the
    // 76 the 2/5 gave it - it was 2.24 px over its own box in the SHIPPED window - and
    // "TEMPS REAL" needs 96, so the Catalan spilled by 17.42 px in every one of the four
    // scenarios that show this row. Nothing about a fraction of a row could have found
    // that: it is the button's word that is long, not the window that is narrow.
    //
    // WHO YIELDS TO WHOM. The follower's word never changes (it is a mode name), while the
    // action's does - "Adapt" grows a duration when a run would take more than a second -
    // so the FOLLOWER is measured and the ACTION takes the remainder. If the action's word
    // grows so long that the two no longer fit, the follower falls back to exactly the 2/5
    // it has today rather than eating into it, which is why this can never be narrower for
    // either button than what it replaces.
    //
    // The follower is capped at half the row on purpose: in Catalan the mode's name is as
    // long as the action's, so the pair comes out even there. The hierarchy the old
    // fraction was drawing - "the follower must never be the button the hand lands on" -
    // is carried by ORDER and COLOUR (the action is the panel's accent, the follower is
    // grey), neither of which a long word can take away.
    //
    // The 2 px gap between the pair is still trimmed off their INNER edges only: reducing
    // both ends pulled the row 1 px in from the column on each side, so this pair alone
    // contributed two more of the seven different edges this narrow column used to have.
    auto actionPair = [] (juce::TextButton& go, juce::TextButton& rt, juce::Rectangle<int> row)
    {
        const int h = row.getHeight();
        auto need = [h] (const juce::TextButton& b)
        { return fgfit::textButtonWidth (b.getButtonText(), h, b.getProperties().contains ("fork")); };
        const int keep = row.getWidth() * 2 / 5;               // what the follower has today
        const int rtW  = juce::jlimit (keep, juce::jmax (keep, row.getWidth() - need (go)), need (rt));
        go.setBounds (row.removeFromLeft (row.getWidth() - rtW).withTrimmedRight (1));
        rt.setBounds (row.withTrimmedLeft (1));
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
        // HOW WIDE THIS COLUMN IS. It used to be a flat 208 px at EVERY window size, so
        // a 1600x900 window handed its whole extra 400 px to rows 1-2 - panels that were
        // already comfortable (the tuning name's cell went 57 -> 229 px) - while the one
        // panel that genuinely overflows stayed exactly as cramped as at the floor.
        // The strings that paid for that are all in here: "De totes les afinacions
        // possibles" was over its combo by 39.96 px, "TEMPS REAL" over its button by
        // 18.42, "+ amplituds" over its toggle - all three at the SHIPPED 1200x650, in
        // Catalan, which is the binding language.
        //
        // The growth is measured from the SHIPPED 1200x650 (1188 px of usable width),
        // not from zero, so every window at or below that size keeps the exact 208 it
        // was fitted to - the floor included. A first attempt took a flat 22% of the
        // width instead, which is 261 px at 1200: rows 1-2 lost 53 px there and started
        // clipping strings of their own ("Transposada avall" needed 117 px of a 109 px
        // combo, the CUSTOM modulator chip 45 of 44), which is the same fault moved to a
        // different panel rather than fixed. Only the width the window did not have at
        // 1200 is redistributed, and 30% of it comes here.
        //
        // Capped at 300: past that the column is wider than its longest sentence needs
        // and the SPECTRUM row is paying for empty gutter.
        const int specW = juce::jlimit (208, 300, 208 + (area.getWidth() - 1188) * 3 / 10);
        const int wallX = area.getRight() - specW;
        // Box inset + the two stroke offsets. ROUNDED, not truncated: 2*1.3 truncates to
        // 2 and leaves the two strokes 0.6 px apart, which is a visible pink sliver down
        // the orange border - the doubled wall in miniature.
        const int lap   = 3 + juce::roundToInt (2.0f * kFrameLine);
        auto sp = area.withLeft (wallX - lap);
        area.setRight (wallX + 3);

        // ONE beating mode now, with two selectors inside it:
        //   SOURCE - from the current tuning (combinations of its own degrees) or
        //            from all tunings (a free search, loadable as a tuning)
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

        //== THE THREE NUMBERS THIS WHOLE COLUMN IS SPACED BY ====================
        //
        // "esta mal alineado". It was spaced by eleven different ad-hoc gaps - 1, 2, 3
        // and 4 px, in no pattern - and by two different row heights (22 everywhere and
        // 24 for the scope box and the action pair), and its values were four different
        // widths (32, 44, 44, 48) anchored to three different edges. Three constants
        // replace all of that, and every row below is laid out from them.
        //
        //   kRowH   one row height, so the tops and bottoms of a column of controls
        //           line up whatever kind of control each one is. A ComboBox already
        //           filled its row and a TextEditor was inset by 1 px top and bottom, so
        //           the two were 22 and 20 in the same stack: a 1 px step that reads as
        //           a wobble down the value column and as nothing you can name.
        //   kGapIn  between two rows of the SAME section.
        //   kGapSec between sections, and around the framed stack as a whole. It is
        //           double kGapIn on purpose - that is the only thing separating the two
        //           kinds of gap once the frame's rules are drawn inside kGapSec - and
        //           the section rule sits at its centre, 2 px clear of a row either side.
        //
        // MEASURED, because this column has no room to be generous with: at 1200x650 the
        // goal-Range shape had 56 px of adapted-partial list against RhythmVerify's 54 px
        // floor. Normalising costs 5 px of gaps in that shape and gives back 2 px on the
        // scope row, 2 px on the action pair and 1 px on a stray one-pixel gap that
        // belonged to nothing - so it is 4 px cheaper than what it replaces, and the
        // frames drawn on top of it are free. Every other shape gains far more: the
        // search from the current tuning gains 39 px, because it was reserving a row for
        // a progress label that mode never shows (see below).
        const int kRowH   = 22;
        const int kGapIn  = 2;
        const int kGapSec = 4;
        // ONE VALUE WIDTH for the whole stack, so the values line up on BOTH edges and
        // not just on the right one. 44, not the 48 the smallest-interval field had: 48
        // would take four pixels off the amplitudes toggle, which is the one control in
        // this column that is already measured to the pixel in Catalan (see the
        // applyLanguage note on why it lost its "+"), and 44 holds five digits with the
        // 4 px indents these editors are built with.
        const int kValW   = 44;
        // The top of each section, in si's coordinates, as the stack is laid out; the
        // frame and its rules are derived from these at the end (see specSections).
        int bandTop = si.getY();
        std::vector<int> bandRules;
        // Close the section that is open and start a new one. `si.getY()` at this moment
        // is the first pixel AFTER the section just finished, so the rule goes at the
        // centre of the kGapSec that follows - which is why sections may only ever be
        // separated by kGapSec, never by kGapIn.
        const auto endSection = [&si, &bandRules, kGapSec]
        {
            bandRules.push_back (si.getY() + kGapSec / 2);
            si.removeFromTop (kGapSec);
        };

        specModeBox.setBounds (si.removeFromTop (34));     // two lines: full mode names fit
        // The mode selector is the column's SUBJECT, not one of its settings, so it sits
        // outside the framed stack - which starts here.
        si.removeFromTop (kGapSec);
        bandTop = si.getY();
        tuneSourceBox.setVisible (beat);
        // The SCOPE means the same thing in both directions - judge every note against the
        // root, or every pair - so the adaptation shares the control rather than growing a
        // second one that could disagree with it.
        tuneScopeBox.setVisible (beat || adapt);
        if (beat)
        {
            tuneSourceBox.setBounds (si.removeFromTop (kRowH));
            si.removeFromTop (kGapIn);
        }
        if (adapt)
        {
            adaptTargetBox.setBounds (si.removeFromTop (kRowH));
            si.removeFromTop (kGapIn);
        }
        if (beat || adapt)
        {
            // It was 24, and before that 30 - sized for two lines of a name it no longer
            // has ("0 vs all" and "All vs all" are one short line in both languages). It
            // is the stack's own row height now, which is what puts it on the same rhythm
            // as everything under it, and the two pixels go where every pixel in this
            // column goes: to the results list.
            // ...and only as wide as "All vs all" / "Tot vs tot" needs. It used to take
            // the column's whole width for two three-word items.
            {
                auto scr = si.removeFromTop (kRowH);
                tuneScopeBox.setBounds (scr.removeFromLeft (
                             juce::jmin (fgfit::snugComboWidth (tuneScopeBox, kRowH), scr.getWidth())));
            }
            // WHAT is compared, above; HOW the search is set up, below.
            endSection();
        }

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
                                      &adaptRegLbl, &adaptRegPeriodsEdit, &adaptRegSpanLbl };
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

        // "QUITA EL TUNING INFO EN EL MODO DE ENCONTRAR TUNINGS A PARTIR DE BATIDOS."
        //
        // The Results / Tuning info pair was a two-state view switch over the list below
        // it, and in THIS mode the list is the whole point: you are reading found tunings
        // to pick one. The second view showed the degrees of whichever one you had
        // already loaded - a different question, answered by the Tuning panel, and its
        // button sat over the results permanently offering to hide them. Both halves go:
        // a lone "Results" button that is always the current view is not a control.
        //
        // chordDegView is forced back to false rather than left at whatever it was, or a
        // user who had switched to the degree view before this build would open the panel
        // to a list that is no longer reachable.
        chordDegView = false;
        chordViewBack.setVisible (false);
        chordViewFwd.setVisible  (false);

        if (seg)
        {
            auto pr = si.removeFromTop (kRowH);
            // (its centredLeft justification is set once, where the label is made - see
            // the specCaptions loop in the constructor. Four rows in this block used to
            // re-decide it on every layout pass, which is four copies of one decision
            // that nothing kept in step.)
            segPeriodLbl.setBounds (pr.removeFromLeft (48));
            segPeriodEdit.setBounds (pr.removeFromLeft (40));
            pr.removeFromLeft (5);
            segTopToggle.setBounds (pr);          // "= top partial"
            si.removeFromTop (kGapIn);
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
                // 5, matching the shipped default - this fallback still said 3, which is
                // what the box defaulted to before "que por defecto sean 5 notas".
                chordNotesBox.setSelectedId (juce::jlimit (2, maxN, keep < 2 ? 5 : keep),
                                             juce::dontSendNotification);
            }

            auto r1 = si.removeFromTop (kRowH);
            if (beat)
            {
                // Fixed widths, NOT `caption`, and deliberately: these two rows carry a
                // second control after the value (Min/Max, and the goal box), so a caption
                // sized to its own words takes those pixels off a ComboBox - and a
                // ComboBox does not report the loss, it just draws less of its item.
                // Measured: routing "Partials" through `caption` takes 61 px instead of
                // 52 and leaves the goal box 79, where "Range" has 3 px to spare. The
                // captions here fit their cells in both languages (RhythmVerify's
                // condensed count is 0), so there is nothing to buy with that risk.
                chordNotesLbl.setBounds (r1.removeFromLeft (44));
                // MEASURED, NOT 48. This box is REBUILT above: from the current tuning it
                // runs 2..12, so on a scale with ten or more degrees its widest item is two
                // digits - and a flat 48 px leaves 11 px of text room once the chevron
                // gutter is taken off, against the 19 that "12" needs. Nothing saw it while
                // the fit sweep measured a combo against its full box; it is the first
                // thing the sweep named once it started subtracting the gutter.
                compact (chordNotesBox, r1.removeFromLeft (
                             juce::jmax (48, fgfit::snugComboWidth (chordNotesBox, kRowH))));
                r1.removeFromLeft (6);
                compact (chordMinMaxBox, r1);
            }
            else
            {
                adaptPartialsLbl.setBounds (r1.removeFromLeft (52)); // "Partials"
                compact (adaptPartialsBox, r1.removeFromLeft (46));
                r1.removeFromLeft (6);
                compact (adaptGoalBox, r1);
            }
            si.removeFromTop (kGapIn);
            auto r2 = si.removeFromTop (kRowH);
            // 44 was measured for "Period"; "Per\xc3\xado" "de" needs 4 more and was condensed.
            caption (chordPerLbl, r2, 44, kValW + 6);
            // ONE RIGHT EDGE - AND, NOW, ONE WIDTH - for the values in this stack. Every
            // row here is [caption][value], and the captions are wildly different lengths
            // - "Notes" against "Smallest interval (c)" - so left-placing each value put
            // the three of them at three different x (1035 / 1036 / 1116 in the shipped
            // window): a stack that reads as one column but is not one. Their captions
            // cannot be made one width (the longest is 125 px inside a 192 px column,
            // which would leave the Notes row's Min/Max box 13 px), so the alignment is
            // taken on the side that CAN hold: the values end where the row ends, which
            // is also where the Notes row's Min/Max box and the buttons under them end.
            //
            // The right edge alone was not enough. Two right-anchored fields 44 and 48 px
            // wide have their LEFT edges 4 px apart, so the column this comment claimed
            // to be building had a step in it that only showed on the side nobody had
            // measured. kValW is that width, and it is now the width of EVERY value in
            // the stack - the two range fields and the register-band field included, and
            // those two used to be 44 and 32 starting at the same x.
            //
            // In the adaptation the period is not the last thing on its row - the
            // amplitudes toggle follows it - so there it stays left, beside the
            // caption it belongs to. That is the one value in the stack that cannot take
            // the shared right edge, and it is not a choice: the toggle needs every
            // remaining pixel of that row in Catalan (see below), so moving the field
            // right would either overlap it or condense its word.
            if (adapt)
            {
                chordPeriodEdit.setBounds (r2.removeFromLeft (kValW));
                // ...and the toggle takes EVERY remaining pixel of the row. The 6 px gap
                // that used to be spent here bought nothing: LookAndFeel_V4 draws the tick
                // box 4 px in from the button's own left edge, so the air was already
                // there and the gap was 6 px of it taken off the one control on this row
                // that had none to spare (see the button text in applyLanguage - the
                // Catalan word is 13.11 px longer than the box it was drawn in).
                adaptAmpsToggle.setBounds (r2);
            }
            else
                chordPeriodEdit.setBounds (r2.removeFromRight (juce::jmin (kValW, r2.getWidth())));
            // The result count is fixed at 16 - one control less to think about.
            chordCountLbl.setVisible (false); chordCountBox.setVisible (false);
            si.removeFromTop (kGapIn);
            auto r3 = si.removeFromTop (kRowH);
            caption (chordMinLbl, r3, 112, kValW); // "Smallest interval (c)" / "Interval mes petit (c)"
            // ...and this is the value whose caption is the long one, so it is the row
            // that was 80 px out of line with the two above it. Nothing follows it in
            // either mode, so it takes the shared right edge unconditionally.
            chordMinEdit.setBounds (r3.removeFromRight (juce::jmin (kValW, r3.getWidth())));
            if (adapt && adaptGoalBox.getSelectedId() == 3)
            {
                si.removeFromTop (kGapIn);
                auto rr = si.removeFromTop (kRowH);
                // The PAIR takes the shared right edge, so the high end of the range lands
                // on the same line as every other value in the stack and the low end one
                // field-width in from it. Laid out right to left for exactly that reason:
                // anchoring the pair on the LEFT (which is what it did) put its right edge
                // wherever the caption's length in the current language happened to leave
                // it, so the row moved when the language did.
                caption (adaptRangeLbl, rr, 58, kValW + 4 + kValW); // "Between"
                adaptHiEdit.setBounds (rr.removeFromRight (juce::jmin (kValW, rr.getWidth())));
                rr.removeFromRight (4);
                adaptLoEdit.setBounds (rr.removeFromRight (juce::jmin (kValW, rr.getWidth())));
            }
            if (adapt)
            {
                // HOW WIDE, and - on the line under it - FROM WHERE. On its own row because
                // the answer genuinely differs per register and the user must be able to
                // see, and change, what was optimised for rather than infer it from the
                // note they happened to be holding.
                si.removeFromTop (kGapIn);
                auto rg = si.removeFromTop (kRowH);
                // ONE control on this row: the caption and a value like "1.5". The
                // aggregate box that used to sit beside it is gone - it could not change
                // the result - and so are the two lines of sentence under it that explained
                // the choice; both are now one paragraph in this field's own hover. The
                // field was 32 px and left-anchored, which put it at the same x as the
                // range row's low field one row up and made the two look like a column
                // they were not; it is kValW and right-anchored now, like every other
                // value here. The caption may still grow into the rest of the row rather
                // than condensing to 70% in a language with a longer word - which is what
                // the Catalan "Periodes" would do at the 46 px the shorter caption needed.
                caption (adaptRegLbl, rg, 58, kValW);
                adaptRegPeriodsEdit.setBounds (rg.removeFromRight (juce::jmin (kValW, rg.getWidth())));
                // WHAT N COMES TO, and WHICH NOTE it hangs off. One line at 10 pt, and it is
                // paid for out of the gaps around it rather than out of the result list:
                // without this line N is a magic number and the anchor is invisible, which
                // is the whole failure mode of a range that follows the hands. Glued
                // directly under the field it explains - no gap at all - because it is
                // that field's read-out, not a row of its own.
                adaptRegSpanLbl.setBounds (si.removeFromTop (12));
            }
            // HOW the search is set up, above; the buttons that RUN it, below.
            if (chord || adapt) endSection();
            if (chord)
            {
                actionPair (chordGoBtn, chordRtBtn, si.removeFromTop (kRowH));
                // The chord search keeps its own progress line, and it keeps it INSIDE
                // this section - it is what the two buttons above it are doing. The
                // adaptation does NOT have one: its read-out below carries the progress,
                // and a second line that was blank most of the time and said
                // "Realtime: 37 retunes" the rest of it was one of the two lines nobody
                // could explain.
                //
                // It used to be laid out for BOTH beating sources - the row was taken off
                // `si` whenever the mode was not the adaptation - while the label itself
                // is only ever VISIBLE in the free search. So combining the current
                // tuning reserved 16 px for a label nobody can see, every time, and the
                // results list under it paid for them.
                si.removeFromTop (kGapIn);
                chordProgLbl.setBounds (si.removeFromTop (14)); // search progress only
            }
            if (adapt)
            {
                // Same pair, same shared edges as the search's Find / Realtime above.
                actionPair (adaptGoBtn, adaptRtBtn, si.removeFromTop (kRowH));
                // Lookahead on its own row UNDER the follower it belongs to: it is the
                // follower's latency trade and means nothing without it, so it must not
                // sit where it could be mistaken for a third action.
                si.removeFromTop (kGapIn);
                lookaheadBtn.setBounds (si.removeFromTop (kRowH));   // the column's own edges
                si.removeFromTop (kGapIn);
                // THE one read-out (see adaptReadoutText). 28 px is two lines at 10.5 pt,
                // and every sentence it can print is written to fit two - RhythmVerify
                // measures all five of them, in both languages, because only one is ever on
                // screen and the state that overflows is always the other one. There is no
                // second line of numbers under it any more and no separate progress row:
                // while a run is going, the progress IS this line.
                adaptInfoLbl.setBounds (si.removeFromTop (28));
            }
            // (the Results / Tuning info switch used to be laid out here - see the note
            // where it is hidden, above: in this mode the results list is the answer, and
            // a button offering to replace it with something the Tuning panel already
            // says was clutter over the thing you came to read. The 25 px it took go to
            // the list.)
        }

        //== THE FRAME AROUND THE SECTIONS ======================================
        // The stack ends here, so this is where the frame that encloses it can be
        // measured. It reaches kGapSec/2 above the first section's first row and the
        // same below the last section's last row - i.e. it lives in the two gaps that
        // already separated the stack from the mode box above and the results list
        // below, and takes not one pixel from either.
        //
        // FOUR pixels wider than the content on each side, and that number is the whole
        // width there is: si is inset 11 px from the column and gSpecScale's own 2.6 px
        // border sits about 5.6 px in, so the ground can be 4 px wider than the content
        // and still leave a pixel and a half of dark between itself and that border.
        // (The first version was 6 px and painted over the border's inner edge.)
        {
            const int frameBot = si.getY() + kGapSec / 2;
            auto frame = juce::Rectangle<int> (si.getX() - 4, bandTop - kGapSec / 2,
                                               si.getWidth() + 8, frameBot - (bandTop - kGapSec / 2));
            // Nothing to enclose is nothing to draw. It cannot happen in any of the three
            // modes as they stand - each lays out at least one row - but a frame around
            // nothing is the exact claim RhythmPanel's comment warns about, so it is
            // stated here rather than left to the modes to keep true.
            const bool haveSections = frame.getHeight() > kGapSec && frame.getWidth() > 8;
            specSections.setVisible (haveSections);
            if (haveSections)
            {
                specSections.setBounds (frame);
                specSections.rules.clear();
                for (const int y : bandRules)
                    specSections.rules.push_back (y - frame.getY());
                specSections.repaint();
            }
        }

        // The result list fills the REST of this same box.
        si.removeFromTop (kGapSec);
        const bool showDeg = dy; // the chord-degree view went with its switch (see above)
        // The SCOPE selector only decides how a combination is RANKED (against 0, or
        // over every pair); how many notes it has is the shared "Notes" box above.
        dissHeader.setVisible (false);
        dissView.setVisible (false);
        nadView.setVisible (showDeg);
        chordView.setVisible (chord && ! showDeg);
        if (adapt)
        {
            // HONEST FAILURE, the bank row's rule again: at the 870x540 floor the rows
            // above fill this column completely and `si` comes out 0 px tall, so the
            // result list was being laid out VISIBLE with no pixels - a viewport nobody
            // can see, scroll or click, with nothing on screen saying the panel ran out
            // of room. One row (24 px) is the least that can show a result; below that it
            // is hidden instead of drawn as a ghost.
            //
            // This makes the failure honest; it does not make the floor big enough. At
            // 870x540 the Adapt column genuinely cannot show its results, which is a
            // layout question for the column as a whole, not for this one line.
            const bool listFits = si.getHeight() >= 24;
            adaptView.setVisible (listFits);
            if (listFits)
            {
                adaptView.setBounds (si);
                const int ah  = adaptList.contentHeight();
                const bool as = ah > si.getHeight();
                adaptList.setSize (as ? si.getWidth() - 10 : si.getWidth(), juce::jmax (si.getHeight(), ah));
            }
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
        // The box's title band IS the chip row, at the one height every chip row in this
        // window uses (fgfit::chip) - the modulator sub-tabs included.
        const int chipH = fgfit::chip::kHeight;

        // Which engine's controls the shared box shows = which engine SOUNDS
        // (engineInForce), the same rule that lights the chips: clicking a chip both
        // selects the view and makes that engine the one generating. Temper mode shows
        // the HARMONIC view - the harmonic engine is the one generating there. The one
        // exception is the empty-workspace pin (see customViewShown in the header).
        const bool customView = customViewShown();

        //-- the chip strip IS the box title (like the Modulators box's tab row) -----
        {
            auto eb = engineBoxBounds.reduced (5, 0);
            auto chips = eb.removeFromTop (fgfit::chip::kHeight);
            for (int i = 0; i < 2; ++i)
                engineChip[i].setBounds (chips.removeFromLeft (
                    juce::jmin (fgfit::chip::width (engineChip[i]), juce::jmax (0, chips.getWidth())))
                        .reduced (fgfit::chip::kInsetX, 0));
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
            //-- HARMONIC view: BASE | HARMONICS over PRIME, TEMPER inside PRIME -------
            auto hb = engineBoxBounds.reduced (5, 0);
            hb.removeFromTop (chipH); hb.removeFromBottom (3);
            // TEMPER is reachable from BOTH views (see the ctor comment). Here it goes
            // at the FOOT OF THE PRIME CELL - "haz que el boton temper to tuning este
            // debajo de todo dentro de la seccion prime" - not spanning the whole box
            // under the cells, where it read as a fourth column of the box rather than
            // as something the prime filter owns. The 20 px row it used to take back
            // goes to the three cells.
            // Base is the wider column so the Base-pitch combo can show "Transposed
            // down" in full without clipping.
            hcBase      = hb.removeFromLeft (hb.getWidth() * 52 / 100);
            auto midCol = hb;
            // HARMONICS holds exactly ONE fader, so it takes exactly what that fader
            // needs and PRIME gets everything else. As a 2/5 : 3/5 fraction it was
            // hoarding 14 px that prime now needs: with TEMPER moved inside it, the
            // prime cell has to hold a mode box, nine tick boxes with their labels AND
            // that row, and the ticks came out 8 px tall - present, laid out, inside
            // their box, and far too small to hit.
            //
            // The half-and-half cap is NOT the thing to tune when the column is short.
            // Serving prime first instead - it has four rows to harmonics' one - reads
            // like the obvious improvement and measured 50 px WORSE (floor 590 -> 640):
            // harmonics cannot shrink at all, because under its caption sits a fader
            // whose own value label goes to negative height the moment the cell drops
            // below label + 24 + inset. Tried, measured, reverted; the note is here so
            // the next person does not spend the same hour.
            const int harmH = juce::jmin (midCol.getHeight() / 2, lblH + 24 + 6); // label + fader + its 3 px inset
            hcHarm      = midCol.withHeight (harmH);
            hcPrime     = midCol.withTrimmedTop (harmH);
            {
                auto bi = hcBase.reduced (5, 3); bi.removeFromTop (lblH);
                // THE WORD IS RESERVED BEFORE THE FADER, not after it. The N fader took a
                // flat 34% and the pitch box took the remainder, so the box's longest item
                // - "Transposed down", "Transposada avall" - was the first thing in the
                // whole window to stop fitting as the window narrowed: it is what the
                // rig's legibility measurement names as the binding control in BOTH
                // languages. A fader is elastic and a word is not, so the box states what
                // it needs and the fader lives on the rest.
                //
                // Still capped at the same 34%, so nothing that fits today gets wider, and
                // floored at 40 px so the fader never becomes a line.
                const int pitchNeed = fgfit::snugComboWidth (basePitchBox, 24);
                const int leftW = juce::jlimit (40, juce::jmax (40, bi.getWidth() * 34 / 100),
                                                bi.getWidth() - 5 - pitchNeed);
                auto left = bi.removeFromLeft (leftW);
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

                // With the switch OFF the controls it governs are DIMMED, not removed.
                // Hiding them would make the panel jump every time the switch is clicked
                // and would leave the user with no idea what turning it on would do; a
                // greyed control says both "this exists" and "it is not acting on your
                // sound right now", which is exactly the state OFF describes.
                {
                    const bool pon = processorRef.apvts.getRawParameterValue ("primeOn")->load() > 0.5f;
                    primeModeBox.setEnabled (pon);
                    primeLimitFader.setEnabled (pon); lblPrimeLimit.setEnabled (pon);
                    for (int i = 0; i < 9; ++i) { primeToggle[i].setEnabled (pon); primeLbl[i].setEnabled (pon); }
                }

                auto pi = hcPrime.reduced (5, 3); pi.removeFromTop (lblH);
                // TEMPER, at the FOOT of the prime cell - reserved BEFORE the mode row
                // and the primes, so the row that must never vanish cannot inherit the
                // leftovers of the two states above it (the nine whitelist ticks are the
                // taller of them). Sized to its own words, like every other control in
                // this pass: a tick box the width of a whole cell is not a tick box.
                {
                    // ...and it yields on HEIGHT by the same rule it already yields on
                    // width. It used to take its 23 px unconditionally, which was free
                    // while the cell below it held one Limit fader and stopped being free
                    // the moment nine tick boxes with their numbers became the default:
                    // the row under it then reached zero height, and a control with no
                    // pixels is the structural fault that sets the whole window's minimum
                    // size. Reserved only when what is left still holds the mode row and
                    // a usable tick row - which is not a reversal of "reserved first", it
                    // is the same promise: TEMPER is never the row that gets squashed.
                    const int needBelow = 22 + 2 + (wl ? 24 : 22);
                    const bool temperRoom = pi.getHeight() - 23 >= needBelow;
                    auto tr2 = temperRoom ? pi.removeFromBottom (20) : juce::Rectangle<int>();
                    // Same rule as the tuning panel's fork buttons: a control that cannot
                    // show its word is HIDDEN, not squashed. A ToggleButton squeezed below
                    // its tick box has NEGATIVE room for its text - at 710x550 this one had
                    // -8 px - and what is left on screen is an unlabelled tick that
                    // re-tunes the whole spectrum. That is worse than absent: the feature
                    // is still reachable everywhere the panel is wide enough to say what
                    // it does.
                    const int tw = fgfit::snugToggleWidth (temperToggle, 20);
                    const bool temperFits = temperRoom && tw <= tr2.getWidth();
                    temperToggle.setVisible (temperFits);
                    if (temperFits) temperToggle.setBounds (tr2.removeFromLeft (tw));
                    if (temperRoom) pi.removeFromBottom (3);
                }
                // Limit / Whitelist, at the width those two words need - not the whole
                // cell. It held a 247 px box for a six-letter mode name. The ON switch
                // takes the space to its right, which that row was already wasting.
                {
                    auto pmr = pi.removeFromTop (22);
                    compact (primeModeBox, pmr.removeFromLeft (
                                 juce::jmin (fgfit::snugComboWidth (primeModeBox, 22), pmr.getWidth())));
                    // Hidden rather than squashed, by the same rule as TEMPER above: a
                    // ToggleButton with no room for its word is an unlabelled tick that
                    // silently filters the whole spectrum. When it will not fit, the
                    // panel is too narrow to be operated anyway - and OFF, its default,
                    // is the state in which nothing is being hidden from the user.
                    pmr.removeFromLeft (6);
                    const int ow = fgfit::snugToggleWidth (primeOnToggle, 20);
                    const bool onFits = ow <= pmr.getWidth();
                    primeOnToggle.setVisible (onFits);
                    if (onFits)
                        primeOnToggle.setBounds (pmr.removeFromRight (ow).withSizeKeepingCentre (ow, 20));
                }
                pi.removeFromTop (2);
                if (wl)
                {
                    // Nine cells out of a width that is almost never a multiple of nine.
                    // The integer division threw the remainder away - up to 8 px, which
                    // is more than half a digit at the size these labels are drawn - so
                    // the first (w % 9) cells take one pixel more and the row ends flush
                    // with the panel instead of short of it.
                    const int wAll = pi.getWidth();
                    const int tw   = wAll / 9;
                    const int extra = wAll - tw * 9;
                    // ...and the two of them SHARE the height instead of one taking a
                    // fixed 13 and the other whatever is left. That subtraction went
                    // negative in a short window and handed the tick box a height of
                    // ZERO - a visible control with no pixels, which is the structural
                    // fault the rig refuses to floor on, and it cost the whole window
                    // 60 px of minimum height the moment Whitelist became the default
                    // mode. Now the tick keeps 10 px before the number gives any back,
                    // and neither ever reaches zero.
                    const int rowH = juce::jmax (1, pi.getHeight());
                    const int lblRowH = juce::jlimit (1, 13, rowH - 10);
                    // The number SHRINKS TO ITS CELL rather than stepping through sizes
                    // somebody picked by eye. Measured at a reference size and scaled:
                    // two digits at 12.5 Bold need 14.6 px, the cell is often 16 and was
                    // 13 at the width where the window's legibility measurement stopped -
                    // first on "11", then on "17", then on "19", which is the signature of
                    // a fixed size meeting a variable box. Floored at 8 so it never
                    // becomes decoration, capped at 12.5 so a roomy panel looks exactly
                    // as it did.
                    const juce::Font ref (juce::FontOptions (10.0f).withStyle ("Bold"));
                    float widest = 1.0f;
                    for (int i = 0; i < 9; ++i)
                        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (
                                                         ref, primeLbl[i].getText()));
                    const float pf = juce::jlimit (8.0f, 12.5f,
                                                   10.0f * (float) juce::jmax (1, tw - 2) / widest);
                    for (int i = 0; i < 9; ++i)
                    {
                        const int cw = tw + (i < extra ? 1 : 0);
                        auto cell = pi.removeFromLeft (cw);
                        primeLbl[i].setFont (juce::Font (juce::FontOptions (pf).withStyle ("Bold")));
                        primeLbl[i].setBounds (cell.removeFromBottom (lblRowH));
                        primeToggle[i].setBounds (cell.withSizeKeepingCentre (
                            juce::jmin (22, cw), juce::jmax (1, juce::jmin (22, cell.getHeight()))));
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
            primeOnToggle.setVisible (false); // the switch belongs to the harmonic view too
            for (int i = 0; i < 9; ++i) { primeToggle[i].setVisible (false); primeLbl[i].setVisible (false); }

            auto ci = engineBoxBounds.reduced (6, 0);
            ci.removeFromTop (chipH + 2); ci.removeFromBottom (4);
            // The bottom rows first, then EVERYTHING left goes to the intervals box:
            // the custom view has far fewer controls than the harmonic one, and the
            // freed space is exactly what the typed spectrum needed - it was one 24 px
            // line squeezed into a fifth of the panel.
            //
            // ...BUT THE BOX YOU TYPE INTO IS THE LAST THING TO SHRINK, NOT THE FIRST.
            // "everything left" also means every pixel the panel is SHORT of comes out of
            // this one control, because it is the only one taking the remainder. At
            // 1200x540 that left it 13 px tall, and at the 870x540 floor the snapshot rig
            // photographs, its own text is not visible at all: a pink outline, on screen,
            // enabled, holding "5/4 3/2 2" and showing none of it. From the player's chair
            // that is a box which ignores everything typed or deleted in it, while Load,
            // Random and Temper - buttons - go on working perfectly.
            //
            // So the rows below give way FIRST, each down to a floor of its own (a 20 px
            // button is still hittable; an 18 px fader still draggable), and only what is
            // left after that comes out of the box. The give is ZERO whenever the panel
            // can afford the natural heights, which is every size at or above the shipped
            // 1200x650 - measured, not assumed: RhythmVerify run 52 asserts the box is
            // still exactly 68 px there, so this cannot move the layout anybody has seen.
            constexpr int kBoxWants   = 43;                     // two 13 px rows inside its 4/5 px borders
            constexpr int kBelowNat   = 22 + 4 + 16 + 4 + 24 + 4;
            constexpr int kBelowFloor = 18 + 2 + 12 + 2 + 20 + 2;
            const int give = juce::jlimit (0, kBelowNat - kBelowFloor,
                                           kBoxWants - (ci.getHeight() - 2 - kBelowNat));
            // Each row moves the same fraction of the way to its own floor, so they stay
            // in proportion instead of one of them collapsing while its neighbour keeps
            // its full height - the fault this whole block exists to stop, one level down.
            auto shrunk = [give, span = kBelowNat - kBelowFloor] (int natural, int floor)
            { return natural - (natural - floor) * give / span; };
            auto repsRow = ci.removeFromBottom (shrunk (22, 18));
            ci.removeFromBottom (shrunk (4, 2));
            auto statusRow = ci.removeFromBottom (shrunk (16, 12));
            ci.removeFromBottom (shrunk (4, 2));
            // [Load / Remove][Random][Temper to tuning] - UNDER the ratios box now:
            // "haz que los botones remove random y temper to tuning esten abajo de la
            // caja de ratios de custom tuning, la cual podras subir un poco arriba para
            // hacer espacio". They were the box's top row, so the thing you type into
            // began a third of the way down its own panel; the box now starts directly
            // under the chip strip and the buttons sit beneath it, where the result of
            // pressing them is what you are already looking at.
            //
            // Two buttons that never change in number, so the row does not re-shape
            // under the user's hand the way it did when the Off pill appeared and
            // disappeared with the loaded scale. RANDOM is always there because it is
            // the one control that can CREATE a spectrum out of nothing, which is
            // exactly the state where a workspace is emptiest. TEMPER is reachable from
            // BOTH views (see the ctor comment) and joins them at the same size rule.
            auto brow = ci.removeFromBottom (shrunk (24, 20));
            ci.removeFromBottom (shrunk (4, 2));
            {
                auto b = brow.reduced (2, 0);
                // A control that cannot show its word is HIDDEN, not squashed - the same
                // rule the tuning panel's fork buttons follow. This row takes three
                // controls left to right, so the LAST one gets whatever survives: at
                // 710x550 the Temper toggle got 20 px, which is NEGATIVE room once its
                // tick box is drawn, leaving an unlabelled tick that re-tunes the whole
                // spectrum. Load and Random are the two that can create a spectrum from
                // nothing, so Temper is the one that goes; it is reachable from the
                // harmonic view too, and from every size wide enough to say what it does.
                auto take = [&b] (juce::Component& c, int w, bool mayHide = false)
                {
                    if (mayHide && w > b.getWidth()) { c.setVisible (false); return; }
                    c.setBounds (b.removeFromLeft (juce::jmin (w, juce::jmax (0, b.getWidth()))));
                    b.removeFromLeft (6);
                };
                take (loadTimbreButton,  fgfit::snugTextButtonWidth (loadTimbreButton, 24));
                take (randomSpectrumBtn, fgfit::snugTextButtonWidth (randomSpectrumBtn, 24));
                take (temperToggle,      fgfit::snugToggleWidth (temperToggle, 24), true);
            }
            ci.removeFromTop (2);                 // the ratios box rides higher now
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

        //-- BOTH ENGINES: SLOPE | FRACTAL | MORPH over DRY/WET over RANGE -------
        {
            auto bo = bothEnginesBounds.reduced (5, 0);
            bo.removeFromTop (chipH); bo.removeFromBottom (3);
            const int bW = bo.getWidth();
            beSlope   = bo.removeFromLeft (bW * 31 / 100);
            beFractal = bo.removeFromLeft (bW * 23 / 100);
            // TWO cells in this column, where there were three: MORPH and DRY/WET are one
            // TRANSITION cell now ("asi economizamos espacio"). It sits between FRACTAL's
            // wall and OCTAVE RANGE, which is where the pair already lived.
            //
            // OCTAVE RANGE / PHASE KEEP EXACTLY THE HEIGHT THEY HAD, computed from the
            // very expression that used to size the two smoothing cells, and the whole row
            // the merge freed goes to TRANSITION. That is deliberate: this column's total
            // height is fixed by the panel, so the saving cannot become empty space - it
            // has to be spent somewhere - and spending it on the cell that lost a row
            // means nothing below it moves by a pixel. At the 870x540 floor that is what
            // makes the merge worth doing at all: the pair's control rows were 9 px tall
            // there (a 26 px cell, less the sub-label band), which is not a size a combo
            // or a fader can be used at. One cell of 52 gives them 16 px each.
            auto mcol = bo;
            const int rangeH = mcol.getHeight() - 2 * juce::jlimit (26, 46, (mcol.getHeight() - 40) / 2);
            beTrans   = mcol.removeFromTop (juce::jmax (0, mcol.getHeight() - rangeH));
            beRange   = mcol;
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
            // THE TRANSITION CELL: title band, the mode dropdown on its own row, then the
            // "Time" caption and its fader on the next.
            //
            // TWO ROWS AND NOT ONE, and the floor is what decides it. Side by side the row
            // would have to hold the box (sized to its widest ITEM - the box does not
            // resize when the mode changes, so anything less clips the others silently),
            // the caption, and a fader that carries a 42 px value read-out. At 870x540
            // this cell is 141 px wide inside its inset: the box alone wants ~85 of them
            // and the caption ~46, which leaves the fader about ten. Stacked, the box gets
            // the whole width and so does the fader, at every size - and the cell still
            // costs one row less than the two it replaced.
            {
                auto ci2 = beTrans.reduced (6, 2); ci2.removeFromTop (lblH - 2);
                // Rows grow with the cell so the wide sizes are not two thin controls
                // floating in a tall box, and the pair is centred in what is left rather
                // than pinned under the title with all the slack at the bottom.
                const int rowH = juce::jlimit (15, 24, (ci2.getHeight() - 3) / 2);
                auto blk = ci2.withSizeKeepingCentre (ci2.getWidth(),
                                                      juce::jmin (ci2.getHeight(), rowH * 2 + 3));
                auto boxRow = blk.removeFromTop (juce::jmin (rowH, blk.getHeight()));
                blk.removeFromTop (juce::jmin (3, blk.getHeight()));
                transitionBox.setBounds (boxRow.removeFromLeft (
                        juce::jmin (fgfit::snugComboWidth (transitionBox, juce::jmax (1, boxRow.getHeight())),
                                    boxRow.getWidth())));
                auto tr2 = blk;
                caption (lblTransTime, tr2, 34, 60);
                // Only the VISIBLE fader is given the rectangle; the other one keeps
                // whatever bounds it had and is not drawn, laid out or probed. Both are
                // set so a mode change cannot leave the incoming fader at a stale size.
                morphTimeFader.setBounds (tr2);
                dryWetTimeFader.setBounds (tr2);
            }
            {
                // RANGE: the spectrum's octave span, as the same [value][+][-] stepper it
                // had in Essentials. Its name is the painted OCTAVE RANGE sub-label now,
                // so the row's own "Spectrum" label was deleted rather than hidden - a
                // member hidden on every single layout pass is a member nothing can ever
                // show, and it kept a dead entry alive in the layout probe.
                //
                // PHASE SHARES THIS ROW, to its RIGHT, as its own titled sub-section -
                // which is where the user asked for it and it is the only side of the
                // column with room. The split is NOT even, on purpose: RANGE's three
                // controls are FIXED width (40 + 24 + 4 + 24 = 92 px), so extra pixels do
                // nothing for them, while width is the whole difference between a fader
                // you can aim and one you cannot. RANGE therefore keeps just what it
                // needs and every spare pixel goes to the fader.
                //
                // MEASURED AT THE FLOOR RATHER THAN HOPED FOR, because the previous pass
                // shipped an 18 px fader here by hoping: at 870x540 this column's cell is
                // 155 px, so RANGE keeps 104 and PHASE gets 51 - a 41 px painted title
                // band ("PHASE" 33 px, "FASE" 27) and a 39 px fader, which is exactly
                // what the SLOPE fader beside it already ships at. The binding constraint
                // was never the fader, it was the Catalan "ABAST OCTAVES" at 88 px: RANGE
                // keeping 104 leaves its band 94 px, so the title still clears by 6 px.
                //
                // Neither side may reach zero width: the layout probe FAILS a control
                // collapsed to nothing, which would raise the window's minimum size
                // instead of merely looking cramped. Hence the clamp, and the cap at half
                // the row so OCTAVE RANGE always reads as the row's primary control.
                // THE VALUE BOX IS SIZED TO THE WIDEST NUMBER IT CAN EVER SHOW, and the
                // room that frees is what pays for the row's two real problems: the "-"
                // button had ONE pixel of glyph room at the floor, and PHASE's own title
                // could not fit its cell at any readable size.
                //
                // 40 px was a hand-picked number for a box whose whole content is an
                // integer: "10" measures 15. Measured off the PARAMETER's own ends rather
                // than the value on screen, or the box would resize under the user as they
                // step through it - the same trap a combo sized to its selected item falls
                // into. 24 stays the minimum so it remains a comfortable read.
                int valW = 24;
                if (auto* rp = processorRef.apvts.getParameter ("spectrumRange"))
                {
                    const auto vf = fgfit::labelFont (sprVal);
                    for (const float u : { 0.0f, 1.0f })
                        valW = juce::jmax (valW, juce::GlyphArrangement::getStringWidthInt (
                                                     vf, juce::String ((int) rp->convertFrom0to1 (u))) + 10);
                }
                // What RANGE's compartment really needs: its stepper, plus the 6 px inset
                // it keeps either side. PHASE gets the rest, and never less than its own
                // title needs (28 px at the painted floor of 9 px, plus the same insets) -
                // it was clamped at 34 and losing the "E" of PHASE in silence.
                const int rangeCtlNeed = valW + 24 + 4 + 24 + 12;   // at full stepper width
                const int phaseTitle   = 40;
                const int phaseW = juce::jlimit (34, juce::jmax (34, beRange.getWidth() / 2),
                                                 juce::jmax (phaseTitle,
                                                             beRange.getWidth() - rangeCtlNeed));
                bePhase = beRange.removeFromRight (phaseW);

                auto ri = beRange.reduced (6, 2); ri.removeFromTop (lblH);
                auto rr = ri.removeFromTop (juce::jmin (24, juce::jmax (0, ri.getHeight())));
                // The two stepper buttons shrink together, to a measured floor, instead of
                // demanding 24 each and letting the LAST of them keep the remainder. That
                // remainder was ZERO at the floor: a "-" button with no room for its own
                // glyph, which is a control that exists and cannot be used. 14 px still
                // holds the WIDER of the two glyphs ("+" measures 8.85, "-" only 4.90) inside
                // the painter's 4 px a side, and the two are
                // always the same width - a "+" wider than its "-" reads as a mistake.
                // 14 was tried first and measured: it fits the "-" and clips the "+".
                const int stepW = juce::jlimit (17, 24, (rr.getWidth() - valW - 4) / 2);
                sprVal.setBounds (rr.removeFromLeft (juce::jmin (valW, rr.getWidth())));
                sprUp.setBounds  (rr.removeFromLeft (juce::jmin (stepW, rr.getWidth())).reduced (0, 1));
                rr.removeFromLeft (juce::jmin (4, rr.getWidth()));
                sprDn.setBounds  (rr.removeFromLeft (juce::jmin (stepW, rr.getWidth())).reduced (0, 1));

                // PHASE, laid out to the same recipe as its neighbours: painted title
                // band, then the control on one row.
                auto pi = bePhase.reduced (6, 2); pi.removeFromTop (lblH);
                auto pr = pi.removeFromTop (juce::jmin (24, juce::jmax (0, pi.getHeight())));
                // The numeric read-out is the first thing to go, for the same reason it
                // was in Essentials: a 38 px number box inside a 45 px cell leaves a 7 px
                // track. The ends are named in the tooltip.
                phaseFader.setTextBoxStyle (pr.getWidth() >= 96 ? juce::Slider::TextBoxRight
                                                                : juce::Slider::NoTextBox,
                                            false, 38, 14);
                phaseFader.setBounds (pr);
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
            // much less than the half-box it used to take and the other cells get the
            // difference - the Reference read-out finally has room to breathe.
            // THREE cells, not four: PHASE moved out to the SPECTRUM panel, beside OCTAVE
            // RANGE, so GAIN and REFERENCE divide what its row used to hold.
            //
            // EVEN THIRDS. OCTAVE used to be capped at 40 px and the other two split what
            // was left, which is invisible at the 870x540 floor (37/37/38 either way) but
            // turns into 126/126/40 at 1600x900: two cells that swell as the window grows
            // and a third that cannot, so the box's last divider sits stranded near the
            // bottom edge and the panel reads as unfinished rather than as three fields.
            // Every cell centres its own control (jmin (22, ...) below), so an even cell
            // spends its extra height as symmetric air around the control rather than on
            // a stretched widget - the growth is distributed, never pooled.
            // The remainder is SPREAD, not pooled. e.getHeight() is rarely a multiple of
            // three, and `h/3, h/3, the rest` hands the whole remainder to the last cell:
            // 167 px comes out 55/55/57, so OCTAVE's divider sits 2 px off where three even
            // fields would put it and the panel's bottom field reads as the odd one out.
            // One pixel at a time from the top instead, so no two cells ever differ by more
            // than one - and the three still tile the box exactly, which is what paint()
            // needs (every divider is drawn at a cell's TOP edge).
            const int eh  = e.getHeight();
            const int ch  = eh / 3;
            const int rem = eh - ch * 3;              // 0, 1 or 2
            ecGain  = e.removeFromTop (ch + (rem > 0 ? 1 : 0));
            ecRef   = e.removeFromTop (ch + (rem > 1 ? 1 : 0));
            ecOct   = e;   // the remainder, so the three cells always tile the box exactly
            // THE LABEL GOES ABOVE THE CONTROL WHEN THE ROW CANNOT HOLD BOTH.
            //
            // The sub-label band is a fixed 78 px at the cell's left. At the 870x540
            // floor the box is 176 px wide, so that leaves 76 for the controls - and the
            // OCTAVE row alone needs 92 ([value 40][+ 24][gap 4][- 24]). What happened
            // instead was silent: the "-" button came out 8 px wide with NO room at all
            // for its glyph, and REFERENCE's "C3 =" caption came out 6 px. Both were
            // inside their cell, neither had collapsed to zero, and every geometric check
            // in the build stayed green while two controls on screen were unusable.
            //
            // Narrowing the band is not the fix: it would clip the painted word instead
            // ("REFERÈNCIA" needs 66 of the 76 it has). So when the row is too narrow for
            // both, the label takes the TOP of the cell and the control the whole width
            // underneath - 13 + 22 = 35 in a 37 px cell. paintedSubLabels() reads the same
            // flag, so the word is drawn where the space was actually reserved for it.
            const int kEssLabelW  = 78;
            const int kEssCtlNeed = 92;   // the OCTAVE stepper, the widest fixed row here
            essStacked = ecOct.reduced (6, 1).getWidth() - kEssLabelW < kEssCtlNeed;
            // One helper so the three cells cannot drift apart, and so "stacked" means
            // exactly one thing.
            auto essCell = [this, kEssLabelW] (juce::Rectangle<int> cell)
            {
                auto c = cell.reduced (6, 1);
                if (essStacked) c.removeFromTop (13);
                else            c.removeFromLeft (kEssLabelW);
                return c;
            };
            // Reference value used to clip its descenders).
            // GAIN / REFERENCE / OCTAVE: the control sits to the RIGHT of its inline
            // sub-label (drawn in paint at the cell's left), or under it when stacked.
            { auto c = essCell (ecGain);
              gainFader.setBounds (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()))); }
            { auto c = essCell (ecRef);
              auto rr = c.withSizeKeepingCentre (c.getWidth(), juce::jmin (22, c.getHeight()))
                          .withWidth (juce::jmin (c.getWidth(), 158));
              caption (lblRefA, rr, 34, 70);
              lblRefHz.setBounds (rr.removeFromRight (juce::jmax (22, fgfit::labelWidth (lblRefHz))));
              refAEdit.setBounds (rr);
              // THE PRECISION FOLLOWS THE WIDTH, and it can only be decided here because
              // this is where the width is decided. Called from the 30 Hz timer as well,
              // but a probe or a photograph can be taken between a resize and the next
              // tick, and at 690x550 that is the difference between "261.626" and the
              // field scrolling itself to "55653". Cheap: refreshRefAText compares the
              // string it built with the one on screen and does nothing when they match.
              refreshRefAText(); }
            { auto c = essCell (ecOct);
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
            const juce::Colour tabCols[4] = { cEnv, cPitch, cFilter, cCustomMod };
            gAmpEnv.setColour (juce::GroupComponent::outlineColourId, tabCols[envTab]);
            gAmpEnv.setColour (juce::GroupComponent::textColourId,    tabCols[envTab]);
            gAmpEnv.setBounds (eb.reduced (3, 0)); // title = "Envelopes" (set in applyLanguage)
            gPitchEnv.setVisible (false);
            gFilter.setVisible (false);

            auto in = eb.reduced (3).reduced (5, 4);
            in.removeFromTop (20);                // clear the box title - chips sit BELOW it
            // The SAME chip band as the engine chips above (fgfit::chip), which is what
            // makes the two rows one size. Height, inset and font are the shared part.
            //
            // TWO ROWS OF TWO WHEN FOUR WILL NOT FIT ON ONE. The four used to divide the
            // box's width evenly, which is fine while the box is wide and a lie when it is
            // not: at the 750x550 floor the box is 121 px, so each chip gets 25 and its
            // WORD gets 17 - against 38 for "FILTER" and 46 for "CUSTOM". These words are
            // the feature. They are the only thing on screen that says which of the four
            // modulator targets you are editing, and a chip strip that has silently
            // condensed all four to 70% is a navigation control you have to guess at.
            //
            // Measured against what the words really need - the painter's own width for
            // the text plus the inset each chip keeps, and NOT fgfit::chip::width, which
            // adds kSnugPad on top. That pad is breathing room a chip sized to its own
            // word asks for; using it as the WRAP threshold made the strip fold at
            // 1200x650, where all four words fit a single row comfortably. The question
            // here is "can these four words be read side by side", not "can they have
            // their ideal padding".
            auto chipNeed = [] (const juce::TextButton& b)
            { return fgfit::textButtonWidth (b.getButtonText(), fgfit::chip::kHeight, false)
                     + 2 * fgfit::chip::kInsetX; };
            const int chipsNeed = chipNeed (envTabB[0]) + chipNeed (envTabB[1])
                                + chipNeed (envTabB[2]) + chipNeed (envTabB[3]);
            const bool chipsWrap = chipsNeed > in.getWidth();
            juce::Rectangle<int> chipRect[4];
            // EQUAL TABS WHILE THEY FIT, PROPORTIONAL WHEN THEY DO NOT.
            //
            // Four equal quarters is the look a tab strip wants, and it holds while the
            // box is wide. It stops being honest the moment the widest WORD does not fit
            // its quarter: "CUSTOM" needs 45.8 px of text room and an even quarter gives
            // it 45.0 at 1190 px of window - eight tenths of a pixel, and the strip
            // silently condenses. AMP has 22 px it is not using at that size.
            //
            // So the even split is kept until it fails, and then the row divides by what
            // each word measures. The shipped 1200x650 and everything above it are
            // untouched; below that the four tabs stop being the same width and start
            // being legible, which is the trade this whole pass keeps making.
            const auto shareOut = [&chipRect, &chipNeed, this] (juce::Rectangle<int> row, int first, int count)
            {
                // The FULL row width, captured once: removeFromLeft shrinks `row` as it
                // goes, so a share computed from row.getWidth() inside the loop is a share
                // of what is left, not of the row. Written that way first, and measured -
                // it gave the last chip 98 px of a 220 px row and the one before it 33.
                const int rowW = row.getWidth();
                int total = 0, widest = 0;
                for (int k = 0; k < count; ++k)
                {
                    const int n = chipNeed (envTabB[first + k]);
                    total += n; widest = juce::jmax (widest, n);
                }
                const int even = rowW / count;
                const bool evenFits = widest <= even;
                for (int k = 0; k < count; ++k)
                    chipRect[first + k] = (k == count - 1)
                        ? row
                        : row.removeFromLeft (evenFits ? even
                                                       : rowW * chipNeed (envTabB[first + k])
                                                             / juce::jmax (1, total));
            };
            if (! chipsWrap)
            {
                shareOut (fgfit::chip::centreInRow (in.removeFromTop (fgfit::chip::kRowHeight)), 0, 4);
            }
            else
            {
                // Two rows, two chips each, in reading order. AMP/PITCH above,
                // FILTER/CUSTOM below - the same order the single row has, so the strip
                // does not reshuffle itself as the window is dragged.
                for (int row = 0; row < 2; ++row)
                {
                    if (row == 1) in.removeFromTop (2);
                    shareOut (fgfit::chip::centreInRow (in.removeFromTop (fgfit::chip::kRowHeight)), row * 2, 2);
                }
            }
            // ONE NEUTRAL FOR THE THREE TABS YOU ARE NOT ON; THE PANEL'S HUE FOR THE ONE
            // YOU ARE.
            //
            // Each unselected chip used to be its OWN identity colour at 20% alpha, and
            // that arithmetic produced four accidents rather than four colours. Over the
            // panel fill the results were AMP #4EDF98 (selected), PITCH #463E2F, FILTER
            // #2D4C50 and CUSTOM #2B4150 - PITCH a warm brown that matches nothing else in
            // the product (and looks like a rendering fault once the panel turns blue on
            // the CUSTOM tab), and FILTER and CUSTOM eleven units apart in ONE channel:
            // two chips side by side that nobody can tell apart. The identity colours were
            // chosen to be maximally separated at FULL strength (GalacticPalette.h); at
            // 20% over a dark fill that separation is simply gone, so the dimming was
            // destroying the very property it was borrowing.
            //
            // A tab strip only has to answer one question - which one am I on - and it
            // answers it with the CONTRAST between the lit chip and the rest, not with
            // four hues. So the three unselected chips share ONE neutral - a white wash of
            // the panel fill, the same family as the 0x14ffffff the zebra rows use, at
            // 0x2e so a chip still reads as a chip rather than as a hole - and the selected
            // chip keeps its full identity colour, which is the hue the whole box beside it
            // is already wearing.
            const juce::Colour unlit (0x2effffff);
            for (int k = 0; k < 4; ++k)
            {
                envTabB[k].setColour (juce::TextButton::buttonColourId,
                                      envTab == k ? tabCols[k] : unlit);
                envTabB[k].setColour (juce::TextButton::textColourOffId,
                                      envTab == k ? juce::Colours::black : juce::Colour (0xffb9c0d0));
                envTabB[k].setBounds (chipRect[k].reduced (fgfit::chip::kInsetX, 0));
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
                    // The column is at least as wide as this fader's own read-out needs:
                    // the flat 34 knew nothing about the value box inside it, which is how
                    // the glide fader ended up drawing "120.0 ms" condensed to 70% in a
                    // 24 px label. Still at least 34, so nothing that fitted gets narrower.
                    const int colW = juce::jmax (34, fgfit::faderValueWidth (*sets[t][i]),
                                                 in.getWidth() / 7);
                    auto c = area2.removeFromLeft (juce::jmin (colW, juce::jmax (34, area2.getWidth())));
                    sets[t][i]->setVisible (true);
                    labs[t][i]->setVisible (true);
                    labs[t][i]->setBounds (c.removeFromTop (13));
                    sets[t][i]->setBounds (c);
                }
                if (on)
                {
                    voiceEnv.setVisible (true);
                    // THE GRAPH LINES UP WITH THE TAB STRIP ABOVE IT.
                    //
                    // It was `withTrimmedLeft (4)` and nothing on the right, while the chip
                    // row is inset by fgfit::chip::kInsetX (1) on BOTH sides of the same
                    // `in` rectangle - so the two stacked elements of one panel started 3 px
                    // apart (292 against 295) and ended 1 px apart (508 against 509). Two
                    // vertical lines that are almost the same line read as a mistake where
                    // one line reads as a decision. Same inset, same constant, both edges.
                    voiceEnv.setBounds (area2.reduced (fgfit::chip::kInsetX, 0));
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
                // [modulator][On][Unmap]. There was a [Draw] button on the right of this
                // row; the drawn-wave editor OPENS BY ITSELF now, docked beside the box,
                // for as long as the selected LFO is on "Custom (draw)". The button had
                // been setVisible(false) unconditionally ever since - a control that
                // could not be reached, sized, clicked or photographed, still carrying a
                // tr() pair the language audits had to keep stepping over.
                auto selRow = in.removeFromTop (20);
                const int nCell = 3;
                const int cw3 = juce::jmax (30, selRow.getWidth() / nCell - 3);
                modSelBox.setBounds (selRow.removeFromLeft (cw3));
                selRow.removeFromLeft (3);
                modOnT.setBounds (selRow.removeFromLeft (juce::jmin (46, cw3)));
                selRow.removeFromLeft (3);
                // Unmap is sized to the word "Unmap" / "Desassigna", not to whatever the
                // row had left over - which was a third of the box for six letters.
                modUnmapBtn.setBounds (selRow.removeFromLeft (
                    juce::jmin (fgfit::snugTextButtonWidth (modUnmapBtn, selRow.getHeight()),
                                selRow.getWidth())));
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
                    // ONE BOX PER ROW WHEN TWO WILL NOT FIT ONE - AND TWO WHEN THEY DO.
                    //
                    // These were side by side, splitting the row in proportion to what
                    // each asked for. Measured with a probe that no longer lies about it,
                    // both were squeezed at the shipped size: "Fundamental / 2^n" needed
                    // 99.6 px of the 92 it got and "Pròpia (dibuixa)" 83.1 of 77 - so
                    // each took a row at its own width, and that cost the tab 22 px of
                    // fader height.
                    //
                    // "HAZ MAS ESTRECHO EL DESPLEGABLE DE LA FORMA DE ONDA... PARA QUE
                    // QUEPA EL DE MODO BPM A SU DERECHA" and "LOS KNOBS DE LA LFO CUSTOM
                    // AHORA SON MUY PEQUENOS, HAZLOS MAS GRANDES CON EL ESPACIO GANADO" -
                    // which are one complaint: a stacked pair leaves the rest of both rows
                    // empty and takes the height out of the knobs underneath. The pairing
                    // is back, but as a MEASURED condition rather than an assumption: the
                    // two go on one row only where their own snug widths both fit, and
                    // fall back to a row each where they do not. Neither is ever squashed,
                    // and every pixel the pairing saves lands on the faders below, which
                    // is where the 22 px came from in the first place.
                    // ...and when they share a row they get exactly what their widest
                    // item needs, without the 12 px of breathing room fgfit::kSnugPad
                    // adds. Measured: the column is 219 px, the two boxes want 93 and 140
                    // with the pad and 81 and 128 without, so the pad alone is the
                    // difference between one row and two. A box sized to its own widest
                    // item is not cramped - it is exact; the pad is a comfort, and a
                    // comfort is what you spend when the alternative is a whole row of
                    // dead space above a set of knobs that then lose 22 px of height.
                    const int waveRoomy = fgfit::snugComboWidth (lfoWaveBox, 20);
                    const int rateRoomy = fgfit::snugComboWidth (lfoRateModeBox, 20);
                    const int waveTight = waveRoomy - fgfit::kSnugPad;
                    const int rateTight = rateRoomy - fgfit::kSnugPad;
                    const bool pairRoomy = waveRoomy + 4 + rateRoomy <= in.getWidth();
                    const bool pairTop   = pairRoomy
                                        || waveTight + 4 + rateTight <= in.getWidth();
                    const int waveNeed = pairTop && ! pairRoomy ? waveTight : waveRoomy;
                    const int rateNeed = pairTop && ! pairRoomy ? rateTight : rateRoomy;
                    {
                        auto r = in.removeFromTop (20);
                        lfoWaveBox.setBounds (r.removeFromLeft (juce::jmin (juce::jmax (24, waveNeed),
                                                                            r.getWidth())).reduced (1, 0));
                        if (pairTop)
                        {
                            r.removeFromLeft (2);
                            lfoRateModeBox.setBounds (r.removeFromLeft (
                                juce::jmin (juce::jmax (24, rateNeed), r.getWidth())).reduced (1, 0));
                        }
                    }
                    in.removeFromTop (2);
                    if (! pairTop)
                    {
                        auto r = in.removeFromTop (20);
                        lfoRateModeBox.setBounds (r.removeFromLeft (juce::jmin (juce::jmax (24, rateNeed),
                                                                                r.getWidth())).reduced (1, 0));
                        in.removeFromTop (2);
                    }
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
            // Under a tuning whose period is NOT an octave the Shepard block needs a
            // SECOND line, for the rise-interval box.
            //
            // Measured honestly - fgfit::comboItemRoom takes off the label's own 5 px
            // border and measures the item in the BOLD face it is really drawn in - the
            // four controls want ~466 px of the 447 this panel has, so on one line the
            // ms/beats rate control was left 13 px wide: a fader nobody can grab. The
            // probe used to certify that row because it measured the lighter face against
            // the label's FULL width, which is 26 px of room that does not exist.
            const bool riseChoice = shepardRiseChoiceExists();
            // Both lines come off the bottom FIRST, for the reason the single one did:
            // reserved after the pile of optional rows above, they would inherit the
            // zero-height leftovers in exactly the states with the most buttons showing.
            auto riseRow = riseChoice ? ti.removeFromBottom (22) : juce::Rectangle<int>();
            if (riseChoice) ti.removeFromBottom (3);
            auto shepRow = ti.removeFromBottom (22);
            ti.removeFromBottom (3);
            // What is still to be placed BELOW the name row: 2 + period(18),
            // nSnug x (3 + 22), 3 + bend(22), 3 + bank(24). Hoisted ABOVE the mode row
            // because two decisions taken up there - whether the name can afford a row of
            // its own, and whether the three fork buttons can afford one - both turn on
            // what is left after this.
            const bool hasPatch = processorRef.activeSclHasPreset();
            const bool hasPresetTuning = processorRef.hasPendingPresetTuning()
                                       && ! processorRef.presetHasDrifted();
            const bool hasProvTuning = processorRef.hasSpectrumProvenance();
            const int  nSnug = (hasPatch ? 1 : 0) + (hasPresetTuning ? 1 : 0) + (hasProvTuning ? 1 : 0);
            const int  need  = 72 + 25 * nSnug;
            auto mrow = ti.removeFromTop (24);
            // THE THREE FORK BUTTONS TAKE THEIR OWN ROW WHEN THE MODE ROW CANNOT HOLD
            // THEM. The mode selector takes a fixed 34% of the row and the buttons take
            // their own widths out of the rest, left to right - so the LAST of them gets
            // whatever survives. At the 750x550 floor that is 26 px for "Add to Bank",
            // which needs 91: a fork glyph and about fifteen pixels of a word, condensed
            // to 70% by a painter that never clips. Every bounds check green, and a button
            // whose label cannot be read is a button you press to find out what it does.
            //
            // Measured against the words in the language on screen. When they do not fit,
            // the buttons drop to a row of their own and the mode selector keeps the first
            // row (which also gives the scale NAME the leftover it needs to stay inline).
            // The row costs 27 px, and the tuning panel's own ladder decides what pays for
            // it - the bank row first, by the order it already states.
            const auto btnW = [] (const juce::TextButton& b)
            { return fgfit::textButtonWidth (b.getButtonText(), 24, b.getProperties().contains ("fork")); };
            const int btnNeed = btnW (loadSclButton) + 4 + btnW (saveTuningBtn) + 4
                              + btnW (tuningBankAddBtn);
            const bool tooTightForButtons = fgfit::snugComboWidth (tuningBox, 24) + 5 + btnNeed
                                              > mrow.getWidth();
            // ...but only if the panel can PAY for that row. Everything in this panel is
            // on one ladder, and a row taken here is a row the ladder below takes from the
            // bank and then from the pitch-bend selector. A legible "Add to Bank" is not
            // worth the control that says how far a note bends, so when the extra row
            // cannot be afforded the buttons stay on the mode row and the ones that do not
            // fit are HIDDEN - Add to Bank first, then Save. Load never goes: it is the
            // only route to a scale file, and a synth with no way to load a tuning is not
            // a microtonal synth.
            const bool canAffordBtnRow = ti.getHeight() - 24 - 27 >= need;
            const bool btnOwnRow = tooTightForButtons && canAffordBtnRow;
            // THE MODE BOX STATES WHAT ITS WIDEST ITEM NEEDS, and only falls back to a
            // share of the row when that is the larger of the two. A flat 34% is a
            // percentage deciding what a word may be: "Scala (.scl)" needs 74 px and the
            // share handed it 73, which is what the rig's legibility walk named as the
            // binding control for the whole window in BOTH languages.
            //
            // Nothing is taken from the buttons that they are not already prepared to
            // lose: they measure their own words and hide, in a stated order, when the row
            // cannot hold them (see the ladder below). A percentage cannot do that.
            // ...and when the row cannot hold BOTH, the box gives up the comfort before
            // Load gives up its word.
            //
            // Load is the one control on this row that can neither be hidden (it is the
            // only route to a scale file) nor be crushed - and it was getting 30 px for a
            // word that needs 32.9, the exact smear this panel's ladder exists to prevent.
            // The box was taking 174 px at EVERY size, because its widest item is
            // "MTS-ESP..." and sizing to the widest item is what stops it resizing under
            // the user's hand as they change modes. That is comfort. At a width where the
            // row cannot pay for it, showing the item that is CURRENTLY SELECTED is the
            // part that carries meaning, so the box falls back to that and no further -
            // never below its own selected word, and Load's width is reserved first.
            //
            // The trade is stated rather than silent: at a narrow window the box resizes
            // when the mode changes. A resizing box is a smaller price than a button
            // nobody can read, and it is only paid where the alternative is that button.
            const int loadReserve = btnOwnRow ? 0 : 5 + btnW (loadSclButton);
            const int modeIdeal   = fgfit::snugComboWidth (tuningBox, 24);
            const int modeShare   = btnOwnRow ? 0 : mrow.getWidth() * 34 / 100;
            const int modeAsk     = juce::jmax (modeIdeal, modeShare);
            const int modeFloor   = fgfit::snugComboWidthForSelection (tuningBox, 24);
            compact (tuningBox, mrow.removeFromLeft (juce::jmin (mrow.getWidth(),
                                    juce::jmax (juce::jmin (modeAsk, mrow.getWidth() - loadReserve),
                                                modeFloor))));
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
                auto brow = mrow;
                if (btnOwnRow) { ti.removeFromTop (3); brow = ti.removeFromTop (24); }
                const auto fit = [&brow] (juce::TextButton& b, bool mayHide)
                {
                    // GalacticLnF's own width rule, in the font the button really draws
                    // in: measured here with a plain 12 pt while the LookAndFeel painted
                    // ~15 pt bold, "Afegeix al banc" came out 24 px too narrow and was
                    // silently squashed.
                    const int w = fgfit::textButtonWidth (b.getButtonText(), brow.getHeight(),
                                                          b.getProperties().contains ("fork"));
                    // A button that cannot show its word is HIDDEN, not squashed: the
                    // painter never clips, it condenses to 70%, so the crushed state is a
                    // fork with an unreadable smear beside it and no sign anything is
                    // wrong. Load is never hidden - see above.
                    if (mayHide && w > brow.getWidth()) { b.setVisible (false); return; }
                    b.setBounds (brow.removeFromLeft (juce::jmin (w, juce::jmax (0, brow.getWidth()))));
                    brow.removeFromLeft (4);
                };
                fit (loadSclButton,    false);
                fit (saveTuningBtn,    true);
                fit (tuningBankAddBtn, true);
                if (! btnOwnRow) mrow = brow;   // the name reads what the buttons left
            }
            // THE SCALE NAME IS THE IDENTITY OF THE LOADED TUNING, so it must not be
            // the scrap left over by everything else on its row. Sized as whatever
            // `mrow` still held after the mode selector and the three buttons, it came
            // out 21 px wide in Catalan while "12ED2" measures 39, and a juce::Label
            // does not clip when it is short of room - it CONDENSES the glyphs to 75%,
            // so the panel drew a squashed name and every bounds check in the build
            // stayed green. Nothing about that is Catalan-specific: this text is a FILE
            // NAME, its length is unbounded, and a long .scl starves the same cell in
            // any language - Catalan's wider buttons only cross the threshold first.
            // So the name stays inline only while the leftover genuinely holds it, and
            // takes a row of its own the moment it does not.
            // ONE GAP BETWEEN THE ROWS OF THIS PANEL.
            //
            // Every row here is taken off the top with its own leading, and two of the five
            // said 2 while the other three said 3 - which, once the panel's spare height is
            // shared out over the gaps (see `lead` below), photographed as 2, 6, 7, 7, 9 px
            // down one narrow column. Nothing was wrong with any single number; there were
            // simply two of them, plus the remainder of an integer division landing in the
            // last gap. The three places that state the ladder's arithmetic (`fixed`,
            // `wants` and the row placements themselves) now all read this, so the gap can
            // never be two different sizes again - and the same value in all three is what
            // keeps the affordability ladder honest about what it is reserving.
            const int kTunRowGap = 3;
            const int nameNeed = tuningNameLabel.getText().isEmpty()
                               ? 0 : fgfit::labelWidth (tuningNameLabel);
            // WHEN THE PANEL IS SHORT, THE NAME AND THE PERIOD SHARE ONE ROW.
            //
            // Both are single-line READ-OUTS, and at the 870x540 floor the panel is
            // ~22 px short of its rows: the name takes a whole row of its own (the
            // leftover beside the mode selector is 13 px), the period takes another, and
            // what runs out at the far end is the row that carries the pitch-bend
            // selector and Recent - two real controls, laid out at 12 px in the plain
            // case and at exactly 0 with a periodic tuning, where the Shepard rise
            // dropdown takes one row more.
            //
            // Merging the two read-outs frees exactly the row those controls need, and
            // costs nothing: "tritave" is 40 px and "Period  3" is 52 in a 289 px panel,
            // so side by side they are not even close to touching. It happens ONLY when
            // the panel cannot afford both rows, so every size that fits today - the
            // shipped 1200x650 included - is laid out exactly as it was.
            //
            // Not merged when the period row carries CONTROLS (an absolute 128-key table
            // puts the override field and its reset button there): that row is not a
            // read-out then, and squeezing a name in beside an editor is how the panel
            // got into this state in the first place.
            const bool nameOwnRow  = nameNeed > mrow.getWidth() && ti.getHeight() >= 22;
            const int  periodNeed  = fgfit::labelWidth (tuningPeriodLbl);
            // The width the shared row would really need. An absolute 128-key table puts
            // the period OVERRIDE field (72) and, when the user has set one, its reset
            // button (46) on that row - so the merge is not refused just because the row
            // carries controls, it is refused when the four things together do not fit.
            // Refusing on the presence of the controls alone cost the tuning BANK its row
            // at the shipped size, for a row that had 150 px to spare.
            const bool periodIsTable = processorRef.tuningIsAbsoluteTable();
            const int  periodExtra   = periodIsTable
                                     ? (72 + 5 + (processorRef.tuningTablePeriodIsUserSet() ? 46 + 4 : 0))
                                     : 0;
            const bool mergeNamePeriod = nameOwnRow
                                       && nameNeed + 8 + periodNeed + periodExtra <= ti.getWidth()
                                       && ti.getHeight() - 22 < need;
            if (nameOwnRow && ! mergeNamePeriod)
            {
                ti.removeFromTop (kTunRowGap);
                tuningNameLabel.setBounds (ti.removeFromTop (20));
            }
            else if (! mergeNamePeriod)
                tuningNameLabel.setBounds (mrow);
            // (merged: the name is placed by the period row below, which is the row it
            // now shares - setting bounds here would put it in the 13 px leftover.)

            //== THE SLACK IS SHARED OUT, NOT POOLED ================================
            // Every row below is a fixed height taken off the top, and the Shepard block
            // was reserved off the bottom, so whatever the panel has spare ended up as
            // ONE void in the middle: 168 px at 1600x900 against 43 at 1200x650, with
            // the Shepard row orphaned against the bottom edge and the rows above
            // huddled under the title. It is the same fault as a paragraph set with all
            // its leading at the end.
            //
            // So the spare height is divided evenly over the gaps that are still to
            // come - one before each remaining row, plus the one that is left between
            // the last row and the Shepard block - and each of those gaps grows by that
            // share. Nothing stretches: the rows keep their heights and their controls,
            // only the air between them changes, which is why no string can start
            // clipping because of this.
            //
            // The three optional "load this tuning from..." buttons are decided HERE
            // rather than at the row that draws them, because the count of them is what
            // makes the division right - measured after the fact it would be a guess.
            // (hasPatch / hasPresetTuning / hasProvTuning / nSnug / need are decided
            // further up, where whether the name can afford a row of its own depends on
            // them - see mergeNamePeriod.)
            //
            // WHAT THE PANEL CAN AFFORD IS DECIDED BEFORE ANYTHING IS PLACED, in a stated
            // order and on a ladder: a row hands back its spare height first, and is only
            // DROPPED when even its minimum will not fit. The row that used to give way
            // was whichever one the top-down walk reached when the height ran out, which
            // is not an order but an accident - and it produced both faults this panel had
            // at the floor: a bend row laid out at 0 px (a control nobody can click, and
            // nothing on screen saying why), and a bank row that reserved its 27 px, found
            // it could not use them, hid itself, and left the space behind as a 15 px void
            // above the Shepard block.
            //
            // The bank gives way first - its combo is reachable from the Load button and
            // the Recent list, so the panel loses a shortcut rather than a capability -
            // and the bend row second. 14 px is the least a row of controls can be and
            // still be one; it is the threshold the bank row already used for itself.
            const int  kRowMin = 14;
            int  bendH = 22, bankH = 24;
            bool bendRow = true, bankRow = true;
            {
                // Period and the optional "load from..." rows are not on the ladder: the
                // period read-out is what the panel is FOR, and each snug row exists only
                // because the state it describes is live right now.
                const int fixed = kTunRowGap + 18 + nSnug * (kTunRowGap + 22);
                int over = fixed + (kTunRowGap + bendH) + (kTunRowGap + bankH) - ti.getHeight();
                if (over > 0) { const int g = juce::jmin (over, bankH - kRowMin); bankH -= g; over -= g; }
                if (over > 0) { bankRow = false; over -= kTunRowGap + bankH; }
                if (over > 0) { const int g = juce::jmin (over, bendH - kRowMin); bendH -= g; over -= g; }
                if (over > 0) { bendRow = false; }
            }
            // What the surviving rows will actually occupy...
            const int  wants = kTunRowGap + 18 + nSnug * (kTunRowGap + 22)
                             + (bendRow ? kTunRowGap + bendH : 0)
                             + (bankRow ? kTunRowGap + bankH : 0);
            // ...over the gaps that can carry the rest: one before each row that survived,
            // plus the one left between the last of them and the Shepard block. A dropped
            // row hands its space back here, which is the whole point of `lead` and what a
            // row hiding itself AFTER the division was computed quietly broke.
            const int  gapCount = 1 + nSnug + (bendRow ? 1 : 0) + (bankRow ? 1 : 0) + 1;
            const int  lead  = juce::jmax (0, ti.getHeight() - wants) / gapCount;

            // Period of the active scale, in the panel's own lilac, always visible. For an
            // absolute 128-key
            // table it is Fibló's ESTIMATE, so the same row also carries the field that
            // overrides it (and a reset back to the estimate) - the readout alone would
            // state a guess as if the tuning had declared it.
            ti.removeFromTop (kTunRowGap + lead);
            {
                auto prow = ti.removeFromTop (18);
                // The name rides on the LEFT of this row when the two were merged (see
                // mergeNamePeriod): it keeps exactly the width its own text needs, and
                // the period read-out takes the rest, so a long name can never push the
                // period out - the merge only happens when both were measured to fit.
                if (mergeNamePeriod)
                {
                    tuningNameLabel.setBounds (prow.removeFromLeft (nameNeed));
                    prow.removeFromLeft (8);
                }
                const bool isTable = periodIsTable;
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
            // The three "load this tuning from somewhere" buttons each get a row, and in
            // that row exactly the width their own sentence needs - not the panel's whole
            // width. "Load spectrum's tuning" was a 420 px slab for 22 characters. Same
            // padding rule as every other narrowed control (fgfit::kSnugPad), and all
            // three share ONE helper so a rename cannot widen one and not its neighbours.
            auto snugRow = [this, lead, kTunRowGap] (juce::Rectangle<int>& ti, juce::TextButton& b)
            {
                ti.removeFromTop (kTunRowGap + lead);   // its share of the panel's spare height
                auto row = ti.removeFromTop (22).reduced (2, 0);
                b.setBounds (row.removeFromLeft (
                    juce::jmin (fgfit::snugTextButtonWidth (b, 22), row.getWidth())));
            };
            // Only for scales this synth wrote: they carry their own patch.
            // (hasPatch / hasPresetTuning / hasProvTuning were decided above, where their
            // count is what divides the spare height.)
            loadItsSpectrumBtn.setVisible (hasPatch);
            if (hasPatch) snugRow (ti, loadItsSpectrumBtn);
            // Only when the preset just loaded carries a tuning that actually
            // differs from the one active now (recomputed live: loading it, or
            // changing the tuning some other way, makes the button disappear again) -
            // AND the preset itself has not drifted (a knob/automation/undo moved
            // something outside its own modulators): once it has drifted, "this
            // preset's tuning" is no longer an honest thing to offer.
            loadPresetTuningBtn.setVisible (hasPresetTuning);
            if (hasPresetTuning) snugRow (ti, loadPresetTuningBtn);
            // The custom spectrum's own tuning, whenever it HAS one (derived by Adapt,
            // or loaded from a scale file). Same idiom as the preset row above, but it
            // stays VISIBLE when that tuning is already active - disabled and saying so
            // (refreshSpectrumTuningButton) - because vanishing right after the click
            // would read as the button failing rather than the job being done.
            loadSpectrumTuningBtn.setVisible (hasProvTuning);
            if (hasProvTuning)
            {
                // The WORD first, then the width: this button says "Load spectrum's
                // tuning" or "Tuning already loaded" depending on state, and sizing it
                // before refreshSpectrumTuningButton() rewrote it would measure the
                // word that is about to be replaced.
                refreshSpectrumTuningButton();
                snugRow (ti, loadSpectrumTuningBtn);
            }
            // Pitch bend and Recent share ONE row now (bend left, Recent right): the
            // Shepard row at the bottom is paid for with exactly the row this merge
            // frees, so the bank underneath keeps the height it had. Bend is hidden
            // in MPE-input mode (the MPE zone fixes the per-note range at +-48, so
            // the choice is irrelevant there) - Recent then takes the whole row.
            const bool bendVis = tmode != 3;
            // ...AND on the row surviving the ladder above. Set here, outside the block
            // that lays the row out: a row that is skipped never reaches the setVisible
            // inside it, so the two controls stayed VISIBLE carrying the bounds the LAST
            // window size gave them - drawn at 1200x650 coordinates over the keyboard of
            // an 870x540 window. Every check in both rigs stayed green through that,
            // because they were inside the window and neither collapsed.
            lblBend.setVisible (bendVis && bendRow); bendBox.setVisible (bendVis && bendRow);
            // ONE LABEL COLUMN for this panel. "Pitch bend" reserved 74 px and "Bank" 38,
            // so two adjacent rows started their controls 36 px apart - the kind of drift
            // that has no name on screen and just makes the panel look slightly wrong.
            // Sized to the WIDER of the two words in the language actually on screen
            // (fgfit::labelWidth, because a juce::Label draws inside its border in a bold
            // face and a cell measured any other way condenses the glyphs silently), with
            // 74 as the floor so nothing that fitted before can stop fitting.
            const int tunLblW = juce::jmax (74, fgfit::labelWidth (lblBend),
                                                fgfit::labelWidth (lblTuningBank));
            // Hoisted out of the row block below because Recent's visibility is set
            // further down (with the bank UI), after that block has closed.
            // Hoisted out of the row block below because Recent's visibility is set
            // further down (with the bank UI), after that block has closed. The plan
            // above already decided whether this row exists at all.
            bool bendRowFits = bendRow;
            bool recentFits  = true;
            if (bendRow)
            {
                ti.removeFromTop (kTunRowGap + lead);   // its share of the panel's spare height
                auto bendRowRect = ti.removeFromTop (bendH);
                if (bendVis)
                {
                    lblBend.setJustificationType (juce::Justification::centredLeft);
                    lblBend.setBounds (bendRowRect.removeFromLeft (tunLblW));
                    compact (bendBox, bendRowRect.removeFromLeft (juce::jmin (120, bendRowRect.getWidth())));
                    bendRowRect.removeFromLeft (6);
                }
                // Recent is ONE button that opens a floating list of every file loaded;
                // the bank stays inline. Both stay reachable in every mode - picking one
                // of them IS how you get into Scala mode, so gating them on already
                // being there was a dead end. It also keeps "+ to bank" from ever
                // feeding a list you cannot see.
                // HONEST FAILURE, the same rule the bank row below already applies: at
                // the 870x540 floor this panel is over-subscribed, and this row is the one
                // the shortage reaches first (the Shepard block is reserved off the bottom,
                // so what runs out is the middle). It used to lay out anyway - 12 px in the
                // plain case, and exactly 0 px with a periodic tuning, where the rise
                // dropdown takes one more row. A VISIBLE control with no pixels is worse
                // than an absent one: it cannot be clicked, it cannot be seen, and nothing
                // on screen says the panel ran out of room. 14 px is the bank row's own
                // threshold, kept identical so the two rows cannot drift apart.
                // Belt and braces: the plan says this row was afforded, so it must have
                // its 22 px. If arithmetic ever says otherwise, hide rather than ghost.
                bendRowFits = bendRowRect.getHeight() >= 14;
                lblBend.setVisible (bendVis && bendRowFits);
                bendBox.setVisible (bendVis && bendRowFits);
                auto rec = bendRowRect.reduced (2, 0);
                // Same rule as the fork buttons above: a button that cannot show its word
                // is hidden, not crushed. Recent takes whatever the bend selector left, so
                // at the floor it was 19 px against the 56 "Recent..." needs - a smear no
                // one can read on a control whose whole job is to be recognised. Every
                // file it lists is still reachable through Load.
                const int recNeed = fgfit::snugTextButtonWidth (recentOpenBtn, 22);
                recentFits = recNeed <= rec.getWidth();
                if (recentFits)
                    recentOpenBtn.setBounds (rec.removeFromLeft (recNeed));
            }
            recentLabel.setVisible (false);
            for (int i = 0; i < 3; ++i) recentBtn[i].setVisible (false);
            const bool bankUi = bankRow;
            // Recent lives in the BEND row, not the bank row - it follows that row's fate.
            // (It was tied to `bankUi` back when that was unconditionally true, which only
            // became a bug the moment the bank row could be dropped.)
            recentOpenBtn.setVisible (bendRowFits && recentFits);
            lblTuningBank.setVisible (bankUi); tuningBankBox.setVisible (bankUi);
            tuningBankLoadBtn.setVisible (bankUi);
            tuningBankClearBtn.setVisible (bankUi);
            if (bankUi)
            {
                ti.removeFromTop (kTunRowGap + lead);   // its share of the panel's spare height
                // Bank row: the label rides on the LEFT of the row (a separate label
                // row no longer fits) - [BANK][combo (flex)][+ .scl][X remove].
                // Whatever is still under it - one share of the slack, by construction -
                // is the gap up to the Shepard block, so that gap matches the others
                // instead of being the whole void.
                auto bkrow = ti.removeFromTop (juce::jmax (0, juce::jmin (bankH, ti.getHeight())));
                // (the plan above already decided this row is affordable; the `fits`
                // check below stays as belt and braces, not as the decision)
                lblTuningBank.setJustificationType (juce::Justification::centredLeft);
                lblTuningBank.setBounds (bkrow.removeFromLeft (tunLblW));
                // TWO BUTTONS IN ONE ROW ARE ONE HEIGHT. This was `.reduced (1, 1)` while
                // "+ .scl" beside it kept `.reduced (1, 0)`, so the row ended with a 24 px
                // button and a 22 px one whose top edge sat 1 px lower - x 902-949 y
                // 416-439 against x 955-976 y 417-438. Nothing was clipped and no check
                // could see it; it just reads as two buttons that were placed on different
                // days. The 1 px sideways stays (it is the air between them); the vertical
                // inset goes, so both fill the row.
                tuningBankClearBtn.setBounds (bkrow.removeFromRight (24).reduced (1, 0));
                bkrow.removeFromRight (3);
                tuningBankLoadBtn.setBounds (bkrow.removeFromRight (juce::jmax (46,
                                                 fgfit::textButtonWidth (tuningBankLoadBtn.getButtonText(), 24, false) + 2))
                                             .reduced (1, 0));
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
                // fgfit::snugComboWidth, the same rule (and the same padding) the six
                // narrowed controls elsewhere in this window use: this row measured its
                // own widest item before that rule existed, and two copies of one
                // measurement are how a padding "system" quietly stops being one.
                auto needFor = [shepH] (juce::ComboBox& c, int nominal)
                { return juce::jmax (nominal, fgfit::snugComboWidth (c, shepH)); };

                auto sh = shepRow;
                // 46 px is the least a beats box ("1/16") or a millisecond fader can be
                // and still be one. Hoisted above the toggle because the toggle now has
                // to know what the rest of the row costs before it decides its own width.
                // 46 px WAS a judgement - "the least a beats box or a millisecond fader
                // can be and still be one" - and it was the last hand-picked number on
                // this row. At the floor the beats box got exactly those 46, which is
                // 18 px of text room, and its widest item "1/16" needs 26: the box was
                // truncating in silence, and no check saw it because the probe measures
                // the SELECTED item and "4" fits anywhere.
                //
                // Measured now, and measured on the SELECTION rather than the widest item,
                // because a closed ComboBox only ever draws the one that is chosen - the
                // widest is what the box ASKS for, so it does not resize as the user
                // changes it, and the selection is what it must never go below. The ms
                // fader's read-out is the same question in its other dress, and the larger
                // of the two is taken so the reservation does not depend on which dress is
                // showing. Floored at 24, the least any control can be clicked.
                const int kRateMin = juce::jmax (24,
                                        juce::jmax (fgfit::snugComboWidthForSelection (shepDivBox, shepH),
                                                    fgfit::faderValueWidth (shepTimeFader)));
                // THE TOGGLE WAS THE LAST CONTROL ON THIS ROW STILL ON A HAND-PICKED
                // NUMBER. Every other box here is sized from the words it holds; this one
                // took 86 px whatever "Shepard" actually needs, and at a narrow window
                // those spare pixels were the ones the mode box was 4.5 short of.
                //
                // It keeps its 86 whenever the row can PAY for 86 + the mode box's own
                // ask + the rate minimum - so the shipped size is pixel-identical - and
                // falls back to what its word needs only when the row cannot. The same
                // affordability ladder the TUNING panel and the fork buttons use: a
                // control gives up its comfort before another gives up its meaning.
                const int roomyToggle = 86;
                // 24 px square: the arrow's own size, on the tightest row in this panel.
                // 24 px square when the row can pay for it, 18 when it cannot - the arrow
                // gives up its comfort before the mode box gives up its word, which is the
                // ladder this row already runs on. Measured: the arrow and its gap cost the
                // mode box 28 px, and at the tight end that took "Project Tempo" from the
                // 73.8 px it needs down to 69 - the rig's fully-legible width went 780 to
                // 840 in one commit. Six pixels back off the arrow is all it took, and an
                // 18 px square is still a comfortable target for a single glyph.
                const int roomyDirW = 24, tightDirW = 18;
                const int wordToggle  = fgfit::toggleWidth (shepOnT.getButtonText(), shepH);
                const int rowCanPay   = roomyToggle + 4 + roomyDirW + 4 + needFor (shepModeBox, 94) + 6 + kRateMin;
                const bool roomy      = sh.getWidth() >= rowCanPay;
                const int kDirW       = roomy ? roomyDirW : tightDirW;
                shepOnT.setBounds (sh.removeFromLeft (roomy
                                                          ? roomyToggle
                                                          : juce::jmax (wordToggle, 1)));
                sh.removeFromLeft (4);
                // THE DIRECTION ARROW, RESERVED BEFORE THE MODE BOX like the rate control
                // below it, and for the same reason: it is 24 px and it is the difference
                // between a Shepard that rises and one that falls, so it must not be what
                // the row spends last. Square, because the glyph is - and because a row
                // this tightly measured has no pixels for a button that is wider than the
                // thing inside it.
                shepDirBtn.setBounds (sh.removeFromLeft (juce::jmin (kDirW, sh.getWidth()))
                                          .withSizeKeepingCentre (juce::jmin (kDirW, sh.getWidth()), shepH));
                sh.removeFromLeft (4);
                // THE RATE CONTROL'S MINIMUM IS RESERVED BEFORE THE MODE BOX IS SIZED.
                //
                // The mode box took whatever its widest item asked for and the rate
                // control got the remainder - which at the 870x540 floor is FOUR PIXELS.
                // A 4 px ComboBox has negative room once its chevron and insets are taken
                // off: nothing to read, nothing to click, and the rig counted it as a text
                // problem because the box was inside its row and had a positive width.
                // The mode names the control; it cannot be allowed to eat it.
                //
                // 46 px is the least a beats box ("1/16") or a millisecond fader can be
                // and still be one, and the mode box keeps everything above that.
                const bool sync = shepModeBox.getSelectedId() == 2;

                const int  modeRoom = juce::jmax (46, sh.getWidth() - 6 - kRateMin);
                compact (shepModeBox, sh.removeFromLeft (juce::jmin (needFor (shepModeBox, 94),
                                                                     juce::jmin (modeRoom, sh.getWidth()))));
                sh.removeFromLeft (6);
                // The rise interval keeps its own line (see riseRow above), right under
                // the mode box it belongs to, so the rate control keeps a usable width.
                shepRiseBox.setVisible (riseChoice);
                if (riseChoice)
                {
                    auto rr = riseRow.withTrimmedLeft (86 + 4);
                    compact (shepRiseBox, rr.removeFromLeft (juce::jmin (needFor (shepRiseBox, 108), rr.getWidth())));
                }
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
    // A VERTICAL FADER TOO NARROW FOR ITS NUMBER SHOWS NO NUMBER.
    //
    // Slider::resized() clamps the value box to the slider's own width, so a fader in a
    // squeezed column keeps a box it cannot fill: the AMP DECAY fader is 11 px wide at
    // the 700x550 floor, which leaves its label ONE pixel once juce::Label's own border
    // is taken off, and "1.000" is then drawn condensed into nothing. The box also costs
    // the fader 15 px of TRACK to say it - so the crushed state is worse in both
    // directions at once.
    //
    // This is not a new rule: the PHASE fader beside it already drops its read-out below
    // 96 px for exactly this reason, with the ends named in its tooltip. This applies the
    // same rule to every fader configFader() built, from the same measurement.
    //
    // Run at the END of resized(), because that is the first moment every fader has its
    // final bounds - and only when the answer CHANGES, since setTextBoxStyle rebuilds the
    // label. Horizontal faders are left alone: they carry their box to the RIGHT, where
    // the rule is a different one and its owner already applies it.
    for (auto* s : configuredFaders)
    {
        if (s->getSliderStyle() != juce::Slider::LinearVertical) continue;
        const int  want  = juce::jmax (46, fgfit::faderValueWidth (*s) + 2);
        const bool fits  = s->getWidth() >= fgfit::faderValueWidth (*s);
        const auto style = fits ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox;
        if (s->getTextBoxPosition() != style)
            s->setTextBoxStyle (style, false, want, 15);
    }
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
    // THROUGH THE PROCESSOR, never straight at the undo manager: a bare Ctrl+Z here had
    // no edit-mode gate, so inside a corner-edit session it unwound the LIVE patch's
    // history into the scratch surface, the next pull absorbed that into the corner, and
    // the "*" then blamed the user for a change they never made. performUndo() refuses
    // while a corner is being edited. The key is still consumed either way - it is being
    // deliberately swallowed, not passed on to something else to act on.
    if (key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier, 0)) { processorRef.performUndo(); return true; }
    if (key == juce::KeyPress ('y', juce::ModifierKeys::ctrlModifier, 0)) { processorRef.performRedo(); return true; }
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
    applyTransitionMode();   // the TRANSITION box follows the two booleans, however they moved
    refreshShepardSyncState(); // beats box goes amber while Sync has no BPM to sync to
    refreshShepardRiseState(); // ...and the switch says why the rise choice is missing
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
        // THE STAMP IS CHECKED EVERY TICK, not only when a new generation lands.
        //
        // A row's chips are degree INDICES into the scale the worker ran over, so once
        // the tuning moves they name degrees the scale in force does not have - "en una
        // pentatonica me dice 7 y 11". That check used to live INSIDE the generation
        // gate, which meant the stale rows stayed on screen for as long as no new result
        // arrived. That was invisible while every view of this list recomputed
        // continuously; it stopped being invisible the moment one of those views was
        // removed, and the verifier caught it as 12-TET's chip 11 under a five-note
        // scale. Clearing here costs one integer compare a tick and cannot go stale.
        if (processorRef.getNadResultDegrees() != processorRef.getScaleDegrees()
            && ! nadList.rows.empty())
        {
            nadList.rows.clear();
            nadList.setSize (nadList.getWidth(), 0);
            nadList.repaint();
        }
        if (processorRef.getNadGeneration() != lastNadGen)
        {
            lastNadGen = processorRef.getNadGeneration();
            // Belt and braces behind the gate above: the list shows nothing rather than
            // something wrong, and the pending recompute fills it a tick later.
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
            // The chips read differently in a tuning with no period and the reason is not
            // on screen anywhere else: no "any" row, because a shape cannot stand for its
            // transpositions in a scale that never repeats, so every combination is listed
            // at the degrees it really uses. Said here rather than inferred from a longer
            // list, and never by printing an octave the scale does not have.
            else if (processorRef.nadHadNoPeriod() && ! nadList.rows.empty())
                txt = tr ("this scale states no period - shapes are listed where they really are",
                          "aquesta escala no declara cap per\xc3\xado" "de - les formes es llisten on realment s\xc3\xb3n");
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
                                  // Capitalised to match "Searching..." - the other half
                                  // of this same one-line read-out, which a reader sees
                                  // in the same place a second earlier.
                                  : chordRtOn ? tr ("Realtime: ", "Temps real: ")
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
        // ...and re-spell the degrees if the tuning moved under us. Cheap: it compares
        // one integer and only rebuilds when the EDO itself changed (see
        // refreshDegreeNames). Here rather than at every load site because the tuning can
        // change from six of them - a .scl, the bank, MTS, Init, undo, a preset.
        refreshDegreeNames();
    }

    const int  rendered  = processorRef.getPartialCount();
    const int  templ     = processorRef.getTemplatePartialCount();
    // Two ways the same sentence can be true: the TEMPLATE hit kMaxPartials on compile,
    // or a SHEPARD copy wanted more than its one kStride slot holds while the band's
    // bottom edge caught up with a density change. Both are caps that change what is
    // heard, so both wear the same label rather than one of them being silent.
    const bool truncated = processorRef.isSpectrumTruncated() || processorRef.isShepardSlotCapped();
    juce::String meter;
    if (templ == 0)
    {
        // Empty template = the Harmonics + prime rules (intersected) left nothing.
        meter = tr ("0 partials \xe2\x80\x94 the Harmonics / Prime filters leave nothing to sound",
                    "0 parcials \xe2\x80\x94 els filtres d'Harm\xc3\xb2nics / de primers no deixen sonar res");
        meterLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
    }
    else if (! processorRef.hasSoundedYet())
    {
        // THREE STATES, NOT TWO, AND THE COLOUR IS THE DIAGNOSIS.
        //
        // This line had one green and one amber. "0 partials audible" came out GREEN in
        // the one state where the instrument is mute - and in the SAME green on a
        // perfectly healthy cold instrument, photographed at 1600x900, because
        // renderedCount is only ever written by a sounding voice, so before the first
        // note the number is not a measurement at all (see SpectrumEngine::reportRendered).
        // One string and one colour cannot be right for both.
        //
        // So the cold state says what is true - nothing has sounded yet - in the grey this
        // window uses for "no information", and stops claiming a count it does not have.
        meter = tr ("not sounded yet", "encara no ha sonat")
              + "   \xe2\x80\x94   " + juce::String (templ) + tr (" in the full timbre", " al timbre complet");
        meterLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
    }
    else
    {
        meter = juce::String (rendered) + tr (" partials audible", " parcials audibles")
              + "   \xe2\x80\x94   " + juce::String (templ) + tr (" in the full timbre", " al timbre complet");
        // ...and once it HAS sounded, a count of zero is a real diagnosis and wears the
        // colour of one. Amber and not red: red in this plugin means TRUNCATED - the
        // engine removed something you asked for. This is "nothing is reaching your ears",
        // which one control puts right, and it wears the same amber as the overlay that
        // explains it. Two words and not a sentence: measured at the 690x550 floor the
        // Catalan line already comes to about 555 px in a 678 px label, and a juce::Label
        // that runs short does not clip, it CONDENSES - so the honest way to say more here
        // was to say less.
        if (truncated)          meter += tr ("   (safety cap)", "   (l\xc3\xadmit de seguretat)");
        else if (rendered == 0) meter += tr ("   \xe2\x80\x94   SILENT", "   \xe2\x80\x94   EN SILENCI");
        meterLabel.setColour (juce::Label::textColourId,
                              truncated     ? juce::Colours::orange
                            : rendered == 0 ? juce::Colour (0xffffb020)
                                            : juce::Colour (0xff7ff59a));
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
    // The NAME decides its own cell - resized() gives it a row of its own when the mode
    // row cannot hold it - and setText() does not lay anything out. Without this, loading
    // a scale whose name is longer than the last one leaves it in the old, too-narrow
    // cell, drawn condensed, until some unrelated relayout happens to come along.
    if (sname != lastTuningNameLaidOut) { lastTuningNameLaidOut = sname; resized(); }

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
                        | (shepardRiseChoiceExists()                 ? 1 << 8 : 0);
    if (packed != lastResetOffState) { lastResetOffState = packed; resized(); }
    refreshSpectrumTuningButton(); // cheap (sig-guarded): keeps word/enabled honest per tick

    // Both halves of PRIME CONTROL change what resized() lays out - the mode picks which
    // sub-controls exist, the switch dims them - and NEITHER change arrives through a
    // click when it comes from host automation, a preset load or an undo. Packed into
    // one int and compared, which is what the mode alone already did.
    const int mode = (int) (processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f)
                   | (processorRef.apvts.getRawParameterValue ("primeOn")->load() > 0.5f ? 2 : 0);
    if (mode != lastPrimeMode) { lastPrimeMode = mode; resized(); }

    if (processorRef.getDissGeneration() != lastDissGeneration)
    {
        lastDissGeneration = processorRef.getDissGeneration();
        refreshDegreeList();
    }

    // Live active-scale dissonance list (recomputes only when timbre/tuning change).
    updateScaleDissonance();

    // Period of the active scale (the lilac read-out in the Tuning box).
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
        // THE VALUE IS THE FILE'S OWN AND STAYS. This field already printed "1/2" for a
        // descending scale and "1" for a degenerate one - the .scl's real last line, and
        // the only place in the window that value is shown. What was missing is the
        // consequence: that line is NOT a repeat interval going up, which is why the
        // degrees list has no period row, why no combination is called rootless, why the
        // register band's unit is marked, and why the Shepard offers no rise-interval
        // choice. Printing the number and letting the user work out why four other
        // read-outs changed is the same silence one step earlier, so the field says it.
        if (! processorRef.tuningStatesPeriod())
            per += tr ("  (no repeat)", "  (no repeteix)");
        tuningPeriodLbl.setText (tr ("Period  ", "Per\xc3\xado" "de  ") + per, juce::dontSendNotification);
    }

    // WHAT THE MTS MASTER ACTUALLY SAID ABOUT ITS KEYBOARD MAPPING.
    //
    // "ESTOY USANDO 31ED2 Y LOS VALORES EN CENTS QUE ME SALEN EN LAS TECLAS NO TIENEN
    //  SENTIDO... EL SONIDO ESTABA BIEN, ERA LA REPRESENTACION EN CENTS, EL NUMERO DE
    //  TECLAS POR PERIODO ERA CORRECTO, 31."
    //
    // Sound right, key count right, cents wrong is a signature: the pitches come from
    // MTS_NoteToFrequency (which is right, or nothing would sound right) while the
    // DEGREE readouts assume degree d lives on MIDI key 60 + d. MTS-ESP does not promise
    // that - it has MTS_GetMapStartKey and MTS_GetRefKey precisely because a master says
    // where its mapping starts and which key is its 1/1 - and this plugin was calling
    // neither. This line prints what the master reports, so the assumption can be
    // checked against it instead of argued about.
    if (processorRef.getTuningMode() == 1)
    {
        const auto m = processorRef.mtsMapInfo();
        // "MTS" is a standard's name and "?" is punctuation - neither is a translation,
        // and wrapping them in a tr pair would invent two whose sides are identical,
        // which the source audit rightly refuses.
        const juce::String unknown ("?");
        juce::String s ("MTS  ");
        if (! m.hasMaster) s << tr ("no master", "sense m\xc3\xa0" "ster");
        else
        {
            s << (m.mapSize  > 0 ? juce::String (m.mapSize)  : unknown)
              << tr (" keys/period", " tecles/per\xc3\xado" "de")
              << tr (", from key ", ", des de la tecla ")
              << (m.mapStart >= 0 ? juce::String (m.mapStart) : unknown)
              << tr (", 1/1 on key ", ", 1/1 a la tecla ")
              << (m.refKey   >= 0 ? juce::String (m.refKey)   : unknown);
        }
        tuningPeriodLbl.setText (s, juce::dontSendNotification);
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
    if (lastEngineShown != ((int) processorRef.engineInForce() * 2
                            + (processorRef.timbreLoaded() ? 1 : 0)) * 2
                           + (processorRef.temperIsPossible() ? 1 : 0))
    {
        // applyEngineState() re-dresses the Load/Remove button itself: "is a spectrum
        // loaded" is half of the very state this comparison is watching, so the button's
        // word arrives with the rest of the panel instead of one tick behind it.
        applyEngineState();
    }
}
