#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

//==============================================================================
// NoteNames.h - "ups and downs" note spelling for an equal division of the octave.
//
// JUCE-FREE on purpose. It is pure arithmetic over one integer (the number of equal
// steps in the octave), it has its own test target (NoteNameTests), and staying free
// of JUCE is what lets the test be diffed line for line against the reference
// JavaScript it was ported from.
//
// NOTATION: ups and downs, devised by Kite Giedraitis.
//     https://en.xen.wiki/w/Kite%27s_ups_and_downs_notation
// SPELLING ALGORITHM: ported from "Notation" by Richie Greene (MIT licence).
//     https://github.com/richiegreene/notation
// GLYPHS: the strings returned here are ASCII in the alphabet of notes.otf by Sintel,
//     from tessera (MIT). Licence texts for both live beside the font, in
//     assets/fonts/NOTATION-LICENSES.txt.
//
// THE STRINGS ARE NOT PLAIN TEXT. In notes.otf 'c'/'d'/'g'/'h' are sharps, 'a'/'e'
// flats, 'v'/'w' a down/an up, '#' a double down, '/' and '\' a lift and a drop. Drawn
// in any other typeface a name reads as a random run of letters ("Cc" instead of C#),
// so ScaleKeyboard draws them in the embedded face and shows degree NUMBERS whenever
// it cannot - see ScaleKeyboard::setDegreeNamer.
//
// This spelling exists ONLY for a period of exactly 2/1. Bohlen-Pierce, any .scl with
// its own period, and any unequal scale get no names here: the letter row A..G *is*
// the diatonic scale of the octave, and pinning it to a period that is not an octave
// would be naming pitches after intervals they do not stand in.
//==============================================================================
namespace notenames
{

// The largest EDO this will spell at all. It bounds WORK, nothing the synth plays: a
// .scl with 20000 degrees would otherwise have this build 20000 strings, on the message
// thread, to answer a question about a keyboard 60 keys wide. Above it the keyboard
// shows degree numbers and the menu says the number out loud.
//
// It is NOT the legibility limit. Spellable EDOs well inside it produce names like
// "wwwwwwwwwwwwwwwwwwwwC" (21 characters, 488-EDO), which no 32 px key can show. That
// judgement belongs to whoever knows the key width, so ScaleKeyboard measures the widest
// name in the tuning against the narrowest key it draws and falls back to numbers - with
// a reason - when it does not fit. See ScaleKeyboard::namesFitTheKeys().
inline constexpr int kMaxEdo = 512;

//== the port ==================================================================
// Everything below mirrors edoNames() in the reference page one statement at a time,
// including the parts that look odd (upd()'s '#' for a double down is a LIGATURE in
// notes.otf, not a sharp). Where JavaScript and C++ disagree the JavaScript wins,
// because the fixture the test asserts against is that page's own output.

// JavaScript's Math.round: half goes UP (towards +infinity), not away from zero.
inline double jsRound (double x) { return std::floor (x + 0.5); }

// The reference's tr(): half rounds TOWARDS ZERO, everything else like Math.round.
// A plain std::round here spells the odd EDOs (17, 24, 31...) differently - the halves
// are exactly where the sharp side and the flat side of a step meet.
inline int roundHalfToZero (double x)
{
    const double f = std::floor (x);
    if (x - f == 0.5) return (int) (x > 0.0 ? f : std::ceil (x));
    return (int) jsRound (x);
}

// Floor-mod. C++ '%' truncates, so a negative degree would land outside the array.
inline int modFloor (int n, int m) { return ((n % m) + m) % m; }

inline std::string repeatChar (char c, int n)
{
    return n > 0 ? std::string ((size_t) n, c) : std::string();
}

struct DegreeSpelling
{
    // Sharp-side and flat-side spelling of one degree: nominal index (0=C..6=B),
    // count of sharps/flats, of ups/downs, and of lifts/drops.
    int sn = 0, s = 0, su = 0, sl = 0;
    int fn = 0, f = 0, fu = 0, fl = 0;
};

// The names of the `m` degrees of an m-EDO, degree 0 first, or an EMPTY vector when
// this EDO has no usable spelling. Empty happens for three honest reasons, all of them
// the reference's own: too few steps for a diatonic skeleton (m < 7, except 5), a fifth
// so flat that the chromatic step collapses, and a spelling that gives two degrees the
// same name (checked here, exactly as the page's upsDowns() does - a name that does not
// identify its degree is worse than a number).
inline std::vector<std::string> edoNames (int m)
{
    if (m < 7 && m != 5)   return {};
    if (m > kMaxEdo)       return {};

    const int p5 = (int) jsRound (std::log (1.5) / std::log (2.0) * (double) m);
    const int p2 = p5 * 2 - m;
    int a1 = ((m % 7 == 0 || m % 5 == 0) && m < 36) ? m : (p5 * 7 - m * 4);
    const int ud = 1;
    const bool penta  = ((double) p5 / (double) m > 0.6) || (m < 35 && m % 5 == 0);
    const bool halves = (a1 % 2 == 0);
    if (halves) a1 = (int) std::floor ((double) a1 / 2.0);

    // The fifth is so flat that the augmented unison vanishes. JavaScript would divide
    // by zero here and spell every degree "Infinity"; there is no spelling to give.
    if (a1 == 0) return {};

    std::vector<DegreeSpelling> N ((size_t) m);

    // The 5 (pentatonic) or 7 (diatonic) degrees that carry a bare nominal.
    {
        const int set[7][2] = { {0,0}, {p2,1}, {m-p5,3}, {p5,4}, {p5+p2,5},
                                {2*p2,2}, {p5+2*p2,6} };
        const int n = penta ? 5 : 7;
        for (int i = 0; i < n; ++i)
        {
            auto& o = N[(size_t) modFloor (set[i][0], m)];
            o.sn = set[i][1];
            o.fn = set[i][1];
        }
    }

    struct Chrom { int ra, ru, ldc; };
    // Sharp side and flat side of the same division: same number of augmented unisons,
    // opposite sign on the ups and on the leftover lift.
    const auto sc = [a1, ud] (int x) -> Chrom
    {
        const int ra  = roundHalfToZero ((double) x / (double) a1);
        const int ru  = roundHalfToZero ((double) ((ra * a1) - x) / (double) ud);
        const int ldc = (ru * ud) - ((ra * a1) - x);
        return { ra, -ru, ldc };
    };
    const auto fc = [a1, ud] (int x) -> Chrom
    {
        const int ra  = roundHalfToZero ((double) x / (double) a1);
        const int ru  = roundHalfToZero ((double) ((ra * a1) - x) / (double) ud);
        const int ldc = ((ra * a1) - x) - (ru * ud);
        return { ra, ru, ldc };
    };
    const auto ss = [] (DegreeSpelling& o, int nom, const Chrom& c)
    { o.sn = nom; o.s = c.ra; o.su = c.ru; o.sl = c.ldc; };
    const auto sf = [] (DegreeSpelling& o, int nom, const Chrom& c)
    { o.fn = nom; o.f = c.ra; o.fu = c.ru; o.fl = c.ldc; };

    int i;
    Chrom c {};
    for (i = 1; i < p2; ++i)
    {
        c = sc (i);
        ss (N[(size_t) modFloor (i, m)],         0, c);
        ss (N[(size_t) modFloor (m - p5 + i, m)], 3, c);
        ss (N[(size_t) modFloor (p5 + i, m)],     4, c);
    }
    if (penta)
    {
        for (i = 1; i < (m - p5) - p2; ++i)
        {
            c = sc (i);
            ss (N[(size_t) modFloor (p2 + i, m)],      1, c);
            ss (N[(size_t) modFloor (p5 + p2 + i, m)], 5, c);
        }
    }
    else
    {
        for (i = 1; i < p2; ++i)
        {
            c = sc (i);
            ss (N[(size_t) modFloor (p2 + i, m)],      1, c);
            ss (N[(size_t) modFloor (p5 + p2 + i, m)], 5, c);
        }
        for (i = 1; i < (m - p5) - (p2 * 2); ++i)
        {
            c = sc (i);
            ss (N[(size_t) modFloor (p2 * 2 + i, m)],      2, c);
            ss (N[(size_t) modFloor (p5 + p2 * 2 + i, m)], 6, c);
        }
    }
    for (i = 1; i < p2; ++i)
    {
        c = fc (i);
        sf (N[(size_t) modFloor (p2 - i, m)],      1, c);
        sf (N[(size_t) modFloor (p5 - i, m)],      4, c);
        sf (N[(size_t) modFloor (p5 + p2 - i, m)], 5, c);
    }
    if (penta)
    {
        for (i = 1; i < (m - p5) - p2; ++i)
        {
            c = fc (i);
            sf (N[(size_t) modFloor (m - i, m)],      0, c);
            sf (N[(size_t) modFloor (m - p5 - i, m)], 3, c);
        }
    }
    else
    {
        for (i = 1; i < (m - p5) - (p2 * 2); ++i)
        {
            c = fc (i);
            sf (N[(size_t) modFloor (m - i, m)],      0, c);
            sf (N[(size_t) modFloor (m - p5 - i, m)], 3, c);
        }
        for (i = 1; i < p2; ++i)
        {
            c = fc (i);
            sf (N[(size_t) modFloor (p2 * 2 - i, m)],      2, c);
            sf (N[(size_t) modFloor (p5 + p2 * 2 - i, m)], 6, c);
        }
    }

    // The nominals are stored in fifths order (F C G D A E B) but indexed in scale
    // order (0=C..6=B), which is what CC below untangles.
    static const char* const LET[7] = { "F", "C", "G", "D", "A", "E", "B" };
    static const int CC[7] = { 1, 3, 5, 0, 2, 4, 6 };
    const auto nom = [] (int x) { return std::string (LET[CC[modFloor (x, 7)]]); };
    const auto upd = [] (int u) -> std::string
    {
        if (u == -2) return "#";     // notes.otf's double-down ligature, NOT a sharp
        if (u ==  2) return "ww";
        return repeatChar ('v', -u) + repeatChar ('w', u);
    };
    const auto lft = [] (int l) { return repeatChar ('\\', -l) + repeatChar ('/', l); };
    const auto sh = [] (int s, bool h) -> std::string
    {
        std::string r;
        if (h)
        {
            if (s % 2 != 0) { if (s == 3) { r += "g"; s -= 3; } else { r += "h"; s -= 1; } }
            if (s > 0) { while (s >= 4) { r += "d"; s -= 4; } if (s >= 2) { r += "c"; s -= 2; } }
        }
        else
        {
            while (s >= 2) { r += "d"; s -= 2; }
            if (s == 1) r += "c";
        }
        return r;
    };
    const auto fl = [] (int s, bool h) -> std::string
    {
        std::string r;
        if (h)
        {
            if (s % 2 != 0) { if (s == 3) { r += "ea"; s -= 3; } else { r += "e"; s -= 1; } }
            if (s > 0) { while (s >= 4) { r += "aa"; s -= 4; } if (s >= 2) { r += "a"; s -= 2; } }
        }
        else
        {
            while (s >= 2) { r += "aa"; s -= 2; }
            if (s == 1) r += "a";
        }
        return r;
    };

    std::vector<std::string> out;
    out.reserve ((size_t) m);
    for (const auto& o : N)
    {
        const std::string sn = lft (o.sl) + upd (o.su) + nom (o.sn) + sh (o.s, halves);
        const std::string fn = lft (o.fl) + upd (o.fu) + nom (o.fn) + fl (o.f, halves);
        const int ts = std::abs (o.s) + std::abs (o.su) + std::abs (o.sl);
        const int tf = std::abs (o.f) + std::abs (o.fu) + std::abs (o.fl);
        // Fewest accidentals wins; on a tie, the shorter string; on that tie, sharps.
        if      (ts == 0 && tf != 0) out.push_back (sn);
        else if (tf == 0 && ts != 0) out.push_back (fn);
        else if (ts == 0 && tf == 0) out.push_back (sn);
        else if (ts < tf)            out.push_back (sn);
        else if (tf < ts)            out.push_back (fn);
        else                         out.push_back (sn.size() <= fn.size() ? sn : fn);
    }

    // A name that does not identify its degree is worse than the number it replaced.
    // This is the gate that rejects 9-EDO, 16-EDO, 23-EDO and the rest: they DO produce
    // strings, and the strings collide.
    for (size_t a = 0; a < out.size(); ++a)
        for (size_t b = a + 1; b < out.size(); ++b)
            if (out[a] == out[b])
                return {};

    return out;
}

// Does this EDO have a usable spelling at all? Same answer as edoNames(m).empty(),
// spelled out so callers reading like English do not have to build the vector.
inline bool haveNamesForEdo (int m) { return ! edoNames (m).empty(); }

// The longest name any EDO from 1..maxEdo produces, in characters. The keyboard's
// text-fit rule has to hold for the WIDEST name it can ever be asked to draw, and
// reconstructing that by eye from a table is exactly how a truncation ships.
inline size_t longestNameLength (int maxEdo)
{
    size_t longest = 0;
    for (int m = 1; m <= maxEdo; ++m)
        for (const auto& s : edoNames (m))
            longest = std::max (longest, s.size());
    return longest;
}

} // namespace notenames
