// Standalone unit tests for the ups-and-downs EDO spelling (no JUCE).
//
//   build\Release\NoteNameTests.exe   -> exit code 0 = all passed.
//
// THE FIXTURE BELOW IS NOT HAND-WRITTEN. It is the reference implementation's OWN
// output: the ported functions (lines 531-586 of the notation reference page, the
// JavaScript this header was translated from) were extracted verbatim, given three
// ES3 shims so Windows' own JScript engine would run them unmodified, and executed for
// m = 1..72. Every string here came out of that run. That is the whole point: a port
// asserted against names a human typed from memory tests the human, and the places
// this algorithm is easy to get wrong (round-half-TOWARDS-zero in tr(), the '#' that
// means a double DOWN, the pentatonic branch) are all places where a plausible-looking
// wrong answer is still a plausible-looking name.
//
// "NULL" is the reference returning no spelling at all - too few steps (1-4, 6), or a
// spelling that gives two degrees the same name (9, 16, 23).
#include "NoteNames.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;

static void check (bool cond, const char* what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (! cond) ++failures;
}

static std::string joined (const std::vector<std::string>& v)
{
    if (v.empty()) return "NULL";
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ' '; s += v[i]; }
    return s;
}

struct RefRow { int edo; const char* names; };

// Generated from the reference page's own JavaScript - see the note at the top.
static const RefRow kReference[] =
{
    { 1, "NULL" },
    { 2, "NULL" },
    { 3, "NULL" },
    { 4, "NULL" },
    { 5, "C D F G A" },
    { 6, "NULL" },
    { 7, "C D E F G A B" },
    { 8, "C wC D F wF G wG A" },
    { 9, "NULL" },
    { 10, "C wC D wD F wF G wG A wA" },
    { 11, "C D E Eh Fe F G A B Bh Ce" },
    { 12, "C Cc D Dc E F Fc G Gc A Ac B" },
    { 13, "C wC Ch D wD F wF Fh G wG Gh A wA" },
    { 14, "C wC D wD E wE F wF G wG A wA B wB" },
    { 15, "C wC vD D wD vF F wF vG G wG vA A wA vC" },
    { 16, "NULL" },
    { 17, "C Ch De D Dh Ee E F Fh Ge G Gh Ae A Ah Be B" },
    { 18, "C wC #D vD D wD vF F wF #G vG G wG #A vA A wA vC" },
    { 19, "C Cc Da D Dc Ea E Ec F Fc Ga G Gc Aa A Ac Ba B Bc" },
    { 20, "C wC #D vD D wD #F vF F wF #G vG G wG #A vA A wA #C vC" },
    { 21, "C wC vD D wD vE E wE vF F wF vG G wG vA A wA vB B wB vC" },
    { 22, "C wC vCc Cc D wD vDc Dc E F wF vFc Fc G wG vGc Gc A wA vAc Ac B" },
    { 23, "NULL" },
    { 24, "C Ch Cc De D Dh Dc Ee E Eh F Fh Fc Ge G Gh Gc Ae A Ah Ac Be B Bh" },
    { 25, "C wC wwC #D vD D wD wwD #F vF F wF wwF #G vG G wG wwG #A vA A wA wwA #C vC" },
    { 26, "C Cc Cd Da D Dc Dd Ea E Ec Fa F Fc Fd Ga G Gc Gd Aa A Ac Ad Ba B Bc Ca" },
    { 27, "C wC Ch De vD D wD Dh Ee vE E F wF Fh Ge vG G wG Gh Ae vA A wA Ah Be vB B" },
    { 28, "C wC #D vD D wD #E vE E wE #F vF F wF #G vG G wG #A vA A wA #B vB B wB #C vC" },
    { 29, "C wC Da Cc vD D wD Ea Dc vE E wE F wF Ga Fc vG G wG Aa Gc vA A wA Ba Ac vB B wB" },
    { 30, "C wC wwC wwwC #D vD D wD wwD wwwD #F vF F wF wwF wwwF #G vG G wG wwG wwwG #A vA A wA wwA wwwA #C vC" },
    { 31, "C Ch Cc Da De D Dh Dc Ea Ee E Eh Fe F Fh Fc Ga Ge G Gh Gc Aa Ae A Ah Ac Ba Be B Bh Ce" },
    { 32, "C wC wwC #Cc #D Cc D wD wwD #Dc #E Dc E F wF wwF #Fc #G Fc G wG wwG #Gc #A Gc A wA wwA #Ac #B Ac B" },
    { 33, "C Cc Cd Daa Da D Dc Dd Eaa Ea E Ec Ed Fa F Fc Fd Gaa Ga G Gc Gd Aaa Aa A Ac Ad Baa Ba B Bc Bd Ca" },
    { 34, "C wC Ch wCh De vD D wD Dh wDh Ee vE E wE F wF Fh wFh Ge vG G wG Gh wGh Ae vA A wA Ah wAh Be vB B wB" },
    { 35, "C wC wwC #D vD D wD wwD #E vE E wE wwE #F vF F wF wwF #G vG G wG wwG #A vA A wA wwA #B vB B wB wwB #C vC" },
    { 36, "C wC vCc Cc wCc vD D wD vDc Dc wDc vE E wE vF F wF vFc Fc wFc vG G wG vGc Gc wGc vA A wA vAc Ac wAc vB B wB vC" },
    { 37, "C wC vCh Ch De wDe vD D wD vDh Dh Ee wEe vE E F wF vFh Fh Ge wGe vG G wG vGh Gh Ae wAe vA A wA vAh Ah Be wBe vB B" },
    { 38, "C Ch Cc Cg Da De D Dh Dc Dg Ea Ee E Eh Ec Fe F Fh Fc Fg Ga Ge G Gh Gc Gg Aa Ae A Ah Ac Ag Ba Be B Bh Bc Ce" },
    { 39, "C wC Da wDa vCc Cc vD D wD Ea wEa vDc Dc vE E wE F wF Ga wGa vFc Fc vG G wG Aa wAa vGc Gc vA A wA Ba wBa vAc Ac vB B wB" },
    { 40, "C Cc Cd Cdc Daa Da D Dc Dd Ddc Eaa Ea E Ec Ed Faa Fa F Fc Fd Fdc Gaa Ga G Gc Gd Gdc Aaa Aa A Ac Ad Adc Baa Ba B Bc Bd Caa Ca" },
    { 41, "C wC Ch Da Cc De vD D wD Dh Ea Dc Ee vE E wE Eh F wF Fh Ga Fc Ge vG G wG Gh Aa Gc Ae vA A wA Ah Ba Ac Be vB B wB Bh" },
    { 42, "C wC wwC wwwC vvvCc #Cc #D Cc D wD wwD wwwD vvvDc #Dc #E Dc E F wF wwF wwwF vvvFc #Fc #G Fc G wG wwG wwwG vvvGc #Gc #A Gc A wA wwA wwwA vvvAc #Ac #B Ac B" },
    { 43, "C wC vCc Cc Da wDa vD D wD vDc Dc Ea wEa vE E wE vEc Ec F wF vFc Fc Ga wGa vG G wG vGc Gc Aa wAa vA A wA vAc Ac Ba wBa vB B wB vBc Bc" },
    { 44, "C wC Da Ch wCh De Cc vD D wD Ea Dh wDh Ee Dc vE E wE F wF Ga Fh wFh Ge Fc vG G wG Aa Gh wGh Ae Gc vA A wA Ba Ah wAh Be Ac vB B wB" },
    { 45, "C Ch Cc Cg Dea Da De D Dh Dc Dg Eea Ea Ee E Eh Ec Fa Fe F Fh Fc Fg Gea Ga Ge G Gh Gc Gg Aea Aa Ae A Ah Ac Ag Bea Ba Be B Bh Bc Ca Ce" },
    { 46, "C wC wwC Da vCc Cc #D vD D wD wwD Ea vDc Dc #E vE E wE vF F wF wwF Ga vFc Fc #G vG G wG wwG Aa vGc Gc #A vA A wA wwA Ba vAc Ac #B vB B wB vC" },
    { 47, "C Cc Cd Cdc Daaa Daa Da D Dc Dd Ddc Eaaa Eaa Ea E Ec Ed Edc Faa Fa F Fc Fd Fdc Gaaa Gaa Ga G Gc Gd Gdc Aaaa Aaa Aa A Ac Ad Adc Baaa Baa Ba B Bc Bd Bdc Caa Ca" },
    { 48, "C wC Ch wCh Cc vDe De vD D wD Dh wDh Dc vEe Ee vE E wE Eh vF F wF Fh wFh Fc vGe Ge vG G wG Gh wGh Gc vAe Ae vA A wA Ah wAh Ac vBe Be vB B wB Bh vC" },
    { 49, "C wC Da wDa wwDa #Cc vCc Cc vD D wD Ea wEa wwEa #Dc vDc Dc vE E wE F wF Ga wGa wwGa #Fc vFc Fc vG G wG Aa wAa wwAa #Gc vGc Gc vA A wA Ba wBa wwBa #Ac vAc Ac vB B wB" },
    { 50, "C wC vCc Cc wCc Da Cd vD D wD vDc Dc wDc Ea Dd vE E wE Fa Ec vF F wF vFc Fc wFc Ga Fd vG G wG vGc Gc wGc Aa Gd vA A wA vAc Ac wAc Ba Ad vB B wB Ca Bc vC" },
    { 51, "C wC vCh Ch wCh vDe De wDe vD D wD vDh Dh wDh vEe Ee wEe vE E wE vF F wF vFh Fh wFh vGe Ge wGe vG G wG vGh Gh wGh vAe Ae wAe vA A wA vAh Ah wAh vBe Be wBe vB B wB vC" },
    { 52, "C Ch Cc Cg Cd Dea Da De D Dh Dc Dg Dd Eea Ea Ee E Eh Ec Eg Fa Fe F Fh Fc Fg Fd Gea Ga Ge G Gh Gc Gg Gd Aea Aa Ae A Ah Ac Ag Ad Bea Ba Be B Bh Bc Bg Ca Ce" },
    { 53, "C wC wwC vDa Da Cc wCc #D vD D wD wwD vEa Ea Dc wDc #E vE E wE #F vF F wF wwF vGa Ga Fc wFc #G vG G wG wwG vAa Aa Gc wGc #A vA A wA wwA vBa Ba Ac wAc #B vB B wB #C vC" },
    { 54, "C wC Da vCh Ch wCh De wDe Cc vD D wD Ea vDh Dh wDh Ee wEe Dc vE E wE F wF Ga vFh Fh wFh Ge wGe Fc vG G wG Aa vGh Gh wGh Ae wAe Gc vA A wA Ba vAh Ah wAh Be wBe Ac vB B wB" },
    { 55, "C wC Ch wCh Cc Da vDe De vD D wD Dh wDh Dc Ea vEe Ee vE E wE Eh Fe vF F wF Fh wFh Fc Ga vGe Ge vG G wG Gh wGh Gc Aa vAe Ae vA A wA Ah wAh Ac Ba vBe Be vB B wB Bh Ce vC" },
    { 56, "C wC wwC Da wDa #Cc vCc Cc #D vD D wD wwD Ea wEa #Dc vDc Dc #E vE E wE vF F wF wwF Ga wGa #Fc vFc Fc #G vG G wG wwG Aa wAa #Gc vGc Gc #A vA A wA wwA Ba wBa #Ac vAc Ac #B vB B wB vC" },
    { 57, "C wC vCc Cc wCc vDa Da wDa vD D wD vDc Dc wDc vEa Ea wEa vE E wE vEc Ec wEc vF F wF vFc Fc wFc vGa Ga wGa vG G wG vGc Gc wGc vAa Aa wAa vA A wA vAc Ac wAc vBa Ba wBa vB B wB vBc Bc wBc vC" },
    { 58, "C wC vCh Ch Da vCc Cc De wDe vD D wD vDh Dh Ea vDc Dc Ee wEe vE E wE vEh Eh F wF vFh Fh Ga vFc Fc Ge wGe vG G wG vGh Gh Aa vGc Gc Ae wAe vA A wA vAh Ah Ba vAc Ac Be wBe vB B wB vBh Bh" },
    { 59, "C wC Da wDa wwDa wwwDa vvvCc #Cc vCc Cc vD D wD Ea wEa wwEa wwwEa vvvDc #Dc vDc Dc vE E wE F wF Ga wGa wwGa wwwGa vvvFc #Fc vFc Fc vG G wG Aa wAa wwAa wwwAa vvvGc #Gc vGc Gc vA A wA Ba wBa wwBa wwwBa vvvAc #Ac vAc Ac vB B wB" },
    { 60, "C wC wwC #Cc vCc Cc wCc wwCc #D vD D wD wwD #Dc vDc Dc wDc wwDc #E vE E wE wwE #F vF F wF wwF #Fc vFc Fc wFc wwFc #G vG G wG wwG #Gc vGc Gc wGc wwGc #A vA A wA wwA #Ac vAc Ac wAc wwAc #B vB B wB wwB #C vC" },
    { 61, "C wC wwC Da Ch wCh vDe De Cc #D vD D wD wwD Ea Dh wDh vEe Ee Dc #E vE E wE vF F wF wwF Ga Fh wFh vGe Ge Fc #G vG G wG wwG Aa Gh wGh vAe Ae Gc #A vA A wA wwA Ba Ah wAh vBe Be Ac #B vB B wB vC" },
    { 62, "C wC Ch wCh Cc wCc Da vDe De vD D wD Dh wDh Dc wDc Ea vEe Ee vE E wE Eh wEh Fe vF F wF Fh wFh Fc wFc Ga vGe Ge vG G wG Gh wGh Gc wGc Aa vAe Ae vA A wA Ah wAh Ac wAc Ba vBe Be vB B wB Bh wBh Ce vC" },
    { 63, "C wC wwC vDa Da wDa vCc Cc wCc #D vD D wD wwD vEa Ea wEa vDc Dc wDc #E vE E wE #F vF F wF wwF vGa Ga wGa vFc Fc wFc #G vG G wG wwG vAa Aa wAa vGc Gc wGc #A vA A wA wwA vBa Ba wBa vAc Ac wAc #B vB B wB #C vC" },
    { 64, "C wC vCc Cc wCc vCd Cd Da wDa vD D wD vDc Dc wDc vDd Dd Ea wEa vE E wE vEc Ec Fa wFa vF F wF vFc Fc wFc vFd Fd Ga wGa vG G wG vGc Gc wGc vGd Gd Aa wAa vA A wA vAc Ac wAc vAd Ad Ba wBa vB B wB vBc Bc Ca wCa vC" },
    { 65, "C wC vCh Ch wCh Da Cc vDe De wDe vD D wD vDh Dh wDh Ea Dc vEe Ee wEe vE E wE Fe Eh vF F wF vFh Fh wFh Ga Fc vGe Ge wGe vG G wG vGh Gh wGh Aa Gc vAe Ae wAe vA A wA vAh Ah wAh Ba Ac vBe Be wBe vB B wB Ce Bh vC" },
    { 66, "C wC wwC Da wDa wwDa vvvCc #Cc vCc Cc #D vD D wD wwD Ea wEa wwEa vvvDc #Dc vDc Dc #E vE E wE vF F wF wwF Ga wGa wwGa vvvFc #Fc vFc Fc #G vG G wG wwG Aa wAa wwAa vvvGc #Gc vGc Gc #A vA A wA wwA Ba wBa wwBa vvvAc #Ac vAc Ac #B vB B wB vC" },
    { 67, "C wC wwC #Cc vCc Cc Da wDa wwDa #D vD D wD wwD #Dc vDc Dc Ea wEa wwEa #E vE E wE wwE #Ec #F Ec F wF wwF #Fc vFc Fc Ga wGa wwGa #G vG G wG wwG #Gc vGc Gc Aa wAa wwAa #A vA A wA wwA #Ac vAc Ac Ba wBa wwBa #B vB B wB wwB #Bc #C Bc" },
    { 68, "C wC wwC vCh Ch wCh #De vDe De wDe #D vD D wD wwD vDh Dh wDh #Ee vEe Ee wEe #E vE E wE #F vF F wF wwF vFh Fh wFh #Ge vGe Ge wGe #G vG G wG wwG vGh Gh wGh #Ae vAe Ae wAe #A vA A wA wwA vAh Ah wAh #Be vBe Be wBe #B vB B wB #C vC" },
    { 69, "C wC Ch wCh Cc wCc Cg Da vDe De vD D wD Dh wDh Dc wDc Dg Ea vEe Ee vE E wE Eh Fa Ec Fe vF F wF Fh wFh Fc wFc Fg Ga vGe Ge vG G wG Gh wGh Gc wGc Gg Aa vAe Ae vA A wA Ah wAh Ac wAc Ag Ba vBe Be vB B wB Bh Ca Bc Ce vC" },
    { 70, "C wC wwC #Da vDa Da vCc Cc wCc wwCc #D vD D wD wwD #Ea vEa Ea vDc Dc wDc wwDc #E vE E wE wwE #F vF F wF wwF #Ga vGa Ga vFc Fc wFc wwFc #G vG G wG wwG #Aa vAa Aa vGc Gc wGc wwGc #A vA A wA wwA #Ba vBa Ba vAc Ac wAc wwAc #B vB B wB wwB #C vC" },
    { 71, "C wC wwC Da vCh Ch wCh vDe De wDe Cc #D vD D wD wwD Ea vDh Dh wDh vEe Ee wEe Dc #E vE E wE vF F wF wwF Ga vFh Fh wFh vGe Ge wGe Fc #G vG G wG wwG Aa vGh Gh wGh vAe Ae wAe Gc #A vA A wA wwA Ba vAh Ah wAh vBe Be wBe Ac #B vB B wB vC" },
    { 72, "C wC vCh Ch wCh vCc Cc wCc vDe De wDe vD D wD vDh Dh wDh vDc Dc wDc vEe Ee wEe vE E wE vEh Eh wEh vF F wF vFh Fh wFh vFc Fc wFc vGe Ge wGe vG G wG vGh Gh wGh vGc Gc wGc vAe Ae wAe vA A wA vAh Ah wAh vAc Ac wAc vBe Be wBe vB B wB vBh Bh wBh vC" },
};

// Every character notes.otf actually has a glyph for, read out of the font's own cmap.
// A name containing anything else would draw as a MISSING-GLYPH box on the keyboard -
// which no layout check can see, because a box has a width like any other character.
static bool inFontAlphabet (const std::string& s)
{
    // Written as a raw string: the alphabet contains a BACKSLASH (the drop accidental),
    // and in an ordinary literal "\a" is the BELL character, which silently swallowed the
    // 'a' - the flat - and made this check fail on every name with a flat in it.
    static const char* const kGlyphs =
        R"( #$()+,-./0123456789:<=>ABCDEFGHIJKLMNOPQRSTUVWXYZ\abcdefghlmnopqrsvw~)";
    for (char c : s)
        if (c == '\0' || std::strchr (kGlyphs, c) == nullptr)
            return false;
    return true;
}

int main()
{
    std::printf ("Ups-and-downs note-name tests\n");

    //--------------------------------------------------------------------------
    std::printf ("against the reference implementation's own output (m = 1..72):\n");
    {
        int mismatches = 0;
        for (const auto& row : kReference)
        {
            const auto got = joined (notenames::edoNames (row.edo));
            if (got != row.names)
            {
                ++mismatches;
                if (mismatches <= 6)
                    std::printf ("    %d-EDO\n      reference: %s\n      port:      %s\n",
                                 row.edo, row.names, got.c_str());
            }
        }
        std::printf ("    %d of %d EDOs differ from the reference\n",
                     mismatches, (int) (sizeof (kReference) / sizeof (kReference[0])));
        check (mismatches == 0, "the port reproduces the reference exactly for every m in 1..72");
    }

    //--------------------------------------------------------------------------
    // The four the task names, spelled out so a reader can SEE what is being claimed
    // rather than trusting a table. 12-EDO is the one everybody can check by eye.
    std::printf ("the familiar ones, read out loud:\n");
    {
        const auto twelve = notenames::edoNames (12);
        check (joined (twelve) == "C Cc D Dc E F Fc G Gc A Ac B",
               "12-EDO is C C# D D# E F F# G G# A A# B ('c' is the sharp glyph)");
        check (twelve.size() == 12, "...and it has twelve of them");

        // 19 and 31 are meantone: sharps and flats are DIFFERENT pitches there, so the
        // spelling needs no ups or downs at all. If either grows a 'v' or a 'w' the port
        // has picked the wrong chromatic step.
        const auto nineteen = notenames::edoNames (19);
        check (joined (nineteen) == "C Cc Da D Dc Ea E Ec F Fc Ga G Gc Aa A Ac Ba B Bc",
               "19-EDO spells C C# Db D D# Eb E E# F F# Gb G G# Ab A A# Bb B B#");
        check (joined (nineteen).find ('v') == std::string::npos
               && joined (nineteen).find ('w') == std::string::npos,
               "...with no ups or downs: 19-EDO is meantone, C# and Db are different keys");

        const auto thirtyOne = notenames::edoNames (31);
        check (joined (thirtyOne) == "C Ch Cc Da De D Dh Dc Ea Ee E Eh Fe F Fh Fc Ga Ge G Gh Gc "
                                     "Aa Ae A Ah Ac Ba Be B Bh Ce",
               "31-EDO spells half-sharps ('h') and half-flats ('e') between the whole ones");
        check (thirtyOne.size() == 31, "...thirty-one of them");

        // 22 is the opposite case: superpythagorean, so it NEEDS ups and downs - it is
        // the notation's reason to exist.
        const auto twentyTwo = notenames::edoNames (22);
        check (joined (twentyTwo) == "C wC vCc Cc D wD vDc Dc E F wF vFc Fc G wG vGc Gc A wA vAc Ac B",
               "22-EDO uses ups ('w') and downs ('v'), which is what this notation is FOR");
        check (twentyTwo.size() == 22, "...twenty-two of them");
    }

    //--------------------------------------------------------------------------
    std::printf ("EDOs with no usable spelling fall back to nothing (the caller shows numbers):\n");
    {
        check (notenames::edoNames (4).empty(),  "4-EDO: too few steps to carry a diatonic skeleton");
        check (notenames::edoNames (1).empty() && notenames::edoNames (2).empty()
               && notenames::edoNames (3).empty() && notenames::edoNames (6).empty(),
               "1, 2, 3 and 6 likewise");
        check (! notenames::edoNames (5).empty(), "...but 5 does have one (the pentatonic branch)");
        check (notenames::edoNames (9).empty() && notenames::edoNames (16).empty()
               && notenames::edoNames (23).empty(),
               "9, 16 and 23 produce COLLIDING names, and a name that does not identify its degree is refused");
        check (notenames::edoNames (notenames::kMaxEdo + 1).empty(),
               "past kMaxEdo nothing is spelled - the work is bounded, and the menu says the number out loud");
    }

    //--------------------------------------------------------------------------
    std::printf ("every spelling this can ever produce (m = 1..%d):\n", notenames::kMaxEdo);
    {
        int spellable = 0, badChars = 0, wrongCount = 0, notUnique = 0, empties = 0;
        size_t longest = 0;
        int longestEdo = 0;
        std::string longestName;
        for (int m = 1; m <= notenames::kMaxEdo; ++m)
        {
            const auto names = notenames::edoNames (m);
            if (names.empty()) continue;
            ++spellable;
            if ((int) names.size() != m) ++wrongCount;
            std::set<std::string> seen;
            for (const auto& n : names)
            {
                if (n.empty()) ++empties;
                if (! inFontAlphabet (n))
                {
                    if (badChars == 0)
                        std::printf ("    first name outside the font's alphabet: %d-EDO \"%s\"\n",
                                     m, n.c_str());
                    ++badChars;
                }
                if (! seen.insert (n).second) ++notUnique;
                if (n.size() > longest) { longest = n.size(); longestEdo = m; longestName = n; }
            }
        }
        std::printf ("    %d of %d EDOs are spellable; longest name is \"%s\" (%d chars, %d-EDO)\n",
                     spellable, notenames::kMaxEdo, longestName.c_str(), (int) longest, longestEdo);
        check (wrongCount == 0, "a spellable EDO always returns exactly m names");
        check (empties == 0,    "no name is empty");
        check (notUnique == 0,  "no EDO ever gives two degrees the same name");
        check (badChars == 0,   "every character has a glyph in notes.otf (no missing-glyph boxes)");
        check (longest == notenames::longestNameLength (notenames::kMaxEdo),
               "longestNameLength() agrees - it is what the keyboard's text-fit check is measured against");
    }

    //--------------------------------------------------------------------------
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
