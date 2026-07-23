#pragma once

#include "PluginProcessor.h"
#include "ScaleKeyboard.h"
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

//==============================================================================
class FractalAdditiveAudioProcessorEditor : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit FractalAdditiveAudioProcessorEditor (FractalAdditiveAudioProcessor&);
    ~FractalAdditiveAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void configGroup (juce::GroupComponent&, juce::Colour);
    void configFader (juce::Slider&, const juce::String& paramID,
                      std::unique_ptr<SliderAttachment>&, juce::Colour, const juce::String& tooltip);
    void configCombo (juce::ComboBox&, const juce::String& paramID,
                      std::unique_ptr<ComboBoxAttachment>&, juce::Colour, const juce::String& tooltip);
    void makeLabel (juce::Label&, juce::Colour);
    void layoutFaders (juce::GroupComponent&, juce::Rectangle<int>, juce::Colour,
                       std::initializer_list<juce::Slider*>, std::initializer_list<juce::Label*>);
    void refocus();
    void pollComputerKeyboard(); // reconcile held keys against the real key state
    void openScalaChooser();
    void openTimbreChooser();
    void openTuningBankChooser();
    void refreshTuningBank(); // sync the bank combo items + selection with the processor
    void loadRecent (int index);
    void forceDefault (const juce::String& paramID);
    void applyTimbreEnablement (bool active);
    void applyLanguage();
    void refreshPresetList();
    void savePresetFlow();
    void updateWarning (int templatePartials);
    void runDissonanceTune();
    void saveDissonanceScl();
    juce::String tr (const char* en, const char* ca) const { return catalan ? juce::String::fromUTF8 (ca) : juce::String (en); }
    void timerCallback() override;

    // ComboBox that refreshes its item list just before its popup opens (so the
    // preset list always reflects the folder contents).
    struct RefreshingComboBox : juce::ComboBox
    {
        std::function<void()> onBeforePopup;
        void showPopup() override { if (onBeforePopup) onBeforePopup(); juce::ComboBox::showPopup(); }
    };

    // Integer-stepped fader. JUCE forces absolute (snap-to-mouse) dragging when
    // one pixel covers less than one step, so Ctrl velocity mode never engages on
    // integer parameters. We disable snapping (see ctor) and stretch the drag
    // distance instead - x4 while Ctrl is held - so these faders get the same
    // fine control as the continuous ones.
    struct SteppedSlider : juce::Slider
    {
        int normalSensitivity = 300;
        void mouseDown (const juce::MouseEvent& e) override
        {
            setMouseDragSensitivity (e.mods.isCtrlDown() ? normalSensitivity * 4 : normalSensitivity);
            juce::Slider::mouseDown (e);
        }
    };

    // Sorted list of the generated scale's degrees: colour chip + interval label
    // + dissonance height on a green->yellow->red gradient. Data set by the timer
    // whenever a new scale is generated.
    struct DissDegreeList : juce::Component, juce::TooltipClient
    {
        struct Row { juce::String num, label, value; juce::Colour chip, valueCol; };
        std::vector<Row> rows;
        juce::String tip;
        juce::String getTooltip() override { return tip; }
        void paint (juce::Graphics& g) override
        {
            if (rows.empty())
                return;
            // floor 13 px: even 25 rows (24 degrees + period) fit the panel height
            const int rowH = juce::jlimit (13, 22, getHeight() / juce::jmax (1, (int) rows.size()));
            int y = 0;
            for (const auto& r : rows)
            {
                if (y + rowH > getHeight()) break;
                auto chipR = juce::Rectangle<int> (0, y + 1, rowH - 2, rowH - 2);
                g.setColour (r.chip);
                g.fillRoundedRectangle (chipR.toFloat(), 3.0f);
                g.setColour (r.chip.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
                g.setFont (juce::Font (juce::FontOptions ((float) rowH * 0.62f).withStyle ("Bold")));
                g.drawText (r.num, chipR, juce::Justification::centred, false);

                g.setColour (juce::Colour (0xffb9c0d0));
                g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                g.drawText (r.label, rowH + 4, y, 70, rowH, juce::Justification::centredLeft, false);

                g.setColour (r.valueCol);
                g.drawText (r.value, rowH + 78, y, getWidth() - rowH - 78, rowH,
                            juce::Justification::centredLeft, false);
                y += rowH;
            }
        }
    };

    // Closable floating notice (e.g. "only N minima exist"). Click X to dismiss.
    struct NoticeOverlay : juce::Component
    {
        juce::String title, body;
        juce::TextButton closeBtn { "X" };
        NoticeOverlay()
        {
            closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            closeBtn.onClick = [this] { setVisible (false); };
            addAndMakeVisible (closeBtn);
        }
        void resized() override { closeBtn.setBounds (getWidth() - 28, 6, 22, 22); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2141a24));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            auto in = getLocalBounds().reduced (22, 14);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.setFont (juce::Font (juce::FontOptions (17.0f).withStyle ("Bold")));
            g.drawText (title, in.removeFromTop (24), juce::Justification::centredLeft);
            in.removeFromTop (6);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (13.5f)));
            g.drawFittedText (body, in, juce::Justification::topLeft, 8, 1.0f);
        }
    };

    // Darkens everything behind the Spectrum-to-Rhythm panel so it does not
    // distract; clicking it (= clicking outside the panel) closes the panel.
    struct Scrim : juce::Component
    {
        std::function<void()> onClick;
        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xa8000008)); }
    };

    // Timbre -> rhythm (MIDI out) panel: pick partials of the CURRENT patch (shown
    // with their just-intonation ratio to the fundamental, or cents when inexact /
    // .scl-derived), route each to a MIDI note + gate length, and set the global
    // divisor x^n. The pulse trains leave on the plugin's MIDI OUTPUT in exact
    // polyrhythm with the sounding timbre (see RhythmEngine.h for the maths).
    struct RhythmPanel : juce::Component, juce::ListBoxModel
    {
        FractalAdditiveAudioProcessor* proc = nullptr;
        std::function<juce::String (double)> ratioName;                       // 3/2 or 702c
        std::function<juce::String (const char*, const char*)> trF;           // EN/CA
        std::function<void()> onEdited;                                       // refocus etc.
        std::function<void()> onClose;                                        // hide panel + scrim

        juce::ListBox      list { "partials", this };
        juce::ToggleButton onT, mpeT;
        juce::ComboBox     xBox, modeBox;
        juce::Slider       nSlider;
        juce::Label        heading, listHead, xLbl, nLbl, info, noteHead, durHead, routeHelp, bpmLbl;
        juce::TextButton   closeB { "X" }, tempoSend, tempoX2, tempoD2;
        juce::Label        slotLbl[RhythmEngine::kSlots];
        juce::TextEditor   pitchE[RhythmEngine::kSlots];
        juce::TextButton   stepB[RhythmEngine::kSlots][4]; // -12 -1 +1 +12
        juce::Slider       durS[RhythmEngine::kSlots];
        juce::TextButton   delB[RhythmEngine::kSlots];
        int numRows = 0, tempoOct = 0;

        // Scale steps FROM the played note: "7st" = 7 steps up, "7st-" (or "-7st")
        // = 7 steps down. Returns INT_MIN when the text is not a steps spec.
        static int parseSteps (juce::String t)
        {
            t = t.trim().toLowerCase().removeCharacters (" ");
            bool down = false;
            if (t.endsWith ("-"))   { down = true; t = t.dropLastCharacters (1); }
            if (t.startsWith ("-")) { down = true; t = t.substring (1); }
            if (! t.endsWith ("st")) return std::numeric_limits<int>::min();
            const auto num = t.dropLastCharacters (2);
            if (num.isEmpty() || ! num.containsOnly ("0123456789")) return std::numeric_limits<int>::min();
            const int v = juce::jlimit (0, 127, num.getIntValue());
            return down ? -v : v;
        }
        static juce::String stepsText (int steps)
        {
            return steps < 0 ? juce::String (-steps) + "st-" : juce::String (steps) + "st";
        }

        // MPE pitch spec: "1987.4c" (cents) or a ratio "3" / "3/2" -> linear ratio.
        static double parseRatioSpec (juce::String t)
        {
            t = t.trim();
            if (t.endsWithIgnoreCase ("c"))
                return std::exp2 (t.dropLastCharacters (1).getDoubleValue() / 1200.0);
            if (t.containsChar ('/'))
            {
                const double p = t.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
                const double q = t.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
                return (p > 0.0 && q > 0.0) ? p / q : 0.0;
            }
            const double v = t.getDoubleValue();
            return v > 0.0 ? v : 0.0;
        }

        void init()
        {
            heading.setJustificationType (juce::Justification::centredLeft);
            heading.setColour (juce::Label::textColourId, juce::Colour (0xff5cb8ff));
            heading.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
            addAndMakeVisible (heading);
            listHead.setJustificationType (juce::Justification::centredLeft);
            listHead.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
            listHead.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            addAndMakeVisible (listHead);
            list.setRowHeight (20);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff141420));
            addAndMakeVisible (list);
            addAndMakeVisible (onT);
            addAndMakeVisible (mpeT);
            for (int v = 1; v <= 12; ++v) xBox.addItem (juce::String (v), v); // idx-mapped for the attachment
            addAndMakeVisible (xBox);
            modeBox.addItem ("Lowest note", 1); // idx-mapped to the rhythmMode parameter
            modeBox.addItem ("All notes",   2);
            addAndMakeVisible (modeBox);

            // DAW-tempo helper: emits CC20 so a MIDI-mapped host tempo follows pulse 1.
            bpmLbl.setJustificationType (juce::Justification::centredLeft);
            bpmLbl.setColour (juce::Label::textColourId, juce::Colour (0xffffc95c));
            bpmLbl.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
            addAndMakeVisible (bpmLbl);
            for (auto* b : { &tempoSend, &tempoX2, &tempoD2 })
            {
                b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
                addAndMakeVisible (*b);
            }
            tempoX2.setButtonText ("x2");
            tempoD2.setButtonText ("/2");
            tempoX2.onClick = [this] { tempoOct = juce::jlimit (-6, 6, tempoOct + 1); refreshLive(); };
            tempoD2.onClick = [this] { tempoOct = juce::jlimit (-6, 6, tempoOct - 1); refreshLive(); };
            tempoSend.onClick = [this]
            {
                if (proc == nullptr) return;
                const double bpm = tempoBpm();
                if (bpm > 0.0)
                    proc->getRhythm().sendController (20, juce::jlimit (0, 127, (int) std::lround (bpm) - 20));
            };
            nSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            nSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 16);
            addAndMakeVisible (nSlider);
            for (auto* l : { &xLbl, &nLbl })
            {
                l->setJustificationType (juce::Justification::centredRight);
                l->setColour (juce::Label::textColourId, juce::Colour (0xffb9c0d0));
                l->setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                addAndMakeVisible (*l);
            }
            info.setJustificationType (juce::Justification::topLeft);
            info.setColour (juce::Label::textColourId, juce::Colour (0xff7ff59a));
            info.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            addAndMakeVisible (info);
            for (auto* h : { &noteHead, &durHead })
            {
                h->setJustificationType (juce::Justification::centredLeft);
                h->setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
                h->setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
                addAndMakeVisible (*h);
            }
            routeHelp.setJustificationType (juce::Justification::topLeft);
            routeHelp.setColour (juce::Label::textColourId, juce::Colour (0xffffc95c));
            routeHelp.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            addAndMakeVisible (routeHelp);
            closeB.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff44445e));
            closeB.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (closeB);

            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                slotLbl[i].setJustificationType (juce::Justification::centredLeft);
                slotLbl[i].setColour (juce::Label::textColourId, juce::Colours::white);
                slotLbl[i].setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                addChildComponent (slotLbl[i]);

                pitchE[i].setJustification (juce::Justification::centred);
                pitchE[i].setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
                pitchE[i].setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff5cb8ff).withAlpha (0.6f));
                pitchE[i].setColour (juce::TextEditor::textColourId, juce::Colours::white);
                pitchE[i].setSelectAllWhenFocused (true);
                pitchE[i].onReturnKey = [this, i] { commitPitch (i); if (onEdited) onEdited(); };
                pitchE[i].onFocusLost = [this, i] { commitPitch (i); };
                addChildComponent (pitchE[i]);
                static const char* stepTxt[4] = { "-12", "-1", "+1", "+12" };
                static const int   stepVal[4] = { -12, -1, 1, 12 };
                for (int b = 0; b < 4; ++b)
                {
                    stepB[i][b].setButtonText (stepTxt[b]);
                    stepB[i][b].setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
                    stepB[i][b].onClick = [this, i, v = stepVal[b]] { stepPitch (i, v); };
                    addChildComponent (stepB[i][b]);
                }

                durS[i].setSliderStyle (juce::Slider::LinearHorizontal);
                durS[i].setRange (5.0, 500.0, 1.0);
                durS[i].setSkewFactorFromMidPoint (80.0);
                durS[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 16);
                durS[i].textFromValueFunction = [] (double v) { return juce::String ((int) v) + " ms"; };
                durS[i].onValueChange = [this, i]
                { if (proc) proc->getRhythm().slots[i].durMs = (float) durS[i].getValue(); };
                addChildComponent (durS[i]);

                delB[i].setButtonText ("X");
                delB[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe0453f));
                delB[i].onClick = [this, i]
                {
                    auto v = readSlots();
                    if (i < (int) v.size()) v.erase (v.begin() + i);
                    writeSlots (std::move (v)); // stays compact, no gaps
                    refreshSlots();
                    list.repaint();
                };
                addChildComponent (delB[i]);
            }
        }

        //-- ListBoxModel: the current template's partials --------------------------
        int getNumRows() override { return numRows; }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            if (t == nullptr || row >= (int) t->partials.size()) return;
            const bool assigned = slotOfPartial (row) >= 0;
            if (assigned) { g.setColour (juce::Colour (0xff2a4a6a)); g.fillRect (0, 0, w, h); }
            g.setColour (juce::Colour (0xff8f96a8));
            g.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
            g.drawText ("P" + juce::String (row + 1), 6, 0, 40, h, juce::Justification::centredLeft);
            g.setColour (assigned ? juce::Colours::white : juce::Colour (0xffb9c0d0));
            g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
            g.drawText (ratioName ? ratioName ((double) t->partials[(size_t) row].ratio) : juce::String(),
                        50, 0, w - 56, h, juce::Justification::centredLeft);
        }
        // The routed trains are kept COMPACT and in LIST ORDER (sorted by partial
        // index): no empty rows between them, no reordering surprises when a
        // partial is added or removed.
        struct SlotData { int partial, steps; bool useRatio; double ratio; float durMs; };
        std::vector<SlotData> readSlots() const
        {
            std::vector<SlotData> v;
            if (proc == nullptr) return v;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const auto& s = proc->getRhythm().slots[i];
                if (s.partial.load() >= 0)
                    v.push_back ({ s.partial.load(), s.steps.load(), s.useRatio.load(),
                                   s.ratio.load(), s.durMs.load() });
            }
            return v;
        }
        void writeSlots (std::vector<SlotData> v)
        {
            if (proc == nullptr) return;
            std::sort (v.begin(), v.end(), [] (const SlotData& a, const SlotData& b) { return a.partial < b.partial; });
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto& s = proc->getRhythm().slots[i];
                if (i < (int) v.size())
                {
                    s.steps = v[(size_t) i].steps;  s.useRatio = v[(size_t) i].useRatio;
                    s.ratio = v[(size_t) i].ratio;  s.durMs    = v[(size_t) i].durMs;
                    s.partial = v[(size_t) i].partial;
                    s.enabled = true;
                }
                else
                {
                    s.enabled = false; s.partial = -1;
                    s.steps = 0; s.useRatio = false; s.ratio = 1.0; s.durMs = 60.0f;
                }
            }
        }
        void listBoxItemClicked (int row, const juce::MouseEvent&) override
        {
            if (proc == nullptr) return;
            auto v = readSlots();
            const auto it = std::find_if (v.begin(), v.end(), [row] (const SlotData& d) { return d.partial == row; });
            if (it != v.end())
                v.erase (it);                            // click an assigned partial -> unassign
            else if ((int) v.size() < RhythmEngine::kSlots)
            {
                v.push_back ({ row, 0, false, 1.0, 60.0f }); // assign (writeSlots sorts + compacts)
                // Routing a partial means the user WANTS pulses -> flip the master On.
                if (auto* p = proc->apvts.getParameter ("rhythmOn"))
                    if (p->getValue() < 0.5f)
                        p->setValueNotifyingHost (1.0f);
            }
            writeSlots (std::move (v));
            refreshSlots();
            list.repaint();
        }
        int slotOfPartial (int p) const
        {
            if (proc == nullptr) return -1;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
                if (proc->getRhythm().slots[i].partial.load() == p) return i;
            return -1;
        }

        //-- Pitch spec of a slot ---------------------------------------------------
        // Plain: a note name ("A4"). MPE out on: also cents ("1987.4c") or ratios
        // ("3", "3/2") -> the exact pitch f0*ratio is sent via per-note bend.
        bool mpeOn() const
        {
            return proc != nullptr && proc->apvts.getRawParameterValue ("rhythmMpe")->load() > 0.5f;
        }
        void commitPitch (int i)
        {
            if (proc == nullptr) return;
            auto& s = proc->getRhythm().slots[i];
            const auto t = pitchE[i].getText();
            if (const int st = parseSteps (t); st != std::numeric_limits<int>::min())
            { s.steps = st; s.useRatio = false; }
            else if (mpeOn())
            {
                if (const double r = parseRatioSpec (t); r > 0.0)
                { s.ratio = r; s.useRatio = true; }
            }
            refreshSlots();
        }
        void stepPitch (int i, int delta)
        {
            if (proc == nullptr) return;
            auto& s = proc->getRhythm().slots[i];
            if (s.useRatio.load())
                s.ratio = s.ratio.load() * std::exp2 ((double) delta / 12.0); // step in 12ED2 semitones
            else
                s.steps = juce::jlimit (-127, 127, s.steps.load() + delta);
            refreshSlots();
        }
        double tempoBpm() const // pulse 1 (first enabled slot) in BPM, times 2^tempoOct
        {
            if (proc == nullptr) return 0.0;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
                if (proc->getRhythm().slots[i].enabled.load())
                {
                    const double r = (double) proc->getRhythm().getRateHz (i);
                    if (r > 0.0) return r * 60.0 * std::exp2 ((double) tempoOct);
                }
            return 0.0;
        }

        //-- Refresh ----------------------------------------------------------------
        void refreshSlots()
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const int p = proc != nullptr ? proc->getRhythm().slots[i].partial.load() : -1;
                const bool used = p >= 0;
                slotLbl[i].setVisible (used); pitchE[i].setVisible (used);
                durS[i].setVisible (used);    delB[i].setVisible (used);
                for (int b = 0; b < 4; ++b) stepB[i][b].setVisible (used);
                if (used)
                {
                    auto& s = proc->getRhythm().slots[i];
                    juce::String rl;
                    if (t != nullptr && p < (int) t->partials.size() && ratioName)
                        rl = juce::String::fromUTF8 (" \xc2\xb7 ") + ratioName ((double) t->partials[(size_t) p].ratio);
                    slotLbl[i].setText ("P" + juce::String (p + 1) + rl, juce::dontSendNotification);
                    if (! pitchE[i].hasKeyboardFocus (false))
                        pitchE[i].setText (s.useRatio.load() && ratioName
                                               ? ratioName (s.ratio.load())
                                               : stepsText (s.steps.load()),
                                           juce::dontSendNotification);
                    durS[i].setValue ((double) s.durMs.load(), juce::dontSendNotification);
                }
            }
        }

        void refreshLive() // pulse rates + polyrhythm chain, from the audio thread's read-outs
        {
            if (proc == nullptr) return;
            juce::String s;
            const float f0 = proc->getRhythm().getDriverF0();
            s << (f0 > 0.0f ? "f0 = " + juce::String (f0, 2) + " Hz\n"
                            : juce::String (trF ? trF ("(hold a note to pulse)", "(mant\xc3\xa9n una nota per polsar)") : ""))
              ;
            float rates[RhythmEngine::kSlots]; int idx[RhythmEngine::kSlots]; int m = 0;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const float r = proc->getRhythm().getRateHz (i);
                if (r > 0.0f) { rates[m] = r; idx[m] = i; ++m; }
            }
            for (int k = 0; k < m; ++k)
                s << "P" << juce::String (proc->getRhythm().slots[idx[k]].partial.load() + 1)
                  << ": " << juce::String (rates[k], 3) << " Hz ("
                  << juce::String (1000.0f / rates[k], 0) << " ms)\n";
            if (m >= 2 && ratioName)
            {
                s << (trF ? trF ("polyrhythm ", "poliritme ") : juce::String ("polyrhythm "));
                for (int k = 0; k < m - 1; ++k)
                    s << ratioName ((double) rates[k] / (double) rates[k + 1]).replaceCharacter ('/', ':')
                      << (k + 2 < m ? ", " : "");
            }
            info.setText (s, juce::dontSendNotification);

            const double bpm = tempoBpm();
            bpmLbl.setText (bpm > 0.0 ? "= " + juce::String (bpm, 1) + " BPM"
                                      : juce::String ("= ... BPM"),
                            juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14, 10);
            auto top = r.removeFromTop (26);
            closeB.setBounds (top.removeFromRight (24).withSizeKeepingCentre (22, 22));
            heading.setBounds (top);
            r.removeFromTop (4);
            auto ctrl = r.removeFromTop (24);
            onT.setBounds (ctrl.removeFromLeft (64));
            xLbl.setBounds (ctrl.removeFromLeft (76));
            ctrl.removeFromLeft (4);
            xBox.setBounds (ctrl.removeFromLeft (56).reduced (0, 1));
            nLbl.setBounds (ctrl.removeFromLeft (84));
            ctrl.removeFromLeft (4);
            nSlider.setBounds (ctrl);
            r.removeFromTop (4);
            auto ctrl2 = r.removeFromTop (24);
            modeBox.setBounds (ctrl2.removeFromLeft (128).reduced (0, 1));
            ctrl2.removeFromLeft (8);
            mpeT.setBounds (ctrl2.removeFromLeft (86));
            ctrl2.removeFromLeft (8);
            tempoSend.setBounds (ctrl2.removeFromLeft (128).reduced (0, 1));
            ctrl2.removeFromLeft (4);
            tempoX2.setBounds (ctrl2.removeFromLeft (32).reduced (0, 1));
            ctrl2.removeFromLeft (4);
            tempoD2.setBounds (ctrl2.removeFromLeft (32).reduced (0, 1));
            ctrl2.removeFromLeft (6);
            bpmLbl.setBounds (ctrl2);
            r.removeFromTop (6);
            auto left = r.removeFromLeft (236);
            listHead.setBounds (left.removeFromTop (16));
            list.setBounds (left);
            r.removeFromLeft (10);
            {
                auto head = r.removeFromTop (14);
                head.removeFromLeft (96);
                head.removeFromRight (28);
                durHead.setBounds (head.removeFromRight (114));
                head.removeFromRight (4);
                noteHead.setBounds (head);
            }
            auto rows = r.removeFromTop (RhythmEngine::kSlots * 26);
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto row = rows.removeFromTop (26).reduced (0, 2);
                slotLbl[i].setBounds (row.removeFromLeft (96));
                delB[i].setBounds (row.removeFromRight (24));
                row.removeFromRight (4);
                durS[i].setBounds (row.removeFromRight (114));
                row.removeFromRight (4);
                stepB[i][0].setBounds (row.removeFromLeft (30));
                row.removeFromLeft (3);
                stepB[i][1].setBounds (row.removeFromLeft (26));
                row.removeFromLeft (3);
                stepB[i][3].setBounds (row.removeFromRight (30));
                row.removeFromRight (3);
                stepB[i][2].setBounds (row.removeFromRight (26));
                row.removeFromRight (3);
                pitchE[i].setBounds (row.reduced (0, 1));
            }
            r.removeFromTop (4);
            routeHelp.setBounds (r.removeFromBottom (48));
            info.setBounds (r);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf2101018));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xff5cb8ff));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
        }
    };

    // Floating centre panel that explains WHY the current settings are silent.
    struct WarningOverlay : juce::Component
    {
        juce::String title, body;
        WarningOverlay() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xf21a1420));
            g.fillRoundedRectangle (r, 10.0f);
            g.setColour (juce::Colour (0xffffb020));
            g.drawRoundedRectangle (r.reduced (1.5f), 10.0f, 3.0f);
            auto in = getLocalBounds().reduced (22, 14);
            g.setColour (juce::Colour (0xffffb020));
            g.setFont (juce::Font (juce::FontOptions (18.0f).withStyle ("Bold")));
            g.drawText (title, in.removeFromTop (26), juce::Justification::centred);
            in.removeFromTop (6);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawFittedText (body, in, juce::Justification::topLeft, 8, 1.0f);
        }
    };

    FractalAdditiveAudioProcessor& processorRef;

    std::unique_ptr<juce::LookAndFeel_V4> lnf;
    juce::TooltipWindow tooltipWindow { this, 500 };
    ScaleKeyboard keyboard;

    SteppedSlider baseNFader, spectrumRangeFader, fractalFader, timbreRepeatsFader;
    juce::Slider  slopeFader, gainFader, cutoffFader, resFader, fEnvAmtFader, octaveFader, pEnvAmtFader;
    juce::Slider  ampAdsr[4], filtAdsr[4], pitchAdsr[4];
    juce::ComboBox basePitchBox, whitelistBox, phaseBox, primeModeBox, primeLimitBox, tuningBox, bendBox,
                   dissCountBox, dissModeBox, tuningBankBox;
    RefreshingComboBox presetBox;
    juce::ToggleButton primeToggle[9], morphToggle;
    juce::Slider       morphTimeFader;
    juce::TextButton   menuButton; // top-bar menu: Init (reset all) / Panic - works in the VST too
    juce::TextButton   rhythmBtn;  // top-bar: opens the spectrum->rhythm MIDI panel
    juce::TextButton   octDn, octUp, sprDn, sprUp; // OCTAVE cell: -1/+1 steppers
    juce::Label        octVal, sprVal;             // their value read-outs
    juce::TextButton loadSclButton, clearSclButton, loadTimbreButton, clearTimbreButton,
                     langButton, savePresetButton, dissBtn, dissSaveBtn, recentBtn[3], tuningBankLoadBtn;
    juce::Label          vibeLabel;   // "Vibe coded by" credit, left of the language button
    juce::HyperlinkButton vibeLink;   // "Asra3" -> Instagram
    juce::TextEditor periodEdit;
    juce::TextEditor intervalEdit; // Spectrum Builder: typed intervals -> partials
    juce::TextEditor refAEdit;     // Reference A: type the Hz directly ("hz" shown to its right)
    WarningOverlay warningOverlay;
    NoticeOverlay  noticeOverlay;
    DissDegreeList degreeList;
    Scrim          rhythmScrim;
    RhythmPanel    rhythmPanel;
    std::unique_ptr<ButtonAttachment>   rhythmOnAtt, rhythmMpeAtt;
    std::unique_ptr<ComboBoxAttachment> rhythmXAtt, rhythmModeAtt;
    std::unique_ptr<SliderAttachment>   rhythmNAtt;
    juce::uint32 lastRhythmGen = 0; // refresh the partial list when the template changes

    juce::GroupComponent gHarmonic, gBase, gWhitelist, gSlope, gSpectrumRange, gFractal,
                         gPrime, gFilter, gAmpEnv, gPitchEnv, gTuning, gTimbre, gSpecScale;
    juce::Label lblBaseN, lblBasePitch, lblRange, lblFractal, lblPrimeLimit, lblCutoff, lblRes, lblEnvAmt,
                lblTimbreRepeats, lblBend, lblPeriod, lblSteps, lblRefA, lblRefHz, lblOctave, lblPitchAmt,
                lblMorphTime, lblTuningBank, tuningStatusLabel, tuningNameLabel, timbreStatusLabel, recentLabel;
    juce::Label ampAdsrLbl[4], filtAdsrLbl[4], pitchAdsrLbl[4], primeLbl[9];

    std::unique_ptr<SliderAttachment>   baseNAtt, slopeAtt, spectrumRangeAtt, fractalAtt, gainAtt,
                                        cutoffAtt, resAtt, fEnvAmtAtt, timbreRepeatsAtt, octaveAtt, pEnvAmtAtt;
    std::unique_ptr<SliderAttachment>   ampAdsrAtt[4], filtAdsrAtt[4], pitchAdsrAtt[4];
    std::unique_ptr<ComboBoxAttachment> basePitchAtt, whitelistAtt, phaseAtt,
                                        primeModeAtt, primeLimitAtt, tuningAtt, bendAtt, tuningBankAtt;
    std::unique_ptr<ButtonAttachment>   primeToggleAtt[9], morphAtt;
    std::unique_ptr<SliderAttachment>   morphTimeAtt;

    std::unique_ptr<juce::FileChooser>  fileChooser;
    juce::Component::SafePointer<juce::AlertWindow> activePrompt; // open save-preset dialog, if any

    juce::Label meterLabel, helpLabel;

    bool catalan = false;
    int  baseKey = 60; // MIDI note of the leftmost computer key; Z/X shift it by one period
    int  lastPrimeMode = -1;
    bool lastTimbreActive = false;
    int  lastResetOffState = -1; // packs Reset/Off visibility to detect changes for relayout
    int  lastDissGeneration = 0; // refresh the degree list / shortfall notice once per generation
    int  lastKbLo = -1, lastKbNum = -1; // last keyboard window applied (re-adapt on tuning change)
    juce::Rectangle<int> harmonicEngineBounds; // macro-group frame
    juce::Rectangle<int> hcBase, hcHarm, hcPrime, hcSlope; // its 4 sub-cells (dividers/labels in paint)
    juce::Rectangle<int> essentialsBounds;     // Essentials macro-group frame
    juce::Rectangle<int> ecPhase, ecGain, ecRef, ecOct; // its 4 sub-cells (dividers/labels in paint)
    void refreshDegreeList (bool showNotice = true);
    void commitRefA();       // parse the Reference-A text box -> set the refA parameter
    void refreshRefAText();  // set the Reference-A text box from the current parameter
    std::map<int, int> heldKeys;
    juce::uint32 lastFocusedMs = 0; // debounce: keep playing through brief focus flickers

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessorEditor)
};
