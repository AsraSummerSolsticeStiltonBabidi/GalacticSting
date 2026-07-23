#pragma once

#include "PluginProcessor.h"
#include "ScaleKeyboard.h"
#include <map>

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

        juce::ListBox      list { "partials", this };
        juce::ToggleButton onT;
        juce::ComboBox     xBox;
        juce::Slider       nSlider;
        juce::Label        heading, listHead, xLbl, nLbl, info, noteHead, durHead, routeHelp;
        juce::TextButton   closeB { "X" };
        juce::Label        slotLbl[RhythmEngine::kSlots];
        juce::Slider       noteS[RhythmEngine::kSlots], durS[RhythmEngine::kSlots];
        juce::TextButton   delB[RhythmEngine::kSlots];
        int numRows = 0;

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
            for (int v = 2; v <= 12; ++v) xBox.addItem (juce::String (v), v - 1); // idx-mapped for the attachment
            addAndMakeVisible (xBox);
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
            closeB.onClick = [this] { setVisible (false); if (onEdited) onEdited(); };
            addAndMakeVisible (closeB);

            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                slotLbl[i].setJustificationType (juce::Justification::centredLeft);
                slotLbl[i].setColour (juce::Label::textColourId, juce::Colours::white);
                slotLbl[i].setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
                addChildComponent (slotLbl[i]);

                noteS[i].setSliderStyle (juce::Slider::LinearHorizontal);
                noteS[i].setRange (0, 127, 1);
                noteS[i].setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 16);
                noteS[i].textFromValueFunction = [] (double v)
                { return juce::MidiMessage::getMidiNoteName ((int) v, true, true, 3); };
                noteS[i].onValueChange = [this, i]
                { if (proc) proc->getRhythm().slots[i].note = (int) noteS[i].getValue(); };
                addChildComponent (noteS[i]);

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
                    if (proc)
                    {
                        proc->getRhythm().slots[i].enabled = false;
                        proc->getRhythm().slots[i].partial = -1;
                    }
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
        void listBoxItemClicked (int row, const juce::MouseEvent&) override
        {
            if (proc == nullptr) return;
            const int existing = slotOfPartial (row);
            if (existing >= 0) // click an assigned partial -> unassign it
            {
                proc->getRhythm().slots[existing].enabled = false;
                proc->getRhythm().slots[existing].partial = -1;
            }
            else               // assign to the first free slot
            {
                for (int i = 0; i < RhythmEngine::kSlots; ++i)
                    if (proc->getRhythm().slots[i].partial.load() < 0)
                    {
                        proc->getRhythm().slots[i].partial = row;
                        proc->getRhythm().slots[i].enabled = true;
                        // Routing a partial means the user WANTS pulses -> flip the
                        // master On switch for them (no silent "why is nothing sent").
                        if (auto* p = proc->apvts.getParameter ("rhythmOn"))
                            if (p->getValue() < 0.5f)
                                p->setValueNotifyingHost (1.0f);
                        break;
                    }
            }
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

        //-- Refresh ----------------------------------------------------------------
        void refreshSlots()
        {
            const auto* t = proc != nullptr ? proc->getLiveTemplate() : nullptr;
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                const int p = proc != nullptr ? proc->getRhythm().slots[i].partial.load() : -1;
                const bool used = p >= 0;
                slotLbl[i].setVisible (used); noteS[i].setVisible (used);
                durS[i].setVisible (used);    delB[i].setVisible (used);
                if (used)
                {
                    juce::String rl;
                    if (t != nullptr && p < (int) t->partials.size() && ratioName)
                        rl = juce::String::fromUTF8 (" \xc2\xb7 ") + ratioName ((double) t->partials[(size_t) p].ratio);
                    slotLbl[i].setText ("P" + juce::String (p + 1) + rl, juce::dontSendNotification);
                    noteS[i].setValue ((double) proc->getRhythm().slots[i].note.load(), juce::dontSendNotification);
                    durS[i].setValue  ((double) proc->getRhythm().slots[i].durMs.load(), juce::dontSendNotification);
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
            r.removeFromTop (6);
            auto left = r.removeFromLeft (236);
            listHead.setBounds (left.removeFromTop (16));
            list.setBounds (left);
            r.removeFromLeft (10);
            {
                auto head = r.removeFromTop (14);
                head.removeFromLeft (104);
                head.removeFromRight (28);
                durHead.setBounds (head.removeFromRight (128));
                head.removeFromRight (4);
                noteHead.setBounds (head);
            }
            auto rows = r.removeFromTop (RhythmEngine::kSlots * 26);
            for (int i = 0; i < RhythmEngine::kSlots; ++i)
            {
                auto row = rows.removeFromTop (26).reduced (0, 2);
                slotLbl[i].setBounds (row.removeFromLeft (104));
                delB[i].setBounds (row.removeFromRight (24));
                row.removeFromRight (4);
                durS[i].setBounds (row.removeFromRight (128));
                row.removeFromRight (4);
                noteS[i].setBounds (row);
            }
            r.removeFromTop (4);
            routeHelp.setBounds (r.removeFromBottom (46));
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
    juce::TextButton   rhythmBtn;  // top-bar: opens the timbre->rhythm MIDI panel
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
    RhythmPanel    rhythmPanel;
    std::unique_ptr<ButtonAttachment>   rhythmOnAtt;
    std::unique_ptr<ComboBoxAttachment> rhythmXAtt;
    std::unique_ptr<SliderAttachment>   rhythmNAtt;
    juce::uint32 lastRhythmGen = 0; // refresh the partial list when the template changes

    juce::GroupComponent gHarmonic, gBase, gWhitelist, gSlope, gSpectrumRange, gFractal,
                         gPrime, gFilter, gAmpEnv, gPitchEnv, gTuning, gTimbre, gSpecScale;
    juce::Label lblBaseN, lblBasePitch, lblRange, lblFractal, lblPrimeLimit, lblCutoff, lblRes, lblEnvAmt,
                lblTimbreRepeats, lblBend, lblPeriod, lblRefA, lblRefHz, lblOctave, lblPitchAmt, lblMorphTime,
                lblTuningBank, tuningStatusLabel, tuningNameLabel, timbreStatusLabel, recentLabel;
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
