#include "SnapshotRig.h"

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

namespace fgsnap
{
namespace
{
    using Proc = FractalAdditiveAudioProcessor;
    using Ed   = FractalAdditiveAudioProcessorEditor;

    //==========================================================================
    // A TINY JSON WRITER, on purpose.
    //
    // juce::JSON would do, but it escapes every non-ASCII code point as \uXXXX - and one
    // of the four things this rig has to PROVE is that the Catalan run really rendered
    // Catalan and not mojibake. A file full of à can be correct and still be
    // unreadable to the person checking it, and "did the accent survive?" is exactly the
    // question that must be answerable by eye. This writes raw UTF-8 and escapes only
    // what JSON actually requires.
    //==========================================================================
    juce::String qs (const juce::String& t)
    {
        juce::String s ("\"");
        for (auto p = t.getCharPointer(); ! p.isEmpty();)
        {
            const auto c = p.getAndAdvance();
            switch (c)
            {
                case '"':  s += "\\\""; break;
                case '\\': s += "\\\\"; break;
                case '\n': s += "\\n";  break;
                case '\r': s += "\\r";  break;
                case '\t': s += "\\t";  break;
                default:
                    if (c < 32) s += "\\u" + juce::String::toHexString ((int) c).paddedLeft ('0', 4);
                    else        s += juce::String::charToString (c);   // UTF-8 straight through
            }
        }
        s += "\"";
        return s;
    }

    juce::String num (double v)
    {
        if (! std::isfinite (v)) return "null";
        if (v == std::floor (v) && std::abs (v) < 1.0e15) return juce::String ((juce::int64) v);
        return juce::String (v, 2);
    }
    juce::String num (int v)  { return juce::String (v); }
    juce::String boo (bool b) { return b ? "true" : "false"; }

    juce::String obj (std::initializer_list<std::pair<const char*, juce::String>> kv)
    {
        juce::String s ("{");
        bool first = true;
        for (const auto& p : kv)
        {
            if (! first) s += ",";
            first = false;
            s += qs (p.first) + ":" + p.second;
        }
        s += "}";
        return s;
    }

    juce::String arr (const juce::StringArray& items)
    {
        if (items.isEmpty()) return "[]";
        return "[\n      " + items.joinIntoString (",\n      ") + "\n    ]";
    }

    juce::String rectJ (juce::Rectangle<int> r)
    {
        return obj ({ { "x", num (r.getX()) }, { "y", num (r.getY()) },
                      { "w", num (r.getWidth()) }, { "h", num (r.getHeight()) },
                      { "right", num (r.getRight()) }, { "bottom", num (r.getBottom()) } });
    }

    juce::String colJ (juce::Colour c) { return qs (c.toDisplayString (true)); }

    //==========================================================================
    // THE PROBE, AS JSON.
    //
    // A PNG shows a designer WHAT is wrong; this says WHICH RECT to change, and lets a
    // reviewer check a claim with arithmetic instead of by eyeballing a picture. Every
    // string is carried with the width it measures in the font it is really drawn in and
    // the width of the box it is drawn into, because that difference - not the rect - is
    // what a truncation is.
    //==========================================================================
    juce::String dumpProbe (const Ed::LayoutProbe& p, juce::Rectangle<int> win,
                            juce::StringArray& problems)
    {
        auto over = [&problems] (const juce::String& what, float need, float avail)
        {
            if (need > (float) avail + 0.5f)
                problems.add (obj ({ { "where", qs (what) }, { "need", num (need) },
                                     { "avail", num ((double) avail) },
                                     { "over",  num ((double) (need - (float) avail)) } }));
            return need > (float) avail + 0.5f;
        };

        juce::StringArray controls, cells, painted, compLbl, wrapped, variants,
                          comboIds, comboTexts, buttonFits, buttonTexts, dividers, corners;

        for (const auto& c : p.controls)
        {
            const bool outside = ! win.contains (c.second);
            const bool empty   = c.second.getWidth() <= 0 || c.second.getHeight() <= 0;
            if (outside) problems.add (obj ({ { "where", qs ("control " + c.first) },
                                             { "note", qs ("outside the window") },
                                             { "rect", rectJ (c.second) } }));
            if (empty)   problems.add (obj ({ { "where", qs ("control " + c.first) },
                                             { "note", qs ("collapsed to nothing") },
                                             { "rect", rectJ (c.second) } }));
            controls.add (obj ({ { "name", qs (c.first) }, { "rect", rectJ (c.second) },
                                 { "inWindow", boo (! outside) } }));
        }
        for (const auto& c : p.cells)
            cells.add (obj ({ { "name", qs (c.first) }, { "rect", rectJ (c.second) } }));

        for (const auto& l : p.paintedLabels)
        {
            // A PAINTED label has no component to squash: it fails by CLIPPING, silently,
            // in one language only. The measured width against the cell is the only thing
            // that can see it.
            const bool bad = over ("paintedLabel " + l.name + " \"" + l.text + "\"",
                                   l.textWidth, (float) l.cell.getWidth());
            painted.add (obj ({ { "name", qs (l.name) }, { "text", qs (l.text) },
                                { "rect", rectJ (l.cell) }, { "textWidth", num (l.textWidth) },
                                { "boxWidth", num (l.cell.getWidth()) },
                                { "over", num ((double) (l.textWidth - (float) l.cell.getWidth())) },
                                { "clipped", boo (bad) }, { "colour", colJ (l.colour) } }));
        }
        for (const auto& l : p.componentLabels)
        {
            // A Label COMPONENT does not clip - it CONDENSES to 70% - so it fails by going
            // unreadably squashed while every bounds check stays green.
            const bool bad = over ("label " + l.name + " \"" + l.text + "\"",
                                   l.textWidth, (float) l.cell.getWidth());
            compLbl.add (obj ({ { "name", qs (l.name) }, { "text", qs (l.text) },
                                { "rect", rectJ (l.cell) }, { "textWidth", num (l.textWidth) },
                                { "boxWidth", num (l.cell.getWidth()) },
                                { "over", num ((double) (l.textWidth - (float) l.cell.getWidth())) },
                                { "squashed", boo (bad) } }));
        }
        auto wrapJ = [&over] (const Ed::LayoutProbe::WrappedLabel& w, juce::StringArray& into)
        {
            const bool bad = over ("wrapped " + w.name, w.needHeight, (float) w.cell.getHeight());
            into.add (obj ({ { "name", qs (w.name) }, { "text", qs (w.text) },
                             { "textArea", rectJ (w.cell) },
                             { "needWidth", num (w.needWidth) }, { "needHeight", num (w.needHeight) },
                             { "boxHeight", num (w.cell.getHeight()) },
                             { "overHeight", num ((double) (w.needHeight - (float) w.cell.getHeight())) },
                             { "cut", boo (bad) } }));
        };
        for (const auto& w : p.wrappedLabels)    wrapJ (w, wrapped);
        for (const auto& w : p.readoutVariants)  wrapJ (w, variants);

        for (const auto& c : p.comboIds)
            comboIds.add (obj ({ { "name", qs (c.first) }, { "selectedId", num (c.second) } }));
        for (const auto& c : p.comboTexts)
        {
            const bool bad = over ("combo " + c.name + " \"" + c.text + "\"", c.need, (float) c.avail);
            comboTexts.add (obj ({ { "name", qs (c.name) }, { "widestItem", qs (c.text) },
                                   { "need", num (c.need) }, { "avail", num (c.avail) },
                                   { "over", num ((double) (c.need - (float) c.avail)) },
                                   { "truncated", boo (bad) } }));
        }
        for (const auto& b : p.buttonFits)
        {
            const bool bad = over ("button " + b.name + " \"" + b.text + "\"", b.need, (float) b.avail);
            buttonFits.add (obj ({ { "name", qs (b.name) }, { "text", qs (b.text) },
                                   { "need", num (b.need) }, { "avail", num (b.avail) },
                                   { "over", num ((double) (b.need - (float) b.avail)) },
                                   { "truncated", boo (bad) } }));
        }
        for (const auto& b : p.buttonTexts)
            buttonTexts.add (obj ({ { "name", qs (b.first) }, { "text", qs (b.second) } }));
        for (const auto& d : p.dividers)
            dividers.add (obj ({ { "name", qs (d.first) },
                                 { "x1", num (d.second.getStartX()) }, { "y1", num (d.second.getStartY()) },
                                 { "x2", num (d.second.getEndX()) },   { "y2", num (d.second.getEndY()) } }));
        for (const auto& c : p.panelCorners)
            corners.add (obj ({ { "panel", qs (c.first) }, { "borderInset", num (c.second) } }));

        return juce::String ("{\n    ")
             + "\"panels\":" + obj ({ { "spectrum", rectJ (p.spectrum) },
                                      { "engineBox", rectJ (p.engineBox) },
                                      { "bothEngines", rectJ (p.bothEngines) },
                                      { "essentials", rectJ (p.essentials) },
                                      { "envelopes", rectJ (p.envelopes) },
                                      { "tuning", rectJ (p.tuning) },
                                      { "engineChips", rectJ (p.engineChips) },
                                      { "specColumn", rectJ (p.specColumn) },
                                      { "titleBar", rectJ (p.titleBar) } })
             + ",\n    \"panelColour\":" + colJ (p.panelColour)
             + ",\n    \"slope\":" + obj ({ { "colour", colJ (p.slopeColour) },
                                            { "frozen", boo (p.slopeFrozen) } })
             + ",\n    \"panelCorners\":" + arr (corners)
             + ",\n    \"walls\":" + obj ({ { "spectrumRight", num (p.wallSpectrumRight) },
                                            { "tuningRight",   num (p.wallTuningRight) },
                                            { "columnLeft",    num (p.wallColumnLeft) } })
             + ",\n    \"keyboard\":" + obj ({ { "component", rectJ (p.keyboardBox) },
                                               { "keysDrawn", rectJ (p.keysDrawn) },
                                               { "cellsInWindow", num (p.keysInWindow) },
                                               { "keysPlayable", num (p.keysPlayable) } })
             + ",\n    \"regPeriodsTooltip\":" + qs (p.regPeriodsTip)
             + ",\n    \"controls\":"        + arr (controls)
             + ",\n    \"cells\":"           + arr (cells)
             + ",\n    \"paintedLabels\":"   + arr (painted)
             + ",\n    \"componentLabels\":" + arr (compLbl)
             + ",\n    \"wrappedLabels\":"   + arr (wrapped)
             + ",\n    \"readoutVariants\":" + arr (variants)
             + ",\n    \"comboIds\":"        + arr (comboIds)
             + ",\n    \"comboTexts\":"      + arr (comboTexts)
             + ",\n    \"buttonFits\":"      + arr (buttonFits)
             + ",\n    \"buttonTexts\":"     + arr (buttonTexts)
             + ",\n    \"dividers\":"        + arr (dividers)
             + "\n  }";
    }

    // THE TRUNCATION SWEEP, verbatim: every visible Label / Button / ComboBox anywhere
    // under the editor (and in both floating panels), each measured against the room its
    // own LookAndFeel really leaves it. This is the list that says which rect to change.
    juce::String dumpTextFits (const std::vector<Ed::TextFit>& fits, juce::StringArray& problems)
    {
        juce::StringArray rows;
        for (const auto& f : fits)
        {
            const bool bad = f.over() > 0.5f;
            if (bad)
                problems.add (obj ({ { "where", qs (f.path + " (" + f.kind + ") \"" + f.text + "\"") },
                                     { "need",  num (f.wraps ? f.needH : f.need) },
                                     { "avail", num (f.wraps ? f.availH : f.avail) },
                                     { "over",  num (f.over()) } }));
            rows.add (obj ({ { "path", qs (f.path) }, { "kind", qs (f.kind) },
                             { "text", qs (f.text) }, { "rect", rectJ (f.bounds) },
                             { "need", num (f.need) },   { "avail", num (f.avail) },
                             { "needH", num (f.needH) }, { "availH", num (f.availH) },
                             { "wraps", boo (f.wraps) }, { "over", num (f.over()) },
                             { "overflows", boo (bad) } }));
        }
        return arr (rows);
    }

    juce::String dumpPlayground (const Ed::PlaygroundProbe& g)
    {
        juce::StringArray corners, modes;
        for (int i = 0; i < 4; ++i)
            corners.add (obj ({ { "corner", qs (juce::String::charToString ((juce::juce_wchar) ('A' + i))) },
                                { "name", qs (g.cornerName[i]) },
                                { "box", rectJ (g.box[i]) }, { "label", rectJ (g.lbl[i]) },
                                { "clear", rectJ (g.clear[i]) }, { "edit", rectJ (g.edit[i]) },
                                { "editText", qs (g.editText[i]) },
                                { "editEnabled", boo (g.editEnabled[i]) } }));
        for (int m = 0; m < 2; ++m)
            modes.add (obj ({ { "text", qs (g.modeText[m]) }, { "rect", rectJ (g.mode[m]) } }));
        return juce::String ("{\n    ")
             + "\"panel\":" + rectJ (g.panel)
             + ",\n    \"pad\":" + rectJ (g.pad)
             + ",\n    \"padArea\":" + rectJ (g.padArea)
             + ",\n    \"corners\":" + arr (corners)
             + ",\n    \"modes\":" + arr (modes)
             + ",\n    \"voices\":" + obj ({ { "label", rectJ (g.voicesL) },
                                             { "control", rectJ (g.voicesC) },
                                             { "text", qs (g.voicesText) } })
             + ",\n    \"status\":" + qs (g.status)
             + "\n  }";
    }

    //==========================================================================
    // THE SCENARIO MATRIX - DATA, so the design pass can extend it without touching a
    // line of the walk. A scenario says how to put the instrument in a state; the views
    // say in which language, at which size and at which scale to photograph it.
    //==========================================================================
    enum Target { tEditor, tPlayground, tPartials };
    enum Flags  { fNone = 0, fSizes = 1, fScale2 = 2 };

    struct Scenario
    {
        const char* id;      // the file-name stem: sorts meaningfully
        const char* what;    // one line, for the log and the JSON
        Target      target;
        unsigned    flags;
        std::function<void (Proc&, Ed&)> setup;
    };

    void setParam (Proc& p, const char* id, float plain)
    {
        if (auto* q = p.apvts.getParameter (id))
        {
            q->setValueNotifyingHost (q->convertTo0to1 (plain));
            p.pollParametersForTests();   // a snapshot run has no host to poll for us
        }
    }

    // A 19-EDO scale: a real tuning with a name, an octave period, and more degrees than
    // 12 - so the keyboard, the degree lists and the tuning read-outs all have content.
    const char* kScale19 =
        "! 19edo.scl\n19-tone equal temperament\n 19\n!\n"
        " 63.158\n 126.316\n 189.474\n 252.632\n 315.789\n 378.947\n 442.105\n 505.263\n"
        " 568.421\n 631.579\n 694.737\n 757.895\n 821.053\n 884.211\n 947.368\n 1010.526\n"
        " 1073.684\n 1136.842\n 2/1\n";
    // ...and one whose period is NOT an octave, which is the ONLY state that puts the
    // Shepard's rise-interval dropdown on screen (see resized(): riseChoice).
    const char* kScaleTritave =
        "! tritave.scl\ntritave period (Bohlen-Pierce-ish)\n 3\n!\n"
        " 435.084\n 884.359\n 3/1\n";

    void loadScale (Proc& p, const char* scl, const char* name)
    { juce::String err; p.loadScalaText (scl, name, err); }

    const std::vector<Scenario>& scenarios()
    {
        static const std::vector<Scenario> v =
        {
            // A COLD instrument. Empty states are where a layout looks worst, and this is
            // the very first thing a new user sees.
            { "01-cold-no-preset", "fresh instance, nothing loaded", tEditor, fSizes | fScale2,
              [] (Proc&, Ed&) {} },
            // The HARMONIC engine, warm: a real tuning loaded.
            { "02-harmonic", "harmonic engine, 19-EDO loaded", tEditor, fSizes | fScale2,
              [] (Proc& p, Ed&) { loadScale (p, kScale19, "19edo"); } },
            // The harmonic view AT ITS WIDEST: the nine prime-whitelist toggles instead of
            // the single LIMIT fader.
            { "03-harmonic-prime-whitelist", "harmonic engine, prime whitelist (9 toggles)", tEditor, fNone,
              [] (Proc& p, Ed&) { loadScale (p, kScale19, "19edo"); setParam (p, "primeMode", 1.0f); } },
            // Manual per-partial amplitudes: the one state that puts a SECOND painted label
            // ("FROZEN - DRAWN BY HAND") into a cell that already has one.
            { "04-harmonic-frozen-slope", "harmonic engine, manual per-partial amps on", tEditor, fNone,
              [] (Proc& p, Ed& e) { setParam (p, "partialManual", 1.0f); e.refreshSlopeFreezeForTests(); } },
            // The CUSTOM engine view: the intervals editor and the Load/Remove button.
            { "05-custom-engine", "custom engine, loaded spectrum", tEditor, fSizes,
              [] (Proc& p, Ed& e)
              { juce::String err; p.loadTimbreText ("5/4 3/2 2", err); e.refreshEngineDressing(); } },
            // The CUSTOM view with nothing loaded - reachable by clicking the chip, and the
            // only place a spectrum can be created in the first place.
            { "06-custom-engine-empty", "custom engine view, nothing loaded", tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEngineChipForTests (1); } },

            // Every modulator sub-tab.
            { "10-mod-amp",    "modulator strip: AMP",    tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (0); } },
            { "11-mod-pitch",  "modulator strip: PITCH",  tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (1); } },
            { "12-mod-filter", "modulator strip: FILTER", tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (2); } },
            { "13-mod-custom-env1", "modulator strip: CUSTOM / Env 1", tEditor, fSizes,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (3); e.setModSelForSnapshot (1); } },
            { "14-mod-custom-env3", "modulator strip: CUSTOM / Env 3", tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (3); e.setModSelForSnapshot (3); } },
            { "15-mod-custom-lfo1", "modulator strip: CUSTOM / LFO 1", tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (3); e.setModSelForSnapshot (4); } },
            { "16-mod-custom-lfo3", "modulator strip: CUSTOM / LFO 3", tEditor, fNone,
              [] (Proc&, Ed& e) { e.clickEnvTabForSnapshot (3); e.setModSelForSnapshot (6); } },

            // Every state of the right-hand Spectrum-vs-Tuning column. Each mode re-lays
            // the whole column, and two of them add rows nothing else reaches.
            { "20-spec-beating-this-tuning", "right column: beating, from this tuning", tEditor, fNone,
              [] (Proc& p, Ed& e) { loadScale (p, kScale19, "19edo");
                                    e.setSpecModeForTests (1); e.setTuneSourceForTests (1); } },
            { "21-spec-beating-free-search", "right column: beating, free tuning search", tEditor, fNone,
              [] (Proc&, Ed& e) { e.setSpecModeForTests (1); e.setTuneSourceForTests (2); } },
            { "22-spec-adapt", "right column: adapt spectrum to tuning", tEditor, fSizes,
              [] (Proc& p, Ed& e) { loadScale (p, kScale19, "19edo");
                                    e.setSpecModeForTests (2); e.setAdaptTargetForTests (1); } },
            { "23-spec-adapt-realtime", "right column: adapt, REALTIME armed", tEditor, fNone,
              [] (Proc& p, Ed& e) { loadScale (p, kScale19, "19edo");
                                    e.setSpecModeForTests (2); e.pressAdaptRealtimeForTests(); } },
            { "24-spec-segment", "right column: from spectrum's segment", tEditor, fNone,
              [] (Proc&, Ed& e) { e.setSpecModeForTests (3); } },

            // THE KEYBOARD WEARING NOTE NAMES instead of degree numbers. A name is far
            // wider than a digit and is drawn in a different typeface, so this is the one
            // scenario where the degree band, the interval label under it and the period
            // number above the tonic are all laid out around text the rest of the matrix
            // never sees. 19-EDO because it is spelled with ordinary sharps and flats -
            // "C# Db" - so a reader can check the picture against what they already know.
            { "32-keyboard-note-names", "keyboard: note NAMES instead of degree numbers (19-EDO)",
              tEditor, fSizes | fScale2,
              [] (Proc& p, Ed& e) { loadScale (p, kScale19, "19edo");
                                    e.setNoteNamesForSnapshot (true); } },

            // The TUNING panel, with and without the conditional Shepard rise dropdown.
            { "30-tuning-octave-period", "tuning panel, octave period (no rise dropdown)", tEditor, fNone,
              [] (Proc& p, Ed&) { loadScale (p, kScale19, "19edo"); setParam (p, "shepOn", 1.0f); } },
            { "31-tuning-tritave-period", "tuning panel, 3/1 period (rise dropdown shows)", tEditor, fSizes,
              [] (Proc& p, Ed&) { loadScale (p, kScaleTritave, "tritave"); setParam (p, "shepOn", 1.0f); } },

            // The Spectrum-to-Rhythm panel: an in-editor overlay, so it lands in the
            // editor's own snapshot.
            { "40-rhythm-panel", "Spectrum-to-Rhythm panel open", tEditor, fNone,
              [] (Proc&, Ed& e) { e.setRhythmPanelForSnapshot (true); } },

            // The two FLOATING windows. Their panels are not children of the editor, so
            // they are photographed in their own right.
            { "50-playground-window", "floating: preset playground", tPlayground, fSizes,
              [] (Proc&, Ed&) {} },
            { "51-partial-amp-window", "floating: per-partial amplitudes (pencil)", tPartials, fSizes,
              [] (Proc& p, Ed&) { setParam (p, "partialManual", 1.0f); } },
        };
        return v;
    }

    //== The sizes each target is photographed at ==============================
    struct Size { int w, h; };

    Size defaultSize (Target t)
    {
        // The editor's shipped size; the two floating panels at the size the truncation
        // sweep already measures them at, so a JSON row and the pixels beside it are the
        // same geometry.
        switch (t) { case tPlayground: return { 560, 520 };
                     case tPartials:   return { 760, 520 };
                     default:          return { 1200, 650 }; }
    }
    Size bigSize (Target t)
    {
        switch (t) { case tPlayground: return { 820, 700 };
                     case tPartials:   return { 1100, 740 };
                     default:          return { 1600, 900 }; }
    }
    // The floating windows declare their own floor (see PlaygroundWindow /
    // PartialAmpWindow: setResizeLimits). The EDITOR has no declared minimum at all - it
    // is a fixed 1200x650 with every box placed by hand - so its floor is MEASURED, not
    // asserted: see findSmallestLaidOutSize.
    Size gEditorMin { 1200, 650 };
    Size minSize (Target t)
    {
        switch (t) { case tPlayground: return { 420, 380 };
                     case tPartials:   return { 320, 240 };
                     default:          return gEditorMin; }
    }

    //==========================================================================
    // THE SMALLEST SIZE THE EDITOR STILL LAYS OUT.
    //
    // Written rather than guessed because a number picked by hand would be fiction, and
    // the whole point of this rig is that the redesign is not built on fiction. The
    // predicate is the one the layout itself can fail: a control collapsed to nothing, a
    // macro panel pushed outside the window, or two macro panels grown into each other.
    // Measured in BOTH languages and the larger answer kept - Catalan runs 20-40% longer
    // nearly everywhere, so the English floor is not the product's floor.
    //==========================================================================
    // The log is APPENDED as it goes, never accumulated and written at the end. A GUI
    // subsystem app has no console to print to when it is launched by a script, so this
    // file is the only account of the run - and the run that most needs an account is the
    // one that dies half way through, which is exactly the run an end-of-walk write loses.
    juce::File gLog;
    void say (const juce::String& line)
    {
        if (gLog != juce::File()) gLog.appendText (line + "\n", false, false, "\n");
        std::printf ("%s\n", line.toRawUTF8());
        std::fflush (stdout);
    }
    //==========================================================================
    // THE TWO STANDING ASSERTIONS.
    //
    // The pictures and the geometry answer "does it fit". Neither of them ever asked the
    // two questions that were actually failing in the shipped product, so both are asked
    // here, on every run, and either one failing fails the run.
    //
    // 1. IS IT DECODED? juce::String(const char*) reads each BYTE as a code point, so a
    //    UTF-8 string built that way turns each of its non-ASCII characters into two
    //    Latin-1 ones - and the first of those two is always U+00C2 or U+00C3 for
    //    anything in the Latin-1 Supplement. So a single scan for those two characters
    //    catches the whole family: "±48 st (MPE)" -> "Â±48 st (MPE)" (shipped, in all 42
    //    main-editor snapshots) and "Fibló" -> "FiblÃ³" (shipped, in the Periods tooltip).
    //    Neither character occurs in English or in Catalan, so ZERO is the right bar.
    //
    // 2. IS IT TRANSLATED? A string that renders the same in en and ca is either a
    //    deliberate decision or a bug, and until now nothing could tell them apart -
    //    "Steps" had a translation sitting unused in the source and nobody could see it.
    //    Every component that draws the same text in both languages is a failure unless
    //    it is DECLARED below with a reason. The declaration is the point: it turns "the
    //    same in both languages" from an accident into something somebody chose.
    //==========================================================================
    juce::StringArray gMojibake, gUntranslated;

    // The two lead bytes of a mis-decoded Latin-1 Supplement character, as characters.
    constexpr juce::juce_wchar kLead1 = 0xc2, kLead2 = 0xc3;

    void checkMojibake (const juce::String& stem, const juce::String& where,
                        const juce::String& text)
    {
        if (! (text.containsChar (kLead1) || text.containsChar (kLead2))) return;
        const auto line = "  " + stem + "   " + where + "   \"" + text.substring (0, 90) + "\"";
        if (! gMojibake.contains (line)) gMojibake.add (line);
    }

    // A string with no run of three letters is a number, a symbol, a unit or a one- or
    // two-letter abbreviation ("N", "St", "x2", "/2", "0.50", "12000 c", "C3 ="). Those
    // are the same in every language by construction and are not worth declaring.
    bool hasWord (const juce::String& s)
    {
        int run = 0;
        for (auto p = s.getCharPointer(); ! p.isEmpty();)
        {
            const auto c = p.getAndAdvance();
            run = juce::CharacterFunctions::isLetter (c) ? run + 1 : 0;
            if (run >= 3) return true;
        }
        return false;
    }

    // THE DECLARED LIST: strings that are the same word in English and in Catalan, and
    // the reason each one is. The rule this window applies to a panel word is "translate
    // it unless the Catalan word IS the English one", so everything here is a case where
    // translating would invent a word no Catalan musician uses.
    bool sameInBothLanguagesOnPurpose (const juce::String& s)
    {
        struct Decl { const char* text; const char* why; };
        static const Decl kDeclared[] =
        {
            { "AMP",          "the same abbreviation in both: amplitude / amplitud" },
            { "GATE",         "universal sequencer term for MIDI note length" },
            { "VEL",          "universal abbreviation of MIDI velocity / velocity MIDI" },
            { "MPE out",      "MPE is the name of a MIDI specification" },
            { "JI: ON",       "JI = Just Intonation, the standard abbreviation in Catalan too" },
            { "JI: OFF",      "JI = Just Intonation, the standard abbreviation in Catalan too" },
            { "Mono",         "the same word in Catalan" },
            { "Notes",        "the same word in Catalan" },
            { "Reps",         "the same abbreviation in both: repeats / repeticions" },
            { "Min",          "international abbreviation, paired with Max" },
            { "Max",          "international abbreviation, paired with Min" },
            { "Res",          "the same abbreviation in both: resonance / resson\xc3\xa0ncia" },
            { "Env",          "the same abbreviation in both: envelope / envolupant" },
            { "Div",          "the same abbreviation in both: divide / divideix" },
            { "Glide",        "the term of art in Catalan too (see \"Glide monof\xc3\xb2nic\")" },
            { "Pitch bend",   "the term of art in Catalan too (the Catalan tooltips say \"bend\")" },
            { "Bend",         "the Phase glide mode: same term of art, and short because it shares its Essentials row with the Phase fader" },
            { "Shepard",      "a person's name" },
            { "\xc2\xb1""2 st",        "a bend range: sign, number and the st abbreviation" },
            { "\xc2\xb1""12 st",       "a bend range: sign, number and the st abbreviation" },
            { "\xc2\xb1""48 st (MPE)", "a bend range plus the name of a MIDI specification" },
            { "+ .scl",       "a file extension" },
            { "Scala (.scl)", "the name of a file format, and a host-visible parameter choice" },
            { "MTS-ESP",      "the name of a protocol, and a host-visible parameter choice" },
            { "Env 1", "modulator identifiers, numbered not named" },
            { "Env 2", "modulator identifiers, numbered not named" },
            { "Env 3", "modulator identifiers, numbered not named" },
            { "LFO 1", "modulator identifiers, numbered not named" },
            { "LFO 2", "modulator identifiers, numbered not named" },
            { "LFO 3", "modulator identifiers, numbered not named" },
            // Not UI words at all: the loaded scale's own name, printed back verbatim.
            { "12ED2",   "the loaded scale's own name" },
            { "19edo",   "the loaded scale's own name" },
            { "tritave", "the loaded scale's own name" },
            { "= ... BPM", "BPM, and dots standing in for a tempo the host has not given" },
        };
        for (const auto& d : kDeclared)
            if (s == juce::String::fromUTF8 (d.text)) return true;
        return false;
    }

    bool laysOut (Ed& ed)
    {
        const auto win = ed.getLocalBounds();
        const auto L   = ed.layoutProbe();
        for (const auto& c : L.controls)
            if (c.second.getWidth() <= 0 || c.second.getHeight() <= 0) return false;
        const juce::Rectangle<int> boxes[] = { L.spectrum, L.essentials, L.tuning,
                                               L.envelopes, L.specColumn };
        for (const auto& b : boxes)
            if (b.isEmpty() || ! win.contains (b)) return false;
        if (L.spectrum.intersects (L.essentials) || L.spectrum.intersects (L.tuning)) return false;
        if (L.keysDrawn.isEmpty()) return false;
        return true;
    }

    Size findSmallestLaidOutSize (Ed& ed)
    {
        const Size start { 1200, 650 };
        Size best { 0, 0 };
        for (int lang = 0; lang < 2; ++lang)
        {
            ed.setLanguageForSnapshot (lang == 1);
            // Width first, at the shipped height, then height at the width that survived.
            // A full 2-D search would be more thorough and is not what this number is for:
            // it names ONE small size worth photographing, and it has to be reproducible.
            int w = start.w;
            for (int trial = start.w; trial >= 640; trial -= 10)
            {
                ed.setSize (trial, start.h);
                if (! laysOut (ed)) break;
                w = trial;
            }
            int h = start.h;
            for (int trial = start.h; trial >= 360; trial -= 10)
            {
                ed.setSize (w, trial);
                if (! laysOut (ed)) break;
                h = trial;
            }
            say (juce::String ("  smallest laid-out size (") + (lang == 1 ? "ca" : "en") + "): "
                 + juce::String (w) + " x " + juce::String (h));
            // The LARGER of the two languages is the product's floor: a size English lays
            // out and Catalan does not is not a size this instrument can be shown at.
            best.w = juce::jmax (best.w, w);
            best.h = juce::jmax (best.h, h);
        }
        ed.setLanguageForSnapshot (false);
        ed.setSize (start.w, start.h);
        return best;
    }

    //==========================================================================
    // THE WALK.
    //==========================================================================
    struct View { bool catalan; Size size; float scale; };

    juce::String stem (const Scenario& s, const View& v)
    {
        // scenario, language, size, scale - in that order, so an ls sorts into something
        // a human can navigate.
        const juce::String sc = v.scale == std::floor (v.scale)
                                  ? juce::String ((int) v.scale) : juce::String (v.scale, 2);
        return juce::String (s.id) + "__" + (v.catalan ? "ca" : "en") + "__"
             + juce::String (v.size.w) + "x" + juce::String (v.size.h) + "__" + sc + "x";
    }


    void walk (const juce::StringArray& args)
    {
        auto valueAfter = [&args] (const char* flag) -> juce::String
        {
            const int i = args.indexOf (flag);
            return (i >= 0 && i + 1 < args.size()) ? args[i + 1] : juce::String();
        };

        const auto outArg = valueAfter ("--snapshot");
        const juce::File dir = juce::File::isAbsolutePath (outArg)
                                 ? juce::File (outArg)
                                 : juce::File::getCurrentWorkingDirectory().getChildFile (outArg);
        const juce::String only = valueAfter ("--only");
        const bool tracing = args.contains ("--trace");
        float baseScale = valueAfter ("--scale").getFloatValue();
        if (! (baseScale > 0.05f && baseScale <= 8.0f)) baseScale = 1.0f;

        dir.createDirectory();
        gLog = dir.getChildFile ("snapshot-log.txt");
        gLog.deleteFile();
        // A RIG THAT DIES MUST SAY WHERE. This walk builds a whole instrument per
        // scenario and paints panels nothing else in this build ever paints; a silent
        // access violation half way through leaves a directory of files that LOOKS
        // complete, which is the one failure mode a snapshot rig must not have.
        juce::SystemStats::setApplicationCrashHandler ([] (void*)
        {
            if (gLog != juce::File())
                gLog.appendText ("\nCRASHED. Backtrace:\n" + juce::SystemStats::getStackBacktrace()
                                 + "\n", false, false, "\n");
        });
        say ("== Fiblo Galactic self-snapshot ==");
        say ("  output: " + dir.getFullPathName());
        say ("  base scale: " + juce::String (baseScale, 2)
                  + (only.isNotEmpty() ? ("   filter: " + only) : juce::String()));

        // The measured floor, first, because the matrix below is expanded from it.
        {
            auto p = std::make_unique<Proc>();
            p->prepareToPlay (48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> base (p->createEditor());
            if (auto* ed = dynamic_cast<Ed*> (base.get()))
            {
                ed->setNoteNamesForSnapshot (false);   // the shipped default, not this machine's
                gEditorMin = findSmallestLaidOutSize (*ed);
            }
            say ("  editor floor used by the matrix: "
                      + juce::String (gEditorMin.w) + " x " + juce::String (gEditorMin.h));
        }

        juce::StringArray index;
        int written = 0, failed = 0;

        for (const auto& sc : scenarios())
        {
            if (only.isNotEmpty() && ! juce::String (sc.id).containsIgnoreCase (only)) continue;

            // ONE instrument per scenario, then every view of it. A fresh processor per
            // VIEW would be honest too but four times slower, and the only things a view
            // changes are the language and the size - both of which the editor supports
            // being changed at runtime (that is what setLanguageForSnapshot is for).
            auto proc = std::make_unique<Proc>();
            proc->prepareToPlay (48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> base (proc->createEditor());
            auto* ed = dynamic_cast<Ed*> (base.get());
            if (ed == nullptr) { say ("  FAIL " + juce::String (sc.id) + ": no editor"); ++failed; continue; }
            ed->setSize (1200, 650);
            // EVERY SCENARIO STARTS FROM THE SHIPPED DEFAULT, whatever this machine's user
            // prefers. Note names are a PREFERENCE read out of a global settings file, so
            // without this line the same 84 pictures would come out differently on a
            // machine where somebody had switched them on - and a baseline that depends on
            // who photographed it is not a baseline. Scenario 32 asks for them back.
            ed->setNoteNamesForSnapshot (false);
            sc.setup (*proc, *ed);
            ed->tickTimerForTests();   // no message loop here: this is what the 30 Hz timer would do

            // THE DRAWN TEXT OF THIS SCENARIO, per view and per language, so the two
            // languages of the SAME view can be compared once the views are done. Keyed
            // by the view's size+scale, because a string may only be reachable at one of
            // them (the small size hides rows).
            std::map<juce::String, std::map<juce::String, juce::String>> drawnEn, drawnCa;

            std::vector<View> views;
            for (int lang = 0; lang < 2; ++lang)
            {
                views.push_back ({ lang == 1, defaultSize (sc.target), baseScale });
                if (sc.flags & fSizes)
                {
                    views.push_back ({ lang == 1, minSize (sc.target), baseScale });
                    views.push_back ({ lang == 1, bigSize (sc.target), baseScale });
                }
                if (sc.flags & fScale2)
                    views.push_back ({ lang == 1, defaultSize (sc.target), 2.0f });
            }

            for (const auto& v : views)
            {
                // A BREADCRUMB PER STEP, flushed to disk. The rig builds a whole editor
                // per scenario and paints panels nothing else in this build ever paints;
                // when one of them dies, "which of the six steps was it in" is the whole
                // difference between a fix and a guess. Off unless --trace is passed.
                auto trace = [&] (const juce::String& step)
                {
                    if (tracing && gLog != juce::File())
                        gLog.appendText ("      . " + step + "\n", false, false, "\n");
                };
                trace ("language + size");
                ed->setLanguageForSnapshot (v.catalan);
                juce::Component* shot = ed;
                // A FLOATING PANEL IS PHOTOGRAPHED INSIDE A HOST, not on its own.
                //
                // In real life these two panels are the CONTENT of a DocumentWindow, and
                // JUCE 8's painter walks the tree from whatever component it is handed:
                // asked to snapshot a parentless, never-shown panel it produced a fully
                // transparent 560x520 image - every pixel A0 - which is exactly the kind
                // of picture a redesign must never be built on. Given the parent its
                // window gives it, it paints. The host is thrown away immediately after;
                // no window is created and nothing reaches a screen either way.
                juce::Component host;
                if (sc.target == tEditor)
                {
                    ed->setSize (v.size.w, v.size.h);
                }
                else
                {
                    ed->setSize (1200, 650);
                    auto& panel = ed->floatingPanelForSnapshot (sc.target == tPlayground,
                                                                v.size.w, v.size.h);
                    host.setSize (v.size.w, v.size.h);
                    host.addAndMakeVisible (panel);
                    panel.setTopLeftPosition (0, 0);
                    shot = &host;
                    trace ("panel " + juce::String (panel.getWidth()) + "x"
                           + juce::String (panel.getHeight()) + ", "
                           + juce::String (panel.getNumChildComponents()) + " children, visible="
                           + (panel.isVisible() ? "1" : "0"));
                }

                // THE PICTURE. No OS compositing, nothing outside these bounds, no window
                // shown to anybody - see SnapshotRig.h.
                trace ("snapshot");
                // SoftwareImageType, not the default native one. createComponentSnapshot
                // already guarantees what this rig needs - it renders the component tree
                // itself, so it can never capture another window, another monitor, or one
                // pixel outside these bounds - and asking for a CPU bitmap makes that
                // guarantee complete: on Windows a JUCE 8 native image is a Direct2D
                // bitmap, i.e. a GPU resource whose readback goes through
                // juce::readFromDirect2DBitmap, and that readback is what crashed a full
                // run at the twenty-second scenario. Nothing about the pixels changes;
                // the same painters draw the same thing into ordinary memory.
                const auto img = shot->createComponentSnapshot (shot->getLocalBounds(), true, v.scale,
                                                                juce::SoftwareImageType());
                trace ("png");
                const auto name = stem (sc, v);
                const auto png  = dir.getChildFile (name + ".png");
                bool ok = img.isValid();
                if (ok)
                {
                    png.deleteFile();
                    juce::FileOutputStream os (png);
                    juce::PNGImageFormat fmt;
                    ok = os.openedOk() && fmt.writeImageToStream (img, os);
                }

                // THE NUMBERS. Taken AFTER the picture, because the truncation sweep opens
                // and closes the rhythm panel and re-lays the floating panels to measure
                // them - it must never be able to change what was photographed.
                juce::StringArray problems;
                trace ("layoutProbe");
                const auto probe = ed->layoutProbe();
                trace ("textFitProbe");
                const auto fits  = ed->textFitProbeForTests (v.catalan);
                trace ("dump");
                const auto probeJson = dumpProbe (probe, ed->getLocalBounds(), problems);
                const auto fitsJson  = dumpTextFits (fits, problems);
                juce::String extra;
                trace ("playgroundProbe");
                if (sc.target == tPlayground)
                    extra = ",\n  \"playgroundProbe\": "
                          + dumpPlayground (ed->playgroundProbeForTests (v.size.w, v.size.h, v.catalan));

                // ASSERTION 1, on the strings themselves (so the report can name the one
                // that is wrong) and then, below, on the finished JSON as a backstop.
                trace ("decode check");
                {
                    const auto stem = juce::String (sc.id) + " " + (v.catalan ? "ca" : "en");
                    auto scan = [&stem] (const juce::String& where, const juce::String& s)
                    { checkMojibake (stem, where, s); };
                    for (const auto& f : fits)                  scan (f.path, f.text);
                    for (const auto& l : probe.paintedLabels)   scan ("painted " + l.name, l.text);
                    for (const auto& l : probe.componentLabels) scan ("label " + l.name, l.text);
                    for (const auto& w : probe.wrappedLabels)   scan ("wrapped " + w.name, w.text);
                    for (const auto& w : probe.readoutVariants) scan ("readout " + w.name, w.text);
                    for (const auto& c : probe.comboTexts)      scan ("combo " + c.name, c.text);
                    for (const auto& b : probe.buttonFits)      scan ("button " + b.name, b.text);
                    for (const auto& b : probe.buttonTexts)     scan ("button " + b.first, b.second);
                    scan ("regPeriodsTooltip", probe.regPeriodsTip);
                }
                // ASSERTION 2's raw material: this view's drawn text, by component path.
                {
                    auto& into = v.catalan ? drawnCa : drawnEn;
                    const auto key = juce::String (v.size.w) + "x" + juce::String (v.size.h)
                                   + "@" + juce::String (v.scale, 2);
                    auto& m = into[key];
                    for (const auto& f : fits) m[f.path + " (" + f.kind + ")"] = f.text;
                }

                juce::String js;
                js << "{\n"
                   << "  \"scenario\": " << qs (sc.id) << ",\n"
                   << "  \"description\": " << qs (sc.what) << ",\n"
                   << "  \"target\": " << qs (sc.target == tEditor ? "editor"
                                              : sc.target == tPlayground ? "playground" : "partials") << ",\n"
                   << "  \"language\": " << qs (v.catalan ? "ca" : "en") << ",\n"
                   << "  \"size\": " << obj ({ { "w", num (v.size.w) }, { "h", num (v.size.h) } }) << ",\n"
                   << "  \"scale\": " << num ((double) v.scale) << ",\n"
                   << "  \"png\": " << qs (png.getFileName()) << ",\n"
                   << "  \"pngPixels\": " << obj ({ { "w", num (img.getWidth()) },
                                                    { "h", num (img.getHeight()) } }) << ",\n"
                   << "  \"editorBounds\": " << rectJ (ed->getLocalBounds()) << ",\n"
                   // The floating panels are measured by the sweep at their own fixed
                   // sizes; said out loud so nobody reads a "playground/..." row as a
                   // measurement of the picture beside it when the picture is not that size.
                   << "  \"floatingSweepSizes\": " << obj ({ { "playground", qs ("560x520") },
                                                             { "partials",   qs ("760x520") } }) << ",\n"
                   << "  \"problems\": " << arr (problems) << ",\n"
                   << "  \"layoutProbe\": " << probeJson << ",\n"
                   << "  \"textFits\": " << fitsJson
                   << extra
                   << "\n}\n";

                // ...and the same question asked of the WHOLE file, so nothing this rig
                // writes can carry a mis-decoded character through a field the scan above
                // does not walk. "Zero occurrences across all 82 JSONs" is the standing
                // bar, and this is the line that actually holds it.
                if (js.containsChar (kLead1) || js.containsChar (kLead2))
                {
                    const auto line = "  " + juce::String (sc.id) + " " + (v.catalan ? "ca" : "en")
                                    + "   (somewhere in the JSON, outside the walked strings)";
                    if (! gMojibake.contains (line)) gMojibake.add (line);
                }

                const auto jf = dir.getChildFile (name + ".json");
                jf.deleteFile();
                ok = ok && jf.replaceWithText (js, false, false, "\n");

                if (ok) ++written; else ++failed;
                say (juce::String (ok ? "  ok   " : "  FAIL ") + name
                          + "   " + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight())
                          + " px, " + juce::String (problems.size()) + " fit problem(s)");
                index.add (obj ({ { "stem", qs (name) }, { "scenario", qs (sc.id) },
                                  { "language", qs (v.catalan ? "ca" : "en") },
                                  { "w", num (v.size.w) }, { "h", num (v.size.h) },
                                  { "scale", num ((double) v.scale) },
                                  { "problems", num (problems.size()) } }));
            }

            // ASSERTION 2. Same scenario, same size, same scale, both languages: any
            // component drawing the SAME text in both is untranslated unless it is
            // declared. Compared by component PATH, so this is the drawn result and not
            // a claim about the source - a tr() pair that exists but is never re-applied
            // (the whole of cause (a)) fails here exactly like a missing one.
            for (const auto& viewEn : drawnEn)
            {
                const auto itCa = drawnCa.find (viewEn.first);
                if (itCa == drawnCa.end()) continue;
                for (const auto& row : viewEn.second)
                {
                    const auto itRow = itCa->second.find (row.first);
                    if (itRow == itCa->second.end()) continue;      // not on screen in ca
                    const auto& text = row.second;
                    if (text != itRow->second) continue;            // translated: fine
                    // A number, a symbol, or an empty label. An EMPTY one is deliberately
                    // not this check's business: a caption that vanished is a different
                    // defect, and it is the fit sweep - which records every visible
                    // component - that has to be the one to notice it.
                    if (! hasWord (text)) continue;
                    if (sameInBothLanguagesOnPurpose (text)) continue;
                    const auto line = "  \"" + text.substring (0, 70) + "\"   "
                                    + juce::String (sc.id) + " " + row.first;
                    if (! gUntranslated.contains (line)) gUntranslated.add (line);
                }
            }
        }

        dir.getChildFile ("index.json").replaceWithText ("{\n  \"rows\": " + arr (index) + "\n}\n",
                                                         false, false, "\n");

        // THE VERDICT. Both assertions are gates, not notes: a run that draws a
        // mis-decoded character, or a word that never changes language, has FAILED even
        // though every picture was written and every box fits.
        if (! gMojibake.isEmpty())
        {
            say (juce::String ("NOT DECODED AS UTF-8 (") + juce::String (gMojibake.size())
                 + " distinct): a string built with juce::String(const char*) instead of"
                   " tr() / String::fromUTF8.");
            for (const auto& l : gMojibake) say (l);
        }
        if (! gUntranslated.isEmpty())
        {
            say (juce::String ("NOT TRANSLATED (") + juce::String (gUntranslated.size())
                 + " distinct): drawn identically in en and ca. Give it a tr() pair, make"
                   " sure applyLanguage() re-applies it, or declare it in"
                   " sameInBothLanguagesOnPurpose() with the reason.");
            for (const auto& l : gUntranslated) say (l);
        }
        const bool allOk = failed == 0 && gMojibake.isEmpty() && gUntranslated.isEmpty();
        say (juce::String ("SNAPSHOT ") + (allOk ? "OK" : "FAILED")
                  + ": " + juce::String (written) + " scenario file pair(s) written, "
                  + juce::String (failed) + " failure(s), "
                  + juce::String (gMojibake.size()) + " not decoded, "
                  + juce::String (gUntranslated.size()) + " not translated, floor "
                  + juce::String (gEditorMin.w) + "x" + juce::String (gEditorMin.h));
    }
} // namespace

void runIfRequested()
{
    // THE ORDINARY INTERACTIVE PATH ENDS ON THE NEXT LINE.
    //
    // isStandaloneApp() is false in every plugin wrapper (JUCE only sets its instance
    // hook from the app's own main), so a VST3 in a DAW leaves here having done nothing
    // at all - it never even looks at a command line, which would be the HOST's.
    if (! juce::JUCEApplicationBase::isStandaloneApp()) return;
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    if (! args.contains ("--snapshot")) return;

    // Our own editors run this constructor too. Without the latch the first one would
    // start a second walk inside the first.
    static bool armed = false;
    if (armed) return;
    armed = true;

    // Posted, not run here: the app has not finished starting yet (this is inside the
    // constructor of the window's own editor), and the walk builds editors of its own.
    juce::MessageManager::callAsync ([args]
    {
        walk (args);
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
    });
}

} // namespace fgsnap
