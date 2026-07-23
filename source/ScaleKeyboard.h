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
        if (lo != rangeLo || cnt != rangeCount) { rangeLo = lo; rangeCount = cnt; baseKey = lo; } // new range -> reset view
        clampView();
        repaint();
    }
    int  getBaseKey() const noexcept { return baseKey; }

    // Optional: a short just-intonation label per degree (e.g. "3/2"), drawn
    // under the degree number - the interval each Sethares minimum lands on.
    void setDegreeLabeller (std::function<juce::String(int)> fn) { getLabel = std::move (fn); }

    // Optional hover text per degree (ratio -> cents, or cents -> nearest just
    // interval). Shown ONLY after the mouse rests ~0.5 s on a key, never while
    // that key is being clicked/dragged.
    void setHoverTextProvider (std::function<juce::String(int)> fn) { getHoverText = std::move (fn); }

    // Below-keyboard hover strip: while a key is hovered, each OTHER key shows its
    // distance to it via getDeltaLabel(hoveredNote, otherNote) (multi-line: steps,
    // cents, and a just ratio when exact); the hovered key shows getSourceLabel(note)
    // (e.g. "from 5/1" for spectrum-segment scales) drawn in `sourceColour`.
    void setDeltaProviders (std::function<juce::String(int,int)> delta,
                            std::function<juce::String(int)> source, juce::Colour srcColour)
    { getDeltaLabel = std::move (delta); getSourceLabel = std::move (source); sourceColour = srcColour; }

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
        const float h = (float) getHeight() - (float) stripH;    // keys use the top; strip is below

        // Below-keyboard delta strip: dark, empty until a key is hovered.
        {
            auto strip = juce::Rectangle<float> (0.0f, h, (float) getWidth(), (float) stripH);
            g.setColour (juce::Colour (0xff0c0c12));
            g.fillRect (strip);
        }
        const int hoveredNote = (hoverArmed && hoverCell >= 0) ? baseKey + hoverCell : -1;

        for (int i = 0; i < cells; ++i)
        {
            const int note = baseKey + i;
            if (note > 127) break;

            const int degree = ((note - kRefKey) % D + D) % D;
            // Visible-light sweep: red (hue 0, low freq) -> violet (~0.82, high freq).
            const float hue = 0.82f * (float) degree / (float) D;
            const bool held = state.isNoteOnForChannels (0xffff, note);

            auto cell = juce::Rectangle<float> (aw + i * w, 0.0f, w, h).reduced (0.6f);
            auto col = juce::Colour::fromHSV (hue, 0.62f, held ? 1.0f : 0.86f, 1.0f);
            g.setColour (held ? col.brighter (0.35f) : col);
            g.fillRect (cell);

            if (held)
            {
                g.setColour (juce::Colours::white);
                g.drawRect (cell, 2.0f);
            }

            // Degree number (as WIDE as the key allows via drawFittedText) + (optional)
            // just-interval label below it.
            const juce::String label = getLabel ? getLabel (degree) : juce::String();
            g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
            if (label.isNotEmpty())
            {
                auto top = cell.removeFromTop (cell.getHeight() * 0.52f);
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (top.getHeight() * 0.98f, w * 0.9f)).withStyle ("Bold")));
                g.drawFittedText (juce::String (degree), top.toNearestInt(), juce::Justification::centred, 1, 0.5f);
                drawIntervalLabel (g, label, cell, w);
            }
            else
            {
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (cell.getHeight() * 0.9f, w * 0.9f)).withStyle ("Bold")));
                g.drawFittedText (juce::String (degree), cell.toNearestInt(), juce::Justification::centred, 1, 0.5f);
            }

            // Below-keyboard strip: on hover, this key's distance to the hovered key
            // (or, under the hovered key itself, its "from n/1" source in the spectrum colour).
            if (hoveredNote >= 0)
            {
                auto sc = juce::Rectangle<float> (aw + i * w, h, w, (float) stripH).reduced (1.0f, 2.0f);
                if (note == hoveredNote)
                {
                    const juce::String src = getSourceLabel ? getSourceLabel (note) : juce::String();
                    if (src.isNotEmpty())
                    {
                        g.setColour (sourceColour);
                        g.setFont (juce::Font (juce::FontOptions (juce::jmin (11.5f, w * 0.36f)).withStyle ("Bold")));
                        g.drawFittedText (src, sc.toNearestInt(), juce::Justification::centred, 2, 0.5f);
                    }
                }
                else
                {
                    const juce::String d = getDeltaLabel ? getDeltaLabel (hoveredNote, note) : juce::String();
                    if (d.isNotEmpty())
                    {
                        g.setColour (juce::Colour (0xffc4ccda));
                        g.setFont (juce::Font (juce::FontOptions (juce::jmin (10.5f, w * 0.32f))));
                        g.drawFittedText (d, sc.toNearestInt(), juce::Justification::centred, 3, 0.4f);
                    }
                }
            }
        }

        // Scroll arrows for scales too wide to show at the readable min key width.
        if (isScrollable())
        {
            const bool canL = baseKey > rangeLo;
            const bool canR = baseKey + numCells() < rangeLo + rangeCount;
            auto drawArrow = [&] (juce::Rectangle<float> zone, bool left, bool enabled)
            {
                g.setColour (juce::Colour (0xff1a1a24));
                g.fillRect (zone);
                g.setColour (enabled ? juce::Colours::white : juce::Colour (0xff44444e));
                auto t = zone.withSizeKeepingCentre (zone.getWidth() * 0.42f, zone.getHeight() * 0.16f);
                juce::Path p;
                if (left) { p.addTriangle (t.getRight(), t.getY(), t.getRight(), t.getBottom(), t.getX(), t.getCentreY()); }
                else      { p.addTriangle (t.getX(), t.getY(), t.getX(), t.getBottom(), t.getRight(), t.getCentreY()); }
                g.fillPath (p);
            };
            drawArrow (juce::Rectangle<float> (0.0f, 0.0f, aw, h), true,  canL);
            drawArrow (juce::Rectangle<float> ((float) getWidth() - aw, 0.0f, aw, h), false, canR);
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

    // Only the keys play; the bottom hover-delta strip is a read-out, the side arrows scroll.
    void mouseDown (const juce::MouseEvent& e) override
    {
        hoverArmed = false;
        if (e.y >= getHeight() - stripH) return; // strip
        const int aw = arrowW();
        if (aw > 0 && e.x < aw)                { scrollBy (-scrollStep()); return; }
        if (aw > 0 && e.x >= getWidth() - aw)  { scrollBy ( scrollStep()); return; }
        playAt (e.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        hoverArmed = false;
        const int aw = arrowW();
        if (e.y < getHeight() - stripH && e.x >= aw && e.x < getWidth() - aw) playAt (e.x);
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
        const int cell = noteAt (e.x) >= 0 ? juce::jlimit (0, numCells() - 1, e.x * numCells() / juce::jmax (1, getWidth())) : -1;
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
        // held (the global TooltipWindow then displays getTooltip()).
        if (! hoverArmed && hoverCell >= 0 && ! isMouseButtonDown()
            && juce::Time::getMillisecondCounter() - hoverStartMs >= 500)
            hoverArmed = true;
        repaint();
    }

    juce::MidiKeyboardState&        state;
    std::function<int()>            getDegrees;
    std::function<juce::String(int)> getLabel;
    std::function<juce::String(int)> getHoverText;
    std::function<juce::String(int,int)> getDeltaLabel;  // (hoveredNote, otherNote) -> multi-line delta
    std::function<juce::String(int)>     getSourceLabel; // note -> "from n/1" (segment scales)
    juce::Colour sourceColour { 0xffff9a3c };            // Spectrum-to-Tuning colour for the source label
    static constexpr int stripH = 30;                    // below-keyboard hover-delta strip height
    int baseKey   = 48; // leftmost VISIBLE key (the scroll position)
    int rangeLo   = 48; // full playable range start
    int rangeCount = 40; // full playable range length (keys); a window of it is shown
    int mouseNote = -1;
    int hoverCell = -1;
    bool hoverArmed = false;
    juce::uint32 hoverStartMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleKeyboard)
};
