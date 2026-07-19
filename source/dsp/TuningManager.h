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

    bool hasMtsMaster() const noexcept
    {
        return client != nullptr && MTS_HasMaster (client);
    }

    bool hasScala() const noexcept { return scalaValid.load (std::memory_order_relaxed); }

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
    double frequencyForNote (int midiNote) const noexcept
    {
        const int n = juce::jlimit (0, 127, midiNote);

        switch (mode.load (std::memory_order_relaxed))
        {
            case Mode::MtsEsp:
                // -1 channel = "don't care" (recommended for MPE). Falls back to
                // 12-TET internally when no master is connected.
                if (client != nullptr)
                    return MTS_NoteToFrequency (client, (char) n, -1);
                break;

            case Mode::Scala:
                // acquire pairs with the release in loadScalaString so the 128
                // table writes are guaranteed visible once we observe the flag.
                if (scalaValid.load (std::memory_order_acquire))
                    return table[(size_t) n].load (std::memory_order_relaxed);
                break;

            case Mode::Equal:
            default:
                break;
        }
        return equalFreq (n);
    }

    //== Message thread =======================================================
    bool loadScalaFile (const juce::File& f, juce::String& errorOut)
    {
        if (! f.existsAsFile())
        {
            errorOut = "File not found.";
            return false;
        }
        return loadScalaString (f.loadFileAsString(), f.getFileNameWithoutExtension(), errorOut);
    }

    bool loadScalaString (const juce::String& text, const juce::String& name, juce::String& errorOut)
    {
        std::vector<double> intervals;
        juce::String desc;
        if (! parseScala (text, intervals, desc, errorOut))
            return false;

        const int count = (int) intervals.size();          // notes per period
        const double period = intervals[(size_t) count - 1]; // last listed = period

        // perDegree[0] = 1/1; perDegree[1..count-1] = listed intervals 1..count-1.
        std::vector<double> perDegree;
        perDegree.reserve ((size_t) count);
        perDegree.push_back (1.0);
        for (int j = 0; j < count - 1; ++j)
            perDegree.push_back (intervals[(size_t) j]);

        for (int n = 0; n < 128; ++n)
        {
            const int d   = n - kRefKey;
            const int oct = (int) std::floor ((double) d / (double) count);
            const int step = d - oct * count; // 0..count-1
            const double f = kRefFreq * std::pow (period, (double) oct) * perDegree[(size_t) step];
            table[(size_t) n].store (f, std::memory_order_relaxed);
        }

        scalaName   = name.isNotEmpty() ? name : desc;
        scalaSource = text;
        scalaDegreeCount.store (count, std::memory_order_relaxed);
        // release: publishes all the table writes above to the audio thread.
        scalaValid.store (true, std::memory_order_release);
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

    void clearScala()
    {
        scalaValid.store (false, std::memory_order_relaxed);
        scalaName.clear();
        scalaSource.clear();
        for (int n = 0; n < 128; ++n)
            table[(size_t) n].store (equalFreq (n), std::memory_order_relaxed);
    }

    juce::String getScalaName()   const { return scalaName; }
    juce::String getScalaSource() const { return scalaSource; }

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
                return "MTS-ESP (no master -> 12-TET)";

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

    // Parse a Scala .scl into the list of interval ratios (degrees 1..N).
    static bool parseScala (const juce::String& text, std::vector<double>& out,
                            juce::String& descOut, juce::String& errorOut)
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
        for (int i = 0; i < count; ++i)
        {
            double ratio = 0.0;
            if (! parsePitch (lines[2 + i], ratio))
            {
                errorOut = "Bad pitch on line " + juce::String (i + 1) + ": \"" + lines[2 + i] + "\"";
                return false;
            }
            out.push_back (ratio);
        }
        return true;
    }

    // One Scala pitch line: a value with a '.' is cents, otherwise a ratio
    // ("a/b" or a bare integer "a" = a/1). Trailing text after the value is a
    // comment and ignored.
    static bool parsePitch (const juce::String& lineIn, double& ratioOut)
    {
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
            return std::isfinite (ratioOut) && ratioOut > 0.0;
        }

        if (token.containsChar ('/'))
        {
            const double num = token.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
            const double den = token.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
            if (den <= 0.0 || num <= 0.0)
                return false;
            ratioOut = num / den;
            return std::isfinite (ratioOut) && ratioOut > 0.0;
        }

        // Bare ratio numerator: must be a plain positive integer (reject "100abc",
        // comma-decimals "1,5", etc. rather than silently truncating them).
        if (! token.containsOnly ("0123456789"))
            return false;
        ratioOut = token.getDoubleValue();
        return ratioOut > 0.0;
    }

    // The audio thread reads the tuning table via these atomics, so they must be
    // lock-free or frequencyForNote() would not be real-time safe.
    static_assert (std::atomic<double>::is_always_lock_free,
                   "TuningManager needs a lock-free atomic<double> for RT-safe reads");

    MTSClient* client = nullptr;
    std::atomic<Mode> mode { Mode::Equal };
    std::array<std::atomic<double>, 128> table {};
    std::atomic<bool> scalaValid { false };
    std::atomic<int>  scalaDegreeCount { 12 };

    juce::String scalaName, scalaSource; // message-thread only
};
