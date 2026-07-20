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
    juce::Slider  slopeFader, gainFader, cutoffFader, resFader, fEnvAmtFader, refAFader;
    juce::Slider  ampAdsr[4], filtAdsr[4];
    juce::ComboBox basePitchBox, whitelistBox, phaseBox, primeModeBox, primeLimitBox, tuningBox, bendBox,
                   dissCountBox;
    RefreshingComboBox presetBox;
    juce::ToggleButton primeToggle[9], dissMaxToggle;
    juce::TextButton loadSclButton, clearSclButton, loadTimbreButton, clearTimbreButton,
                     langButton, savePresetButton, dissBtn, dissSaveBtn, recentBtn[3];
    juce::Label          vibeLabel;   // "Vibe coded by" credit, left of the language button
    juce::HyperlinkButton vibeLink;   // "Asra3" -> Instagram
    juce::TextEditor periodEdit;
    WarningOverlay warningOverlay;
    NoticeOverlay  noticeOverlay;
    DissDegreeList degreeList;

    juce::GroupComponent gBase, gWhitelist, gSlope, gSpectrumRange, gFractal, gPhase, gGain,
                         gPrime, gFilter, gAmpEnv, gTuning, gTimbre, gSpecScale, gReference;
    juce::Label lblBaseN, lblBasePitch, lblRange, lblPrimeLimit, lblCutoff, lblRes, lblEnvAmt,
                lblTimbreRepeats, lblBend, lblPeriod, lblRefA,
                tuningStatusLabel, tuningNameLabel, timbreStatusLabel, recentLabel;
    juce::Label ampAdsrLbl[4], filtAdsrLbl[4], primeLbl[9];

    std::unique_ptr<SliderAttachment>   baseNAtt, slopeAtt, spectrumRangeAtt, fractalAtt, gainAtt,
                                        cutoffAtt, resAtt, fEnvAmtAtt, timbreRepeatsAtt, refAAtt;
    std::unique_ptr<SliderAttachment>   ampAdsrAtt[4], filtAdsrAtt[4];
    std::unique_ptr<ComboBoxAttachment> basePitchAtt, whitelistAtt, phaseAtt,
                                        primeModeAtt, primeLimitAtt, tuningAtt, bendAtt;
    std::unique_ptr<ButtonAttachment>   primeToggleAtt[9];

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
    void refreshDegreeList (bool showNotice = true);
    std::map<int, int> heldKeys;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessorEditor)
};
