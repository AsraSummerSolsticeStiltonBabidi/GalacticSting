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

    // Lowest MIDI note shown at the left edge; the reference key that defines
    // degree 0 (matches TuningManager::kRefKey = 60 so every tonic reads "0").
    void setRange (int lowestKey) { baseKey = juce::jlimit (0, 120, lowestKey); repaint(); }
    int  getBaseKey() const noexcept { return baseKey; }

    // Optional: a short just-intonation label per degree (e.g. "3/2"), drawn
    // under the degree number - the interval each Sethares minimum lands on.
    void setDegreeLabeller (std::function<juce::String(int)> fn) { getLabel = std::move (fn); }

    // Optional hover text per degree (ratio -> cents, or cents -> nearest just
    // interval). Shown ONLY after the mouse rests 1.25 s on a key, never while
    // that key is being clicked/dragged.
    void setHoverTextProvider (std::function<juce::String(int)> fn) { getHoverText = std::move (fn); }

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
        const float w = (float) getWidth() / (float) cells;
        const auto h = (float) getHeight();

        for (int i = 0; i < cells; ++i)
        {
            const int note = baseKey + i;
            if (note > 127) break;

            const int degree = ((note - kRefKey) % D + D) % D;
            // Visible-light sweep: red (hue 0, low freq) -> violet (~0.82, high freq).
            const float hue = 0.82f * (float) degree / (float) D;
            const bool held = state.isNoteOnForChannels (0xffff, note);

            auto cell = juce::Rectangle<float> (i * w, 0.0f, w, h).reduced (0.6f);
            auto col = juce::Colour::fromHSV (hue, 0.62f, held ? 1.0f : 0.86f, 1.0f);
            g.setColour (held ? col.brighter (0.35f) : col);
            g.fillRect (cell);

            if (held)
            {
                g.setColour (juce::Colours::white);
                g.drawRect (cell, 2.0f);
            }

            // Degree number + (optional) just-interval label below it.
            const juce::String label = getLabel ? getLabel (degree) : juce::String();
            g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
            if (label.isNotEmpty())
            {
                auto top = cell.removeFromTop (cell.getHeight() * 0.52f);
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (18.0f, w * 0.5f)).withStyle ("Bold")));
                g.drawText (juce::String (degree), top, juce::Justification::centred, false);
                drawIntervalLabel (g, label, cell, w);
            }
            else
            {
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (18.0f, w * 0.55f)).withStyle ("Bold")));
                g.drawText (juce::String (degree), cell, juce::Justification::centred, false);
            }
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

    void mouseDown (const juce::MouseEvent& e) override { hoverArmed = false; playAt (e.x); }
    void mouseDrag (const juce::MouseEvent& e) override { hoverArmed = false; playAt (e.x); }
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

    int numCells() const
    {
        const int target = juce::jmax (1, getWidth() / 26); // ~26 px per cell
        return juce::jlimit (1, 128 - baseKey, target);
    }

    int noteAt (int x) const
    {
        const int cells = numCells();
        if (cells <= 0 || getWidth() <= 0) return -1;
        const int i = juce::jlimit (0, cells - 1, x * cells / getWidth());
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
        // Arm the hover tooltip only after 1.25 s resting on the same key with no
        // button held (the global TooltipWindow then displays getTooltip()).
        if (! hoverArmed && hoverCell >= 0 && ! isMouseButtonDown()
            && juce::Time::getMillisecondCounter() - hoverStartMs >= 1250)
            hoverArmed = true;
        repaint();
    }

    juce::MidiKeyboardState&        state;
    std::function<int()>            getDegrees;
    std::function<juce::String(int)> getLabel;
    std::function<juce::String(int)> getHoverText;
    int baseKey  = 48; // C3
    int mouseNote = -1;
    int hoverCell = -1;
    bool hoverArmed = false;
    juce::uint32 hoverStartMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleKeyboard)
};
