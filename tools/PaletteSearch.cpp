// PaletteSearch - the search that CHOSE the palette in source/GalacticPalette.h.
//
// "haz que todas las demas relaciones de colores incluidas la que hay entre spectrum y
//  tuning sean lo mas contrastadas y diferentes posibles."
//
// "As different as possible" is a maximin problem, not a matter of taste, so it is
// solved here rather than eyeballed: place the identity colours in OKLab so that the
// SMALLEST pairwise distance in the whole set is as large as it can be, subject to
// every colour staying legible on the page and inside the sRGB gamut. The
// Spectrum-vs-Tuning colour is not placed at all - it is pinned to the midpoint of
// Spectrum and Tuning (rule (a)) and moves as they move, which is why pushing those two
// apart is also what gives the blend its own identity.
//
// This tool is committed because the palette is a RESULT, and a result whose derivation
// is not in the tree is a magic number. Run it to reproduce or to re-derive after a
// colour is added:  build/PaletteSearch_artefacts/Release/PaletteSearch.exe
//
// It prints the BEFORE matrix (the palette as it stood), the search, and the AFTER
// matrix with the hexes to paste into GalacticPalette.h. It changes nothing itself.

#include "GalacticPalette.h"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace galactic;
using colour::Lab;
using colour::Vision;

namespace
{
    // The palette as it stood before this pass, in the same order as the roles below.
    // Kept here rather than read from the header so the BEFORE matrix survives the
    // header being updated - otherwise "before" silently becomes "after".
    const std::uint32_t kBefore[11] = {
        0x5cb8ff,  // Spectrum / Harmonic engine
        0xb27bff,  // Tuning
        0xff77c8,  // Custom engine
        0x8ef0c4,  // Both engines
        0xf062cf,  // Fractal
        0x4fe09a,  // Essentials
        0x3ad9b0,  // Amp envelope
        0xff5b52,  // Pitch
        0x3fd0e6,  // Filter
        0x9a7bff,  // Custom modulator
        0x32c8be   // Scale dissonance
    };
    // Index 0 and 1 are the two PARENTS of the derived blend - the search never sees
    // them as ordinary points, because the twelfth colour hangs off them.
    const char* kRole[11] = { "Spectrum/Harmonic", "Tuning", "Custom engine", "Both engines",
                              "Fractal", "Essentials", "Amp envelope", "Pitch", "Filter",
                              "Custom modulator", "Scale dissonance" };
    constexpr int kN = 11;

    // THE CONSTRAINTS. "As different as possible" is only meaningful once "possible" is
    // written down, and every one of these is a property the window already depends on:
    //
    //   contrast   - each colour is a border, a title chip and a label on the page, so
    //                it has to be legible against it. 7:1 is WCAG AAA for large text;
    //                the palette this replaces did not manage it (worst 6.2:1).
    //   lightness  - a floor because these sit on near-black, and a CEILING because a
    //                colour that light stops being a colour: the SPECTRUM panel already
    //                shipped once in a neutral that "read as white beside the coloured
    //                panels around it", and unconstrained maximin walks straight back
    //                there (it wanted #faffec and #e8ff05). The band also keeps
    //                GalacticLnF's black-on-light chip text rule stable.
    //   chroma     - same reason from the other side: an identity colour must BE a
    //                colour. RhythmVerify already refuses a panel under 0.35 saturation.
    //   blend      - the derived twelfth has to satisfy all of that too. It is not a
    //                free point, so the only way to fix an illegal blend is to move its
    //                parents, which is exactly the pressure rule (a) is meant to exert.
    constexpr double kMinContrast = 7.0;
    constexpr double kMinL = 0.66, kMaxL = 0.93;
    constexpr double kMinChroma = 0.11;

    std::vector<std::uint32_t> withBlend (const std::uint32_t* c)
    {
        std::vector<std::uint32_t> v (c, c + kN);
        v.push_back (colour::mixOklch (c[0], c[1], 0.5));   // rule (a): the derived twelfth
        return v;
    }

    double minPairwise (const std::vector<std::uint32_t>& v, Vision vis)
    {
        double worst = 1.0e9;
        for (std::size_t i = 0; i < v.size(); ++i)
            for (std::size_t j = i + 1; j < v.size(); ++j)
                worst = std::min (worst, colour::deltaE (colour::simulate (v[i], vis),
                                                         colour::simulate (v[j], vis)));
        return worst;
    }

    bool legal (const Lab& lab)
    {
        if (lab.L < kMinL || lab.L > kMaxL) return false;
        if (colour::chroma (lab) < kMinChroma) return false;
        const auto lin = colour::fromOklab (lab);
        if (! colour::inGamut (lin)) return false;
        return colour::contrastRatio (colour::fromLinear (lin), palette::kPage) >= kMinContrast;
    }
    // Same rules, applied to a colour that already exists as sRGB - which is how the
    // derived blend has to be judged: it is produced, not placed.
    bool legalRgb (std::uint32_t rgb)
    {
        const auto lab = colour::toOklab (rgb);
        return lab.L >= kMinL && lab.L <= kMaxL && colour::chroma (lab) >= kMinChroma
            && colour::contrastRatio (rgb, palette::kPage) >= kMinContrast;
    }

    // The objective. PRIMARY is the trichromatic minimum - that is the rule as stated.
    // The dichromatic minimum rides along at a small weight purely as a TIE-BREAK: many
    // placements reach the same trichromatic optimum and there is no reason to pick one
    // that happens to collapse under deuteranopia.
    double score (const std::uint32_t* c)
    {
        const auto v = withBlend (c);
        if (! legalRgb (v.back())) return -1.0e6;   // the blend broke a rule: reject
        const double tri = minPairwise (v, Vision::normal);
        const double di  = std::min (minPairwise (v, Vision::deuteranopia),
                                     minPairwise (v, Vision::protanopia));
        // The dichromatic term is weighted, not merely a tie-break: the two common
        // dichromacies collapse most of the hue circle onto one axis, so a palette that
        // ignores them can reach a large trichromatic minimum with two colours that are
        // literally the same colour to a deuteranope - which is where this palette
        // started (0.7 apart, indistinguishable). The trichromatic minimum still leads.
        return tri + 0.60 * di;
    }

    void printMatrix (const char* title, const std::vector<std::uint32_t>& v,
                      const std::vector<std::string>& names, Vision vis)
    {
        std::printf ("\n%s\n     ", title);
        for (std::size_t j = 0; j < v.size(); ++j) std::printf ("%4d", (int) j);
        std::printf ("\n");
        double worst = 1.0e9; std::size_t wi = 0, wj = 1;
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            std::printf ("%2d   ", (int) i);
            for (std::size_t j = 0; j < v.size(); ++j)
            {
                if (j <= i) { std::printf ("   ."); continue; }
                const double d = colour::deltaE (colour::simulate (v[i], vis),
                                                 colour::simulate (v[j], vis));
                std::printf ("%4.0f", d);
                if (d < worst) { worst = d; wi = i; wj = j; }
            }
            std::printf ("   %-19s #%06x\n", names[i].c_str(), v[i]);
        }
        std::printf ("     worst pair: %s vs %s = %.1f\n",
                     names[wi].c_str(), names[wj].c_str(), worst);
    }
}

int main()
{
    std::vector<std::string> names (kRole, kRole + kN);
    names.push_back ("Spectrum vs Tuning");

    //== BEFORE ============================================================
    {
        const auto v = withBlend (kBefore);
        printMatrix ("BEFORE  (OKLab distance x100, normal vision)", v, names, Vision::normal);
        std::printf ("  BEFORE minimum: normal %.1f   deuteranopia %.1f   protanopia %.1f\n",
                     minPairwise (v, Vision::normal),
                     minPairwise (v, Vision::deuteranopia),
                     minPairwise (v, Vision::protanopia));
        std::printf ("  BEFORE contrast against the page:");
        double worstC = 1.0e9; std::size_t wc = 0;
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            const double c = colour::contrastRatio (v[i], palette::kPage);
            if (c < worstC) { worstC = c; wc = i; }
        }
        std::printf (" worst %.1f:1 (%s)\n", worstC, names[wc].c_str());
    }

    //== SEARCH ============================================================
    // Simulated annealing from many restarts. The space is small (11 points x 3 axes)
    // and the objective is cheap, so brute force is honest here; the annealing is only
    // there because a maximin surface is full of plateaux a hill-climber sits down on.
    std::mt19937 rng (20260828u);
    std::uniform_real_distribution<double> uni (0.0, 1.0);

    std::uint32_t best[kN];
    std::copy (kBefore, kBefore + kN, best);
    double bestScore = -1.0e9; // never the old palette: it breaks the contrast floor
    std::printf ("\nsearching (start %.2f)...\n", bestScore);

    for (int restart = 0; restart < 160; ++restart)
    {
        Lab cur[kN];
        // Half the restarts begin at the palette we have, half from scratch: starting
        // only from the old one would find the nearest local improvement and call it
        // optimal, and starting only from noise would throw away a good basin.
        for (int i = 0; i < kN; ++i)
        {
            // A start that is already ILLEGAL is not a start: the annealer only ever
            // tests candidate MOVES, so a colour that begins outside the rules and is
            // never successfully moved rides all the way to the answer breaking them.
            // The old palette does exactly this - its Custom modulator is 6.2:1 against
            // the page, under the 7:1 floor - and it came out of the search untouched
            // and illegal until this line existed.
            const bool seeded = restart % 2 == 0 && legal (colour::toOklab (kBefore[i]));
            if (seeded) cur[i] = colour::toOklab (kBefore[i]);
            else
            {
                do {
                    cur[i] = { kMinL + uni (rng) * (kMaxL - kMinL),
                               (uni (rng) - 0.5) * 0.5,
                               (uni (rng) - 0.5) * 0.5 };
                } while (! legal (cur[i]));
            }
        }
        auto asRgb = [] (const Lab* p, std::uint32_t* out)
        { for (int i = 0; i < kN; ++i) out[i] = colour::fromLinear (colour::fromOklab (p[i])); };

        std::uint32_t rgb[kN];
        asRgb (cur, rgb);
        double curScore = score (rgb);

        for (int step = 0; step < 24000; ++step)
        {
            const double T = 0.6 * std::pow (0.0002 / 0.6, (double) step / 24000.0);
            const int    k = (int) (uni (rng) * kN) % kN;
            const double s = 0.06 * (T / 0.6) + 0.004;
            Lab trial = cur[k];
            trial.L += (uni (rng) - 0.5) * s;
            trial.a += (uni (rng) - 0.5) * s * 2.0;
            trial.b += (uni (rng) - 0.5) * s * 2.0;
            if (! legal (trial)) continue;

            const Lab keep = cur[k];
            cur[k] = trial;
            asRgb (cur, rgb);
            const double s2 = score (rgb);
            if (s2 >= curScore || uni (rng) < std::exp ((s2 - curScore) / (T * 8.0)))
                curScore = s2;
            else
                cur[k] = keep;
        }
        asRgb (cur, rgb);
        const double fin = score (rgb);
        if (fin > bestScore) { bestScore = fin; std::copy (rgb, rgb + kN, best); }
    }

    //== ASSIGN THE COLOURS TO THE ROLES ===================================
    // The SET is what the search optimises; which colour wears which name is free, so
    // it is settled afterwards by keeping each section as close as possible to the
    // colour it already had. It costs nothing in separation and it means PITCH is still
    // warm and FILTER is still cool - the palette is re-spaced, not re-invented.
    //
    // The two blend parents are structurally points 0 and 1 (the twelfth colour is
    // their midpoint), so only their ORDER is free; the other nine are permuted.
    {
        // Which colour wears which name is free, and so is which PAIR of them are the
        // blend's parents: nothing in the search cares, because the objective only sees
        // the twelve points. So every ordered pair is tried as (Spectrum, Tuning) - the
        // blend recomputed for each, since it hangs off them - and among the pairs that
        // keep the separation the search actually reached, the assignment that moves the
        // window LEAST from the colours it already wears wins.
        //
        // Without this the pairs were pinned to points 0 and 1 and the answer came out
        // with a green SPECTRUM panel, an olive TUNING and an orange PITCH for no gain
        // at all: on a maximin plateau every rotation of the palette scores the same,
        // and nothing was choosing between them.
        double drift[kN][kN];      // drift[point][role]
        for (int i = 0; i < kN; ++i)
            for (int r = 0; r < kN; ++r)
                drift[i][r] = colour::deltaE (best[i], kBefore[r]);

        // The bar: the best minimum any parent choice reaches, less 2 percent. A pair
        // that gives up real separation to look familiar is not the trade this is for.
        double reachable = 0.0;
        for (int s = 0; s < kN; ++s)
            for (int t = 0; t < kN; ++t)
            {
                if (s == t) continue;
                std::uint32_t v[kN]; std::copy (best, best + kN, v);
                std::swap (v[0], v[s]); std::swap (v[1], v[s == 1 ? 0 : t]);
                const auto set = withBlend (v);
                if (! legalRgb (set.back())) continue;
                reachable = std::max (reachable, minPairwise (set, Vision::normal));
            }
        const double bar = reachable * 0.98;

        std::uint32_t out[kN];
        double bestCost = 1.0e18;
        bool found = false;
        for (int s = 0; s < kN; ++s)
            for (int t = 0; t < kN; ++t)
            {
                if (s == t) continue;
                std::vector<std::uint32_t> set (best, best + kN);
                set.push_back (colour::mixOklch (best[s], best[t], 0.5));
                if (! legalRgb (set.back())) continue;
                if (minPairwise (set, Vision::normal) < bar) continue;

                // The other nine points to the other nine roles: the optimal assignment,
                // by subset DP over which points are already spoken for. Nine factorial
                // per pair was four and a half minutes of the tool's five.
                std::vector<int> rest;
                for (int i = 0; i < kN; ++i) if (i != s && i != t) rest.push_back (i);
                const int m = (int) rest.size();
                std::vector<double> dp ((std::size_t) 1 << m, 1.0e18);
                std::vector<int> pick ((std::size_t) 1 << m, -1);
                dp[0] = 0.0;
                for (int mask = 0; mask < (1 << m); ++mask)
                {
                    if (dp[(std::size_t) mask] >= 1.0e17) continue;
                    int role = 2;
                    for (int k = 0; k < m; ++k) if (mask & (1 << k)) ++role;   // roles fill in order
                    for (int k = 0; k < m; ++k)
                    {
                        if (mask & (1 << k)) continue;
                        const int nxt = mask | (1 << k);
                        const double c = dp[(std::size_t) mask] + drift[rest[(std::size_t) k]][role];
                        if (c < dp[(std::size_t) nxt]) { dp[(std::size_t) nxt] = c; pick[(std::size_t) nxt] = k; }
                    }
                }
                const double cost = drift[s][0] + drift[t][1] + dp[((std::size_t) 1 << m) - 1];
                if (cost < bestCost)
                {
                    bestCost = cost; found = true;
                    out[0] = best[s]; out[1] = best[t];
                    int mask = (1 << m) - 1;
                    for (int role = m + 1; role >= 2; --role)
                    {
                        const int k = pick[(std::size_t) mask];
                        out[role] = best[rest[(std::size_t) k]];
                        mask &= ~(1 << k);
                    }
                }
            }
        std::printf ("  best reachable minimum over parent choices %.1f; kept >= %.1f;"
                     " total movement from the old palette %.0f\n", reachable, bar, bestCost);
        if (found) std::copy (out, out + kN, best);
    }

    //== AFTER =============================================================
    {
        const auto v = withBlend (best);
        printMatrix ("AFTER  (OKLab distance x100, normal vision)", v, names, Vision::normal);
        printMatrix ("AFTER  (deuteranopia)",  v, names, Vision::deuteranopia);
        printMatrix ("AFTER  (protanopia)",    v, names, Vision::protanopia);
        std::printf ("\n  AFTER minimum: normal %.1f   deuteranopia %.1f   protanopia %.1f\n",
                     minPairwise (v, Vision::normal),
                     minPairwise (v, Vision::deuteranopia),
                     minPairwise (v, Vision::protanopia));
        std::printf ("  contrast against the page:\n");
        for (std::size_t i = 0; i < v.size(); ++i)
            std::printf ("    %-19s #%06x  %5.1f:1\n", names[i].c_str(), v[i],
                         colour::contrastRatio (v[i], palette::kPage));
        // The blend has to stay visibly BETWEEN its parents (rule (a) wins ties).
        const double dp = colour::deltaE (best[0], best[1]);
        std::printf ("  blend: %.1f from Spectrum, %.1f from Tuning, parents %.1f apart\n",
                     colour::deltaE (v[11], best[0]), colour::deltaE (v[11], best[1]), dp);
        std::printf ("\n  paste into GalacticPalette.h:\n");
        for (int i = 0; i < kN; ++i)
            std::printf ("    %-19s 0x%06x\n", kRole[i], best[i]);
    }
    return 0;
}
