#pragma once

#include <array>
#include <cmath>
#include <cstdint>

//==============================================================================
// THE COLOUR SYSTEM, AND THE ARITHMETIC THAT DECIDES IT.
//
// "haz que el color de spectrum vs tuning sea una mezcla del color de spectrum y del
//  color de tuning. haz que todas las demas relaciones de colores incluidas la que hay
//  entre spectrum y tuning sean lo mas contrastadas y diferentes posibles."
//
// Two rules that pull against each other on purpose:
//
//   (a) SPECTRUM VS TUNING IS DERIVED. Its colour is literally the mixture of the
//       Spectrum colour and the Tuning colour, computed here, never a third hex typed
//       in by hand - so it stays a mixture when either parent changes, and the panel
//       that belongs to both READS as belonging to both.
//
//   (b) EVERY OTHER PAIR IS AS FAR APART AS POSSIBLE, by MEASUREMENT. The palette lives
//       in OKLab (perceptually uniform: equal distances look equally different, which
//       is exactly what "as different as possible" has to mean), the pairwise distances
//       are computed, and the set maximises the SMALLEST of them subject to every
//       colour staying legible on the page.
//
// Rule (b) is what makes rule (a) work rather than fighting it: Spectrum-vs-Tuning is
// pinned to the midpoint of Spectrum and Tuning, so the further apart those two are,
// the further their midpoint is from both of them. Pushing the parents apart IS how the
// derived colour earns its own identity. Where the two rules genuinely conflict, rule
// (a) wins - the blend must stay visibly between its parents.
//
// Everything here is deliberately free of JUCE, so the same code that DEFINES the
// palette can be compiled into a test that MEASURES it. A palette whose separation is
// asserted by a different implementation than the one that produced it is a palette
// with no guarantee at all.
//
// Colours are 0xRRGGBB. Alpha is the caller's business.
//==============================================================================
namespace galactic
{
namespace colour
{
    //== sRGB <-> linear ====================================================
    inline double srgbToLinear (double c) noexcept
    { return c <= 0.04045 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4); }
    inline double linearToSrgb (double c) noexcept
    {
        c = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow (c, 1.0 / 2.4) - 0.055;
    }

    struct Rgb { double r = 0.0, g = 0.0, b = 0.0; };   // linear, 0..1

    inline Rgb toLinear (std::uint32_t rgb) noexcept
    {
        return { srgbToLinear ((double) ((rgb >> 16) & 0xff) / 255.0),
                 srgbToLinear ((double) ((rgb >>  8) & 0xff) / 255.0),
                 srgbToLinear ((double) ( rgb        & 0xff) / 255.0) };
    }
    inline std::uint32_t fromLinear (const Rgb& c) noexcept
    {
        auto q = [] (double v)
        {
            const long n = std::lround (linearToSrgb (v) * 255.0);
            return (std::uint32_t) (n < 0 ? 0 : (n > 255 ? 255 : n));
        };
        return (q (c.r) << 16) | (q (c.g) << 8) | q (c.b);
    }
    // True when the linear triple is inside the sRGB cube - i.e. fromLinear would not
    // have had to clip it. An "optimised" colour outside the gamut is not a colour: it
    // lands on the cube's wall and takes its neighbours' distance with it.
    inline bool inGamut (const Rgb& c, double tol = 1.0e-9) noexcept
    {
        return c.r >= -tol && c.r <= 1.0 + tol && c.g >= -tol && c.g <= 1.0 + tol
            && c.b >= -tol && c.b <= 1.0 + tol;
    }

    //== OKLab (Björn Ottosson, 2020) =======================================
    // The perceptually uniform space the whole rule is stated in. CIELAB would also
    // serve; OKLab is chosen because its hue lines stay straight for saturated blues,
    // which is where CIELAB is worst and where half this palette lives.
    struct Lab { double L = 0.0, a = 0.0, b = 0.0; };

    inline Lab toOklab (const Rgb& c) noexcept
    {
        const double l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
        const double m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
        const double s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
        const double l_ = std::cbrt (l), m_ = std::cbrt (m), s_ = std::cbrt (s);
        return { 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
                 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
                 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_ };
    }
    inline Rgb fromOklab (const Lab& c) noexcept
    {
        const double l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
        const double m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
        const double s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
        const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
        return {  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                 -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                 -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s };
    }
    inline Lab toOklab (std::uint32_t rgb) noexcept { return toOklab (toLinear (rgb)); }

    // Perceptual distance, x100 so the numbers read like CIELAB's familiar ones
    // (OKLab's L runs 0..1, so a raw distance of 0.30 is a big difference).
    inline double deltaE (const Lab& x, const Lab& y) noexcept
    {
        const double dL = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
        return 100.0 * std::sqrt (dL * dL + da * da + db * db);
    }
    inline double deltaE (std::uint32_t x, std::uint32_t y) noexcept
    { return deltaE (toOklab (x), toOklab (y)); }

    // OKLCh - the same space in polar form. Chroma is "how much colour", hue is "which
    // colour"; both are needed as numbers because the palette's constraints are stated
    // in them (a colour with no chroma is the neutral steel this product already threw
    // out once, and it is invisible to any check phrased in a and b).
    inline double chroma (const Lab& c) noexcept { return std::sqrt (c.a * c.a + c.b * c.b); }
    inline double hueRad (const Lab& c) noexcept { return std::atan2 (c.b, c.a); }

    // THE MIXTURE (rule (a)).
    //
    // Interpolated in POLAR OKLCh - lightness and chroma straight, hue along the SHORT
    // arc - not along the straight line in (a, b). That line passes near the neutral
    // axis whenever the two parents are far apart in hue, and rule (b) is busy pushing
    // them exactly that far apart: mixed rectangularly, a saturated blue and a saturated
    // gold average to a grey-tan that reads as neither parent and as no colour at all -
    // which is the "neutral steel that read as white" this product already retired once.
    // Along the arc the mixture keeps its parents' chroma and lands on the hue between
    // them, which is what "a mixture of these two" means when you look at it.
    inline std::uint32_t mixOklch (std::uint32_t x, std::uint32_t y, double t = 0.5) noexcept
    {
        const Lab a = toOklab (x), b = toOklab (y);
        const double ca = chroma (a), cb = chroma (b);
        double h = hueRad (b) - hueRad (a);                     // shortest way round
        const double pi = 3.14159265358979323846;
        while (h >  pi) h -= 2.0 * pi;
        while (h < -pi) h += 2.0 * pi;
        const double L = a.L + (b.L - a.L) * t;
        const double C = ca + (cb - ca) * t;
        const double H = hueRad (a) + h * t;
        return fromLinear (fromOklab ({ L, C * std::cos (H), C * std::sin (H) }));
    }

    //== WCAG contrast, for legibility on the page ==========================
    inline double relLuminance (std::uint32_t rgb) noexcept
    {
        const Rgb c = toLinear (rgb);
        return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    }
    inline double contrastRatio (std::uint32_t x, std::uint32_t y) noexcept
    {
        const double a = relLuminance (x), b = relLuminance (y);
        return (std::max (a, b) + 0.05) / (std::min (a, b) + 0.05);
    }

    //== Dichromacy (Viénot, Brettel & Mollon 1999) =========================
    // Not decoration: half of this palette's separation is carried by hue, and the two
    // common dichromacies collapse whole regions of hue onto one line. A palette that
    // is "maximally different" only for trichromats is a palette that stops working for
    // roughly one man in twelve.
    enum class Vision { normal, deuteranopia, protanopia };

    inline std::uint32_t simulate (std::uint32_t rgb, Vision v) noexcept
    {
        if (v == Vision::normal) return rgb;
        const Rgb c = toLinear (rgb);
        double L = 17.8824    * c.r + 43.5161   * c.g + 4.11935  * c.b;
        double M =  3.45565   * c.r + 27.1554   * c.g + 3.86714  * c.b;
        double S =  0.0299566 * c.r +  0.184309 * c.g + 1.46709  * c.b;
        if (v == Vision::protanopia)   L = 2.02344  * M - 2.52581 * S;
        else                           M = 0.494207 * L + 1.24827 * S;   // deuteranopia
        return fromLinear ({  0.080944030   * L - 0.13050440   * M + 0.11672066   * S,
                             -0.010248500   * L + 0.05401940   * M - 0.11361540   * S,
                             -0.000365294   * L - 0.00412161   * M + 0.69351294   * S });
    }
}

//==============================================================================
// THE PALETTE ITSELF.
//
// Every section colour in the window, in one place, as plain numbers a test can read.
// They used to live in an anonymous namespace inside PluginEditor.cpp, where nothing
// could measure them - which is how cTuning (0xb27bff) and the CUSTOM modulator tab
// (0xff9a7bff) ended up 5 units of OKLab apart, two panels the eye cannot tell apart,
// with every check in the build green.
//==============================================================================
namespace palette
{
    // The page behind everything, and the interior fill of a macro panel. Legibility is
    // measured against the page: it is the darker of the two and therefore the harder.
    inline constexpr std::uint32_t kPage        = 0x0c0c12;
    inline constexpr std::uint32_t kPanelFill   = 0x17171f;

    //== THE IDENTITY COLOURS ==============================================
    // The set rule (b) covers: one colour per thing the user has to tell apart.
    // Chosen by maximising the minimum pairwise OKLab distance across the whole set
    // (see tools/PaletteSearch.cpp) subject to every one of them keeping a WCAG
    // contrast of at least 7:1 against the page, so each stays a legible border, title
    // chip and label. They are NOT hand-picked hues any more.
    inline constexpr std::uint32_t kHarmonic    = 0xa6a816; // SPECTRUM panel / HARMONIC engine
    inline constexpr std::uint32_t kTuning      = 0xb38bd2; // TUNING panel
    inline constexpr std::uint32_t kTimbre      = 0xffaad2; // CUSTOM engine
    inline constexpr std::uint32_t kSlope       = 0xdcf686; // BOTH ENGINES (Slope/Fractal/Morph/Range)
    inline constexpr std::uint32_t kFractal     = 0xf159ff; // the Fractal fader
    inline constexpr std::uint32_t kEssentials  = 0x6bff02; // Essentials panel
    inline constexpr std::uint32_t kEnv         = 0x4edf98; // AMP envelope tab
    inline constexpr std::uint32_t kPitch       = 0xfcb65c; // PITCH tab
    inline constexpr std::uint32_t kFilter      = 0x80fdff; // FILTER tab
    inline constexpr std::uint32_t kCustomMod   = 0x75c5ff; // CUSTOM modulator tab
    inline constexpr std::uint32_t kScaleDiss   = 0x29ad9c; // live scale-dissonance list

    // ...AND THE ONE THAT IS NOT CHOSEN AT ALL (rule (a)). The Spectrum-vs-Tuning column
    // belongs to both of the panels it sits between, so it wears the mixture of their
    // two colours - derived here, in OKLab, from the very constants above. There is no
    // hex for it on purpose: change either parent and this follows, which is the whole
    // point of the rule. The Spectrum panel's colour is the HARMONIC engine's, the one
    // it wears by default and the identity the SPECTRUM title chip carries.
    inline std::uint32_t specScale() noexcept
    { return colour::mixOklch (kHarmonic, kTuning, 0.5); }

    //== The set, named, for the checks =====================================
    struct Entry { const char* name; std::uint32_t rgb; };

    // 12 entries: the eleven chosen colours plus the derived blend. Anything that is
    // SEMANTIC rather than an identity is deliberately absent - Load is green, Reset is
    // red and Override is amber because of what they MEAN, and moving them to win a
    // distance contest would cost more than it bought.
    inline std::array<Entry, 12> identitySet() noexcept
    {
        return { { { "Spectrum/Harmonic", kHarmonic },
                   { "Custom engine",     kTimbre },
                   { "Both engines",      kSlope },
                   { "Fractal",           kFractal },
                   { "Essentials",        kEssentials },
                   { "Amp envelope",      kEnv },
                   { "Pitch",             kPitch },
                   { "Filter",            kFilter },
                   { "Custom modulator",  kCustomMod },
                   { "Tuning",            kTuning },
                   { "Scale dissonance",  kScaleDiss },
                   { "Spectrum vs Tuning", specScale() } } };
    }

    // The worst pair in the set, under a given vision. Returned rather than asserted so
    // the checks can print WHICH two collided - "the palette got worse" is useless
    // without the names.
    struct WorstPair { double distance = 1.0e9; const char* a = ""; const char* b = ""; };

    inline WorstPair worstPair (colour::Vision v = colour::Vision::normal) noexcept
    {
        const auto set = identitySet();
        WorstPair w;
        for (std::size_t i = 0; i < set.size(); ++i)
            for (std::size_t j = i + 1; j < set.size(); ++j)
            {
                const double d = colour::deltaE (colour::simulate (set[i].rgb, v),
                                                 colour::simulate (set[j].rgb, v));
                if (d < w.distance) { w.distance = d; w.a = set[i].name; w.b = set[j].name; }
            }
        return w;
    }

    // The least legible colour in the set against the page.
    inline WorstPair worstContrast() noexcept
    {
        WorstPair w;
        for (const auto& e : identitySet())
        {
            const double c = colour::contrastRatio (e.rgb, kPage);
            if (c < w.distance) { w.distance = c; w.a = e.name; w.b = "page"; }
        }
        return w;
    }
}
}
