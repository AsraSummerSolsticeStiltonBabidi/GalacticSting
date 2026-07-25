#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>

//==============================================================================
// ScaleKeyboard: replaces the piano keyboard with one equal-width cell per MIDI
// key (no black/white distinction). Each cell is coloured by its scale DEGREE,
// sweeping the visible-light order red (low) -> violet (high) across one period
// and repeating every period, and is labelled with the degree number (0-based,
// resetting to 0 at each period). Click / drag to play; held notes (from any
// source, incl. the computer keyboard) are highlighted.
//
// The number of degrees per period comes from the active tuning (12 for 12-TET,
// the loaded scale's count for Scala, the master's map size for MTS-ESP).
//==============================================================================
class ScaleKeyboard : public juce::Component,
                      public juce::TooltipClient,
                      private juce::Timer
{
public:
    ScaleKeyboard (juce::MidiKeyboardState& s, std::function<int()> degreesProvider)
        : state (s), getDegrees (std::move (degreesProvider))
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false); // keep computer-keyboard play alive after a click
        startTimerHz (30); // reflect held notes + tuning changes live
    }

    ~ScaleKeyboard() override { stopTimer(); }

    // The visible key window: lowest MIDI note at the left edge (the reference key
    // that defines degree 0 - matches TuningManager::kRefKey = 60 so every tonic
    // reads "0") and how many keys to show. `numKeys <= 0` restores the automatic
    // ~26 px-per-cell count. Adapting the window to the tuning's period keeps wide
    // scales from exposing sub-audio / ultrasonic keys.
    // The full playable range [lowestKey, lowestKey+numKeys). If more keys fit than
    // the minimum readable width allows, only a window is shown and left/right arrows
    // scroll through the rest (so a dense scale like 31-EDO never gets microscopic).
    void setRange (int lowestKey, int numKeys = 0)
    {
        const int lo = juce::jlimit (0, 127, lowestKey);
        const int cnt = numKeys > 0 ? juce::jmin (numKeys, 128 - lo) : juce::jmax (1, getWidth() / 30);
        if (lo != rangeLo || cnt != rangeCount)
        {
            rangeLo = lo; rangeCount = cnt;
            // Keep the current view; clampView() below slides it the MINIMUM amount
            // into the new range (never teleport to the far end of the keyboard).
        }
        clampView();
        repaint();
    }
    int  getBaseKey() const noexcept { return baseKey; }

    // Scroll the visible window (arrow keys / UI). Public so the editor can bind
    // the computer keyboard's left/right arrows to it.
    void scrollView (int deltaKeys) { scrollBy (deltaKeys); }

    // Auto-follow: shift the visible window the MINIMUM amount that brings the
    // played range [lo..hi] on screen (no jump when it already is).
    void ensureVisible (int lo, int hi)
    {
        const int cells = numCells();
        if (! isScrollable() || hi < lo) return;
        if (hi - lo + 1 > cells) return; // span wider than the window: following one
                                         // end would ping-pong at timer rate - stay put
        if (lo < baseKey)                 scrollBy (lo - baseKey);
        else if (hi >= baseKey + cells)   scrollBy (hi - (baseKey + cells - 1));
    }

    // Screen position of the hovered key's eye (top-centre of its strip cell), so the
    // floating hover window can park right above it with a pointer down to the key.
    // Returns (0,0) when nothing is hovered.
    juce::Point<int> getHoverAnchorScreen() const
    {
        if (hoverCell < 0) return {};
        const int aw = arrowW();
        const float w = (float) keysWidth() / (float) juce::jmax (1, numCells());
        return localPointToGlobal (juce::Point<int> (aw + (int) (((float) hoverCell + 0.5f) * w), 0));
    }
    bool isHoverActive() const noexcept { return hoverArmed && hoverCell >= 0; }

    // Optional: a short just-intonation label per degree (e.g. "3/2"), drawn
    // under the degree number - the interval each Sethares minimum lands on.
    void setDegreeLabeller (std::function<juce::String(int)> fn) { getLabel = std::move (fn); }

    // Optional hover text per degree (ratio -> cents, or cents -> nearest just
    // interval). Shown ONLY after the mouse rests ~0.5 s on a key, never while
    // that key is being clicked/dragged.
    void setHoverTextProvider (std::function<juce::String(int)> fn) { getHoverText = std::move (fn); }
    std::function<juce::String(int)> getHoverTextProvider() const { return getHoverText; }

    // Below-keyboard hover strip: while a key is hovered, each OTHER key shows its
    // distance to it via getDeltaLabel(hoveredNote, otherNote) (multi-line: steps,
    // cents, and a just ratio when exact); the hovered key shows getSourceLabel(note)
    // (e.g. "from 5/1" for spectrum-segment scales) drawn in `sourceColour`.
    void setDeltaProviders (std::function<juce::String(int,int)> delta,
                            std::function<juce::String(int)> source, juce::Colour srcColour)
    { getDeltaLabel = std::move (delta); getSourceLabel = std::move (source); sourceColour = srcColour; }
    void setNoteHzProvider (std::function<double(int)> fn) { getNoteHz = std::move (fn); }

    // While hovering, every OTHER key's strip cell also shows the Sethares sensory
    // dissonance of that key AGAINST the hovered one, coloured green (consonant) ->
    // red (rough) like the interval list.
    struct DissInfo { juce::String text; juce::Colour colour; };
    void setHoverDissProvider (std::function<DissInfo(int,int)> fn) { getHoverDiss = std::move (fn); }

    // Scale editing: right-click a key -> onEditKey(note) (Scala mode: type a new
    // cents/ratio value); Ctrl+left-click -> onAddKey(note, fracWithinKey) adds a
    // note at that spot (folded into every period repetition).
    std::function<void(int)>        onEditKey;
    std::function<void(int,double)> onAddKey;

    juce::String getTooltip() override
    {
        if (hoverArmed && hoverCell >= 0 && getHoverText != nullptr)
        {
            const int note = baseKey + hoverCell;
            if (note <= 127)
            {
                const int D = juce::jmax (1, getDegrees ? getDegrees() : 12);
                return getHoverText (((note - kRefKey) % D + D) % D);
            }
        }
        return {};
    }

    void paint (juce::Graphics& g) override
    {
        const int D = juce::jmax (1, getDegrees ? getDegrees() : 12);
        const int cells = numCells();
        const float aw = (float) arrowW();                       // scroll-arrow zones (0 if all keys fit)
        const float w = (float) keysWidth() / (float) cells;
        const float keysY = (float) stripH;                      // strip on TOP, keys BELOW it (at screen bottom)
        const float keysH = (float) getHeight() - (float) stripH;

        // Info strip ABOVE the keys: dark; shows period-key Hz normally, hover deltas on hover.
        g.setColour (juce::Colour (0xff0c0c12));
        g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), (float) stripH));
        const int hoveredNote = (hoverArmed && hoverCell >= 0) ? baseKey + hoverCell : -1;
        bool heldOffL = false, heldOffR = false; // a sounding note is left/right of the view

        for (int i = 0; i < cells; ++i)
        {
            const int note = baseKey + i;
            if (note > 127) break;

            const int degree = ((note - kRefKey) % D + D) % D;
            const float hue = 0.82f * (float) degree / (float) D;
            const bool held = state.isNoteOnForChannels (0xffff, note);

            auto cell = juce::Rectangle<float> (aw + i * w, keysY, w, keysH).reduced (0.6f);
            // The hovered key stretches a little into the strip - a clear "this one".
            if (note == hoveredNote && degree != 0)
                cell = cell.withTop (cell.getY() - 7.0f);
            auto col = juce::Colour::fromHSV (hue, 0.62f, held ? 1.0f : 0.86f, 1.0f);
            g.setColour (held ? col.brighter (0.35f) : col);
            g.fillRect (cell);
            if (held) { g.setColour (juce::Colours::white); g.drawRect (cell, 2.0f); }

            // Degree number (wide, but a touch smaller so it clears the key edges in
            // dense scales) + the just-interval label below it.
            const juce::String label = getLabel ? getLabel (degree) : juce::String();
            g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
            if (label.isNotEmpty())
            {
                auto top = cell.removeFromTop (cell.getHeight() * 0.52f);
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (top.getHeight() * 0.76f, w * 0.66f)).withStyle ("Bold")));
                g.drawFittedText (juce::String (degree), top.reduced (1, 0).toNearestInt(), juce::Justification::centred, 1, 0.5f);
                drawIntervalLabel (g, label, cell, w);
            }
            else
            {
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (cell.getHeight() * 0.66f, w * 0.66f)).withStyle ("Bold")));
                g.drawFittedText (juce::String (degree), cell.reduced (1, 0).toNearestInt(), juce::Justification::centred, 1, 0.5f);
            }

            // Strip cell (above the key): hover deltas; the hovered key itself gets an
            // EYE whose pupil takes the key's colour (a "you are looking here" marker
            // that covers nothing - the strip is empty space). No hover: period Hz.
            const bool infoActive  = hoveredNote >= 0;
            auto sc = juce::Rectangle<float> (aw + i * w, 0.0f, w, (float) stripH).reduced (1.5f, 3.0f);
            if (hoveredNote >= 0 && note == hoveredNote)
            {
                auto eyeArea = sc.withTrimmedBottom (8.0f); // clear of the stretched key top
                auto eye = eyeArea.withSizeKeepingCentre (juce::jmin (eyeArea.getWidth() - 2.0f, 26.0f),
                                                          juce::jmin (eyeArea.getHeight(), 16.0f));
                g.setColour (juce::Colour (0xffe8ecf4));           // sclera outline
                g.drawEllipse (eye, 1.4f);
                auto iris = eye.withSizeKeepingCentre (eye.getHeight() * 0.78f, eye.getHeight() * 0.78f);
                g.setColour (col);                                  // iris = the key's colour
                g.fillEllipse (iris);
                g.setColour (juce::Colours::black);                 // pupil
                g.fillEllipse (iris.withSizeKeepingCentre (iris.getWidth() * 0.42f, iris.getHeight() * 0.42f));
            }
            else if (infoActive)
            {
                // Bottom BAND: this key's Sethares dissonance AGAINST the hovered one,
                // in its consonance colour; the delta lines live strictly ABOVE it so
                // the white numbers can never overlap the coloured values.
                auto dc = sc;
                auto db = dc.removeFromBottom (14.0f);
                if (getHoverDiss)
                {
                    const auto info = getHoverDiss (hoveredNote, note);
                    if (info.text.isNotEmpty())
                    {
                        g.setColour (info.colour);
                        g.setFont (juce::Font (juce::FontOptions (juce::jmin (11.0f, w * 0.34f)).withStyle ("Bold")));
                        g.drawFittedText (info.text, db.toNearestInt(), juce::Justification::centred, 1, 0.4f);
                    }
                }
                const juce::String d = getDeltaLabel ? getDeltaLabel (hoveredNote, note) : juce::String();
                if (d.isNotEmpty())
                {
                    g.setColour (juce::Colour (0xffc4ccda));
                    g.setFont (juce::Font (juce::FontOptions (juce::jmin (10.5f, w * 0.32f))));
                    g.drawFittedText (d, dc.toNearestInt(), juce::Justification::centred, 3, 0.5f);
                }
            }
            else if ((hoveredNote < 0 || ! infoActive) && degree == 0 && getNoteHz) // period keys: their frequency (khz over 1000)
            {
                const double hz = getNoteHz (note);
                if (hz > 0.0)
                {
                    // MIDI C3 = the tuning's C3 reference itself: Essentials green +
                    // bold, with REFERENCE spelled out above the number.
                    const bool isRef = note == kRefKey;
                    auto hzArea = sc;
                    if (isRef)
                    {
                        // Sits DIRECTLY above its Hz number (tight band, no gap) and is
                        // allowed to spill over the neighbouring key widths instead of
                        // being squeezed horizontally into unreadable letters.
                        auto band = hzArea.removeFromTop (11.0f);
                        band = band.withSizeKeepingCentre (juce::jmax (band.getWidth(), 74.0f), band.getHeight());
                        g.setColour (juce::Colour (0xff4fe09a));
                        g.setFont (juce::Font (juce::FontOptions (9.5f).withStyle ("Bold")));
                        g.drawText ("REFERENCE", band.toNearestInt(), juce::Justification::centred, false);
                        hzArea.setTop (hzArea.getY() - 3.0f); // pull the number up to the word
                    }
                    g.setColour (isRef ? juce::Colour (0xff4fe09a) : juce::Colour (0xffc4ccda));
                    g.setFont (isRef
                        ? juce::Font (juce::FontOptions (juce::jmin (12.0f, w * 0.36f)).withStyle ("Bold"))
                        : juce::Font (juce::FontOptions (juce::jmin (11.0f, w * 0.34f))));
                    const juce::String t = juce::String::fromUTF8 ("\xe2\x89\x88 ")
                        + (hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + "khz"
                                        : juce::String ((int) std::lround (hz)) + "hz");
                    g.drawFittedText (t, hzArea.toNearestInt(), juce::Justification::centred, 1, 0.4f);
                }
            }
        }

        // Sounding notes outside the visible window: flag their direction so the
        // scroll arrows can point you back to what you're playing.
        for (int n = 0; n <= 127; ++n)
            if (state.isNoteOnForChannels (0xffff, n))
            {
                if (n < baseKey) heldOffL = true;
                else if (n >= baseKey + cells) heldOffR = true;
            }

        // Scroll arrows for scales too wide to show at the readable min key width.
        // They glow GREEN when a note you're playing sits off-view in that direction.
        if (isScrollable())
        {
            const bool canL = baseKey > rangeLo;
            const bool canR = baseKey + numCells() < rangeLo + rangeCount;
            auto drawArrow = [&] (juce::Rectangle<float> zone, bool left, bool enabled, bool playing)
            {
                g.setColour (playing ? juce::Colour (0xff1e5a2e) : juce::Colour (0xff1a1a24));
                g.fillRect (zone);
                g.setColour (playing ? juce::Colour (0xff54e678)
                                     : (enabled ? juce::Colours::white : juce::Colour (0xff44444e)));
                const float s = playing ? 0.24f : 0.16f; // bigger when it's pointing at your notes
                auto t = zone.withSizeKeepingCentre (zone.getWidth() * 0.5f, zone.getHeight() * s);
                juce::Path p;
                if (left) p.addTriangle (t.getRight(), t.getY(), t.getRight(), t.getBottom(), t.getX(), t.getCentreY());
                else      p.addTriangle (t.getX(), t.getY(), t.getX(), t.getBottom(), t.getRight(), t.getCentreY());
                g.fillPath (p);
            };
            drawArrow (juce::Rectangle<float> (0.0f, keysY, aw, keysH), true,  canL, heldOffL);
            drawArrow (juce::Rectangle<float> ((float) getWidth() - aw, keysY, aw, keysH), false, canR, heldOffR);
        }
    }

    // Draw a just-interval label inside `area`. A fraction "p/q" is drawn STACKED
    // (numerator over a bar over denominator) so it fits even very narrow keys and
    // never gets truncated to "..."; anything else (a cents value "317c") is drawn
    // on one line, auto-shrunk to fit.
    void drawIntervalLabel (juce::Graphics& g, const juce::String& label,
                            juce::Rectangle<float> area, float w)
    {
        const int slash = label.indexOfChar ('/');
        if (slash > 0 && label.indexOfChar ('c') < 0)
        {
            const auto num = label.substring (0, slash);
            const auto den = label.substring (slash + 1);
            auto numArea = area.removeFromTop (area.getHeight() * 0.5f);
            auto denArea = area;
            const float fs = juce::jmin (13.0f, numArea.getHeight() * 0.95f, w * 0.42f);
            g.setFont (juce::Font (juce::FontOptions (fs).withStyle ("Bold")));
            g.drawFittedText (num, numArea.toNearestInt(), juce::Justification::centredBottom, 1, 0.5f);
            g.drawFittedText (den, denArea.toNearestInt(), juce::Justification::centredTop,    1, 0.5f);
            // fraction bar
            const float bw = juce::jmin (numArea.getWidth() * 0.6f, fs * 1.4f);
            g.fillRect (juce::Rectangle<float> (numArea.getCentreX() - bw * 0.5f,
                                                numArea.getBottom() - 0.5f, bw, 1.2f));
        }
        else
        {
            g.setFont (juce::Font (juce::FontOptions (juce::jmin (12.0f, w * 0.4f)).withStyle ("Bold")));
            g.drawFittedText (label, area.reduced (1.0f, 0.0f).toNearestInt(),
                              juce::Justification::centred, 1, 0.4f);
        }
    }

    // Only the keys (below the top info strip) play; the side arrows scroll.
    void mouseDown (const juce::MouseEvent& e) override
    {
        hoverArmed = false;
        if (e.y < stripH) return; // top info strip
        const int aw = arrowW();
        if (aw > 0 && e.x < aw)                { scrollBy (-scrollStep()); return; }
        if (aw > 0 && e.x >= getWidth() - aw)  { scrollBy ( scrollStep()); return; }
        if (e.mods.isRightButtonDown())        // edit this degree (Scala mode)
        {
            if (onEditKey) { const int n = noteAt (e.x); if (n >= 0) onEditKey (n); }
            return;
        }
        if (e.mods.isCtrlDown())               // add a note at this exact spot
        {
            if (onAddKey)
            {
                const int n = noteAt (e.x);
                if (n >= 0)
                {
                    const double pos = (double) (e.x - aw) * (double) numCells() / (double) keysWidth();
                    onAddKey (n, juce::jlimit (0.0, 0.999, pos - std::floor (pos)));
                }
            }
            return;
        }
        playAt (e.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        hoverArmed = false;
        const int aw = arrowW();
        if (e.y >= stripH && e.x >= aw && e.x < getWidth() - aw) playAt (e.x);
        else releaseHeld();
    }
    int scrollStep() const { return juce::jmax (1, numCells() - 1); } // near-page step
    void mouseUp   (const juce::MouseEvent&)   override
    {
        releaseHeld();
        hoverArmed = false;
        hoverStartMs = juce::Time::getMillisecondCounter(); // re-arm from scratch
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        // Derive the hovered CELL from noteAt so the arrow zones are accounted for
        // (the old width-proportional math drifted one key off next to the arrows).
        const int n    = noteAt (e.x);
        const int cell = n >= 0 ? n - baseKey : -1;
        if (cell != hoverCell)
        {
            hoverCell    = cell;
            hoverArmed   = false;
            hoverStartMs = juce::Time::getMillisecondCounter();
        }
    }
    void mouseExit (const juce::MouseEvent&) override { hoverCell = -1; hoverArmed = false; }

private:
    static constexpr int kRefKey = 60;

    static constexpr int kMinCellW = 32; // a key never gets narrower than this (readability)
    static constexpr int kArrowW   = 22; // scroll-arrow zone width when the scale doesn't fit

    // How many keys are shown at once: everything if it fits at the min width, else a
    // window (with arrows reserved) sized to the min width.
    int numCells() const
    {
        const int fit = juce::jmax (1, getWidth() / kMinCellW);
        if (rangeCount <= fit) return rangeCount;
        return juce::jlimit (1, rangeCount, juce::jmax (1, (getWidth() - 2 * kArrowW) / kMinCellW));
    }
    bool isScrollable() const { return numCells() < rangeCount; }
    int  arrowW()      const { return isScrollable() ? kArrowW : 0; }
    int  keysWidth()   const { return juce::jmax (1, getWidth() - 2 * arrowW()); }
    void clampView() { baseKey = juce::jlimit (rangeLo, rangeLo + rangeCount - numCells(), baseKey); }
    void scrollBy (int d) { baseKey = juce::jlimit (rangeLo, rangeLo + rangeCount - numCells(), baseKey + d); repaint(); }

    // Screen x -> MIDI note (only within the keys area; the arrow zones return -1).
    int noteAt (int x) const
    {
        const int cells = numCells();
        const int aw = arrowW();
        if (cells <= 0 || x < aw || x >= getWidth() - aw) return -1;
        const int i = juce::jlimit (0, cells - 1, (x - aw) * cells / keysWidth());
        const int note = baseKey + i;
        return note <= 127 ? note : -1;
    }

    void playAt (int x)
    {
        const int note = noteAt (x);
        if (note == mouseNote) return;
        releaseHeld();
        if (note >= 0)
        {
            state.noteOn (1, note, 0.85f);
            mouseNote = note;
        }
    }

    void releaseHeld()
    {
        if (mouseNote >= 0)
        {
            state.noteOff (1, mouseNote, 0.0f);
            mouseNote = -1;
        }
    }

    void timerCallback() override
    {
        // Arm the hover tooltip only after resting on the same key with no button
        // held (the global TooltipWindow then displays getTooltip()). Slightly
        // quicker than a standard tooltip - these read-outs are core UI.
        if (! hoverArmed && hoverCell >= 0 && ! isMouseButtonDown()
            && juce::Time::getMillisecondCounter() - hoverStartMs >= 350)
            hoverArmed = true;
        repaint();
    }

    juce::MidiKeyboardState&        state;
    std::function<int()>            getDegrees;
    std::function<juce::String(int)> getLabel;
    std::function<juce::String(int)> getHoverText;
    std::function<juce::String(int,int)> getDeltaLabel;  // (hoveredNote, otherNote) -> multi-line delta
    std::function<juce::String(int)>     getSourceLabel; // note -> "from n/1" (segment scales)
    std::function<double(int)>           getNoteHz;      // note -> frequency (period keys show it)
    std::function<DissInfo(int,int)>     getHoverDiss;   // (hovered, other) -> dissonance line + colour
    juce::Colour sourceColour { 0xffff9a3c };            // Spectrum-to-Tuning colour for the source label
    static constexpr int stripH = 56;                    // top info strip (3 delta lines + dissonance band)
    int baseKey   = 48; // leftmost VISIBLE key (the scroll position)
    int rangeLo   = 48; // full playable range start
    int rangeCount = 40; // full playable range length (keys); a window of it is shown
    int mouseNote = -1;
    int hoverCell = -1;
    bool hoverArmed = false;
    juce::uint32 hoverStartMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleKeyboard)
};
