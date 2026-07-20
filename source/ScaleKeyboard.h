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
                auto top = cell.removeFromTop (cell.getHeight() * 0.58f);
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (17.0f, w * 0.52f)).withStyle ("Bold")));
                g.drawText (juce::String (degree), top, juce::Justification::centred, false);
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (12.0f, w * 0.42f)).withStyle ("Bold")));
                g.drawText (label, cell, juce::Justification::centredTop, false);
            }
            else
            {
                g.setFont (juce::Font (juce::FontOptions (juce::jmin (18.0f, w * 0.55f)).withStyle ("Bold")));
                g.drawText (juce::String (degree), cell, juce::Justification::centred, false);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override { playAt (e.x); }
    void mouseDrag (const juce::MouseEvent& e) override { playAt (e.x); }
    void mouseUp   (const juce::MouseEvent&)   override { releaseHeld(); }

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

    void timerCallback() override { repaint(); }

    juce::MidiKeyboardState&        state;
    std::function<int()>            getDegrees;
    std::function<juce::String(int)> getLabel;
    int baseKey  = 48; // C3
    int mouseNote = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleKeyboard)
};
