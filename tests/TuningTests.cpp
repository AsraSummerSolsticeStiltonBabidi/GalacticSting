// Unit tests for the microtuning subsystem (TuningManager).
// Verifies Scala .scl parsing and the note->frequency mapping.
//
//   build\...\TuningTests.exe  -> exit code 0 = all passed, non-zero = failures.

#include "dsp/TuningManager.h"

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

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILURES",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
