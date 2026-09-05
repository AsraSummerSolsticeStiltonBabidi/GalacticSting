// Unit tests for the microtuning subsystem (TuningManager).
// Verifies Scala .scl parsing and the note->frequency mapping.
//
//   build\...\TuningTests.exe  -> exit code 0 = all passed, non-zero = failures.

#include "dsp/TuningManager.h"

#include <algorithm>
#include <cstdio>
#include <cmath>

static int failures = 0;

static void check (bool cond, const char* what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (! cond) ++failures;
}

// Relative tolerance in cents is more musically meaningful than absolute Hz.
static bool approxHz (double a, double b, double tolHz = 0.01)
{
    return std::abs (a - b) <= tolHz;
}

// Distance between two frequencies in cents - the unit the .tun conversion makes
// its promise in (Hz tolerances mean something different at every octave).
static double centsApart (double a, double b)
{
    return std::abs (1200.0 * std::log2 (a / b));
}

// Write a .tun in EXACTLY the layout FractalAdditiveAudioProcessor::saveCurrentTuningTun
// produces: integer-cent [Tuning] first, then the authoritative [Exact Tuning].
// The coarse block comes first on purpose - if the parser let it win, every test
// below would pass to ~0.5 cent and hide the loss.
static juce::String makeTun (const double (&freqs)[128], bool withCoarse = true)
{
    juce::String s;
    s << "; AnaMark TUN v2 - written by a unit test\n\n";
    if (withCoarse)
    {
        s << "[Tuning]\n";
        for (int n = 0; n < 128; ++n)
            s << "note " << n << "="
              << juce::String ((int) std::llround (1200.0 * std::log2 (freqs[n] / freqs[0]))) << "\n";
    }
    s << "\n[Exact Tuning]\nbasefreq=" << juce::String (freqs[0], 10) << "\n";
    for (int n = 0; n < 128; ++n)
        s << "note " << n << "=" << juce::String (1200.0 * std::log2 (freqs[n] / freqs[0]), 6) << "\n";
    return s;
}

// Every MIDI note of the loaded tuning against the frequencies the file states.
static double worstCents (const TuningManager& t, const double (&freqs)[128])
{
    double worst = 0.0;
    for (int n = 0; n < 128; ++n)
        worst = std::max (worst, centsApart (t.frequencyForNote (n), freqs[n]));
    return worst;
}

int main()
{
    std::printf ("TuningManager tests\n");

    const double A4  = 440.0;
    const double C4  = TuningManager::kRefFreq;      // middle C anchor
    const double C5  = C4 * 2.0;
    const double C3  = C4 / 2.0;
    const double A3  = 220.0;
    const double C6  = C4 * 4.0;

    //--------------------------------------------------------------------------
    std::printf ("Equal temperament:\n");
    {
        TuningManager t;
        t.setMode (TuningManager::Mode::Equal);
        check (approxHz (t.frequencyForNote (69), A4), "A4 = 440");
        check (approxHz (t.frequencyForNote (60), C4), "C4 = middle C");
        check (approxHz (t.frequencyForNote (72), C5), "C5 = 2*C4");
        check (approxHz (t.frequencyForNote (48), C3), "C3 = C4/2");
    }

    //--------------------------------------------------------------------------
    std::printf ("MTS-ESP with no master falls back to 12-TET:\n");
    {
        TuningManager t;
        t.setMode (TuningManager::Mode::MtsEsp);
        check (approxHz (t.frequencyForNote (69), A4, 0.05), "A4 = 440 (fallback)");
        check (approxHz (t.frequencyForNote (60), C4, 0.05), "C4 = middle C (fallback)");
    }

    //--------------------------------------------------------------------------
    std::printf ("Scala: 12-TET file (cents) reproduces standard tuning:\n");
    {
        const juce::String scl =
            "! test12.scl\n"
            "!\n"
            "12-tone equal temperament\n"
            " 12\n"
            "!\n"
            " 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n 600.0\n"
            " 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 1200.0\n";
        TuningManager t; juce::String err;
        const bool ok = t.loadScalaString (scl, "test12", err);
        t.setMode (TuningManager::Mode::Scala);
        check (ok, "parsed 12-note cents file");
        check (approxHz (t.frequencyForNote (69), A4, 0.05), "A4 = 440");
        check (approxHz (t.frequencyForNote (60), C4),       "C4 = 1/1 anchor");
        check (approxHz (t.frequencyForNote (72), C5, 0.05), "C5 = 2*C4 (one period up)");
        check (approxHz (t.frequencyForNote (57), A3, 0.05), "A3 = 220 (period down)");
        check (approxHz (t.frequencyForNote (84), C6, 0.1),  "C6 = 4*C4 (two periods up)");
    }

    //--------------------------------------------------------------------------
    // A 1-note-per-period scale maps EACH successive key to a new period, so one
    // MIDI key == one octave here (correct Scala semantics for "equal 1").
    std::printf ("Scala: single 2/1 interval = one octave per key:\n");
    {
        const juce::String scl =
            "One note per octave\n"
            "1\n"
            "2/1\n";
        TuningManager t; juce::String err;
        const bool ok = t.loadScalaString (scl, "oct", err);
        t.setMode (TuningManager::Mode::Scala);
        check (ok, "parsed 1-note ratio file");
        check (approxHz (t.frequencyForNote (60), C4),       "key 60 = C4 (1/1)");
        check (approxHz (t.frequencyForNote (61), C4 * 2.0), "key 61 = one octave up");
        check (approxHz (t.frequencyForNote (59), C4 / 2.0), "key 59 = one octave down");
    }

    //--------------------------------------------------------------------------
    std::printf ("Scala: ratio parsing (just fifth) and cents/ratio equivalence:\n");
    {
        // 2 notes per period: degree 1 = 3/2, degree 2 (period) = 2/1.
        const juce::String sclRatio = "just fifth\n2\n3/2\n2/1\n";
        TuningManager t; juce::String err;
        t.loadScalaString (sclRatio, "fifth", err);
        t.setMode (TuningManager::Mode::Scala);
        check (approxHz (t.frequencyForNote (60), C4),            "deg0 = C4");
        check (approxHz (t.frequencyForNote (61), C4 * 1.5),      "deg1 = 3/2 * C4");
        check (approxHz (t.frequencyForNote (62), C4 * 2.0),      "deg2 = octave");

        // Same scale expressed in cents (701.955 ~ 3/2) should match within ~0.01c.
        const juce::String sclCents = "just fifth in cents\n2\n701.955000865\n1200.0\n";
        TuningManager t2; juce::String err2;
        t2.loadScalaString (sclCents, "fifthC", err2);
        t2.setMode (TuningManager::Mode::Scala);
        check (approxHz (t2.frequencyForNote (61), C4 * 1.5, 0.02), "cents 701.955 ~ 3/2");
    }

    //--------------------------------------------------------------------------
    std::printf ("Ableton .ascl: @ABL REFERENCE_PITCH re-anchors the scale:\n");
    {
        // 12-TET in cents, but the file pins degree 9 of octave 3 (= A4) to 432 Hz.
        const juce::String ascl =
            "! test.ascl\n"
            "12-TET at A=432\n"
            " 12\n"
            "!\n"
            " 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n 600.0\n"
            " 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 1200.0\n"
            "!\n"
            "! @ABL REFERENCE_PITCH 3 9 432.0\n"
            "! @ABL SOURCE unit test\n";
        TuningManager t; juce::String err;
        const bool ok = t.loadScalaString (ascl, "a432", err);
        t.setMode (TuningManager::Mode::Scala);
        check (ok, "parsed .ascl with @ABL lines");
        check (approxHz (t.frequencyForNote (69), 432.0, 0.05), "A4 = 432 (file reference wins)");
        check (approxHz (t.frequencyForNote (57), 216.0, 0.05), "A3 = 216 (octave preserved)");
        check (approxHz (t.frequencyForNote (60), 432.0 * std::pow (2.0, -9.0 / 12.0), 0.05),
               "C4 shifted consistently with the new anchor");
    }

    //--------------------------------------------------------------------------
    std::printf (".ascl hardening: out-of-range and malformed references:\n");
    {
        const juce::String twelve =
            "12-TET\n 12\n"
            " 100.0\n 200.0\n 300.0\n 400.0\n 500.0\n 600.0\n"
            " 700.0\n 800.0\n 900.0\n 1000.0\n 1100.0\n 1200.0\n";

        // Reference far above the MIDI range (A9 = 14080 Hz) must still resolve
        // exactly (closed form), keeping A4 = 440 - no clamped mis-anchor.
        {
            TuningManager t; juce::String err;
            t.loadScalaString (twelve + "! @ABL REFERENCE_PITCH 8 9 14080.0\n", "hi", err);
            t.setMode (TuningManager::Mode::Scala);
            check (approxHz (t.frequencyForNote (69), 440.0, 0.05), "out-of-range reference: A4 stays 440");
        }
        // Non-numeric octave token: the @ABL line is IGNORED (plain .scl anchor).
        {
            TuningManager t; juce::String err;
            t.loadScalaString (twelve + "! @ABL REFERENCE_PITCH three 9 440.0\n", "bad-oct", err);
            t.setMode (TuningManager::Mode::Scala);
            check (approxHz (t.frequencyForNote (60), TuningManager::kRefFreq, 0.05),
                   "non-numeric octave ignored -> middle-C anchor kept");
        }
        // Infinite frequency: rejected (no inf table / silent blast).
        {
            TuningManager t; juce::String err;
            t.loadScalaString (twelve + "! @ABL REFERENCE_PITCH 3 9 1e999\n", "inf", err);
            t.setMode (TuningManager::Mode::Scala);
            check (approxHz (t.frequencyForNote (69), 440.0, 0.05), "infinite reference freq ignored");
        }
        // 53-EDO with a reference 2 periods up: key 60 must stay ~kRefFreq * (1046.502/expected)
        // - here we just require the anchor to resolve finitely and periodically.
        {
            juce::String edo53 = "53-EDO\n 53\n";
            for (int i = 1; i <= 53; ++i)
                edo53 += " " + juce::String (1200.0 * i / 53.0, 6) + "\n";
            edo53 += "! @ABL REFERENCE_PITCH 5 0 1046.502\n"; // virtual key 60 + 2*53 = 166
            TuningManager t; juce::String err;
            const bool ok = t.loadScalaString (edo53, "53edo", err);
            t.setMode (TuningManager::Mode::Scala);
            check (ok, "53-EDO .ascl parsed");
            check (approxHz (t.frequencyForNote (60), 1046.502 / 4.0, 0.05),
                   "53-EDO reference 2 periods up -> key 60 = freq/4");
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("clearScala reverts to 12-TET:\n");
    {
        TuningManager t; juce::String err;
        t.loadScalaString ("x\n1\n3/1\n", "weird", err); // tritave, clearly non-12
        t.setMode (TuningManager::Mode::Scala);
        check (! approxHz (t.frequencyForNote (72), C5, 1.0), "non-octave scale really differs");
        t.clearScala();
        check (approxHz (t.frequencyForNote (69), A4), "after clear, A4 = 440");
    }

    //--------------------------------------------------------------------------
    std::printf ("parsePeriodSpec (integer / fraction / cents):\n");
    {
        double r = 0.0; juce::String line;
        check (TuningManager::parsePeriodSpec ("2", r, line) && approxHz (r, 2.0, 1e-9) && line == "2/1",
               "\"2\" -> ratio 2, scl line 2/1");
        check (TuningManager::parsePeriodSpec ("3", r, line) && approxHz (r, 3.0, 1e-9) && line == "3/1",
               "\"3\" -> ratio 3 (tritave)");
        check (TuningManager::parsePeriodSpec ("3/2", r, line) && approxHz (r, 1.5, 1e-9) && line == "3/2",
               "\"3/2\" -> ratio 1.5, exact line");
        check (TuningManager::parsePeriodSpec ("701.955c", r, line) && approxHz (r, 1.5, 1e-4),
               "\"701.955c\" -> ~1.5");
        check (! TuningManager::parsePeriodSpec ("1", r, line),   "\"1\" rejected (no period)");
        check (! TuningManager::parsePeriodSpec ("abc", r, line), "garbage rejected");
        check (! TuningManager::parsePeriodSpec ("0/3", r, line), "zero numerator rejected");
        check (! TuningManager::parsePeriodSpec ("-700c", r, line), "negative cents rejected");
        // The three notations the period box promises, in every spelling.
        check (TuningManager::parsePeriodSpec ("1200", r, line) && approxHz (r, 2.0, 1e-9),
               "bare \"1200\" read as cents (too big for a ratio)");
        check (TuningManager::parsePeriodSpec ("701.955 cents", r, line) && approxHz (r, 1.5, 1e-4),
               "\"701.955 cents\" -> ~1.5");
        check (TuningManager::parsePeriodSpec (juce::String::fromUTF8 ("1200\xc2\xa2"), r, line)
                   && approxHz (r, 2.0, 1e-9), "the cent sign is accepted");
        check (TuningManager::parsePeriodSpec ("3/2.0", r, line) && approxHz (r, 1.5, 1e-9),
               "decimals allowed inside a fraction");
        check (TuningManager::parsePeriodSpec ("2.5", r, line) && approxHz (r, 2.5, 1e-9)
                   && line.getDoubleValue() > 1500.0, "decimal ratio kept exactly, written as cents");
        check (! TuningManager::parsePeriodSpec ("2.5.5", r, line), "two dots rejected");
    }

    //--------------------------------------------------------------------------
    std::printf ("reference A transposes everything:\n");
    {
        TuningManager t;
        t.setMode (TuningManager::Mode::Equal);
        t.setReferenceA (432.0);
        check (approxHz (t.frequencyForNote (69), 432.0, 0.01), "A4 = 432 after setReferenceA(432)");
        check (approxHz (t.frequencyForNote (57), 216.0, 0.01), "A3 = 216 (everything scaled)");
        t.setReferenceA (440.0);
        check (approxHz (t.frequencyForNote (69), 440.0, 0.01), "back to 440");
    }

    //--------------------------------------------------------------------------
    std::printf ("malformed files are rejected (no silent wrong tuning):\n");
    {
        TuningManager t; juce::String err;
        check (! t.loadScalaString ("only one line", "bad", err), "too short rejected");
        check (! t.loadScalaString ("desc\n3\n100.0\n", "bad2", err), "declared count > actual rejected");
        check (! t.loadScalaString ("desc\n1\nx.5\n", "bad3", err), "non-numeric cents 'x.5' rejected");
        check (! t.loadScalaString ("desc\n1\n1.0.0\n", "bad4", err), "double-dot cents '1.0.0' rejected");
        check (! t.loadScalaString ("desc\n1\n100abc\n", "bad5", err), "garbage bare ratio '100abc' rejected");
        check (! t.loadScalaString ("desc\n1\nwww.x.com\n", "bad6", err), "URL-like token rejected");
        check (! t.loadScalaString ("desc\n99999\n2/1\n", "bad7", err), "absurd note count rejected");
        // ...but well-formed cents/ratios (incl. descending negative cents) still load.
        check (t.loadScalaString ("desc\n2\n-100.0\n2/1\n", "ok1", err), "negative (descending) cents accepted");
        check (t.loadScalaString ("desc\n1\n1200.0\n", "ok2", err), "plain cents octave accepted");
    }

    //======================================================================================
    std::printf ("a few thousand mutations of a valid .scl: no crash, and no silent nonsense:\n");
    {
        // A USER DRAGS ANY FILE ONTO THIS PLUGIN. The hand-written cases above cover the
        // mistakes a person makes while EDITING a scale; this covers the file that was
        // never a scale, or was one until something truncated it. Two claims, and the
        // second is the one that matters:
        //   * the parser returns. It may refuse - refusing is the right answer for most of
        //     these - but it may not crash, hang or read off the end.
        //   * and when it ACCEPTS, what it accepted has to be a usable tuning. A parser
        //     that says yes to rubbish silently retunes the user's whole project, which is
        //     worse than one that says no to something valid: the second is visible.
        //
        // Deterministic mutations, not random ones, so a failure is reproducible from the
        // index alone and the run means the same thing on every machine.
        const juce::String base =
            "! fuzz.scl\na scale to break\n 5\n!\n 100.0\n 3/2\n 700.0\n 9/8\n 2/1\n";

        int accepted = 0, refused = 0, bad = 0;
        juce::String firstBad;
        const auto tryOne = [&] (const juce::String& text, const juce::String& what)
        {
            TuningManager t; juce::String err;
            // SCALA MODE, or frequencyForNote returns 12-TET and this whole check measures
            // nothing: the first version left the default mode and PASSED with the table
            // deliberately corrupted. Third vacuous pass of the session, and the same
            // shape as the other two - the thing under test was not connected.
            t.setMode (TuningManager::Mode::Scala);
            const bool ok = t.loadScalaString (text, "fuzz", err);
            if (! ok) { ++refused; return; }
            ++accepted;
            // ACCEPTED: then it must be a tuning somebody can play. Every one of the 128
            // keys has to be a FINITE, POSITIVE frequency - a NaN is a dead note that
            // poisons the host's mix, a zero or a negative is not a pitch at all.
            //
            // NOT "strictly ascending", which is what this asked first and got 363 of 364
            // mutations flagged. A .scl may state its degrees in any order and this build
            // deliberately accepts descending ones, so a non-monotonic table is the FILE
            // faithfully represented, not a defect. An assertion that fails on almost
            // everything is an assertion about the wrong thing.
            bool good = t.degreesPerPeriod() > 0 && std::isfinite (t.periodRatio())
                     && t.periodRatio() > 0.0;
            for (int n = 0; good && n < 128; ++n)
            {
                const double f = t.frequencyForNote (n);
                if (! std::isfinite (f) || f <= 0.0) good = false;
            }
            if (! good) { ++bad; if (firstBad.isEmpty()) firstBad = what; }
        };

        // 1. Truncation at every 1/64th of the file: the half-written download.
        for (int i = 0; i <= 64; ++i)
            tryOne (base.substring (0, base.length() * i / 64), "truncated at " + juce::String (i) + "/64");
        // 2. Every single byte replaced by each of a handful of awkward characters.
        const char* pokes = "\0\n\r\t 0.-/!x";
        for (int i = 0; i < base.length(); ++i)
            for (int p = 0; p < 10; ++p)
            {
                auto m = base;
                m = m.replaceSection (i, 1, juce::String::charToString ((juce::juce_wchar) pokes[p]));
                tryOne (m, "byte " + juce::String (i) + " -> poke " + juce::String (p));
            }
        // 3. Each line duplicated, and each line deleted.
        {
            juce::StringArray lines;
            lines.addLines (base);
            for (int i = 0; i < lines.size(); ++i)
            {
                auto dup = lines; dup.insert (i, lines[i]);
                tryOne (dup.joinIntoString ("\n"), "line " + juce::String (i) + " duplicated");
                auto del = lines; del.remove (i);
                tryOne (del.joinIntoString ("\n"), "line " + juce::String (i) + " deleted");
            }
        }
        // 4. The count line, which is the one number the rest of the file is measured
        //    against, set to everything it should never be.
        for (const char* n : { "0", "-1", "1", "4", "6", "2147483647", "-2147483648",
                               "99999999999999999999", "3.5", "0x10", "", " " })
        {
            auto m = base.replace ("\n 5\n", "\n " + juce::String (n) + "\n");
            tryOne (m, juce::String ("count = '") + n + "'");
        }
        // 5. Not a text file at all.
        {
            juce::String binary;
            for (int i = 0; i < 512; ++i) binary += juce::String::charToString ((juce::juce_wchar) (i % 256));
            tryOne (binary, "512 bytes of binary");
            tryOne ("", "empty file");
            tryOne (juce::String::repeatedString ("A", 100000), "100 kB of one letter");
        }

        std::printf ("    %d mutations: %d accepted, %d refused\n", accepted + refused, accepted, refused);
        check (bad == 0, "every mutation the parser ACCEPTS is a playable tuning "
                         "(a finite, positive frequency on all 128 keys)");
        if (bad != 0)
            std::printf ("    %d accepted but unplayable; first was: %s\n", bad, firstBad.toRawUTF8());
        // ...and it must not refuse EVERYTHING either, or the check above is vacuous.
        check (accepted > 0, "and the sweep is not vacuous: some mutations are still valid scales");
    }

    //--------------------------------------------------------------------------
    // AnaMark .tun. A .tun states 128 ABSOLUTE pitches, so the test that matters is
    // note-for-note: "it loaded" proves nothing about what the keys now sound at.
    std::printf ("AnaMark .tun: 12ED2 round trip (every key, 0.01 cent):\n");
    {
        double f[128];
        for (int n = 0; n < 128; ++n) f[n] = 440.0 * std::pow (2.0, (n - 69) / 12.0);

        TuningManager t; juce::String err;
        const bool ok = t.loadTunString (makeTun (f), "12ed2", err);
        t.setMode (TuningManager::Mode::Scala);
        check (ok, ("parsed 12ED2 .tun (" + err + ")").toRawUTF8());
        const double worst = worstCents (t, f);
        std::printf ("    worst error over 128 keys: %.8f cents\n", worst);
        check (worst <= 0.01, "every key within 0.01 cents of the file");
        check (t.degreesPerPeriod() == 12, "12 degrees per period (not the trivial 1-per-100c)");
        check (std::abs (t.periodRatio() - 2.0) < 1e-9, "period is 2/1");
    }

    std::printf ("AnaMark .tun: 31-EDO round trip (prime EDO, 1/1 off the key grid):\n");
    {
        // 1/1 deliberately NOT on middle C and NOT at a standard pitch: this catches a
        // converter that quietly re-anchors the scale to its own reference.
        double f[128];
        for (int n = 0; n < 128; ++n) f[n] = 256.0 * std::pow (2.0, (n - 57) / 31.0);

        TuningManager t; juce::String err;
        const bool ok = t.loadTunString (makeTun (f), "31edo", err);
        t.setMode (TuningManager::Mode::Scala);
        check (ok, ("parsed 31-EDO .tun (" + err + ")").toRawUTF8());
        const double worst = worstCents (t, f);
        std::printf ("    worst error over 128 keys: %.8f cents\n", worst);
        check (worst <= 0.01, "every key within 0.01 cents of the file");
        check (t.degreesPerPeriod() == 31, "31 degrees per period");
        check (std::abs (t.periodRatio() - 2.0) < 1e-9, "period is 2/1");
    }

    std::printf ("AnaMark .tun: basefreq is honoured (A=432 stays A=432):\n");
    {
        double f[128];
        for (int n = 0; n < 128; ++n) f[n] = 432.0 * std::pow (2.0, (n - 69) / 12.0);

        TuningManager t; juce::String err;
        check (t.loadTunString (makeTun (f), "a432", err), "parsed A=432 .tun");
        t.setMode (TuningManager::Mode::Scala);
        std::printf ("    A4 loaded as %.6f Hz\n", t.frequencyForNote (69));
        check (centsApart (t.frequencyForNote (69), 432.0) <= 0.01, "A4 = 432, not 440");
        check (centsApart (t.frequencyForNote (60), 432.0 * std::pow (2.0, -9.0 / 12.0)) <= 0.01,
               "C4 moved with it (the whole file, not just A)");
    }

    std::printf ("AnaMark .tun: 1/4-comma meantone at A=415 (irregular scale, real world):\n");
    {
        // Not an equal division: the degrees differ from each other, so a converter
        // that assumed equal steps would be caught here and nowhere above.
        const double mt[12] = { 0.0, 76.049, 193.157, 310.265, 386.314, 503.422,
                                579.471, 696.578, 772.627, 889.735, 1006.843, 1082.892 };
        double f[128];
        for (int n = 0; n < 128; ++n)
        {
            const int oct = (int) std::floor ((n - 60) / 12.0);   // degree 0 = C
            const int deg = (n - 60) - oct * 12;
            f[n] = 415.0 * std::pow (2.0, (mt[deg] - mt[9]) / 1200.0) * std::pow (2.0, (double) oct);
        }
        TuningManager t; juce::String err;
        check (t.loadTunString (makeTun (f), "meantone", err), "parsed meantone .tun");
        t.setMode (TuningManager::Mode::Scala);
        const double worst = worstCents (t, f);
        std::printf ("    worst error over 128 keys: %.8f cents\n", worst);
        check (worst <= 0.01, "every key within 0.01 cents of the file");
        check (centsApart (t.frequencyForNote (69), 415.0) <= 0.01, "A4 = 415");
    }

    std::printf ("AnaMark .tun: tolerant of comments, case and whitespace:\n");
    {
        double f[128];
        for (int n = 0; n < 128; ++n) f[n] = 440.0 * std::pow (2.0, (n - 69) / 12.0);

        juce::String s;
        s << "# written by some other editor\n"
          << "[Info]\nName=whatever it likes\n\n"
          << "  [ EXACT TUNING ]   ; the section that counts\n"
          << "\tBaseFreq = " << juce::String (f[0], 10) << "   ; MIDI 0\n";
        for (int n = 0; n < 128; ++n)
            s << "  NOTE" << n << "\t=  "
              << juce::String (1200.0 * std::log2 (f[n] / f[0]), 6) << " ; key " << n << "\n";

        TuningManager t; juce::String err;
        check (t.loadTunString (s, "messy", err), ("parsed messy .tun (" + err + ")").toRawUTF8());
        t.setMode (TuningManager::Mode::Scala);
        check (worstCents (t, f) <= 0.01, "messy .tun still exact to 0.01 cents");
        check (TuningManager::looksLikeTun (s), "content sniff recognises it as .tun");
    }

    std::printf ("AnaMark .tun: v1 fallback to the integer [Tuning] block:\n");
    {
        // No [Exact Tuning] at all: integer cents are all there is, so the result must
        // match those integers exactly - not be silently "corrected" to 12-TET.
        double f[128];
        for (int n = 0; n < 128; ++n) f[n] = 440.0 * std::pow (2.0, (n - 69) / 12.0);

        juce::String s;
        s << "; AnaMark TUN v1\n[Tuning]\n";
        for (int n = 0; n < 128; ++n)
            s << "note " << n << "=" << juce::String (100 * n) << "\n";

        TuningManager t; juce::String err;
        check (t.loadTunString (s, "v1", err), ("parsed v1 .tun (" + err + ")").toRawUTF8());
        t.setMode (TuningManager::Mode::Scala);
        // v1 has no basefreq: the standard's own MIDI-0 = 8.1757989156 Hz applies,
        // which is 12-TET at A=440.
        double worst = 0.0;
        for (int n = 0; n < 128; ++n)
            worst = std::max (worst, centsApart (t.frequencyForNote (n), f[n]));
        std::printf ("    worst error vs 12-TET A=440: %.8f cents\n", worst);
        check (worst <= 0.01, "v1 integer cents land on 12-TET A=440");
    }

    //--------------------------------------------------------------------------
    // A non-repeating .tun is held as an ABSOLUTE 128-key table. It used to be refused,
    // which made ordinary .tun content - a stretched piano, per-note tweaks - unloadable.
    std::printf ("AnaMark .tun: a stretched (non-repeating) mapping loads as a table:\n");
    {
        // A Railsback-like piano curve: flat in the bass, sharp in the treble, ~+-8 cents
        // at the ends. Every interval therefore changes with position, so NO key distance
        // repeats exactly and the scale path (degrees + period) cannot hold it.
        double f[128];
        for (int n = 0; n < 128; ++n)
        {
            const double d = (double) (n - 60);
            f[n] = 440.0 * std::pow (2.0, ((n - 69) * 100.0 + 0.002 * d * std::abs (d)) / 1200.0);
        }

        // Load a scale first, so the table has to actually REPLACE it (and so none of the
        // table metadata below can be a leftover from the scale).
        TuningManager t; juce::String err;
        {
            double ref[128];
            for (int i = 0; i < 128; ++i) ref[i] = 440.0 * std::pow (2.0, (i - 69) / 12.0);
            juce::String e2;
            check (t.loadTunString (makeTun (ref), "12ed2", e2), "12ED2 seed scale loaded");
            check (! t.usesAbsoluteTable(), "the seed is a SCALE, not a table");
        }
        t.setMode (TuningManager::Mode::Scala);

        const bool ok = t.loadTunString (makeTun (f), "railsback", err);
        check (ok, ("stretched .tun loaded (" + err + ")").toRawUTF8());
        check (t.usesAbsoluteTable(), "held as an absolute 128-key table");

        const double worst = worstCents (t, f);
        std::printf ("    worst error over 128 keys: %.8f cents\n", worst);
        check (worst <= 0.01, "every key within 0.01 cents of the file");

        // The estimate: a stretched octave scale is 2/1, a couple of cents out.
        const auto est = t.getTablePeriodEstimate();
        std::printf ("    estimated period: %.6f (%d keys), worst deviation %.3f cents, %s\n",
                     est.ratio, est.keys, est.devCents,
                     est.snapped ? "canonical" : "raw measurement");
        check (est.found, "a period was found");
        check (est.snapped && est.p == 2 && est.q == 1, "estimated period is 2/1");
        check (std::abs (t.periodRatio() - 2.0) < 1e-12, "periodRatio() answers 2/1");
        check (t.degreesPerPeriod() == 12, "12 keys per period (not the trivial 1-per-100c)");
        check (est.devCents > 0.01 && est.devCents < 6.0,
               "the deviation is reported, not hidden (a few cents)");
    }

    std::printf ("Table mode: the period is METADATA - it cannot move a pitch:\n");
    {
        double f[128];
        for (int n = 0; n < 128; ++n)
        {
            const double d = (double) (n - 60);
            f[n] = 440.0 * std::pow (2.0, ((n - 69) * 100.0 + 0.002 * d * std::abs (d)) / 1200.0);
        }
        TuningManager t; juce::String err;
        check (t.loadTunString (makeTun (f), "railsback", err), "table loaded");
        t.setMode (TuningManager::Mode::Scala);
        const double before = worstCents (t, f);

        juce::String perr;
        check (t.setTablePeriodSpec ("3/1", perr), ("period override accepted (" + perr + ")").toRawUTF8());
        check (t.tablePeriodIsUserSet(), "the period is now the user's");
        check (std::abs (t.periodRatio() - 3.0) < 1e-12, "periodRatio() follows the override");
        std::printf ("    3/1 override -> %d keys per period, %.3f cents off\n",
                     t.degreesPerPeriod(), t.tablePeriodDevCents());
        check (t.degreesPerPeriod() == 19, "19 keys span a tritave in a 100-cent-step table");
        const double after = worstCents (t, f);
        std::printf ("    worst key error before %.8f -> after %.8f cents\n", before, after);
        check (after <= 0.01 && after == before, "NOT ONE of the 128 pitches moved");

        check (! t.setTablePeriodSpec ("banana", perr), "unreadable period refused");
        check (std::abs (t.periodRatio() - 3.0) < 1e-12, "and the accepted one still stands");

        t.resetTablePeriodToEstimate();
        check (! t.tablePeriodIsUserSet() && std::abs (t.periodRatio() - 2.0) < 1e-12,
               "reset goes back to the estimate");
        check (worstCents (t, f) == before, "the reset moved no pitch either");
    }

    std::printf ("Table mode: a stretched TRITAVE table estimates 3/1, not 2/1:\n");
    {
        // Proof that the estimate follows the file rather than assuming an octave: 13
        // steps per (stretched) 3/1, Bohlen-Pierce-like.
        const double step = 1200.0 * std::log2 (3.0) / 13.0;
        double f[128];
        for (int n = 0; n < 128; ++n)
        {
            const double d = (double) (n - 60);
            f[n] = 261.6255653 * std::pow (2.0, (d * step + 0.002 * d * std::abs (d)) / 1200.0);
        }
        TuningManager t; juce::String err;
        check (t.loadTunString (makeTun (f), "bp-stretched", err), "stretched tritave table loaded");
        t.setMode (TuningManager::Mode::Scala);
        check (t.usesAbsoluteTable(), "held as a table (it does not repeat)");
        const auto est = t.getTablePeriodEstimate();
        std::printf ("    estimated period: %.6f (%d keys), %.3f cents off\n",
                     est.ratio, est.keys, est.devCents);
        check (est.snapped && est.p == 3 && est.q == 1, "estimated period is 3/1");
        check (t.degreesPerPeriod() == 13, "13 keys per tritave");
        check (worstCents (t, f) <= 0.01, "every key still exact to 0.01 cents");
    }

    std::printf ("Table mode: survives a save/reload round trip (source + period):\n");
    {
        double f[128];
        for (int n = 0; n < 128; ++n)
        {
            const double d = (double) (n - 60);
            f[n] = 440.0 * std::pow (2.0, ((n - 69) * 100.0 + 0.002 * d * std::abs (d)) / 1200.0);
        }
        TuningManager t; juce::String err;
        check (t.loadTunString (makeTun (f), "railsback", err), "table loaded");
        t.setMode (TuningManager::Mode::Scala);
        check (t.setTablePeriodSpec ("1200.0c", err), "user period set");

        // Exactly what getStateInformation stores and setStateInformation gives back.
        const auto snap = t.captureTuning();
        check (snap.source.isNotEmpty(), "the state carries the tuning source");
        check (snap.periodSpec == "1200.0c", "and the period the user chose");

        TuningManager t2; juce::String err2;
        check (t2.restoreTuning (snap, err2), ("restored into a fresh instance (" + err2 + ")").toRawUTF8());
        t2.setMode (TuningManager::Mode::Scala);
        check (t2.usesAbsoluteTable(), "still a table after the round trip");
        const double worst = worstCents (t2, f);
        std::printf ("    worst error over 128 keys after reload: %.8f cents\n", worst);
        check (worst <= 0.01, "every key comes back within 0.01 cents");
        check (t2.tablePeriodIsUserSet(), "the period is still the USER's, not the estimate");
        check (std::abs (t2.periodRatio() - 2.0) < 1e-9, "and it is the one they chose");
        check (t2.degreesPerPeriod() == 12, "degrees per period come back with it");
    }

    std::printf ("A .tun that DOES repeat still takes the scale path:\n");
    {
        double f[128];
        for (int n = 0; n < 128; ++n) f[n] = 440.0 * std::pow (2.0, (n - 69) / 12.0);
        TuningManager t; juce::String err;
        check (t.loadTunString (makeTun (f), "12ed2", err), "12ED2 .tun loaded");
        t.setMode (TuningManager::Mode::Scala);
        check (! t.usesAbsoluteTable(), "NOT a table: degrees + period are available");
        double r = 0.0;
        check (t.scalaDegreeRatio (7, r) && std::abs (1200.0 * std::log2 (r) - 700.0) < 1e-6,
               "the scale's own degree ratios are readable");
        // And the table path must NOT pretend to have degrees.
        double f2[128];
        for (int n = 0; n < 128; ++n)
        {
            const double d = (double) (n - 60);
            f2[n] = 440.0 * std::pow (2.0, ((n - 69) * 100.0 + 0.002 * d * std::abs (d)) / 1200.0);
        }
        check (t.loadTunString (makeTun (f2), "railsback", err), "stretched .tun loaded over it");
        check (! t.scalaDegreeRatio (7, r), "a table reports NO degree ratios (nothing invents them)");
        long long p = 0, q = 0;
        check (! t.scalaDegreeRational (0, p, q), "and no rational degree sources either");
    }

    std::printf ("AnaMark .tun: malformed files are refused without crashing:\n");
    {
        TuningManager t; juce::String err;
        check (! t.loadTunString ("", "empty", err), "empty file refused");
        check (! t.loadTunString ("just some text\nwith no sections", "text", err),
               "text with no sections refused");
        check (! t.loadTunString ("[Exact Tuning]\nbasefreq=440\n", "nonotes", err),
               "basefreq but no notes refused");
        check (! t.loadTunString ("[Exact Tuning]\nbasefreq=abc\nnote 0=0\n", "badbase", err),
               "non-numeric basefreq refused");
        check (! t.loadTunString ("[Exact Tuning]\nbasefreq=440\nnote 0=0\nnote 1=x\n", "badnote", err),
               "non-numeric note value refused");
        {
            // 100 of the 128 keys: the missing 28 would have to be invented.
            juce::String s ("[Exact Tuning]\nbasefreq=8.1757989156\n");
            for (int n = 0; n < 100; ++n) s << "note " << n << "=" << juce::String (100 * n) << "\n";
            check (! t.loadTunString (s, "partial", err), "partial (100/128 keys) .tun refused");
            std::printf ("    error: %s\n", err.toRawUTF8());
        }
        {
            // Every key the same pitch. There is no period here at all, but the 128 pitches
            // are perfectly usable, so the table takes it and says the period is unknown
            // instead of refusing a file whose every key it can sound exactly.
            juce::String s ("[Exact Tuning]\nbasefreq=440\n");
            for (int n = 0; n < 128; ++n) s << "note " << n << "=0\n";
            check (t.loadTunString (s, "flat", err), "all-128-keys-identical .tun loads as a table");
            t.setMode (TuningManager::Mode::Scala);
            check (t.usesAbsoluteTable(), "held as a table");
            check (centsApart (t.frequencyForNote (0), 440.0) <= 0.01
                   && centsApart (t.frequencyForNote (127), 440.0) <= 0.01, "every key is 440 Hz, as written");
            check (! t.getTablePeriodEstimate().found, "and NO period is claimed for it");
        }
    }

    //======================================================================================
    std::printf ("the same treatment for a .tun: 128 absolute frequencies, mutated:\n");
    {
        // THE SECOND FILE TYPE A USER DRAGS IN, and a completely different parser: an
        // INI with sections, keys and 128 absolute frequencies, where the .scl side is a
        // list of intervals. The failure modes are its own - a section header that is
        // gone, a key that repeats, a basefreq that is text - so it gets its own sweep
        // rather than trusting the .scl one to have covered the idea.
        //
        // Same two claims, and the same reason the second is the one that matters: a
        // parser that says yes to rubbish silently retunes the user's whole project.
        // TWO BASES, because loadTunString has two paths and one of them was getting all
        // the traffic: a REPEATING table is converted to a .scl and goes through
        // loadScalaString, so a 12-TET .tun never reaches loadAbsoluteTable at all. The
        // second base is deliberately irregular - no interval repeats per octave - so the
        // absolute path is swept too. Found by sabotaging the absolute path and watching
        // this check not notice.
        double f12[128], fIrr[128];
        for (int n = 0; n < 128; ++n)
        {
            f12[n]  = 440.0 * std::pow (2.0, (n - 69) / 12.0);
            fIrr[n] = 55.0 * std::pow (2.0, n / 12.0 + 0.011 * std::sin (n * 0.7));
        }
        const juce::String base = makeTun (f12);
        const juce::String baseIrr = makeTun (fIrr);

        int accepted = 0, refused = 0, bad = 0;
        juce::String firstBad;
        const auto tryTun = [&] (const juce::String& text, const juce::String& what)
        {
            TuningManager t; juce::String err;
            t.setMode (TuningManager::Mode::Scala);   // .tun loads into the same table
            if (! t.loadTunString (text, "tunfuzz", err)) { ++refused; return; }
            ++accepted;
            bool good = std::isfinite (t.periodRatio()) && t.periodRatio() > 0.0;
            for (int n = 0; good && n < 128; ++n)
            {
                const double fr = t.frequencyForNote (n);
                if (! std::isfinite (fr) || fr <= 0.0) good = false;
            }
            if (! good) { ++bad; if (firstBad.isEmpty()) firstBad = what; }
        };

        // Truncation, which for an INI means losing whole sections from the bottom up.
        for (int i = 0; i <= 64; ++i)
            tryTun (base.substring (0, base.length() * i / 64), "truncated at " + juce::String (i) + "/64");
        // The structural pieces, each broken in turn: the section headers, the base
        // frequency, and the note keys' numbering.
        for (const auto& r : { std::pair<const char*, const char*> { "[Exact Tuning]", "[Exact Tuning" },
                               { "[Exact Tuning]", "[Exakt Tuning]" },
                               { "[Exact Tuning]", "" },
                               { "[Tuning]", "" },
                               { "basefreq=", "basefreq" },
                               { "basefreq=", "basefreq=abc" },
                               { "basefreq=", "basefreq=-1e9" },
                               { "basefreq=", "basefreq=0" },
                               { "note 0=", "note -1=" },
                               { "note 0=", "note 999=" },
                               { "note 0=", "note x=" },
                               { "note 0=", "note 0=nan" },
                               { "note 0=", "note 0=1e400" },
                               { "note 1=", "note 1=" } })
            tryTun (base.replace (r.first, r.second), juce::String (r.first) + " -> '" + r.second + "'");
        // Every line deleted, one at a time, across the whole file.
        {
            juce::StringArray lines;
            lines.addLines (base);
            for (int i = 0; i < lines.size(); ++i)
            {
                auto del = lines; del.remove (i);
                tryTun (del.joinIntoString ("\n"), "line " + juce::String (i) + " deleted");
            }
        }
        // ...and not a text file at all.
        // ...and the whole sweep again on the irregular base, which is the only way the
        // absolute-table path is reached at all.
        for (int i = 0; i <= 64; ++i)
            tryTun (baseIrr.substring (0, baseIrr.length() * i / 64),
                    "irregular, truncated at " + juce::String (i) + "/64");
        {
            juce::StringArray lines;
            lines.addLines (baseIrr);
            for (int i = 0; i < lines.size(); ++i)
            {
                auto del = lines; del.remove (i);
                tryTun (del.joinIntoString ("\n"), "irregular, line " + juce::String (i) + " deleted");
            }
        }
        tryTun (baseIrr.replace ("basefreq=", "basefreq=0"), "irregular, basefreq 0");
        tryTun (baseIrr.replace ("[Exact Tuning]", ""), "irregular, no Exact Tuning section");

        tryTun ("", "empty file");
        tryTun (juce::String::repeatedString ("\n", 5000), "5000 blank lines");

        std::printf ("    %d mutations: %d accepted, %d refused\n", accepted + refused, accepted, refused);
        check (bad == 0, "every .tun mutation the parser ACCEPTS gives a finite, positive "
                         "frequency on all 128 keys");
        if (bad != 0)
            std::printf ("    %d accepted but unplayable; first was: %s\n", bad, firstBad.toRawUTF8());
        check (accepted > 0, "and the .tun sweep is not vacuous either");
    }

    //==========================================================================
    // A DEGREE NUMBER IS AN INDEX INTO *THIS* SCALE, 0..N-1 - never a semitone.
    //
    // "los resultados de tuning info aparecen con numeros que no corresponden a los
    //  numeros de tunings que no son 12edo, tipo en una pentatonica me dice 7 y 11 como
    //  recomendaciones de acordes en ese tuning, cuando el periodo es 2"
    //
    // Everything in the UI that prints a degree as a NUMBER indexes the loaded scale by
    // degreesPerPeriod(): the recommendation chips, the Degrees list, the keyboard hues.
    // So the number that source hands out has to be the loaded scale's own count for
    // every shape of tuning, and each degree it names has to sit inside one period.
    // If this ever answered 12 for a five-note scale, every readout downstream would
    // print semitones and call them degrees.
    {
        std::printf ("\ndegree numbers belong to the loaded scale, whatever its shape:\n");

        // Checks the whole contract in one place: N degrees, and degrees 0..N-1 all
        // ascending inside one period (degree N IS the period).
        auto degreesRunFrom0 = [] (TuningManager& t, int expectN, const char* what)
        {
            const int N = t.degreesPerPeriod();
            std::printf ("  %-22s -> %d degrees, period %.5f\n", what, N, t.periodRatio());
            check (N == expectN, what);
            bool inside = N > 0, ascending = true;
            double prev = 0.0;
            for (int d = 0; d < N; ++d)
            {
                double r = 0.0;
                if (! t.scalaDegreeRatio (d, r)) { inside = false; break; }
                if (! (r >= 1.0 - 1.0e-9 && r < t.periodRatio() - 1.0e-9)) inside = false;
                if (r <= prev - 1.0e-9) ascending = false;
                prev = r;
            }
            check (inside, "  ...and degrees 0..N-1 all land inside ONE period");
            check (ascending, "  ...in order, so a bigger number is never a lower pitch");
        };

        juce::String err;
        {   // (1) A five-note scale: the user's pentatonic. Legal numbers are 0..4.
            TuningManager t;
            check (t.loadScalaString ("! penta.scl\nmajor pentatonic\n 5\n!\n 9/8\n 5/4\n 3/2\n 5/3\n 2/1\n",
                                      "penta", err), "a five-note pentatonic loads");
            t.setMode (TuningManager::Mode::Scala);
            degreesRunFrom0 (t, 5, "5-note pentatonic");
        }
        {   // (2) 19-EDO: more degrees than 12, so a 12-wide table would UNDER-count.
            juce::String s ("! edo19.scl\n19-EDO\n 19\n!\n");
            for (int i = 1; i <= 19; ++i) s << " " << juce::String (1200.0 * (double) i / 19.0, 6) << "\n";
            TuningManager t;
            check (t.loadScalaString (s, "edo19", err), "a 19-EDO scale loads");
            t.setMode (TuningManager::Mode::Scala);
            degreesRunFrom0 (t, 19, "19-EDO");
        }
        {   // (3) Bohlen-Pierce: 13 degrees to a 3/1 period. Both numbers are unusual,
            //     and a readout that assumed an octave would place every degree wrongly.
            juce::String s ("! bp.scl\nBohlen-Pierce equal\n 13\n!\n");
            for (int i = 1; i <= 13; ++i)
                s << " " << juce::String (1200.0 * std::log2 (3.0) * (double) i / 13.0, 6) << "\n";
            TuningManager t;
            check (t.loadScalaString (s, "bp13", err), "a Bohlen-Pierce 3/1 scale loads");
            t.setMode (TuningManager::Mode::Scala);
            check (std::abs (t.periodRatio() - 3.0) < 1.0e-6, "  its period really is 3/1, not 2/1");
            degreesRunFrom0 (t, 13, "Bohlen-Pierce 13");
        }
        {   // (4) An absolute .tun table: the file states 128 pitches and NO degrees, so
            //     the count is how many KEYS span the period. 24 keys to the octave here,
            //     which must be answered as 24 - not as the file's key count and not as 12.
            // STRETCHED, so it cannot be reduced back to a .scl: any table whose period
            // repeats exactly IS a scale and is converted to one, and then this is not
            // testing the table path at all. Quarter-tone steps that widen as they climb.
            double f[128];
            for (int n = 0; n < 128; ++n)
                f[n] = 8.1757989156 * std::pow (2.0, (50.0 * (double) n + 0.004 * (double) (n * n)) / 1200.0);
            TuningManager t;
            check (t.loadTunString (makeTun (f), "tun24", err), "a quarter-tone .tun table loads");
            t.setMode (TuningManager::Mode::Scala);
            check (t.usesAbsoluteTable(), "  held as an absolute table (no degrees in the file)");
            const auto est = t.getTablePeriodEstimate();
            std::printf ("  %-22s -> %d keys per period, period %.5f (estimate: %d keys, found %d)\n",
                         "stretched .tun table", t.degreesPerPeriod(), t.periodRatio(),
                         est.keys, (int) est.found);
            check (est.found && t.degreesPerPeriod() == est.keys,
                   "  the degree count is the KEYS the FILE's own period spans, not a constant");
            check (t.degreesPerPeriod() == 24,
                   "  quarter-tone steps -> 0..23, so no readout can print a 12-ET semitone here");
        }
    }

    //--------------------------------------------------------------------------
    // equalDivisionsOfOctave: the gate the keyboard's NOTE NAMES hang off. Ups-and-downs
    // spelling names the degrees of an equal division of the OCTAVE, so anything else
    // has to answer 0 and keep its degree numbers. The failure this guards against is
    // not a crash - it is a keyboard confidently printing "C D E F G A B" over a scale
    // whose degrees are nothing of the kind.
    std::printf ("equal divisions of the octave (the note-name gate):\n");
    {
        // A helper that builds an N-EDO .scl the way a real one is written.
        const auto edoScl = [] (int n)
        {
            juce::String s;
            s << n << "-EDO\n " << n << "\n!\n";
            for (int i = 1; i <= n; ++i)
                s << " " << juce::String (1200.0 * (double) i / (double) n, 6) << "\n";
            return s;
        };

        {
            TuningManager t;  // untouched = 12-TET
            check (t.equalDivisionsOfOctave() == 12, "plain 12-TET answers 12");
        }
        for (int n : { 5, 19, 22, 31, 53 })
        {
            TuningManager t; juce::String err;
            check (t.loadScalaString (edoScl (n), "edo", err), "an N-EDO .scl loads");
            t.setMode (TuningManager::Mode::Scala);
            check (t.equalDivisionsOfOctave() == n, "  ...and is measured back as its own N");
        }
        {
            // 96-EDO: wide enough that a window starting at the reference key would run
            // off the top of the keyboard. The answer must not depend on where we asked.
            TuningManager t; juce::String err;
            t.loadScalaString (edoScl (96), "edo96", err);
            t.setMode (TuningManager::Mode::Scala);
            check (t.equalDivisionsOfOctave() == 96,
                   "96-EDO answers 96 - the measuring window slides down instead of running off key 127");
        }
        {
            // Bohlen-Pierce: 13 EQUAL steps, but of 3/1. Equal is not enough; the period
            // has to be the octave, or the letters name intervals that are not there.
            juce::String bp; bp << "Bohlen-Pierce\n 13\n!\n";
            for (int i = 1; i <= 13; ++i)
                bp << " " << juce::String (1200.0 * std::log2 (3.0) * (double) i / 13.0, 6) << "\n";
            TuningManager t; juce::String err;
            t.loadScalaString (bp, "bp", err);
            t.setMode (TuningManager::Mode::Scala);
            check (t.degreesPerPeriod() == 13, "Bohlen-Pierce has 13 degrees...");
            check (t.equalDivisionsOfOctave() == 0,
                   "  ...and answers 0: its period is 3/1, so there are no octave degrees to name");
        }
        {
            // Ptolemy's intense diatonic: an octave, seven degrees, UNequal steps.
            TuningManager t; juce::String err;
            t.loadScalaString ("Ptolemy 5-limit major\n 7\n!\n 9/8\n 5/4\n 4/3\n 3/2\n 5/3\n 15/8\n 2/1\n",
                               "ji", err);
            t.setMode (TuningManager::Mode::Scala);
            check (t.equalDivisionsOfOctave() == 0,
                   "a 5-limit JI major scale answers 0 - an octave, but not divided equally");
        }
        {
            // One cent off equal, everywhere. This is the tolerance's whole job: a scale
            // a cent away from 12-EDO is a different scale, and naming its degrees after
            // 12-EDO's would be a quiet lie about what is sounding.
            juce::String warped; warped << "almost 12-EDO\n 12\n!\n";
            for (int i = 1; i <= 12; ++i)
                warped << " " << juce::String (100.0 * (double) i + (i == 5 ? 1.0 : 0.0), 6) << "\n";
            TuningManager t; juce::String err;
            t.loadScalaString (warped, "warp", err);
            t.setMode (TuningManager::Mode::Scala);
            check (t.degreesPerPeriod() == 12, "a scale one cent off 12-EDO still has 12 degrees...");
            check (t.equalDivisionsOfOctave() == 0, "  ...and is NOT called 12-EDO");
        }
    }

    //==========================================================================
    // TEMPER MODE MUST NOT INVENT AN OCTAVE.
    //
    // "Temper to tuning" snaps every harmonic-engine partial onto the grid
    // "degree x period^k". That grid EXISTS only if the tuning states a period that
    // ASCENDS. Two legal .scl files state none:
    //
    //   * a DESCENDING scale - the last listed interval is below 1/1;
    //   * a degenerate period of exactly 1/1 - every "period" lands on the same pitch.
    //
    // periodRatio() answers 2.0 for both, because displays (key colours, degree
    // readouts) always need a finite number to lay out with. Temper used to read THAT
    // and snap the whole spectrum to a 2/1 grid appearing nowhere in the user's file,
    // with nothing on screen saying so - the user's tuning, silently changed.
    //
    // So there are now TWO questions, and this is the pair the sound path must ask:
    // statedPeriodRatio() / hasUsablePeriod() tell the truth (0.0 / false), and only
    // periodRatio() keeps the display fallback. The processor gates temper on
    // hasUsablePeriod() and the toggle says why it is refused.
    std::printf ("\ntemper mode refuses a tuning with no period, instead of inventing 2/1:\n");
    {
        auto probe = [] (const char* scl, const char* what, bool expectUsable, double expectStated)
        {
            TuningManager t; juce::String err;
            check (t.loadScalaString (scl, "probe", err), what);
            t.setMode (TuningManager::Mode::Scala);
            std::printf ("  %-26s -> stated %.5f, usable %d, display fallback %.5f\n",
                         what, t.statedPeriodRatio(), (int) t.hasUsablePeriod(), t.periodRatio());
            check (t.hasUsablePeriod() == expectUsable, "  ...usable-period verdict");
            check (std::abs (t.statedPeriodRatio() - expectStated) < 1.0e-9,
                   "  ...and the period it STATES is the file's own, never a substitute");
        };

        // (1) A degenerate period of exactly 1/1. Representable, legal to write, and no
        //     grid at all: degree x 1^k is the degree list over and over on one pitch.
        probe ("! unison.scl\nperiod of exactly 1/1\n 3\n!\n 1/1\n 1/1\n 1/1\n",
               "period-1 scale", false, 0.0);

        // (2) A DESCENDING scale: every listed interval falls. Its period is 1/2, which
        //     is not a repeat interval going up, so temper has nothing to snap to.
        probe ("! down.scl\ndescending\n 3\n!\n -300.0\n -700.0\n 1/2\n",
               "descending scale", false, 0.0);

        // (3) The control: a normal octave scale still tempers, to ITS OWN 2/1 - the fix
        //     must refuse the two above without disabling temper for everybody else.
        probe ("! penta.scl\nmajor pentatonic\n 5\n!\n 9/8\n 5/4\n 3/2\n 5/3\n 2/1\n",
               "normal 2/1 pentatonic", true, 2.0);

        // (4) ...and a non-octave period is a usable period too: the refusal is about
        //     "does it ascend", never about "is it an octave".
        probe ("! bp.scl\nBohlen-Pierce-ish\n 4\n!\n 7/5\n 9/5\n 11/5\n 3/1\n",
               "3/1 period scale", true, 3.0);

        // And the display fallback is still there for whoever needs a number: a caller
        // that only paints must never divide by zero because the scale is degenerate.
        {
            TuningManager t; juce::String err;
            t.loadScalaString ("! unison.scl\nperiod of exactly 1/1\n 2\n!\n 1/1\n 1/1\n", "u", err);
            t.setMode (TuningManager::Mode::Scala);
            check (std::abs (t.periodRatio() - 2.0) < 1.0e-12,
                   "periodRatio() still hands displays a finite 2.0 - it is the SOUND path that must ask");
        }
    }

    //==========================================================================
    // A .tun EXPORT IS A ROUND TRIP, OR IT IS NOTHING.
    //
    // The user asked for .tun export specifically so their tunings would TRAVEL. An
    // exporter that alters them is worse than none - and Fibló's did: the [Scale Begin]
    // block it wrote was ignored by its own reader, which re-guessed the structure from
    // the 128 pitches. The guess takes the SMALLEST exact repeat and only overrides it
    // for a CANONICAL period, so 9 equal degrees per 1000 cents came back as 1 degree
    // per 111.111 cents. Same frequencies to the last decimal; a different scale in
    // every readout, on the keyboard, and in what the next export then declared.
    //
    // So the assertion is the whole trip: load, export, load the export, and compare the
    // PITCHES *and* the STRUCTURE - degree count, period, and whether it is held as a
    // repeating scale at all.
    std::printf ("\n.tun export round trip - pitches AND structure survive:\n");
    {
        auto roundTrip = [] (TuningManager& src, const char* what, bool expectTable, int expectDegrees)
        {
            juce::String tun, err;
            if (! src.writeTunText (tun, err))
            { check (false, what); std::printf ("      export refused: %s\n", err.toRawUTF8()); return; }

            TuningManager back;
            if (! back.loadTunString (tun, "roundtrip", err))
            { check (false, what); std::printf ("      reload refused: %s\n", err.toRawUTF8()); return; }
            back.setMode (TuningManager::Mode::Scala);

            double worst = 0.0;
            for (int n = 0; n < 128; ++n)
                worst = std::max (worst, centsApart (back.frequencyForNote (n), src.frequencyForNote (n)));

            std::printf ("  %-24s in: %d deg / %.4fc / table %d   out: %d deg / %.4fc / table %d"
                         "   worst %.6fc\n",
                         what, src.degreesPerPeriod(), 1200.0 * std::log2 (src.periodRatio()),
                         (int) src.usesAbsoluteTable(),
                         back.degreesPerPeriod(), 1200.0 * std::log2 (back.periodRatio()),
                         (int) back.usesAbsoluteTable(), worst);

            check (worst <= 0.01, "  every one of the 128 pitches comes back (<= 0.01c)");
            check (back.usesAbsoluteTable() == expectTable,
                   expectTable ? "  a non-repeating map stays a TABLE - no scale is invented for it"
                               : "  a scale comes back as a SCALE, not flattened into a table");
            check (back.degreesPerPeriod() == expectDegrees, "  the degree count is the one that went in");
            if (! expectTable)
                check (std::abs (1200.0 * std::log2 (back.periodRatio() / src.periodRatio())) <= 0.01,
                       "  and so is the period - the export never re-slices the user's scale");
        };

        juce::String err;

        // (1) A normal octave .scl: seven unequal degrees. Key 0 is degree 3 here, not
        //     degree 0, which is exactly why the exported block has to say where degree 0
        //     sits instead of leaving a reader to assume key 0.
        {
            TuningManager t;
            check (t.loadScalaString ("Ptolemy 5-limit major\n 7\n!\n 9/8\n 5/4\n 4/3\n 3/2\n 5/3\n 15/8\n 2/1\n",
                                      "ji", err), "a 7-note just major loads");
            t.setMode (TuningManager::Mode::Scala);
            roundTrip (t, "7-note just major", false, 7);
        }

        // (2) A NON-OCTAVE, NON-CANONICAL period. This is the case the guess destroyed:
        //     nothing in kCanonPeriods is 1000 cents, so the smallest exact repeat (one
        //     equal step) won and the scale came back as 1 degree per 111.111 cents.
        {
            juce::String s ("nine equal degrees per 1000 cents\n 9\n!\n");
            for (int i = 1; i <= 9; ++i) s << " " << juce::String (1000.0 * (double) i / 9.0, 6) << "\n";
            TuningManager t;
            check (t.loadScalaString (s, "n9", err), "a 9-per-1000c scale loads");
            t.setMode (TuningManager::Mode::Scala);
            roundTrip (t, "9 per 1000c", false, 9);
        }

        // (3) Bohlen-Pierce: a non-octave period that IS canonical (3/1). It survived the
        //     guess, so it is the control that proves the fix did not break what worked.
        {
            juce::String s ("Bohlen-Pierce equal\n 13\n!\n");
            for (int i = 1; i <= 13; ++i)
                s << " " << juce::String (1200.0 * std::log2 (3.0) * (double) i / 13.0, 6) << "\n";
            TuningManager t;
            check (t.loadScalaString (s, "bp", err), "a Bohlen-Pierce 3/1 scale loads");
            t.setMode (TuningManager::Mode::Scala);
            roundTrip (t, "Bohlen-Pierce 13 per 3/1", false, 13);
        }

        // (4) An ABSOLUTE, NON-REPEATING .tun - a stretched piano curve. It has no scale,
        //     so the export must declare none and the reload must stay a table. Inventing
        //     a scale here would be the same lie in the other direction.
        {
            double fr[128];
            for (int n = 0; n < 128; ++n)
                fr[n] = 8.1757989156 * std::pow (2.0, (100.0 * (double) n + 0.004 * (double) (n * n)) / 1200.0);
            TuningManager t;
            check (t.loadTunString (makeTun (fr), "stretched", err), "a stretched .tun table loads");
            t.setMode (TuningManager::Mode::Scala);
            check (t.usesAbsoluteTable(), "  and is held as an absolute table");
            roundTrip (t, "stretched .tun table", true, t.degreesPerPeriod());
        }

        // (5) THE USER'S OWN SCALES, read off disk exactly as the plugin reads them. These
        //     are the files this whole feature exists for; a round trip that works only on
        //     scales written by the test is not the promise that was made.
        for (const char* fn : { "scales/19_edo.scl", "scales/just_intonation_major.scl" })
        {
            juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (fn));
            if (! f.existsAsFile())
            { std::printf ("  (skipped %s - not found from %s)\n", fn,
                           juce::File::getCurrentWorkingDirectory().getFullPathName().toRawUTF8()); continue; }
            TuningManager t;
            check (t.loadScalaFile (f, err), fn);
            t.setMode (TuningManager::Mode::Scala);
            roundTrip (t, f.getFileName().toRawUTF8(), false, t.degreesPerPeriod());
        }
    }

    //== THE CENTS PRINTED ON A KEY ARE THE SCALE'S, NOT 12-TET'S ==================
    //
    // "ESTOY USANDO 31ED2 Y LOS VALORES EN CENTS QUE ME SALEN EN LAS TECLAS NO TIENEN
    //  SENTIDO, ME DICE VALORES DE 100 EN 100 EN LAS PRIMERAS TECLAS, CUANDO DEBERIAN
    //  AUMENTAR DE 30 Y PICO EN 30 Y PICO... EL SONIDO ESTABA BIEN, ERA LA REPRESENTACION
    //  EN CENTS, EL NUMERO DE TECLAS POR PERIODO ERA CORRECTO, 31."
    //
    // The key labels come from degreeRatio(), the same call the chord search, the
    // dissonance lists and the .scl export all read - so if it answers 12-TET while the
    // sound is right, four separate readouts are lying at once and only the loudest one
    // got noticed. Measured here on BOTH ways a 31ed2 can arrive: as a .scl (a real
    // degree list) and as an AnaMark .tun (128 absolute keys, no degree list at all),
    // because those take different branches inside degreeRatio().
    {
        std::printf ("\ndegree cents for 31ed2, both ways it can be loaded:\n");
        const double step = 1200.0 / 31.0;

        const auto degreeCents = [] (const TuningManager& t, int d)
        { return 1200.0 * std::log2 (juce::jmax (1.0e-12, t.degreeRatio (d))); };

        const auto report = [&] (TuningManager& t, const char* how)
        {
            std::printf ("  %-22s %d degrees/period, first six:", how, t.degreesPerPeriod());
            for (int d = 0; d < 6; ++d) std::printf (" %.1f", degreeCents (t, d));
            std::printf ("  (a step is %.1f)\n", step);
            check (t.degreesPerPeriod() == 31, (juce::String (how) + ": 31 degrees per period").toRawUTF8());
            double worst = 0.0; int worstD = 0;
            for (int d = 1; d <= 31; ++d)
            {
                const double err = std::abs (degreeCents (t, d) - step * (double) d);
                if (err > worst) { worst = err; worstD = d; }
            }
            std::printf ("    worst degree error: %.4f cents at degree %d\n", worst, worstD);
            check (worst < 0.05,
                   (juce::String (how) + ": every degree's cents are the scale's own, "
                    "not a 12-TET semitone grid").toRawUTF8());
        };

        juce::String err;
        {
            // As a .scl: 31 cents values, written the way Scala Workshop writes them.
            juce::String scl ("! 31ed2.scl\n!\n31 equal divisions of the octave\n 31\n!\n");
            for (int d = 1; d <= 31; ++d) scl << " " << juce::String (step * (double) d, 6) << "\n";
            TuningManager t;
            check (t.loadScalaString (scl, "31ed2.scl", err), "a 31ed2 .scl loads");
            t.setMode (TuningManager::Mode::Scala);
            report (t, ".scl degree list");
        }
        {
            // As a .tun: 128 absolute keys and no degree list. degreeRatio() takes its
            // OTHER branch here - it reads the key the degree lands on - and that branch
            // is the one a 12-TET answer would come out of.
            juce::String tun ("[Tuning]\n");
            for (int k = 0; k < 128; ++k)
                tun << "note " << k << "=" << juce::String (step * (double) (k - 60), 6) << "\n";
            TuningManager t;
            check (t.loadTunString (tun, "31ed2.tun", err), "a 31ed2 .tun loads");
            t.setMode (TuningManager::Mode::Scala);
            std::printf ("  (.tun held as an absolute table: %d)\n", (int) t.usesAbsoluteTable());
            report (t, ".tun absolute table");
        }
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILURES",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
