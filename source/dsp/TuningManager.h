#pragma once

//==============================================================================
// TuningManager: turns a MIDI note number into a frequency in Hz, using one of
// three tuning sources. It is the single authority the voices ask for pitch, so
// the whole synth is microtuning-aware.
//
//   * Equal        - standard 12-tone equal temperament (A4 = 440 Hz).
//   * MTS-ESP      - follow a connected ODDSound MTS-ESP master (Ableton can host
//                    one globally). If no master is present it transparently
//                    falls back to 12-TET, so it is always safe to select.
//   * Scala (.scl) - a user-loaded Scala scale file, any number of notes per
//                    period, ratios or cents. 1/1 is anchored to middle C.
//                    Ableton .ascl files are a superset of .scl (extra data lives
//                    in "! @ABL ..." comment lines); we honour @ABL REFERENCE_PITCH
//                    so the file's own anchor octave/degree/frequency wins.
//
// Note that Ableton Live 12's tuning systems / Microtuner do NOT hand the plugin
// a file: they retune via per-note MPE pitch bend (range +-48 st), which the
// synth follows in any mode. Loading the same .ascl here is for standalone use.
//
// Real-time contract: frequencyForNote() is called from the audio thread once
// per block per voice. It only reads atomics / the MTS shared-memory table, so
// it never allocates or locks. loadScala*() runs on the message thread and
// publishes a new 128-note table by storing atomics; a note may briefly read a
// mix of old/new entries across different keys, but every single value is read
// atomically (no torn doubles) so the worst case is one block at the old pitch.
//==============================================================================

#include <juce_core/juce_core.h>
#include "libMTSClient.h"

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

class TuningManager
{
public:
    enum class Mode { Equal = 0, MtsEsp = 1, Scala = 2 };

    // Where a scale degree came from - a genuine RATIONAL (written as a ratio "3/2"
    // or a bare integer "3") vs. a CENTS/tempered value ("701.955"). This is the
    // SOURCE of just-ness: only a rational-sourced degree is a true JI interval;
    // a cents value only ever APPROXIMATES one (the sole exception, 1200c = 2/1, is
    // handled by the display). Kept per-degree so the UI can show an exact ratio when
    // it is one and an "≈p/q *" approximation (never a false exact ratio) when it is not.
    struct DegreeSrc { bool rational = false; long long p = 1, q = 1; };

    // Anchor: scale degree 0 (the 1/1) sits on middle C. With a 12-TET .scl this
    // reproduces standard tuning (key 69 = A = 440 Hz).
    static constexpr int    kRefKey  = 60;
    static constexpr double kRefFreq = 261.6255653005986; // 12-TET middle C

    TuningManager()
    {
        for (int n = 0; n < 128; ++n)
            table[(size_t) n].store (equalFreq (n), std::memory_order_relaxed);

        client = MTS_RegisterClient(); // safe even if libMTS is not installed
    }

    ~TuningManager()
    {
        if (client != nullptr)
            MTS_DeregisterClient (client);
    }

    //== Mode ==================================================================
    void setMode (Mode m) noexcept { mode.store (m, std::memory_order_relaxed); }
    Mode getMode() const noexcept  { return mode.load (std::memory_order_relaxed); }

    // Master reference: A = <hz>. Every mode is transposed by hz/440, so setting
    // A=432 moves the WHOLE instrument down by ~32 cents without changing ratios.
    void setReferenceA (double hz) noexcept
    {
        if (hz > 0.0 && std::isfinite (hz))
            refScale.store (hz / 440.0, std::memory_order_relaxed);
    }
    double referenceScale() const noexcept { return refScale.load (std::memory_order_relaxed); }

    // Whole-instrument octave transpose: multiplies every note by 2^octaves, on top
    // of the A-reference. Pure frequency scale, so it works in every tuning mode.
    void setOctaveShift (int octaves) noexcept
    {
        octScale.store (std::pow (2.0, (double) juce::jlimit (-5, 5, octaves)), std::memory_order_relaxed);
    }

    bool hasMtsMaster() const noexcept
    {
        return client != nullptr && MTS_HasMaster (client);
    }

    bool hasScala() const noexcept { return scalaValid.load (std::memory_order_relaxed); }

    // The master's OWN frequency for a key, with no reference multiplier applied -
    // so the UI can show what MTS is really doing (see the reference handling in
    // the processor's timerCallback). 0 when no master is connected.
    double mtsFrequencyForKey (int midiNote) const noexcept
    {
        if (! hasMtsMaster()) return 0.0;
        return MTS_NoteToFrequency (client, (char) juce::jlimit (0, 127, midiNote), -1);
    }

    // Degrees (notes) per period of the ACTIVE tuning, for the colour keyboard.
    // 12 for 12-TET; the loaded scale's count in Scala mode; the master's map size
    // (or 12) in MTS mode.
    int degreesPerPeriod() const noexcept
    {
        switch (mode.load (std::memory_order_relaxed))
        {
            case Mode::Scala:
                if (scalaValid.load (std::memory_order_relaxed))
                    return scalaDegreeCount.load (std::memory_order_relaxed);
                break;
            case Mode::MtsEsp:
                if (client != nullptr && MTS_HasMaster (client))
                {
                    const int m = (int) MTS_GetMapSize (client);
                    if (m > 0) return m;
                }
                break;
            case Mode::Equal:
            default:
                break;
        }
        return 12;
    }

    // Period (repeat interval) of the ACTIVE tuning as a frequency ratio: 2/1 for
    // 12-TET and for octave scales, the .scl's last listed interval in Scala mode
    // (3/1 for Bohlen-Pierce...), the master's period in MTS. Needed wherever a
    // degree index has to be wrapped into the MIDI range without losing octaves:
    // deriving it from the key table breaks for scales with >67 degrees.
    double periodRatio() const noexcept
    {
        switch (mode.load (std::memory_order_relaxed))
        {
            case Mode::Scala:
                if (scalaValid.load (std::memory_order_relaxed))
                {
                    const double p = scalaPeriod.load (std::memory_order_relaxed);
                    if (p > 1.0001 && std::isfinite (p)) return p;
                }
                break;
            case Mode::MtsEsp:
                if (client != nullptr && MTS_HasMaster (client))
                {
                    const double p = (double) MTS_GetPeriodRatio (client);
                    if (p > 1.0001 && std::isfinite (p)) return p;
                }
                break;
            case Mode::Equal:
            default:
                break;
        }
        return 2.0;
    }

    //== Audio thread =========================================================
    // Note-on gate: an MTS-ESP master with a partial keyboard map can mark keys
    // as unmapped, which must NOT sound. 12-TET and Scala never filter.
    bool shouldFilterNote (int midiNote) const noexcept
    {
        if (mode.load (std::memory_order_relaxed) == Mode::MtsEsp && client != nullptr)
            return MTS_ShouldFilterNote (client, (char) juce::jlimit (0, 127, midiNote), -1);
        return false;
    }

    // Base pitch of a MIDI note under the active tuning (before any pitch bend).
    // The A=440 reference multiplier applies to every mode ("transpose all").
    double frequencyForNote (int midiNote) const noexcept
    {
        const int n = juce::jlimit (0, 127, midiNote);
        const double ref = refScale.load (std::memory_order_relaxed)
                         * octScale.load (std::memory_order_relaxed);

        switch (mode.load (std::memory_order_relaxed))
        {
            case Mode::MtsEsp:
                // -1 channel = "don't care" (recommended for MPE). Falls back to
                // 12-TET internally when no master is connected.
                if (client != nullptr)
                    return MTS_NoteToFrequency (client, (char) n, -1) * ref;
                break;

            case Mode::Scala:
                // acquire pairs with the release in loadScalaString so the 128
                // table writes are guaranteed visible once we observe the flag.
                if (scalaValid.load (std::memory_order_acquire))
                    return table[(size_t) n].load (std::memory_order_relaxed) * ref;
                break;

            case Mode::Equal:
            default:
                break;
        }
        return equalFreq (n) * ref;
    }

    //== Message thread =======================================================
    // Loads a Scala .scl/.ascl OR an AnaMark .tun. The format is decided by the
    // CONTENT as well as the extension: a .tun saved (or renamed) as .scl would
    // otherwise go to the .scl parser, which reads its "note 12=1200" lines as
    // pitch tokens and would either fail or load a scale the file never described.
    bool loadScalaFile (const juce::File& f, juce::String& errorOut)
    {
        if (! f.existsAsFile())
        {
            errorOut = "File not found.";
            return false;
        }
        const auto text = f.loadFileAsString();
        const auto name = f.getFileNameWithoutExtension();
        if (f.hasFileExtension ("tun") || looksLikeTun (text))
            return loadTunString (text, name, errorOut);
        return loadScalaString (text, name, errorOut);
    }

    bool loadScalaString (const juce::String& text, const juce::String& name, juce::String& errorOut)
    {
        std::vector<double> intervals;
        std::vector<DegreeSrc> intervalSrc;
        juce::String desc;
        if (! parseScala (text, intervals, desc, errorOut, &intervalSrc))
            return false;

        const int count = (int) intervals.size();          // notes per period
        const double period = intervals[(size_t) count - 1]; // last listed = period

        // perDegree[0] = 1/1; perDegree[1..count-1] = listed intervals 1..count-1.
        std::vector<double> perDegree;
        perDegree.reserve ((size_t) count);
        perDegree.push_back (1.0);
        for (int j = 0; j < count - 1; ++j)
            perDegree.push_back (intervals[(size_t) j]);

        // Per-degree SOURCE (ratio vs cents), degree 0..count-1 + the period. Degree 0
        // is the exact unison 1/1; degree j = the source of listed interval j-1; the
        // period = the source of the last listed interval.
        std::vector<DegreeSrc> degSrc;
        degSrc.reserve ((size_t) count);
        degSrc.push_back ({ true, 1, 1 });
        for (int j = 1; j < count; ++j)
            degSrc.push_back (intervalSrc[(size_t) (j - 1)]);
        const DegreeSrc periodSrc = intervalSrc[(size_t) count - 1];

        // Build anchored at middle C first (degree 0 on key 60), into a local
        // buffer so an @ABL reference can rescale before publishing.
        std::array<double, 128> freqs {};
        for (int n = 0; n < 128; ++n)
        {
            const int d   = n - kRefKey;
            const int oct = (int) std::floor ((double) d / (double) count);
            const int step = d - oct * count; // 0..count-1
            freqs[(size_t) n] = kRefFreq * std::pow (period, (double) oct) * perDegree[(size_t) step];
        }

        // Ableton .ascl: "! @ABL REFERENCE_PITCH <octave> <degree> <freq>" pins
        // scale degree <degree> of octave <octave> (middle C octave = 3) to <freq>.
        // The reference frequency is computed in CLOSED FORM (same math as the
        // table build), so an anchor outside the 0..127 key range - possible for
        // large-count EDOs - still rescales exactly instead of clamping onto the
        // wrong key and silently shifting the whole tuning.
        {
            AblReference ref;
            if (parseAblReference (text, ref))
            {
                const int d    = (ref.octave - 3) * count + ref.degree; // offset from kRefKey (may exceed MIDI range)
                const int oct  = (int) std::floor ((double) d / (double) count);
                const int step = d - oct * count; // 0..count-1
                const double fAtRef = kRefFreq * std::pow (period, (double) oct) * perDegree[(size_t) step];
                const double scale  = ref.freq / fAtRef;
                if (std::isfinite (fAtRef) && fAtRef > 0.0 && std::isfinite (scale) && scale > 0.0)
                    for (auto& f : freqs) f *= scale;
            }
        }

        for (int n = 0; n < 128; ++n)
            table[(size_t) n].store (freqs[(size_t) n], std::memory_order_relaxed);

        scalaName   = name.isNotEmpty() ? name : desc;
        scalaSource = text;
        scalaPeriod.store (period, std::memory_order_relaxed);
        scalaPerDegree = perDegree;            // the AUTHORITATIVE degree ratios: the 128-key
                                               // table cannot represent a scale with >128 degrees
        scalaDegSrc    = std::move (degSrc);   // message-thread only (display/source queries)
        scalaPeriodSrc = periodSrc;
        scalaDegreeCount.store (count, std::memory_order_relaxed);
        // release: publishes all the table writes above to the audio thread.
        scalaValid.store (true, std::memory_order_release);
        return true;
    }

    //== AnaMark .tun =========================================================
    // A .tun states 128 ABSOLUTE frequencies, which is strictly more general than
    // this class's representation (degrees + period + reference). It is converted
    // to the equivalent .scl EXACTLY or not at all - see tunToScala. Going through
    // .scl text (rather than writing the 128-key table directly) is what keeps the
    // rest of the plugin working: the degree ratios, the period, the tuning bank
    // and the saved state all read the .scl source, and a table-only load would
    // leave every one of them describing the PREVIOUS scale.
    bool loadTunString (const juce::String& text, const juce::String& name, juce::String& errorOut)
    {
        juce::String scl;
        if (! tunToScala (text, name, scl, errorOut))
            return false; // nothing published: the old tuning is still the one sounding
        return loadScalaString (scl, name, errorOut);
    }

    // Cheap sniff: does this text look like an AnaMark .tun rather than a .scl?
    // A .scl has no "[section]" headers at all, so one named section is decisive.
    static bool looksLikeTun (const juce::String& text)
    {
        juce::StringArray raw;
        raw.addLines (text);
        for (const auto& l : raw)
        {
            const auto s = stripTunComment (l).trim();
            if (! s.startsWithChar ('['))
                continue;
            // Same unwrapping as parseTun: "[ EXACT TUNING ]" is as legal as
            // "[Exact Tuning]", and a sniff that only matched the tight spelling
            // would send such a file to the .scl parser.
            const auto sec = s.fromFirstOccurrenceOf ("[", false, false)
                              .upToFirstOccurrenceOf ("]", false, false).trim().toLowerCase();
            if (sec == "exact tuning" || sec == "tuning"
                || sec == "scale begin" || sec == "functional tuning")
                return true;
        }
        return false;
    }

    // Convert an AnaMark .tun (v1/v2) into the .scl that sounds IDENTICALLY.
    //
    // The file gives 128 absolute pitches; a .scl gives N degrees repeating at a
    // period, plus (via @ABL REFERENCE_PITCH) one absolute anchor. So the mapping
    // must actually repeat, and we must find the repeat: the period is the smallest
    // key distance k whose interval is the same everywhere in the file to within
    // 0.01 cents. Ties are broken towards a simple period (2/1, 3/1...) because
    // EVERY equal division is also trivially 1-periodic - reading 12ED2 as "1 note
    // per 100-cent period" sounds right but tells the keyboard, the snapping grid
    // and the UI that the scale has one degree.
    //
    // If no such k exists the file is REFUSED (nothing is loaded). A stretched or
    // per-key-mapped .tun cannot be held here, and loading a quietly flattened
    // version of someone's tuning is worse than loading none.
    static bool tunToScala (const juce::String& tunText, const juce::String& name,
                            juce::String& sclOut, juce::String& errorOut)
    {
        std::array<double, 128> f {};
        if (! parseTun (tunText, f, errorOut))
            return false;

        // Everything below works in cents above key 0 - the quantity the .tun itself
        // is written in, so no extra rounding enters the comparison.
        std::array<double, 128> c {};
        for (int n = 0; n < 128; ++n)
            c[(size_t) n] = 1200.0 * std::log2 (f[(size_t) n] / f[0]);

        constexpr double kTolCents = 0.01; // the exactness this conversion promises
        // periodRatio() is used to snap partials at period^k far outside the MIDI
        // range, so a "period" backed by two or three key pairs would retune sound
        // the file never described. Demand at least a dozen repeats as evidence.
        constexpr int kMinPairs = 12;

        int    firstK = 0, simpleK = 0;
        double firstP = 0.0, simpleP = 0.0;
        for (int k = 1; k <= 128 - kMinPairs; ++k)
        {
            const double P = c[(size_t) k] - c[0];
            if (! (P > kTolCents) || ! std::isfinite (P))
                continue; // a period has to ascend; a descending/unison one is not a repeat

            bool repeats = true;
            for (int n = 0; n + k < 128 && repeats; ++n)
                repeats = std::abs ((c[(size_t) (n + k)] - c[(size_t) n]) - P) <= kTolCents;
            if (! repeats)
                continue;

            if (firstK == 0) { firstK = k; firstP = P; }
            if (isSimplePeriod (P)) { simpleK = k; simpleP = P; break; }
        }

        const int    count       = simpleK != 0 ? simpleK : firstK;
        const double periodCents = simpleK != 0 ? simpleP : firstP;
        if (count == 0)
        {
            errorOut = "This .tun does not repeat at any interval, so it cannot be held "
                       "exactly as a scale (degrees + period). Nothing was loaded - a "
                       "silently approximated tuning would be worse than none.";
            return false;
        }

        const double periodRatio = std::pow (2.0, periodCents / 1200.0);

        // The file's own map, continued outside 0..127 by its period, so the degrees
        // can be taken from the window that starts on middle C (where loadScalaString
        // always puts degree 0) even when the scale has more degrees than the keys
        // above middle C - a 100-note EDO would otherwise run off the end of the file.
        const auto keyFreq = [&] (int n)
        {
            const int oct = (int) std::floor ((double) n / (double) count);
            return f[(size_t) (n - oct * count)] * std::pow (periodRatio, (double) oct);
        };

        const double fRef = keyFreq (kRefKey);
        if (! (fRef > 0.0) || ! std::isfinite (fRef))
        {
            errorOut = "This .tun has no usable pitch on middle C.";
            return false;
        }

        std::vector<double> degCents ((size_t) count, 0.0);
        for (int j = 0; j < count; ++j)
            degCents[(size_t) j] = 1200.0 * std::log2 (keyFreq (kRefKey + j) / fRef);

        // Self-check: reproduce all 128 keys the way loadScalaString will (degree
        // (n-60) mod count, times period^octave, anchored on fRef) and compare with
        // the file. This is the guarantee, not a debug aid: any mistake in the wrap
        // above would otherwise ship as a subtly wrong tuning.
        for (int n = 0; n < 128; ++n)
        {
            const int d    = n - kRefKey;
            const int oct  = (int) std::floor ((double) d / (double) count);
            const int step = d - oct * count;
            const double got = fRef * std::pow (periodRatio, (double) oct)
                             * std::pow (2.0, degCents[(size_t) step] / 1200.0);
            if (! (std::abs (1200.0 * std::log2 (got / f[(size_t) n])) <= kTolCents))
            {
                errorOut = "This .tun cannot be represented exactly as a scale (key "
                         + juce::String (n) + " would be off). Nothing was loaded.";
                return false;
            }
        }

        // Cents are written with 10 decimals: ~1e-7 cents of round-trip error, four
        // orders below the 0.01 cent the conversion promises.
        juce::String s;
        s << "! converted from AnaMark .tun by Fiblo Galactic - pitches are exact,\n"
          << "! and the reference pitch below carries the file's ABSOLUTE tuning.\n"
          << (name.isNotEmpty() ? name : juce::String ("AnaMark .tun"))
          << " (.tun, " << count << " per " << juce::String (periodCents, 4) << "c)\n"
          << " " << count << "\n";
        for (int j = 1; j < count; ++j)
            s << " " << juce::String (degCents[(size_t) j], 10) << "\n";
        s << " " << juce::String (periodCents, 10) << "\n"
          << "! @ABL REFERENCE_PITCH 3 0 " << juce::String (fRef, 10) << "\n";

        sclOut = s;
        return true;
    }

    // Exact ratio of scale degree `step` (0..count-1) from the loaded .scl itself,
    // NOT via the 128-key table - the table only holds 128 degree classes, so a
    // scale with more degrees than that cannot be read back through it.
    // Message thread only (same rule as scalaDegSrc).
    bool scalaDegreeRatio (int step, double& ratioOut) const
    {
        if (! scalaValid.load (std::memory_order_acquire)) return false;
        if (! juce::isPositiveAndBelow (step, (int) scalaPerDegree.size())) return false;
        ratioOut = scalaPerDegree[(size_t) step];
        return ratioOut > 0.0;
    }

    // Did scale degree `degree` (0..count-1, or `count` for the period) come from a
    // genuine RATIONAL source in the loaded .scl? Returns true + the exact p/q. Only
    // valid in Scala mode; other modes have no per-degree source (query the mode).
    bool scalaDegreeRational (int degree, long long& p, long long& q) const
    {
        if (! scalaValid.load (std::memory_order_acquire) || degree < 0)
            return false;
        const int count = scalaDegreeCount.load (std::memory_order_relaxed);
        const DegreeSrc* s = nullptr;
        if (degree == count)                               s = &scalaPeriodSrc;
        else if (degree < (int) scalaDegSrc.size())        s = &scalaDegSrc[(size_t) degree];
        if (s == nullptr || ! s->rational) return false;
        p = s->p; q = s->q;
        return true;
    }

    // Parse a .scl into its degree ratios (index 0 = 1/1) and period. Used by the
    // "Scala as timbre" feature, which is independent of the tuning table.
    static bool parseScalaScale (const juce::String& text, std::vector<double>& perDegreeOut,
                                 double& periodOut, juce::String& errorOut)
    {
        std::vector<double> intervals;
        juce::String desc;
        if (! parseScala (text, intervals, desc, errorOut))
            return false;

        const int count = (int) intervals.size();
        periodOut = intervals[(size_t) count - 1];
        perDegreeOut.clear();
        perDegreeOut.reserve ((size_t) count);
        perDegreeOut.push_back (1.0);
        for (int j = 0; j < count - 1; ++j)
            perDegreeOut.push_back (intervals[(size_t) j]);
        return true;
    }

    // Parse a scale-period spec typed by the user, in any of the three notations:
    //   whole number   "3"        (a ratio 3/1)
    //   fraction       "3/2", "5/4.5"
    //   cents          "701.955c", "701.955 cents", "701.955\xc2\xa2", or a bare
    //                  number too big to be a ratio ("1200" -> 1200 c)
    // On success returns the ratio and the exact line to write as the .scl period
    // (ratios kept as fractions so they stay exact; cents kept as cents).
    static bool parsePeriodSpec (const juce::String& textIn, double& ratioOut, juce::String& sclLineOut)
    {
        auto t = textIn.trim().removeCharacters (" \t")
                    .replace (juce::String::fromUTF8 ("\xc2\xa2"), "c")   // the cent sign
                    .replace ("cents", "c", true).replace ("cent", "c", true);
        if (t.isEmpty())
            return false;

        const auto validRatio = [&] (double r) { return std::isfinite (r) && r > 1.02 && r <= 16.0; };

        // A bare number that is too large to be a ratio can only have been meant as
        // cents - accept it rather than rejecting what the user clearly intended.
        if (! t.endsWithIgnoreCase ("c") && ! t.containsChar ('/')
            && t.containsOnly ("0123456789.") && t.getDoubleValue() > 16.0)
            t += "c";

        if (t.endsWithIgnoreCase ("c"))
        {
            const auto num = t.dropLastCharacters (1);
            if (! num.containsOnly ("0123456789.")
                || num.indexOfChar ('.') != num.lastIndexOfChar ('.') || num.isEmpty())
                return false;
            const double cents = num.getDoubleValue();
            const double r = std::pow (2.0, cents / 1200.0);
            if (! validRatio (r))
                return false;
            ratioOut   = r;
            sclLineOut = juce::String (cents, 5);
            return true;
        }

        if (t.containsChar ('/'))
        {
            const auto a = t.upToFirstOccurrenceOf ("/", false, false);
            const auto b = t.fromFirstOccurrenceOf ("/", false, false);
            if (! a.containsOnly ("0123456789.") || ! b.containsOnly ("0123456789.")
                || a.indexOfChar ('.') != a.lastIndexOfChar ('.')
                || b.indexOfChar ('.') != b.lastIndexOfChar ('.')
                || a.isEmpty() || b.isEmpty())
                return false;
            const double num = a.getDoubleValue(), den = b.getDoubleValue();
            if (den <= 0.0)
                return false;
            const double r = num / den;
            if (! validRatio (r))
                return false;
            ratioOut   = r;
            sclLineOut = a + "/" + b;
            return true;
        }

        if (! t.containsOnly ("0123456789.") || t.indexOfChar ('.') != t.lastIndexOfChar ('.'))
            return false;
        const double r = t.getDoubleValue();
        if (! validRatio (r))
            return false;
        ratioOut   = r;
        // A decimal ratio has no exact fraction: write it as cents, which is exact
        // in the .scl sense (the file keeps what the user asked for).
        sclLineOut = t.containsChar ('.') ? juce::String (1200.0 * std::log2 (r), 5)
                                          : t + "/1";
        return true;
    }

    void clearScala()
    {
        scalaValid.store (false, std::memory_order_relaxed);
        scalaName.clear();
        scalaSource.clear();
        scalaPerDegree.clear();
        for (int n = 0; n < 128; ++n)
            table[(size_t) n].store (equalFreq (n), std::memory_order_relaxed);
    }

    juce::String getScalaName()   const { return scalaName; }
    juce::String getScalaSource() const { return scalaSource; }

    // Public wrapper over the .scl pitch-line parser ("702.0" cents / "3/2" ratio),
    // for the keyboard's scale-editing prompts.
    static bool parsePitchSpec (const juce::String& text, double& ratioOut, DegreeSrc& srcOut)
    { return parsePitchSrc (text, ratioOut, srcOut); }

    // Name of the scale actually in effect (MTS master's, or the loaded .scl), for the UI.
    juce::String currentScaleName() const
    {
        switch (mode.load (std::memory_order_relaxed))
        {
            case Mode::MtsEsp: if (hasMtsMaster())                            return juce::String (MTS_GetScaleName (client)); break;
            case Mode::Scala:  if (scalaValid.load (std::memory_order_relaxed)) return scalaName; break;
            case Mode::Equal:  default: break;
        }
        return {};
    }

    // Human-readable description of what is actually sounding right now (for the UI).
    juce::String getStatusText() const
    {
        switch (mode.load (std::memory_order_relaxed))
        {
            case Mode::MtsEsp:
                if (hasMtsMaster())
                    return "MTS-ESP: " + juce::String (MTS_GetScaleName (client));
                return "MTS-ESP - 12ED2"; // nothing is sending: 12ED2 is what sounds

            case Mode::Scala:
                if (scalaValid.load (std::memory_order_relaxed))
                    return "Scala: " + scalaName;
                return "Scala (no file -> 12-TET)";

            case Mode::Equal:
            default:
                return "12-TET (A=440)";
        }
    }

private:
    static double equalFreq (int n) noexcept
    {
        return 440.0 * std::pow (2.0, (n - 69) / 12.0);
    }

    // Ableton .ascl "@ABL REFERENCE_PITCH <octave> <degree> <freq>" (inside a
    // '!' comment line). Returns false if the file has none (plain .scl).
    struct AblReference { int octave = 3; int degree = 0; double freq = 0.0; };

    static bool parseAblReference (const juce::String& text, AblReference& out)
    {
        juce::StringArray raw;
        raw.addLines (text);
        for (const auto& l : raw)
        {
            const auto t = l.trim();
            if (! t.startsWith ("!"))
                continue;
            const auto body = t.substring (1).trim();
            if (! body.startsWith ("@ABL"))
                continue;
            auto rest = body.substring (4).trim();
            if (! rest.startsWith ("REFERENCE_PITCH"))
                continue;
            rest = rest.substring (15).trim();

            juce::StringArray tok;
            tok.addTokens (rest, " \t", "\"");
            tok.removeEmptyStrings();
            if (tok.size() < 3)
                return false;

            // getIntValue() would turn a non-numeric token into 0 and silently
            // mis-anchor the whole tuning - validate before parsing (a bad @ABL
            // line degrades to plain-.scl middle-C anchoring instead).
            const auto isIntToken = [] (const juce::String& s)
            {
                const auto body = s.startsWith ("-") ? s.substring (1) : s;
                return body.isNotEmpty() && body.containsOnly ("0123456789");
            };
            if (! isIntToken (tok[0]) || ! isIntToken (tok[1]))
                return false;

            out.octave = tok[0].getIntValue();
            out.degree = tok[1].getIntValue();
            out.freq   = tok[2].getDoubleValue();
            return std::isfinite (out.freq) && out.freq > 0.0 && out.degree >= 0;
        }
        return false;
    }

    //== AnaMark .tun parsing =================================================
    // ';' is the .tun comment character; '#' turns up in files written by other
    // editors. Either starts a comment that runs to the end of the line.
    static juce::String stripTunComment (const juce::String& line)
    {
        int cut = line.length();
        for (const auto ch : { ';', '#' })
        {
            const int i = line.indexOfChar (ch);
            if (i >= 0 && i < cut) cut = i;
        }
        return line.substring (0, cut);
    }

    // A number as .tun writes them. getDoubleValue() turns "abc" into 0.0, which
    // here would mean "note 60 = the basefreq" - a silently wrong pitch instead of
    // a refused file.
    static bool isTunNumber (const juce::String& s)
    {
        if (s.isEmpty() || ! s.containsOnly ("0123456789.+-eE"))
            return false;
        if (s.indexOfChar ('.') != s.lastIndexOfChar ('.'))
            return false;
        return juce::CharacterFunctions::isDigit (s[0]) || s[0] == '+' || s[0] == '-' || s[0] == '.';
    }

    // Is this period one of the intervals a real scale actually repeats at? Used
    // only to break the tie between "12 notes per 2/1" and the mathematically
    // equally valid but musically useless "1 note per 100 cents".
    static bool isSimplePeriod (double cents)
    {
        for (const double r : { 2.0, 3.0, 4.0, 1.5, 2.5, 1.25, 6.0, 8.0 })
            if (std::abs (cents - 1200.0 * std::log2 (r)) <= 0.01)
                return true;
        return false;
    }

    // Read the 128 absolute frequencies out of an AnaMark .tun (v1/v2).
    // [Exact Tuning] is authoritative (fractional cents over basefreq); the integer
    // [Tuning] block is only the fallback for v1 files that have no exact block.
    // A file that specifies only SOME of the 128 keys is refused rather than
    // completed by guesswork: the spec's continuation rule is not what every writer
    // means, and inventing the missing keys is inventing the user's tuning.
    static bool parseTun (const juce::String& text, std::array<double, 128>& freqOut,
                          juce::String& errorOut)
    {
        juce::StringArray raw;
        raw.addLines (text);

        juce::String section;
        double baseFreq = 0.0;
        std::array<double, 128> exact {}, coarse {};
        std::array<bool, 128>   haveExact {}, haveCoarse {};
        int nExact = 0, nCoarse = 0;

        for (const auto& rawLine : raw)
        {
            const auto line = stripTunComment (rawLine).trim();
            if (line.isEmpty())
                continue;

            if (line.startsWithChar ('['))
            {
                section = line.fromFirstOccurrenceOf ("[", false, false)
                              .upToFirstOccurrenceOf ("]", false, false).trim().toLowerCase();
                continue;
            }

            const int eq = line.indexOfChar ('=');
            if (eq < 0)
                continue; // free text ([Info] name/author lines and the like)

            // "note 60" and "note60" are both in the wild; whitespace around '=' is free.
            const auto key = line.substring (0, eq).trim().toLowerCase().removeCharacters (" \t");
            const auto val = line.substring (eq + 1).trim();

            if (key == "basefreq")
            {
                // The exact block's basefreq wins; any other one only fills a gap.
                if (! isTunNumber (val))
                {
                    errorOut = "Bad basefreq in .tun: \"" + val + "\".";
                    return false;
                }
                if (section == "exact tuning" || baseFreq <= 0.0)
                    baseFreq = val.getDoubleValue();
                continue;
            }

            if (! key.startsWith ("note") || key.length() <= 4
                || ! key.substring (4).containsOnly ("0123456789"))
                continue;

            const int n = key.substring (4).getIntValue();
            if (! juce::isPositiveAndBelow (n, 128))
                continue; // .tun allows notes past 127 for other purposes; MIDI stops here

            if (! isTunNumber (val))
            {
                errorOut = "Bad value for note " + juce::String (n) + " in .tun: \"" + val + "\".";
                return false;
            }
            const double cents = val.getDoubleValue();
            if (! std::isfinite (cents))
            {
                errorOut = "Note " + juce::String (n) + " in this .tun is not a finite pitch.";
                return false;
            }

            if (section == "exact tuning")
            {
                if (! haveExact[(size_t) n]) ++nExact;
                exact[(size_t) n] = cents; haveExact[(size_t) n] = true;
            }
            else if (section == "tuning")
            {
                if (! haveCoarse[(size_t) n]) ++nCoarse;
                coarse[(size_t) n] = cents; haveCoarse[(size_t) n] = true;
            }
        }

        const bool useExact = nExact > 0;
        const int  found    = useExact ? nExact : nCoarse;
        if (found == 0)
        {
            errorOut = "Not a valid .tun file: no [Exact Tuning] or [Tuning] notes found.";
            return false;
        }
        if (found < 128)
        {
            errorOut = "This .tun only specifies " + juce::String (found) + " of the 128 MIDI "
                       "notes. Nothing was loaded - the rest would have to be invented.";
            return false;
        }

        // v1 files carry no basefreq: the standard pins MIDI note 0 to 8.1757989156 Hz
        // (12-TET, A=440). That is the spec's own value, not a guess of ours.
        if (baseFreq <= 0.0)
            baseFreq = 8.1757989156;
        if (! std::isfinite (baseFreq))
        {
            errorOut = "This .tun has an unusable basefreq.";
            return false;
        }

        for (int n = 0; n < 128; ++n)
        {
            const double cents = useExact ? exact[(size_t) n] : coarse[(size_t) n];
            const double hz    = baseFreq * std::pow (2.0, cents / 1200.0);
            if (! (hz > 0.0) || ! std::isfinite (hz))
            {
                errorOut = "Note " + juce::String (n) + " in this .tun is not a usable frequency.";
                return false;
            }
            freqOut[(size_t) n] = hz;
        }
        return true;
    }

    // Parse a Scala .scl into the list of interval ratios (degrees 1..N). When
    // `srcOut` is provided it receives, per interval, whether it was a genuine
    // rational (ratio/integer) or a cents value.
    static bool parseScala (const juce::String& text, std::vector<double>& out,
                            juce::String& descOut, juce::String& errorOut,
                            std::vector<DegreeSrc>* srcOut = nullptr)
    {
        juce::StringArray raw;
        raw.addLines (text);

        // Drop comment lines ('!'); keep blank lines so the (possibly empty)
        // description line stays in its positional slot.
        juce::StringArray lines;
        for (auto& l : raw)
        {
            const auto t = l.trimStart();
            if (t.startsWith ("!"))
                continue;
            lines.add (l.trim());
        }

        if (lines.size() < 2)
        {
            errorOut = "Not a valid .scl file (too short).";
            return false;
        }

        descOut = lines[0];
        const int count = lines[1].getIntValue();
        if (count < 1 || count > 4096) // real scales are tiny; cap guards a bad header
        {
            errorOut = "Invalid note count (" + juce::String (count) + ").";
            return false;
        }
        if (lines.size() < 2 + count)
        {
            errorOut = "File declares " + juce::String (count) + " notes but has fewer.";
            return false;
        }

        out.clear();
        out.reserve ((size_t) count);
        if (srcOut != nullptr) { srcOut->clear(); srcOut->reserve ((size_t) count); }
        for (int i = 0; i < count; ++i)
        {
            double ratio = 0.0;
            DegreeSrc src;
            if (! parsePitchSrc (lines[2 + i], ratio, src))
            {
                errorOut = "Bad pitch on line " + juce::String (i + 1) + ": \"" + lines[2 + i] + "\"";
                return false;
            }
            out.push_back (ratio);
            if (srcOut != nullptr) srcOut->push_back (src);
        }
        return true;
    }

    // One Scala pitch line: a value with a '.' is cents, otherwise a ratio
    // ("a/b" or a bare integer "a" = a/1). Trailing text after the value is a
    // comment and ignored. `srcOut` records whether it was a genuine rational.
    static bool parsePitchSrc (const juce::String& lineIn, double& ratioOut, DegreeSrc& srcOut)
    {
        srcOut = {};
        auto line = lineIn.trim();
        if (line.isEmpty())
            return false;

        // Take the first whitespace-delimited token.
        auto token = line.upToFirstOccurrenceOf (" ", false, false);
        token = token.upToFirstOccurrenceOf ("\t", false, false).trim();
        if (token.isEmpty())
            return false;

        if (token.containsChar ('.'))
        {
            // Cents. pow(2,x) is always > 0, so the value itself never fails the
            // test — we must reject a non-numeric token here or a line like "x.5"
            // (or "1.0.0") would be silently accepted as a near-0-cent unison.
            if (! token.containsOnly ("0123456789.eE+-")
                || token.indexOfChar ('.') != token.lastIndexOfChar ('.'))
                return false;
            const double cents = token.getDoubleValue();
            ratioOut = std::pow (2.0, cents / 1200.0);
            srcOut.rational = false; // cents -> tempered, never claimed as an exact ratio
            return std::isfinite (ratioOut) && ratioOut > 0.0;
        }

        if (token.containsChar ('/'))
        {
            const auto numT = token.upToFirstOccurrenceOf ("/", false, false);
            const auto denT = token.fromFirstOccurrenceOf ("/", false, false);
            const double num = numT.getDoubleValue();
            const double den = denT.getDoubleValue();
            if (den <= 0.0 || num <= 0.0)
                return false;
            ratioOut = num / den;
            // Genuine rational only when both parts are plain integers.
            if (numT.containsOnly ("0123456789") && denT.containsOnly ("0123456789"))
            { srcOut.rational = true; srcOut.p = (long long) numT.getLargeIntValue(); srcOut.q = (long long) denT.getLargeIntValue(); }
            return std::isfinite (ratioOut) && ratioOut > 0.0;
        }

        // Bare ratio numerator: must be a plain positive integer (reject "100abc",
        // comma-decimals "1,5", etc. rather than silently truncating them).
        if (! token.containsOnly ("0123456789"))
            return false;
        ratioOut = token.getDoubleValue();
        srcOut.rational = true; srcOut.p = (long long) token.getLargeIntValue(); srcOut.q = 1;
        return ratioOut > 0.0;
    }
    static bool parsePitch (const juce::String& lineIn, double& ratioOut)
    { DegreeSrc s; return parsePitchSrc (lineIn, ratioOut, s); }

    // The audio thread reads the tuning table via these atomics, so they must be
    // lock-free or frequencyForNote() would not be real-time safe.
    static_assert (std::atomic<double>::is_always_lock_free,
                   "TuningManager needs a lock-free atomic<double> for RT-safe reads");

    MTSClient* client = nullptr;
    std::atomic<Mode> mode { Mode::Equal };
    std::atomic<double> refScale { 1.0 }; // A-reference / 440
    std::atomic<double> octScale { 1.0 }; // whole-instrument octave transpose (2^oct)
    std::array<std::atomic<double>, 128> table {};
    std::atomic<bool> scalaValid { false };
    std::atomic<int>  scalaDegreeCount { 12 };
    std::atomic<double> scalaPeriod { 2.0 };

    juce::String scalaName, scalaSource; // message-thread only
    std::vector<DegreeSrc> scalaDegSrc;  // per-degree ratio-vs-cents source (message-thread only)
    std::vector<double> scalaPerDegree;  // exact degree ratios (message-thread only)
    DegreeSrc    scalaPeriodSrc;         // the period's source
};
