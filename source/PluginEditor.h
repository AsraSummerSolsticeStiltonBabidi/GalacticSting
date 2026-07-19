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
    void openScalaChooser();
    void openTimbreChooser();
    void loadRecent (int index);
    void forceDefault (const juce::String& paramID);
    void applyTimbreEnablement (bool active);
    void applyLanguage();
    juce::String tr (const char* en, const char* ca) const { return catalan ? juce::String::fromUTF8 (ca) : juce::String (en); }
    void timerCallback() override;

    FractalAdditiveAudioProcessor& processorRef;

    std::unique_ptr<juce::LookAndFeel_V4> lnf;
    juce::TooltipWindow tooltipWindow { this, 500 };
    ScaleKeyboard keyboard;

    juce::Slider baseNFader, slopeFader, spectrumRangeFader, fractalFader, gainFader,
                 cutoffFader, resFader, fEnvAmtFader, timbreRepeatsFader;
    juce::Slider ampAdsr[4], filtAdsr[4];
    juce::ComboBox basePitchBox, whitelistBox, phaseBox, primeModeBox, primeLimitBox, tuningBox;
    juce::ToggleButton primeToggle[9];
    juce::TextButton loadSclButton, clearSclButton, loadTimbreButton, clearTimbreButton,
                     langButton, recentBtn[3];

    juce::GroupComponent gBase, gWhitelist, gSlope, gSpectrumRange, gFractal, gPhase, gGain,
                         gPrime, gFilter, gAmpEnv, gTuning, gTimbre;
    juce::Label lblBaseN, lblBasePitch, lblRange, lblPrimeLimit, lblCutoff, lblRes, lblEnvAmt,
                lblTimbreRepeats, tuningStatusLabel, timbreStatusLabel, recentLabel;
    juce::Label ampAdsrLbl[4], filtAdsrLbl[4], primeLbl[9];

    std::unique_ptr<SliderAttachment>   baseNAtt, slopeAtt, spectrumRangeAtt, fractalAtt, gainAtt,
                                        cutoffAtt, resAtt, fEnvAmtAtt, timbreRepeatsAtt;
    std::unique_ptr<SliderAttachment>   ampAdsrAtt[4], filtAdsrAtt[4];
    std::unique_ptr<ComboBoxAttachment> basePitchAtt, whitelistAtt, phaseAtt,
                                        primeModeAtt, primeLimitAtt, tuningAtt;
    std::unique_ptr<ButtonAttachment>   primeToggleAtt[9];

    std::unique_ptr<juce::FileChooser>  fileChooser;

    juce::Label meterLabel, helpLabel;

    bool catalan = false;
    int  baseKey = 60; // MIDI note of the leftmost computer key; Z/X shift it by one period
    int  lastPrimeMode = -1;
    bool lastTimbreActive = false;
    std::map<int, int> heldKeys;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FractalAdditiveAudioProcessorEditor)
};
