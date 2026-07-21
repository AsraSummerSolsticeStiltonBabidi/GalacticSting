#include "PluginEditor.h"
#include "BinaryData.h"
#include "dsp/Dissonance.h"

namespace
{
    // Embedded title typeface (loaded once).
    const juce::Typeface::Ptr& titleTypeface()
    {
        static juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (
            BinaryData::Kablammo_ttf, (size_t) BinaryData::Kablammo_ttfSize);
        return tf;
    }

    // Brighter, higher-contrast section colours.
    const juce::Colour cBase      { 0xff5cb8ff };
    const juce::Colour cWhitelist { 0xff5ff2b0 };
    const juce::Colour cSlope     { 0xff8ef0c4 };
    const juce::Colour cRange     { 0xffffc95c };
    const juce::Colour cFractal   { 0xffff9a3c };
    const juce::Colour cPhase     { 0xff8ec5ff };
    const juce::Colour cPrime     { 0xffffd84d };
    const juce::Colour cFilter    { 0xff4fe6ff };
    const juce::Colour cEnv       { 0xffff8f8f };
    const juce::Colour cPitch     { 0xff40d8c8 }; // pitch envelope (teal)
    const juce::Colour cTuning    { 0xffb9a4ff };
    const juce::Colour cTimbre    { 0xfff58bff };
    const juce::Colour cGain      { 0xffef7dff };
    const juce::Colour cLoad      { 0xff2fb155 }; // green = Load buttons
    const juce::Colour cReset     { 0xffe0453f }; // red   = Reset / Off buttons

    // UI wrapper over the (unit-tested, pure-C++) label logic in Dissonance.h.
    juce::String justName (double ratio)
    {
        return juce::String::fromUTF8 (dissonance::justIntervalName (ratio).c_str());
    }

    // Contiguous scale-degree layout (not a piano): the home row A..L is degrees
    // 0..8, then the top row Q..P continues higher, 9..18 - one unbroken run that
    // matches the colour keyboard. Z / X shift the whole thing by one period.
    struct KeyNote { int ch; int off; };
    const KeyNote kKeyMap[] = {
        {'a',0},{'s',1},{'d',2},{'f',3},{'g',4},{'h',5},{'j',6},{'k',7},{'l',8},
        {'q',9},{'w',10},{'e',11},{'r',12},{'t',13},{'y',14},{'u',15},{'i',16},{'o',17},{'p',18}
    };

    // Bold group titles in a filled chip (so nothing overlaps the title), a thicker
    // rounded frame, and bold labels -> more contrast + clarity.
    struct GalacticLnF : juce::LookAndFeel_V4
    {
        GalacticLnF()
        {
            setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff20202b));
            setColour (juce::ComboBox::textColourId, juce::Colours::white);
            setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff20202b));
            setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff3a3a52));
        }

        juce::Font getLabelFont (juce::Label& l) override
        {
            // A slider's value read-out is a child Label: keep it SMALL (and not bold)
            // so two decimals fit in the narrow box instead of clipping to "...".
            if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
                return juce::Font (juce::FontOptions (10.0f));
            return juce::Font (juce::FontOptions (juce::jmax (12.5f, l.getFont().getHeight())).withStyle ("Bold"));
        }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return juce::Font (juce::FontOptions (juce::jmin (16.0f, (float) buttonHeight * 0.62f)).withStyle ("Bold"));
        }

        // Slider value read-outs: a small font + generous horizontal squish so the
        // value ALWAYS fits (e.g. two decimals) instead of being clipped to "...".
        juce::Label* createSliderTextBox (juce::Slider& s) override
        {
            auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
            l->setFont (juce::Font (juce::FontOptions (11.5f)));
            l->setMinimumHorizontalScale (0.4f); // shrink-to-fit rather than ellipsize
            l->setJustificationType (juce::Justification::centred);
            return l;
        }

        void drawGroupComponentOutline (juce::Graphics& g, int w, int h, const juce::String& text,
                                        const juce::Justification&, juce::GroupComponent& group) override
        {
            const auto col = group.findColour (juce::GroupComponent::outlineColourId);
            constexpr float titleH = 21.0f;
            auto frame = juce::Rectangle<float> (1.3f, titleH * 0.5f,
                                                 (float) w - 2.6f, (float) h - titleH * 0.5f - 1.3f);

            // Interior panel: lighter than the page (0xff0c0c12) and subtly tinted by the
            // section colour, so each box reads as a raised, distinct panel. Sharp
            // (square) corners, not rounded.
            g.setColour (juce::Colour (0xff17171f).interpolatedWith (col, 0.05f));
            g.fillRect (frame);
            g.setColour (col.withAlpha (0.95f));
            g.drawRect (frame, 2.6f); // thick square outline

            // Title: no background chip - BLACK glyphs with a thick coloured stroke
            // (the frame's colour), so the title reads as outlined black text.
            const juce::Font tf (juce::FontOptions (16.0f).withStyle ("Bold"));
            juce::GlyphArrangement ga;
            ga.addLineOfText (tf, text, 12.0f, titleH - 5.5f);
            juce::Path p;
            ga.createPath (p);
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (juce::Colours::black);
            g.fillPath (p);
        }
    };
}

//==============================================================================
FractalAdditiveAudioProcessorEditor::FractalAdditiveAudioProcessorEditor (
        FractalAdditiveAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      keyboard (p.keyboardState, [this] { return processorRef.getScaleDegrees(); })
{
    lnf = std::make_unique<GalacticLnF>();
    setLookAndFeel (lnf.get());

    configGroup (gBase, cBase);
    configFader (baseNFader,   "baseN",    baseNAtt,     cBase, "Base harmonic N");
    configCombo (basePitchBox, "baseMode", basePitchAtt, cBase, "Original = harmonics at natural positions.  Transposed down = lowest partial at the played pitch.");
    makeLabel (lblBaseN, cBase); makeLabel (lblBasePitch, cBase);

    // Morphing: when ON, a spectrum change glides each partial to its new position
    // (a per-partial pitch bend) over the Morph time, instead of snapping.
    morphToggle.setColour (juce::ToggleButton::textColourId, cBase);
    morphToggle.setColour (juce::ToggleButton::tickColourId, cBase);
    morphToggle.setColour (juce::ToggleButton::tickDisabledColourId, cBase.withAlpha (0.4f));
    morphToggle.setMouseClickGrabsKeyboardFocus (false);
    morphToggle.onClick = [this] { refocus(); };
    addAndMakeVisible (morphToggle);
    morphAtt = std::make_unique<ButtonAttachment> (processorRef.apvts, "morph", morphToggle);
    configFader (morphTimeFader, "morphTime", morphTimeAtt, cBase, "Morph transition time in seconds: how long partials take to glide to the new spectrum.");
    morphTimeFader.setSliderStyle (juce::Slider::LinearHorizontal);
    morphTimeFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);
    makeLabel (lblMorphTime, cBase);

    configGroup (gWhitelist, cWhitelist);
    configCombo (whitelistBox, "selection", whitelistAtt, cWhitelist, "Which harmonics may sound: All / Even / Odd / Prime");
    configGroup (gSlope, cSlope);
    configFader (slopeFader, "curveSlope", slopeAtt, cSlope, "Amplitude roll-off. 0 = flat/bright, 1 = saw, 2 = triangle.  (Also thins the highs in Timbre mode.)");
    configGroup (gSpectrumRange, cRange);
    configFader (spectrumRangeFader, "spectrumRange", spectrumRangeAtt, cRange, "Hard-cut the spectrum this many octaves above the played note (10 = full range, no cut).");
    makeLabel (lblRange, cRange);
    configGroup (gFractal, cFractal);
    configFader (fractalFader, "fractalLevel", fractalAtt, cFractal, "Fractal self-similarity level (1 = off)");
    configGroup (gPhase, cPhase);
    configCombo (phaseBox, "phaseMode", phaseAtt, cPhase, "Aligned: tonal pulse, never clips.  Scattered: louder, noisier.");
    configGroup (gGain, cGain);
    configFader (gainFader, "masterGain", gainAtt, cGain, "Master output gain");

    configGroup (gPrime, cPrime);
    configCombo (primeModeBox,  "primeMode",  primeModeAtt,  cPrime, "Limit = drop harmonics with a prime factor above the value.  Whitelist = only the ticked primes.");
    configCombo (primeLimitBox, "primeLimit", primeLimitAtt, cPrime, "Largest prime factor allowed (Limit mode).");
    makeLabel (lblPrimeLimit, cPrime);
    const char* pn[] = { "2","3","5","7","11","13","17","19","23" };
    for (int i = 0; i < 9; ++i)
    {
        primeToggle[i].setButtonText ({});
        primeToggle[i].setColour (juce::ToggleButton::tickColourId, cPrime);
        primeToggle[i].setColour (juce::ToggleButton::tickDisabledColourId, cPrime.withAlpha (0.4f));
        primeToggle[i].setMouseClickGrabsKeyboardFocus (false);
        primeToggle[i].setTooltip ("Allow prime " + juce::String (pn[i]));
        primeToggle[i].onClick = [this] { refocus(); };
        addAndMakeVisible (primeToggle[i]);
        primeToggleAtt[i] = std::make_unique<ButtonAttachment> (processorRef.apvts, "primeWL" + juce::String (i), primeToggle[i]);
        makeLabel (primeLbl[i], cPrime); primeLbl[i].setText (pn[i], juce::dontSendNotification);
    }

    configGroup (gFilter, cFilter);
    configFader (cutoffFader,  "filterCutoff",    cutoffAtt,  cFilter, "Low-pass cutoff");
    configFader (resFader,     "filterResonance", resAtt,     cFilter, "Resonance");
    configFader (fEnvAmtFader, "filterEnvAmount", fEnvAmtAtt, cFilter, "How far the filter envelope moves the cutoff (octaves)");
    makeLabel (lblCutoff, cFilter); makeLabel (lblRes, cFilter); makeLabel (lblEnvAmt, cFilter);

    configGroup (gAmpEnv, cEnv);
    configGroup (gPitchEnv, cPitch);
    configFader (pEnvAmtFader, "pitchEnvAmount", pEnvAmtAtt, cPitch, "How far the pitch envelope bends the pitch (semitones, +/-).");
    makeLabel (lblPitchAmt, cPitch);
    const char* ampIds[]   = { "ampAttack","ampDecay","ampSustain","ampRelease" };
    const char* fltIds[]   = { "filterAttack","filterDecay","filterSustain","filterRelease" };
    const char* pitchIds[] = { "pitchAttack","pitchDecay","pitchSustain","pitchRelease" };
    const char* adsrTxt[]  = { "A","D","S","R" };
    for (int i = 0; i < 4; ++i)
    {
        configFader (ampAdsr[i],   ampIds[i],   ampAdsrAtt[i],   cEnv,    "Amplitude envelope");
        configFader (filtAdsr[i],  fltIds[i],   filtAdsrAtt[i],  cFilter, "Filter envelope");
        configFader (pitchAdsr[i], pitchIds[i], pitchAdsrAtt[i], cPitch,  "Pitch envelope");
        makeLabel (ampAdsrLbl[i],   cEnv);    ampAdsrLbl[i].setText   (adsrTxt[i], juce::dontSendNotification);
        makeLabel (filtAdsrLbl[i],  cFilter); filtAdsrLbl[i].setText  (adsrTxt[i], juce::dontSendNotification);
        makeLabel (pitchAdsrLbl[i], cPitch);  pitchAdsrLbl[i].setText (adsrTxt[i], juce::dontSendNotification);
    }

    // Tuning (scale) + Recent .scl.
    configGroup (gTuning, cTuning);
    configCombo (tuningBox, "tuningMode", tuningAtt, cTuning,
                 "12-TET = standard.  MTS-ESP = follow a connected ODDSound master.  Scala = the loaded .scl scale.");
    loadSclButton.setColour (juce::TextButton::buttonColourId, cLoad);
    loadSclButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadSclButton.setMouseClickGrabsKeyboardFocus (false);
    loadSclButton.setTooltip ("Load a Scala scale file (retunes the notes).");
    loadSclButton.onClick = [this] { openScalaChooser(); };
    addAndMakeVisible (loadSclButton);
    clearSclButton.setColour (juce::TextButton::buttonColourId, cReset);
    clearSclButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearSclButton.setMouseClickGrabsKeyboardFocus (false);
    clearSclButton.setTooltip ("Back to 12-TET.");
    clearSclButton.onClick = [this] { processorRef.clearScala(); refocus(); };
    addAndMakeVisible (clearSclButton);
    makeLabel (recentLabel, cTuning); recentLabel.setJustificationType (juce::Justification::centredLeft);
    for (int i = 0; i < 3; ++i)
    {
        recentBtn[i].setColour (juce::TextButton::buttonColourId, cTuning.withAlpha (0.28f));
        recentBtn[i].setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        recentBtn[i].setMouseClickGrabsKeyboardFocus (false);
        recentBtn[i].onClick = [this, i] { loadRecent (i); };
        addChildComponent (recentBtn[i]);
    }
    tuningStatusLabel.setJustificationType (juce::Justification::centredLeft);
    tuningStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8)); // dim prefix
    tuningStatusLabel.setFont (juce::Font (juce::FontOptions (13.5f).withStyle ("Bold")));
    addAndMakeVisible (tuningStatusLabel);
    tuningNameLabel.setJustificationType (juce::Justification::centredLeft);
    tuningNameLabel.setColour (juce::Label::textColourId, juce::Colours::white); // the loaded scale, highlighted
    tuningNameLabel.setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));
    addAndMakeVisible (tuningNameLabel);

    // Timbre (.scl as spectrum).
    configGroup (gTimbre, cTimbre);
    loadTimbreButton.setColour (juce::TextButton::buttonColourId, cLoad);
    loadTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    loadTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    loadTimbreButton.setTooltip ("Use a Scala file's intervals AS the timbre (partials). Slope + fractal + repeats shape it; the harmonic controls turn off.");
    loadTimbreButton.onClick = [this] { openTimbreChooser(); };
    addAndMakeVisible (loadTimbreButton);
    clearTimbreButton.setColour (juce::TextButton::buttonColourId, cReset);
    clearTimbreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    clearTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    clearTimbreButton.setTooltip ("Back to the harmonic engine.");
    clearTimbreButton.onClick = [this] { processorRef.clearTimbre(); refocus(); };
    addAndMakeVisible (clearTimbreButton);
    configFader (timbreRepeatsFader, "timbreRepeats", timbreRepeatsAtt, cTimbre,
                 "How many times the scale's period stacks into the timbre. More = reaches higher.");
    timbreRepeatsFader.setSliderStyle (juce::Slider::LinearHorizontal); // compact box
    timbreRepeatsFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 16);
    makeLabel (lblTimbreRepeats, cTimbre);
    timbreStatusLabel.setJustificationType (juce::Justification::centredLeft);
    timbreStatusLabel.setColour (juce::Label::textColourId, cTimbre);
    timbreStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    addAndMakeVisible (timbreStatusLabel);

    // Language toggle (top-right).
    langButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
    langButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    langButton.setMouseClickGrabsKeyboardFocus (false);
    langButton.onClick = [this] { catalan = ! catalan; applyLanguage(); refocus(); };
    addAndMakeVisible (langButton);

    // Credit: "Vibe coded by Asra3" (Asra3 is a link to Instagram), left of the language button.
    vibeLabel.setText ("Vibe coded by", juce::dontSendNotification);
    vibeLabel.setJustificationType (juce::Justification::centredRight);
    vibeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8f96a8));
    vibeLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    vibeLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (vibeLabel);

    vibeLink.setButtonText ("Asra3");
    vibeLink.setURL (juce::URL ("https://www.instagram.com/asra3333333/"));
    vibeLink.setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff5cb8ff));
    vibeLink.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")),
                      false, juce::Justification::centredLeft);
    vibeLink.setMouseClickGrabsKeyboardFocus (false);
    vibeLink.setTooltip ("https://www.instagram.com/asra3333333/");
    addAndMakeVisible (vibeLink);

    // Preset browser (top-left): a combo that lists Documents/Galactic Sting/Presets
    // and a Save button. The list refreshes every time the popup opens.
    presetBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff55557a));
    presetBox.setMouseClickGrabsKeyboardFocus (false);
    presetBox.onBeforePopup = [this] { refreshPresetList(); };
    presetBox.onChange = [this]
    {
        const auto name = presetBox.getText();
        if (name.isNotEmpty() && processorRef.loadPreset (name))
            applyTimbreEnablement (processorRef.isTimbreActive());
        refocus();
    };
    addAndMakeVisible (presetBox);
    refreshPresetList();

    savePresetButton.setColour (juce::TextButton::buttonColourId, cLoad);
    savePresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    savePresetButton.setMouseClickGrabsKeyboardFocus (false);
    savePresetButton.onClick = [this] { savePresetFlow(); };
    addAndMakeVisible (savePresetButton);

    // Pitch-bend range (inside the Tuning box).
    configCombo (bendBox, "bendRange", bendAtt, cTuning, {});
    makeLabel (lblBend, cTuning);

    // "Spectrum to Scale" (Sethares): generate + count + reverse + period + save,
    // plus the sorted degree/height list.
    configGroup (gSpecScale, cRange);
    configGroup (gReference, cGain);
    dissBtn.setColour (juce::TextButton::buttonColourId, cTuning.withAlpha (0.35f));
    dissBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    dissBtn.setMouseClickGrabsKeyboardFocus (false);
    dissBtn.onClick = [this] { runDissonanceTune(); };
    addAndMakeVisible (dissBtn);
    for (int i = 1; i <= 24; ++i)
        dissCountBox.addItem (juce::String (i), i);
    dissCountBox.setSelectedId (7, juce::dontSendNotification);
    dissCountBox.setColour (juce::ComboBox::outlineColourId, cTuning.withAlpha (0.7f));
    dissCountBox.setMouseClickGrabsKeyboardFocus (false);
    dissCountBox.onChange = [this] { refocus(); };
    addAndMakeVisible (dissCountBox);
    dissMaxToggle.setColour (juce::ToggleButton::textColourId, cTuning);
    dissMaxToggle.setColour (juce::ToggleButton::tickColourId, cTuning);
    dissMaxToggle.setMouseClickGrabsKeyboardFocus (false);
    dissMaxToggle.onClick = [this] { refocus(); };
    addAndMakeVisible (dissMaxToggle);
    dissSaveBtn.setColour (juce::TextButton::buttonColourId, cLoad);
    dissSaveBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    dissSaveBtn.setMouseClickGrabsKeyboardFocus (false);
    dissSaveBtn.onClick = [this] { saveDissonanceScl(); };
    addAndMakeVisible (dissSaveBtn);
    periodEdit.setText ("2", juce::dontSendNotification);
    periodEdit.setJustification (juce::Justification::centred);
    periodEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    periodEdit.setColour (juce::TextEditor::outlineColourId, cRange.withAlpha (0.7f));
    periodEdit.setSelectAllWhenFocused (true);
    periodEdit.onReturnKey = [this] { refocus(); }; // give the keys back to note-playing
    periodEdit.onEscapeKey = [this] { refocus(); };
    addAndMakeVisible (periodEdit);
    makeLabel (lblPeriod, cRange);
    addAndMakeVisible (degreeList);

    // Spectrum Builder: typed intervals -> partials (Enter applies them).
    intervalEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff20202b));
    intervalEdit.setColour (juce::TextEditor::outlineColourId, cTimbre.withAlpha (0.7f));
    intervalEdit.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    intervalEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
    intervalEdit.onReturnKey = [this]
    {
        juce::String err;
        if (intervalEdit.getText().trim().isNotEmpty()
            && ! processorRef.loadTimbreText (intervalEdit.getText(), err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                tr ("Spectrum Builder", "Constructor d'espectre"), err, this);
        refocus();
    };
    intervalEdit.onEscapeKey = [this] { refocus(); };
    addAndMakeVisible (intervalEdit);

    // Master reference A = <hz> (transposes everything, all tuning modes).
    refAFader.setSliderStyle (juce::Slider::LinearHorizontal);
    refAFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 18);
    refAFader.setColour (juce::Slider::thumbColourId, cGain);
    refAFader.setColour (juce::Slider::trackColourId, cGain.withAlpha (0.55f));
    refAFader.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    refAFader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    refAFader.setMouseClickGrabsKeyboardFocus (false);
    refAFader.setVelocityModeParameters (0.6, 1, 0.06, true, juce::ModifierKeys::ctrlModifier);
    refAFader.onDragEnd = [this] { refocus(); };
    addAndMakeVisible (refAFader);
    refAAtt = std::make_unique<SliderAttachment> (processorRef.apvts, "refA", refAFader);
    makeLabel (lblRefA, cGain);

    addChildComponent (noticeOverlay);

    // A generation may already exist (editor reopened): repopulate the degree
    // list WITHOUT resurrecting an already-dismissed shortfall notice.
    lastDissGeneration = processorRef.getDissGeneration();
    refreshDegreeList (false);

    // Floating "why is this silent?" explainer (shown by the timer when the
    // template is empty).
    addChildComponent (warningOverlay);

    // Integer faders: relative drag (no snap-to-click) so the drag distance,
    // not the track length, sets the resolution; Ctrl stretches it x4 (see
    // SteppedSlider). Base N (1..64) gets the longest travel = least sensitive.
    for (auto* s : { &baseNFader, &spectrumRangeFader, &fractalFader, &timbreRepeatsFader })
        s->setSliderSnapsToMousePosition (false);
    baseNFader.normalSensitivity         = 520;
    spectrumRangeFader.normalSensitivity = 300;
    fractalFader.normalSensitivity       = 260;
    timbreRepeatsFader.normalSensitivity = 280;

    meterLabel.setJustificationType (juce::Justification::centred);
    meterLabel.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    addAndMakeVisible (meterLabel);
    helpLabel.setJustificationType (juce::Justification::centred);
    helpLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0b0));
    helpLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (helpLabel);

    keyboard.setDegreeLabeller ([this] (int d) { return justName (processorRef.getDegreeRatio (d)); });
    keyboard.setHoverTextProvider ([this] (int d) -> juce::String
    {
        // LITERAL interval of this key vs the tonic - no octave folding, so a
        // 5/2 degree in a tritave scale reads as 5/2 / 1586 c, never "5/4".
        const double ratio = processorRef.getDegreeRatio (d);
        if (! (ratio > 0.0) || ! std::isfinite (ratio))
            return {};
        const double cents = 1200.0 * std::log2 (ratio);
        long long p = 1, q = 1;
        dissonance::bestFraction (ratio, p, q);
        const double fracCents = 1200.0 * std::log2 ((double) p / (double) q);
        const auto frac = juce::String (p) + "/" + juce::String (q);

        if (std::abs (cents - fracCents) < 0.5) // the key IS this ratio -> show it in cents
            return frac + " = " + juce::String (cents, 2) + " c";

        // Cents key -> nearest simple just interval + signed error.
        const int err = juce::roundToInt (cents - fracCents);
        return juce::String (cents, 2) + " c   (" + frac + (err >= 0 ? " +" : " -")
             + juce::String (std::abs (err)) + "c)";
    });
    addAndMakeVisible (keyboard);

    applyLanguage();
    applyTimbreEnablement (processorRef.isTimbreActive());
    lastTimbreActive = processorRef.isTimbreActive();

    setWantsKeyboardFocus (true);
    setSize (1280, 636); // still fits a 720p work area incl. the standalone's Options bar
    startTimerHz (30); // also the reconcile rate for stuck/ghost computer-keyboard notes
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });
}

FractalAdditiveAudioProcessorEditor::~FractalAdditiveAudioProcessorEditor()
{
    stopTimer();
    // Release any computer-keyboard notes still held when the UI closes.
    for (const auto& kv : heldKeys)
        processorRef.keyboardState.noteOff (1, kv.second, 0.0f);
    heldKeys.clear();
    // Dismiss a still-open save-preset dialog so no orphaned modal window
    // survives the editor (its callback is already SafePointer-guarded).
    if (activePrompt != nullptr)
        activePrompt->exitModalState (0);
    setLookAndFeel (nullptr);
}

//==============================================================================
void FractalAdditiveAudioProcessorEditor::refocus() { grabKeyboardFocus(); }

void FractalAdditiveAudioProcessorEditor::forceDefault (const juce::String& paramID)
{
    if (auto* prm = processorRef.apvts.getParameter (paramID))
        prm->setValueNotifyingHost (prm->getDefaultValue());
}

void FractalAdditiveAudioProcessorEditor::loadRecent (int index)
{
    const auto paths = processorRef.getRecentSclPaths();
    if (index < paths.size())
    {
        juce::String err;
        if (! processorRef.loadScalaFile (juce::File (paths[index]), err))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load scale", err, this);
    }
    refocus();
}

void FractalAdditiveAudioProcessorEditor::applyLanguage()
{
    using juce::dontSendNotification;
    langButton.setButtonText (catalan ? "ENG" : "CAT");
    langButton.setTooltip (tr ("Switch the interface language to Catal\xc3\xa0.",
                               "Torna la interf\xc3\xad" "cie a l'angl\xc3\xa8s."));

    gBase.setText          (tr ("Base", "Base"));
    gWhitelist.setText     (tr ("Harmonics", "Harm\xc3\xb2nics"));
    gSlope.setText         (tr ("Decay Slope", "Pendent"));
    gSpectrumRange.setText (tr ("Range", "Rang"));
    gFractal.setText       (tr ("Fractal", "Fractal"));
    gPhase.setText         (tr ("Phase", "Fase"));
    gGain.setText          (tr ("Gain", "Guany"));
    gPrime.setText         (tr ("Prime Control", "Control primers"));
    gFilter.setText        (tr ("Filter", "Filtre"));
    gAmpEnv.setText        (tr ("Volume Envelope", "Envolupant volum"));
    gPitchEnv.setText      (tr ("Pitch Envelope", "Envolupant to"));
    gTuning.setText        (tr ("Tuning", "Afinaci\xc3\xb3"));
    gTimbre.setText        (tr ("Spectrum Builder", "Constructor d'espectre"));
    intervalEdit.setTextToShowWhenEmpty (tr ("intervals: 3/2 2 700c ...", "intervals: 3/2 2 700c ..."),
                                         juce::Colour (0xff8f96a8));
    gSpecScale.setText     (tr ("Spectrum to Scale", "Espectre a escala"));
    gReference.setText     (tr ("Reference", "Refer\xc3\xa8ncia"));

    lblBaseN.setText     ("N", dontSendNotification);
    lblBasePitch.setText (tr ("Pitch", "To"), dontSendNotification);
    lblRange.setText     (tr ("Oct", "Oct"), dontSendNotification);
    lblPrimeLimit.setText(tr ("Limit", "L\xc3\xadmit"), dontSendNotification);
    lblCutoff.setText    (tr ("Cut", "Tall"), dontSendNotification);
    lblRes.setText       (tr ("Res", "Res"), dontSendNotification);
    lblEnvAmt.setText    (tr ("Env", "Env"), dontSendNotification);
    lblPitchAmt.setText  (tr ("St", "St"), dontSendNotification);
    lblMorphTime.setText (tr ("Time", "Temps"), dontSendNotification);
    morphToggle.setButtonText (tr ("Morph", "Morph"));
    lblTimbreRepeats.setText (tr ("Reps", "Reps"), dontSendNotification);
    lblBend.setText      (tr ("Pitch bend", "Pitch bend"), dontSendNotification);
    lblPeriod.setText    (tr ("Period", "Per\xc3\xado" "de"), dontSendNotification);
    lblRefA.setText      ("A =", dontSendNotification);
    degreeList.tip = tr (
        "Each row is a degree of the generated scale: colour chip = its keyboard colour, then its interval, then its DISSONANCE HEIGHT on Sethares' curve (how rough that interval sounds with this exact timbre). Lower = more consonant. Sorted most-consonant first; the height colour runs green (period, most consonant) -> yellow -> red (roughest).",
        "Cada fila \xc3\xa9s un grau de l'escala generada: xip de color = el seu color al teclat, despr\xc3\xa9s l'interval, i l'AL\xc3\x87""ADA DE DISSONANCIA a la corba de Sethares (com de aspre sona amb aquest timbre). M\xc3\xa9s baix = m\xc3\xa9s consonant. Ordenats de m\xc3\xa9s consonant primer; el color va de verd (per\xc3\xado""de, m\xc3\xa9s consonant) -> groc -> vermell (m\xc3\xa9s aspre).");
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);

    loadSclButton.setButtonText    (tr ("Load .scl", "Obrir .scl"));
    clearSclButton.setButtonText   (tr ("Reset", "Reinicia"));
    loadTimbreButton.setButtonText (tr ("Load .scl", "Obrir .scl"));
    clearTimbreButton.setButtonText(tr ("Off", "Apaga"));
    savePresetButton.setButtonText (tr ("Save", "Desa"));
    presetBox.setTextWhenNothingSelected (tr ("Presets...", "Presets..."));
    dissBtn.setButtonText          (tr ("Diss. tune", "Afina diss."));
    dissMaxToggle.setButtonText    ("Max");
    dissSaveBtn.setButtonText      (tr ("Save", "Desa"));

    helpLabel.setText (tr ("Play: A S D F G H J K L = the scale in order, then Q W E R T Y U I O P continue higher.   Z / X = down / up one scale period.",
                           "Toca: A S D F G H J K L = l'escala en ordre, i Q W E R T Y U I O P segueixen m\xc3\xa9s agudes.   Z / X = avall / amunt un per\xc3\xado" "de."),
                       dontSendNotification);

    //-- Tooltips: one idea per sentence, no loose ends. ------------------------
    baseNFader.setTooltip (tr (
        "Which harmonic of a virtual fundamental the played key becomes. The spectrum is the harmonic series from N upward (N, N+1, N+2...), so raising N packs in more, closer partials. 1 = normal harmonic series. Hold Ctrl while dragging for fine control.",
        "Quin harm\xc3\xb2nic d'una fonamental virtual passa a ser la tecla tocada. L'espectre \xc3\xa9s la s\xc3\xa8rie harm\xc3\xb2nica des de N cap amunt (N, N+1, N+2...), aix\xc3\xad que pujar N afegeix parcials m\xc3\xa9s densos. 1 = s\xc3\xa8rie harm\xc3\xb2nica normal. Mant\xc3\xa9n Ctrl mentre arrossegues per a ajust fi."));
    basePitchBox.setTooltip (tr (
        "Where that series sits. 'Transposed down': the lowest surviving partial is moved onto the played pitch; with high N and Aligned phase this creates an audible slow pulse (the difference tone) - that is the intended effect, not extra notes. 'Original': harmonics stay at their natural frequencies, so raising N just makes the sound brighter and thinner.",
        "On queda la s\xc3\xa8rie. 'Transposada avall': el parcial m\xc3\xa9s greu es mou al to tocat; amb N alta i fase Alineada apareix un pols greu audible (el to diferencial) - \xc3\xa9s l'efecte buscat, no notes extra. 'Original': els harm\xc3\xb2nics queden a la seva freq\xc3\xbc\xc3\xa8ncia natural, i pujar N nom\xc3\xa9s fa el so m\xc3\xa9s brillant i prim."));
    whitelistBox.setTooltip (tr (
        "Keeps only these harmonic numbers: All, Even (2,4,6...), Odd (1,3,5...) or Prime (2,3,5,7...). A harmonic must ALSO pass Prime Control - both filters apply at once, and if together they exclude everything the sound is silent.",
        "Nom\xc3\xa9s deixa aquests n\xc3\xbameros d'harm\xc3\xb2nic: Tots, Parells (2,4,6...), Senars (1,3,5...) o Primers (2,3,5,7...). L'harm\xc3\xb2nic TAMB\xc3\x89 ha de passar el Control de primers - els dos filtres s'apliquen alhora, i si entre tots dos ho exclouen tot, no sona res."));
    slopeFader.setTooltip (tr (
        "How fast higher partials lose volume: 0 = all equal (brightest), 1 = like a saw (1/n), 2 = like a triangle, higher = darker. Also shapes the stacked scale in Timbre mode.",
        "Com de r\xc3\xa0pid perden volum els parcials aguts: 0 = tots iguals (m\xc3\xa9s brillant), 1 = com una serra (1/n), 2 = com un triangle, m\xc3\xa9s = m\xc3\xa9s fosc. Tamb\xc3\xa9 d\xc3\xb3na forma a l'escala apilada en mode Timbre."));
    spectrumRangeFader.setTooltip (tr (
        "Hard cut: removes every partial more than this many octaves above the played note. 10 = no cut (full range).",
        "Tall dur: elimina tot parcial m\xc3\xa9s agut que aquest nombre d'octaves per sobre de la nota tocada. 10 = sense tall (rang complet)."));
    fractalFader.setTooltip (tr (
        "Replaces each partial with a scaled copy of the whole spectrum, this many levels deep. 1 = off. Each level multiplies density and adds self-similar, inharmonic detail.",
        "Substitueix cada parcial per una c\xc3\xb2pia escalada de tot l'espectre, tants nivells com indiquis. 1 = apagat. Cada nivell multiplica la densitat i afegeix detall autosemblant i inharm\xc3\xb2nic."));
    phaseBox.setTooltip (tr (
        "Starting phase of the partials. 'Aligned': all peak together, forming a periodic pulse - tonal, never clips, and with dense spectra the pulse itself is heard as a low difference tone. 'Scattered': random phases - smoother, louder, no pulse.",
        "Fase inicial dels parcials. 'Alineat': tots fan el pic alhora i formen un pols peri\xc3\xb2""dic - tonal, mai clipa, i amb espectres densos el pols s'escolta com un to diferencial greu. 'Dispers': fases aleat\xc3\xb2ries - m\xc3\xa9s suau, m\xc3\xa9s fort, sense pols."));
    gainFader.setTooltip (tr ("Final output volume, in dB.", "Volum final de sortida, en dB."));
    primeModeBox.setTooltip (tr (
        "How prime numbers filter the harmonics. 'Limit': a harmonic passes only if its largest prime factor is at most the chosen limit. 'Whitelist': it passes only if EVERY prime factor is ticked below.",
        "Com filtren els primers els harm\xc3\xb2nics. 'L\xc3\xadmit': un harm\xc3\xb2nic passa nom\xc3\xa9s si el seu factor primer m\xc3\xa9s gran \xc3\xa9s com a molt el l\xc3\xadmit triat. 'Llista': passa nom\xc3\xa9s si TOTS els seus factors primers estan marcats a sota."));
    primeLimitBox.setTooltip (tr (
        "Largest prime factor a harmonic may contain. Example: limit 7 keeps 14 (2*7) and 25 (5*5) but drops 11 and 22 (2*11). Off = no prime filtering.",
        "Factor primer m\xc3\xa9s gran que pot tenir un harm\xc3\xb2nic. Exemple: l\xc3\xadmit 7 mant\xc3\xa9 14 (2*7) i 25 (5*5) per\xc3\xb2 elimina 11 i 22 (2*11). No = sense filtre de primers."));
    const char* pn2[] = { "2","3","5","7","11","13","17","19","23" };
    for (int i = 0; i < 9; ++i)
        primeToggle[i].setTooltip (tr ("Allow harmonics whose prime factors include ",
                                       "Permet harm\xc3\xb2nics amb el factor primer ") + juce::String (pn2[i]));
    cutoffFader.setTooltip (tr ("Low-pass filter cutoff, in Hz. Partials above it are attenuated.",
                                "Freq\xc3\xbc\xc3\xa8ncia de tall del filtre passa-baixos, en Hz. Els parcials per sobre s'atenuen."));
    resFader.setTooltip (tr ("Resonance: a volume peak right at the cutoff frequency.",
                             "Res(son\xc3\xa0ncia): un pic de volum just a la freq\xc3\xbc\xc3\xa8ncia de tall."));
    fEnvAmtFader.setTooltip (tr (
        "How many octaves the filter envelope moves the cutoff. Positive opens it, negative closes it, 0 = envelope off.",
        "Quantes octaves mou el tall l'envolupant de filtre. Positiu l'obre, negatiu el tanca, 0 = envolupant sense efecte."));
    for (int i = 0; i < 4; ++i)
    {
        ampAdsr[i].setTooltip (tr ("Volume envelope: Attack, Decay, Sustain (level), Release. Times in seconds.",
                                   "Envolupant de volum: Atac, Decay, Sustain (nivell), Release. Temps en segons."));
        filtAdsr[i].setTooltip (tr ("Filter envelope: Attack, Decay, Sustain (level), Release. Times in seconds.",
                                    "Envolupant de filtre: Atac, Decay, Sustain (nivell), Release. Temps en segons."));
        pitchAdsr[i].setTooltip (tr ("Pitch envelope: Attack, Decay, Sustain (level), Release. Bends pitch by the Amount (semitones). Sustain 0 = a pitch blip that settles back to the played note.",
                                     "Envolupant de to: Atac, Decay, Sustain (nivell), Release. Desafina el to segons la Quantitat (semitons). Sustain 0 = un cop de to que torna a la nota tocada."));
    }
    tuningBox.setTooltip (tr (
        "Which tuning the keys follow. 12-TET: standard. MTS-ESP: follow an ODDSound MTS-ESP master running in the session. Scala: the .scl/.ascl file loaded below. Note: Ableton Live's own tuning arrives as MPE pitch bend and works in ANY of these modes (set Bend to +-48).",
        "Quina afinaci\xc3\xb3 segueixen les tecles. 12-TET: est\xc3\xa0ndard. MTS-ESP: segueix un mestre MTS-ESP d'ODDSound actiu a la sessi\xc3\xb3. Scala: el fitxer .scl/.ascl carregat a sota. Nota: l'afinaci\xc3\xb3 pr\xc3\xb2pia d'Ableton Live arriba com a pitch bend MPE i funciona en QUALSEVOL d'aquests modes (posa Bend a +-48)."));
    loadSclButton.setTooltip (tr (
        "Load a Scala .scl or Ableton .ascl file to retune the keys. The 1/1 sits on middle C unless the file sets its own reference (@ABL REFERENCE_PITCH).",
        "Carrega un fitxer Scala .scl o Ableton .ascl per reafinar les tecles. L'1/1 queda al Do central tret que el fitxer porti la seva refer\xc3\xa8ncia (@ABL REFERENCE_PITCH)."));
    clearSclButton.setTooltip (tr ("Unload the scale and return to standard 12-TET.",
                                   "Descarrega l'escala i torna al 12-TET est\xc3\xa0ndard."));
    for (int i = 0; i < 3; ++i)
        recentBtn[i].setTooltip (tr ("Reload this recently used file as the scale.",
                                     "Torna a carregar aquest fitxer recent com a escala."));
    bendBox.setTooltip (tr (
        "Per-note pitch-bend range. +-48 st is the MPE standard and is REQUIRED for Ableton Live 12's tuning systems and the Microtuner device (enable MPE on this plugin in Live). Use +-2 for a classic keyboard pitch wheel.",
        "Rang del pitch bend per nota. +-48 st \xc3\xa9s l'est\xc3\xa0ndard MPE i \xc3\x89S NECESSARI per als sistemes d'afinaci\xc3\xb3 d'Ableton Live 12 i el Microtuner (activa MPE al plugin dins de Live). Fes servir +-2 per a la roda d'un teclat cl\xc3\xa0ssic."));
    loadTimbreButton.setTooltip (tr (
        "Load a .scl/.ascl whose intervals become the PARTIALS of the sound, replacing the harmonic engine. Slope, Fractal, Range and Reps still shape it; the greyed-out controls do not apply.",
        "Carrega un .scl/.ascl els intervals del qual passen a ser els PARCIALS del so, substituint el motor harm\xc3\xb2nic. Pendent, Fractal, Rang i Reps encara hi actuen; els controls en gris no apliquen."));
    clearTimbreButton.setTooltip (tr ("Turn Timbre mode off and return to the harmonic engine.",
                                      "Apaga el mode Timbre i torna al motor harm\xc3\xb2nic."));
    timbreRepeatsFader.setTooltip (tr (
        "How many periods of the scale are stacked as partials. More repeats = the timbre reaches higher in the spectrum.",
        "Quants per\xc3\xado""des de l'escala s'apilen com a parcials. M\xc3\xa9s repeticions = el timbre arriba m\xc3\xa9s amunt de l'espectre."));
    presetBox.setTooltip (tr ("Load a saved preset.", "Carrega un preset desat."));
    savePresetButton.setTooltip (tr ("Save the current sound as a preset (stored in Documents/Galactic Sting/Presets).",
                                     "Desa el so actual com a preset (a Documents/Galactic Sting/Presets)."));
    dissBtn.setTooltip (tr (
        "Computes the Sethares sensory-dissonance curve of the CURRENT timbre and builds a scale from its most consonant points (the curve's minima), then loads it as the active tuning. With Max ticked it uses the roughest points (maxima) instead.",
        "Calcula la corba de dissonancia sensorial de Sethares del timbre ACTUAL i construeix una escala amb els seus punts m\xc3\xa9s consonants (els m\xc3\xadnims de la corba), i la carrega com a afinaci\xc3\xb3 activa. Amb Max marcat fa servir els punts m\xc3\xa9s aspres (m\xc3\xa0xims)."));
    dissCountBox.setTooltip (tr (
        "How many notes the generated scale gets (curve minima - or maxima in Max mode), plus the period as the final note.",
        "Quantes notes tindra l'escala generada (m\xc3\xadnims de la corba - o m\xc3\xa0xims en mode Max), m\xc3\xa9s el per\xc3\xado" "de com a nota final."));
    dissMaxToggle.setTooltip (tr (
        "Reverse mode: pick the curve's MAXIMA (the most dissonant intervals) instead of the minima.",
        "Mode invers: tria els MAXIMS de la corba (els intervals m\xc3\xa9s dissonants) en lloc dels m\xc3\xadnims."));
    dissSaveBtn.setTooltip (tr (
        "Save the generated scale as a .scl file (it also joins the Recent list).",
        "Desa l'escala generada com a fitxer .scl (tamb\xc3\xa9 s'afegeix a Recents)."));
    periodEdit.setTooltip (tr (
        "Period of the generated scale: an integer (\"3\"), a fraction (\"3/2\"), or cents with a trailing c (\"701.955c\"). Default 2 = octave. Wider periods find new extremes inside them; narrower ones omit those outside.",
        "Per\xc3\xado" "de de l'escala generada: un enter (\"3\"), una fracci\xc3\xb3 (\"3/2\") o cents amb una c final (\"701.955c\"). Per defecte 2 = octava. Un per\xc3\xado" "de m\xc3\xa9s ample troba nous extrems a dins; un de m\xc3\xa9s estret omet els de fora."));
    refAFader.setTooltip (tr (
        "Reference pitch: A = this many Hz (default 440). Transposes the WHOLE instrument in every tuning mode. Hold Ctrl for fine control.",
        "To de refer\xc3\xa8ncia: La = aquests Hz (per defecte 440). Transposa TOT l'instrument en qualsevol mode d'afinaci\xc3\xb3. Mant\xc3\xa9n Ctrl per a ajust fi."));

    auto localise = [] (juce::ComboBox& b, const juce::StringArray& items)
    {
        for (int i = 0; i < items.size(); ++i) b.changeItemText (i + 1, items[i]);
        b.setSelectedId (b.getSelectedId(), juce::dontSendNotification); // refresh displayed text
    };
    localise (basePitchBox, { tr ("Transposed down", "Transposada avall"), tr ("Original", "Original") });
    localise (whitelistBox, { tr ("All", "Tots"), tr ("Even", "Parells"), tr ("Odd", "Senars"), tr ("Prime", "Primers") });
    localise (phaseBox,     { tr ("Aligned", "Alineat"), tr ("Scattered", "Dispers") });
    localise (primeModeBox, { tr ("Limit", "L\xc3\xadmit"), tr ("Whitelist", "Llista") });
    localise (primeLimitBox,{ tr ("Off", "No"), "2","3","5","7","11","13","17","19","23" });
    localise (bendBox,      { "\xc2\xb1""2 st", "\xc2\xb1""12 st", "\xc2\xb1""48 st (MPE)" });

    repaint(); // title uses the language
}

//==============================================================================
void FractalAdditiveAudioProcessorEditor::refreshPresetList()
{
    const auto names = processorRef.getPresetNames();
    const auto current = presetBox.getText();
    presetBox.clear (juce::dontSendNotification);
    int id = 1;
    for (const auto& n : names)
        presetBox.addItem (n, id++);
    // keep showing the loaded preset's name without re-triggering a load
    if (current.isNotEmpty() && names.contains (current))
        presetBox.setText (current, juce::dontSendNotification);
}

void FractalAdditiveAudioProcessorEditor::savePresetFlow()
{
    auto* aw = new juce::AlertWindow (tr ("Save preset", "Desar preset"),
                                      tr ("Name for this preset:", "Nom per a aquest preset:"),
                                      juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("name", presetBox.getText(), {});
    aw->addButton (tr ("Save", "Desa"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (tr ("Cancel", "Cancel\xc2\xb7la"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    activePrompt = aw; // so the destructor can dismiss an orphaned dialog

    // The AlertWindow is a top-level window that OUTLIVES this editor if the host
    // closes the plugin UI while the dialog is up -> capture a SafePointer, never
    // raw `this`, and bail out if the editor is gone.
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<FractalAdditiveAudioProcessorEditor> (this), aw] (int result)
        {
            if (safe == nullptr)
                return;

            const auto name = aw->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty())
            {
                const auto stored = safe->processorRef.savePreset (name); // sanitised name, or empty
                if (stored.isNotEmpty())
                {
                    safe->refreshPresetList();
                    safe->presetBox.setText (stored, juce::dontSendNotification);
                }
                else
                {
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        safe->tr ("Save preset", "Desar preset"),
                        safe->tr ("Could not save the preset. Try a different name.",
                                  "No s'ha pogut desar el preset. Prova un altre nom."),
                        safe.getComponent());
                }
            }
            safe->refocus();
        }), true);
}

void FractalAdditiveAudioProcessorEditor::runDissonanceTune()
{
    double periodRatio = 2.0;
    juce::String periodLine = "2/1";
    if (! TuningManager::parsePeriodSpec (periodEdit.getText(), periodRatio, periodLine))
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Spectrum to Scale", "Espectre a escala"),
            tr ("Invalid period. Use an integer (\"3\"), a fraction (\"3/2\"), or cents with a trailing c (\"701.955c\").",
                "Per\xc3\xado" "de no v\xc3\xa0lid. Fes servir un enter (\"3\"), una fracci\xc3\xb3 (\"3/2\") o cents amb una c final (\"701.955c\")."),
            this);
        refocus();
        return;
    }

    const int  n      = juce::jmax (1, dissCountBox.getSelectedId());
    const bool maxima = dissMaxToggle.getToggleState();
    juce::String err;
    if (! processorRef.generateDissonanceScale (n, maxima, periodRatio, periodLine, err))
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            tr ("Spectrum to Scale", "Espectre a escala"), err, this);
    refocus();
}

void FractalAdditiveAudioProcessorEditor::refreshDegreeList (bool showNotice)
{
    const auto& degs = processorRef.getDissDegrees();
    const double g0 = processorRef.getDissGreenHeight();
    const double r0 = processorRef.getDissRedHeight();
    const int    D  = processorRef.getDissFound() + 1; // degrees per period incl. the tonic

    degreeList.rows.clear();
    for (const auto& d : degs)
    {
        DissDegreeList::Row row;
        row.num   = juce::String (d.degree);
        row.label = juce::String::fromUTF8 (dissonance::justIntervalName (d.ratio).c_str());
        row.value = juce::String (d.height, 2);
        row.chip  = juce::Colour::fromHSV (0.82f * (float) d.degree / (float) juce::jmax (1, D), 0.62f, 0.9f, 1.0f);

        // green (most-consonant degree) -> yellow -> red (curve max).
        const double t = juce::jlimit (0.0, 1.0, (d.height - g0) / juce::jmax (1.0e-12, r0 - g0));
        const juce::Colour green (0xff3ec659), yellow (0xffe8d44d), red (0xffe0453f);
        row.valueCol = t < 0.5 ? green.interpolatedWith (yellow, (float) (t * 2.0))
                               : yellow.interpolatedWith (red, (float) ((t - 0.5) * 2.0));
        degreeList.rows.push_back (std::move (row));
    }
    degreeList.repaint();

    // Fewer extremes than requested -> closable notice explaining why. A
    // generation that satisfies the request dismisses any stale notice.
    if (processorRef.getDissFound() >= processorRef.getDissRequested() || ! showNotice)
    {
        if (processorRef.getDissFound() >= processorRef.getDissRequested())
            noticeOverlay.setVisible (false);
    }
    else
    {
        noticeOverlay.title = tr ("Fewer degrees than requested", "Menys graus dels demanats");
        noticeOverlay.body  = tr ("You asked for ", "N'has demanat ")
            + juce::String (processorRef.getDissRequested())
            + tr (" but this timbre's dissonance curve only has ", " per\xc3\xb2 la corba de dissonancia d'aquest timbre nom\xc3\xa9s t\xc3\xa9 ")
            + juce::String (processorRef.getDissFound())
            + tr (" usable extremes in the chosen period. A curve only forms a notch where partials interact; sparse spectra (few partials, e.g. a near-sine) form few notches - a pure sine forms none. The scale was generated with all the extremes that exist. Add partials (higher N, Fractal, flatter Slope) or widen the period for more.",
                  " extrems usables dins del per\xc3\xado" "de triat. La corba nom\xc3\xa9s fa un clot on els parcials interactuen; els espectres escassos (pocs parcials) en fan pocs - un sinus pur no en fa cap. L'escala s'ha generat amb tots els extrems existents. Afegeix parcials (N m\xc3\xa9s alta, Fractal, Pendent m\xc3\xa9s pla) o eixampla el per\xc3\xado" "de per obtenir-ne m\xc3\xa9s.");
        noticeOverlay.setVisible (true);
        noticeOverlay.toFront (false);
        noticeOverlay.repaint();
    }
}

void FractalAdditiveAudioProcessorEditor::saveDissonanceScl()
{
    if (! processorRef.hasGeneratedScl())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            tr ("Save scale", "Desar escala"),
            tr ("Generate a dissonance scale first (the 'Diss. tune' button).",
                "Primer genera una escala de dissonancia (bot\xc3\xb3 'Afina diss.')."), this);
        return;
    }

    fileChooser = std::make_unique<juce::FileChooser> (
        tr ("Save the generated scale (.scl)", "Desa l'escala generada (.scl)"),
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile (processorRef.getGeneratedSclName() + ".scl"),
        "*.scl");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.saveGeneratedScl (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "Save scale", err, this);
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::updateWarning (int templatePartials)
{
    const bool silent = (templatePartials == 0) && ! processorRef.isTimbreActive();
    if (! silent)
    {
        warningOverlay.setVisible (false);
        return;
    }

    const auto& ap = processorRef.apvts;
    const int  sel       = (int) ap.getRawParameterValue ("selection")->load();
    const bool wl        = ap.getRawParameterValue ("primeMode")->load() > 0.5f;
    const int  limitIdx  = (int) ap.getRawParameterValue ("primeLimit")->load();
    const int  baseN     = (int) ap.getRawParameterValue ("baseN")->load();
    unsigned mask = 0;
    for (int i = 0; i < 9; ++i)
        if (ap.getRawParameterValue ("primeWL" + juce::String (i))->load() > 0.5f)
            mask |= (1u << i);

    // Plain-language cause + the single most direct fix. The synth has two
    // harmonic filters (Harmonics and Prime Control) that BOTH must let a
    // harmonic through; when they overlap to nothing, every partial is removed.
    juce::String why;
    if (sel == 3 && wl)
        why = tr ("Two filters are cancelling out.  'Harmonics = Prime' keeps only prime-numbered harmonics, and the Prime whitelist then keeps only harmonics built from the ticked primes.  A prime is only itself, so together they can keep nothing above 23.\n\nFix: set Harmonics to 'All', or set Prime Control to 'Limit / Off'.",
                  "Dos filtres s'anul\xc2\xb7len.  'Harm\xc3\xb2nics = Primers' nom\xc3\xa9s deixa els harm\xc3\xb2nics de n\xc3\xbamero primer, i la Llista de primers nom\xc3\xa9s deixa els fets amb els primers marcats.  Un primer nom\xc3\xa9s \xc3\xa9s ell mateix, aix\xc3\xad que junts no poden deixar res per sobre de 23.\n\nSoluci\xc3\xb3: posa Harm\xc3\xb2nics en 'Tots', o el Control de primers en 'L\xc3\xadmit / No'.");
    else if (wl && mask == 0)
        why = tr ("The Prime whitelist has no primes ticked, so no harmonic is allowed through.\n\nFix: tick at least one prime, or switch Prime Control to 'Limit'.",
                  "La Llista de primers no t\xc3\xa9 cap primer marcat, aix\xc3\xad que no passa cap harm\xc3\xb2nic.\n\nSoluci\xc3\xb3: marca almenys un primer, o posa el Control de primers en 'L\xc3\xadmit'.");
    else if (sel == 3 && ! wl && limitIdx > 0)
        why = tr ("'Harmonics = Prime' keeps only prime harmonics, but the Prime Limit then removes every prime bigger than the limit - and your Base N starts the series above it.\n\nFix: raise the Prime Limit (or set it to Off), or lower Base N.",
                  "'Harm\xc3\xb2nics = Primers' nom\xc3\xa9s deixa harm\xc3\xb2nics primers, per\xc3\xb2 el L\xc3\xadmit de primers treu tot primer m\xc3\xa9s gran que el l\xc3\xadmit - i la teva Base N comen\xc3\xa7""a la s\xc3\xa8rie per sobre.\n\nSoluci\xc3\xb3: puja el L\xc3\xadmit de primers (o posa'l en No), o abaixa la Base N.");
    else
        why = tr ("The Harmonics filter and the Prime Control filter, applied together, remove every harmonic of the current Base N.\n\nFix: relax either filter (Harmonics = All, or Prime Control = Off), or change Base N.",
                  "El filtre d'Harm\xc3\xb2nics i el de Control de primers, aplicats alhora, treuen tots els harm\xc3\xb2nics de la Base N actual.\n\nSoluci\xc3\xb3: relaxa qualsevol dels dos (Harm\xc3\xb2nics = Tots, o Control de primers = No), o canvia la Base N.");

    juce::ignoreUnused (baseN);
    warningOverlay.title = tr ("No sound: every partial is filtered out",
                               "Sense so: s'han filtrat tots els parcials");
    warningOverlay.body  = why;
    warningOverlay.setVisible (true);
    warningOverlay.repaint();
}

void FractalAdditiveAudioProcessorEditor::openScalaChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a scale (.scl / .ascl)", juce::File(), "*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.loadScalaFile (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load scale", err, this);
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::openTimbreChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a scale file as timbre (.scl / .ascl)", juce::File(), "*.scl;*.ascl");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
            {
                juce::String err;
                if (! processorRef.loadTimbreFile (fc.getResult(), err))
                    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load timbre", err, this);
            }
            refocus();
        });
}

void FractalAdditiveAudioProcessorEditor::applyTimbreEnablement (bool active)
{
    const bool en = ! active;
    baseNFader.setEnabled (en);
    for (auto* c : { &basePitchBox, &whitelistBox, &phaseBox, &primeModeBox, &primeLimitBox })
        c->setEnabled (en);
    for (auto& t : primeToggle) t.setEnabled (en);

    const float a = active ? 0.4f : 1.0f;
    for (auto* g : { &gBase, &gWhitelist, &gPhase, &gPrime }) g->setAlpha (a);
    baseNFader.setAlpha (a); basePitchBox.setAlpha (a); whitelistBox.setAlpha (a);
    phaseBox.setAlpha (a); primeModeBox.setAlpha (a); primeLimitBox.setAlpha (a);
    for (auto& t : primeToggle) t.setAlpha (a);
    for (auto& l : primeLbl)    l.setAlpha (a);
    lblBaseN.setAlpha (a); lblBasePitch.setAlpha (a); lblPrimeLimit.setAlpha (a);
}

void FractalAdditiveAudioProcessorEditor::makeLabel (juce::Label& l, juce::Colour c)
{
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, c.brighter (0.15f));
    l.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
    addAndMakeVisible (l);
}

void FractalAdditiveAudioProcessorEditor::configGroup (juce::GroupComponent& g, juce::Colour col)
{
    g.setColour (juce::GroupComponent::outlineColourId, col);
    g.setColour (juce::GroupComponent::textColourId, col);
    addAndMakeVisible (g);
}

void FractalAdditiveAudioProcessorEditor::configFader (juce::Slider& s, const juce::String& paramID,
                                                       std::unique_ptr<SliderAttachment>& att, juce::Colour col,
                                                       const juce::String& tooltip)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 46, 15);
    s.setColour (juce::Slider::thumbColourId, col);
    s.setColour (juce::Slider::trackColourId, col.withAlpha (0.55f));
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setMouseClickGrabsKeyboardFocus (false);
    s.setTooltip (tooltip);
    // Uniform fine-adjust: hold Ctrl while dragging ANY fader to switch to slow
    // velocity mode (some sliders had it by default, others not).
    s.setVelocityModeParameters (0.6, 1, 0.06, true, juce::ModifierKeys::ctrlModifier);
    s.onDragEnd = [this] { refocus(); };
    addAndMakeVisible (s);
    att = std::make_unique<SliderAttachment> (processorRef.apvts, paramID, s);
}

void FractalAdditiveAudioProcessorEditor::configCombo (juce::ComboBox& b, const juce::String& paramID,
                                                       std::unique_ptr<ComboBoxAttachment>& att, juce::Colour col,
                                                       const juce::String& tooltip)
{
    b.setColour (juce::ComboBox::outlineColourId, col.withAlpha (0.7f));
    b.setColour (juce::ComboBox::arrowColourId, col);
    b.setMouseClickGrabsKeyboardFocus (false);
    b.setTooltip (tooltip);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processorRef.apvts.getParameter (paramID)))
        b.addItemList (choice->choices, 1);
    b.onChange = [this] { refocus(); };
    addAndMakeVisible (b);
    att = std::make_unique<ComboBoxAttachment> (processorRef.apvts, paramID, b);
}

void FractalAdditiveAudioProcessorEditor::layoutFaders (juce::GroupComponent& g, juce::Rectangle<int> area, juce::Colour,
                                                        std::initializer_list<juce::Slider*> faders,
                                                        std::initializer_list<juce::Label*> labels)
{
    g.setBounds (area.reduced (3));
    auto in = area.reduced (3).reduced (5, 4); in.removeFromTop (14); // clear the title chip
    const int n = (int) faders.size();
    const int cw = in.getWidth() / juce::jmax (1, n);
    auto fit = faders.begin(); auto lit = labels.begin();
    for (int i = 0; i < n; ++i, ++fit, ++lit)
    {
        auto c = in.removeFromLeft (cw);
        (*lit)->setBounds (c.removeFromTop (13));
        (*fit)->setBounds (c);
    }
}

//==============================================================================
void FractalAdditiveAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0c0c12));

    auto titleArea = getLocalBounds().removeFromTop (68);
    g.setColour (juce::Colour (0xff2a2a3c));
    g.fillRect (0, titleArea.getBottom() - 1, getWidth(), 1);

    // Title: Title Case, each letter in an EQUAL-SIZED SQUARE (sharp corners),
    // keyboard hue logic (first and last letter red), letter black or white by
    // the square's brightness - like the keys. Every letter is stretched to FILL
    // its square (see the loop) - it grows and deforms to almost touch the edges.
    const auto title = catalan ? juce::String::fromUTF8 ("Fibl\xc3\xb3 Gal\xc3\xa0" "ctic")
                               : juce::String ("Galactic Sting");
    const int L = title.length();
    const int period = juce::jmax (1, L - 1); // last letter wraps to degree 0 = red

    const float side   = 52.0f;         // all squares identical, big
    const float gap    = 4.0f;
    const float margin = side * 0.10f;  // small breathing room to the square's edge

    int squares = 0, spaces = 0;
    for (int i = 0; i < L; ++i) (title[i] == ' ' ? spaces : squares)++;
    const float total = (float) squares * (side + gap) + (float) spaces * (side * 0.5f);

    // Nominal-size font; each glyph's path is rescaled per-square below, so the
    // exact height here only needs enough resolution for a clean outline.
    const juce::Font glyphFont = titleTypeface() != nullptr
        ? juce::Font (juce::FontOptions (titleTypeface()).withHeight (64.0f))
        : juce::Font (juce::FontOptions (64.0f).withStyle ("Bold"));

    float x = ((float) getWidth() - total) * 0.5f;
    const float cy = (float) titleArea.getCentreY();
    for (int i = 0; i < L; ++i)
    {
        if (title[i] == ' ') { x += side * 0.5f; continue; }

        const float hue = 0.82f * (float) (i % period) / (float) period;
        const auto sq  = juce::Rectangle<float> (x, cy - side * 0.5f, side, side);
        const auto col = juce::Colour::fromHSV (hue, 0.62f, 0.9f, 1.0f); // same S/V as the keys
        g.setColour (col);
        g.fillRect (sq); // sharp corners

        // Draw the letter as a glyph PATH stretched non-uniformly (X and Y scaled
        // independently) to fill the square minus the margin: it grows AND deforms
        // to almost touch the edges, so a narrow letter (I) widens and a wide one
        // (M) squashes - every square ends up equally, boldly filled.
        juce::GlyphArrangement ga;
        ga.addLineOfText (glyphFont, title.substring (i, i + 1), 0.0f, 0.0f);
        juce::Path gp;
        ga.createPath (gp);
        const auto gb = gp.getBounds();
        if (! gb.isEmpty() && gb.getWidth() > 0.0f && gb.getHeight() > 0.0f)
        {
            const auto target = sq.reduced (margin);
            const auto t = juce::AffineTransform::translation (-gb.getCentreX(), -gb.getCentreY())
                               .scaled (target.getWidth()  / gb.getWidth(),
                                        target.getHeight() / gb.getHeight())
                               .translated (target.getCentreX(), target.getCentreY());
            gp.applyTransform (t);
            g.setColour (col.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white);
            g.fillPath (gp);
        }
        x += side + gap;
    }
}

void FractalAdditiveAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto titleBar = full.removeFromTop (68);
    langButton.setBounds (titleBar.removeFromRight (86).withSizeKeepingCentre (66, 24));
    titleBar.removeFromRight (4);
    {
        auto credit = titleBar.removeFromRight (156).withSizeKeepingCentre (156, 20);
        vibeLink.setBounds (credit.removeFromRight (46));
        credit.removeFromRight (3); // small gap between "by" and the link
        vibeLabel.setBounds (credit);
    }
    auto presetArea = titleBar.removeFromLeft (272).reduced (10, 0);
    presetBox.setBounds (presetArea.removeFromLeft (190).withSizeKeepingCentre (190, 24));
    presetArea.removeFromLeft (6);
    savePresetButton.setBounds (presetArea.withSizeKeepingCentre (juce::jmin (56, presetArea.getWidth()), 24));

    auto area = full.reduced (6, 4);

    {
        auto kb = area.removeFromBottom (88).reduced (2, 5);
        int kbLo = 36, kbNum = 0;
        processorRef.getKeyboardKeyRange (kbLo, kbNum);
        lastKbLo = kbLo; lastKbNum = kbNum;
        keyboard.setRange (kbLo, kbNum);
        keyboard.setBounds (kb);
    }
    helpLabel.setBounds (area.removeFromBottom (16));
    meterLabel.setBounds (area.removeFromBottom (18));

    auto compact = [] (juce::Component& c, juce::Rectangle<int> r)
    { c.setBounds (r.withSizeKeepingCentre (r.getWidth(), juce::jmin (24, r.getHeight()))); };
    auto oneFader = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Slider& s, juce::Label* lbl)
    {
        grp.setBounds (b.reduced (3));
        auto r = b.reduced (3).reduced (5, 4); r.removeFromTop (14);
        if (lbl) lbl->setBounds (r.removeFromTop (13));
        s.setBounds (r);
    };
    auto oneCombo = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Component& c)
    {
        grp.setBounds (b.reduced (3));
        auto r = b.reduced (3).reduced (6, 4); r.removeFromTop (16);
        compact (c, r);
    };

    //== Right panel: Spectrum to Scale (full height) ===========================
    {
        auto sp = area.removeFromRight (224);
        gSpecScale.setBounds (sp.reduced (3));
        auto si = sp.reduced (3).reduced (8, 5); si.removeFromTop (16);
        auto r1 = si.removeFromTop (24);
        dissBtn.setBounds (r1.removeFromLeft (r1.getWidth() - 52).reduced (1, 0));
        r1.removeFromLeft (4);
        compact (dissCountBox, r1);
        si.removeFromTop (4);
        auto r2 = si.removeFromTop (24);
        lblPeriod.setBounds (r2.removeFromLeft (42));
        periodEdit.setBounds (r2.removeFromLeft (58).reduced (0, 1));
        r2.removeFromLeft (4);
        dissMaxToggle.setBounds (r2.removeFromLeft (50));
        dissSaveBtn.setBounds (r2.reduced (1, 0));
        si.removeFromTop (6);
        degreeList.setBounds (si);
    }
    area.removeFromRight (2);

    const int rowH = (area.getHeight() - 4) / 2;

    //== Row 1: Base | (Harmonics over Prime) | Slope | Range | Fractal | (Phase over Gain)
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int W = row.getWidth();

        {
            auto bb = row.removeFromLeft (W * 22 / 100);
            gBase.setBounds (bb.reduced (3));
            auto bi = bb.reduced (3).reduced (6, 4); bi.removeFromTop (16);
            auto left = bi.removeFromLeft (bi.getWidth() * 40 / 100); // N fader column
            lblBaseN.setBounds (left.removeFromTop (13)); baseNFader.setBounds (left);
            bi.removeFromLeft (5);
            // Right column: Pitch, then the Morphing toggle + its Time control.
            lblBasePitch.setBounds (bi.removeFromTop (13));
            compact (basePitchBox, bi.removeFromTop (24));
            bi.removeFromTop (8);
            morphToggle.setBounds (bi.removeFromTop (22));
            bi.removeFromTop (2);
            lblMorphTime.setJustificationType (juce::Justification::centredLeft);
            auto mr = bi.removeFromTop (20);
            lblMorphTime.setBounds (mr.removeFromLeft (36));
            morphTimeFader.setBounds (mr);
        }

        // Harmonics stacked on top of Prime Control.
        {
            auto colBox = row.removeFromLeft (W * 28 / 100);
            auto hb = colBox.removeFromTop (colBox.getHeight() * 2 / 5);
            {
                gWhitelist.setBounds (hb.reduced (3));
                auto r = hb.reduced (3).reduced (6, 4); r.removeFromTop (14);
                compact (whitelistBox, r);
            }

            const bool wl = processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
            primeLimitBox.setVisible (! wl); lblPrimeLimit.setVisible (! wl);
            for (int i = 0; i < 9; ++i) { primeToggle[i].setVisible (wl); primeLbl[i].setVisible (wl); }

            gPrime.setBounds (colBox.reduced (3));
            auto pi = colBox.reduced (3).reduced (6, 4); pi.removeFromTop (16);
            compact (primeModeBox, pi.removeFromTop (22));
            pi.removeFromTop (2);
            if (wl)
            {
                const int tw = pi.getWidth() / 9;
                for (int i = 0; i < 9; ++i)
                {
                    auto cell = pi.removeFromLeft (tw);
                    primeLbl[i].setBounds (cell.removeFromBottom (13));
                    primeToggle[i].setBounds (cell.withSizeKeepingCentre (juce::jmin (22, tw), juce::jmin (22, cell.getHeight())));
                }
            }
            else
            {
                lblPrimeLimit.setBounds (pi.removeFromLeft (42).withSizeKeepingCentre (42, 22));
                compact (primeLimitBox, pi.removeFromLeft (juce::jmin (110, pi.getWidth())));
            }
        }

        // Narrow single-fader boxes.
        oneFader (row.removeFromLeft (W * 12 / 100), gSlope,         slopeFader,         nullptr);
        oneFader (row.removeFromLeft (W * 12 / 100), gSpectrumRange, spectrumRangeFader, &lblRange);
        oneFader (row.removeFromLeft (W * 12 / 100), gFractal,       fractalFader,       nullptr);

        // Phase (combo only, short box) stacked over Gain.
        {
            auto colBox = row;
            auto ph = colBox.removeFromTop (juce::jmin (76, colBox.getHeight() * 2 / 5));
            gPhase.setBounds (ph.reduced (3));
            auto pr = ph.reduced (3).reduced (6, 4); pr.removeFromTop (14);
            compact (phaseBox, pr);
            oneFader (colBox, gGain, gainFader, nullptr);
        }
    }

    //== Row 2: Filter | Volume Env | Tuning(+Recent) | Timbre ==================
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int W = row.getWidth();

        layoutFaders (gFilter, row.removeFromLeft (W * 27 / 100), cFilter,
            { &cutoffFader, &resFader, &fEnvAmtFader, &filtAdsr[0], &filtAdsr[1], &filtAdsr[2], &filtAdsr[3] },
            { &lblCutoff, &lblRes, &lblEnvAmt, &filtAdsrLbl[0], &filtAdsrLbl[1], &filtAdsrLbl[2], &filtAdsrLbl[3] });

        layoutFaders (gAmpEnv, row.removeFromLeft (W * 15 / 100), cEnv,
            { &ampAdsr[0], &ampAdsr[1], &ampAdsr[2], &ampAdsr[3] },
            { &ampAdsrLbl[0], &ampAdsrLbl[1], &ampAdsrLbl[2], &ampAdsrLbl[3] });

        // Pitch envelope: Amount (semitones) + A D S R, like the filter envelope.
        layoutFaders (gPitchEnv, row.removeFromLeft (W * 18 / 100), cPitch,
            { &pEnvAmtFader, &pitchAdsr[0], &pitchAdsr[1], &pitchAdsr[2], &pitchAdsr[3] },
            { &lblPitchAmt, &pitchAdsrLbl[0], &pitchAdsrLbl[1], &pitchAdsrLbl[2], &pitchAdsrLbl[3] });

        {
            auto tb = row.removeFromLeft (W * 24 / 100);
            gTuning.setBounds (tb.reduced (3));
            auto ti = tb.reduced (3).reduced (8, 5); ti.removeFromTop (15);
            compact (tuningBox, ti.removeFromTop (24)); ti.removeFromTop (4);
            auto brow = ti.removeFromTop (23);
            const bool resetVis = processorRef.getTuningMode() != 0 || processorRef.tuningHasScala();
            clearSclButton.setVisible (resetVis);
            if (resetVis)
            {
                loadSclButton.setBounds (brow.removeFromLeft (brow.getWidth() / 2).reduced (2, 0));
                clearSclButton.setBounds (brow.reduced (2, 0));
            }
            else
            {
                loadSclButton.setBounds (brow.reduced (2, 0));
            }
            ti.removeFromTop (4);
            recentLabel.setBounds (ti.removeFromTop (14));
            auto rrow = ti.removeFromTop (26); // taller -> bigger recent-button text
            const int rw = rrow.getWidth() / 3;
            for (int i = 0; i < 3; ++i) recentBtn[i].setBounds (rrow.removeFromLeft (rw).reduced (2, 1));
            ti.removeFromTop (3);
            {
                auto srow = ti.removeFromTop (18);
                tuningStatusLabel.setBounds (srow.removeFromLeft (juce::jmin (62, srow.getWidth())));
                tuningNameLabel.setBounds (srow);
            }
            ti.removeFromTop (2);
            auto bendRow = ti.removeFromTop (22);
            lblBend.setJustificationType (juce::Justification::centredLeft);
            lblBend.setBounds (bendRow.removeFromLeft (74));
            compact (bendBox, bendRow.removeFromLeft (juce::jmin (120, bendRow.getWidth())));
        }

        // Right column: "Spectrum Builder" (load .scl OR type intervals + Reps) over "Reference".
        {
            auto colBox = row;
            auto sb = colBox.removeFromTop (colBox.getHeight() * 64 / 100);
            gTimbre.setBounds (sb.reduced (3));
            auto ti = sb.reduced (3).reduced (8, 4); ti.removeFromTop (15);
            auto brow = ti.removeFromTop (22);
            const bool offVis = processorRef.isTimbreActive();
            clearTimbreButton.setVisible (offVis);
            if (offVis)
            {
                loadTimbreButton.setBounds (brow.removeFromLeft (brow.getWidth() * 6 / 10).reduced (2, 0));
                clearTimbreButton.setBounds (brow.reduced (2, 0));
            }
            else
            {
                loadTimbreButton.setBounds (brow.reduced (2, 0));
            }
            ti.removeFromTop (3);
            intervalEdit.setBounds (ti.removeFromTop (22).reduced (1, 0)); // type intervals, Enter builds
            ti.removeFromTop (3);
            timbreStatusLabel.setBounds (ti.removeFromTop (14));
            ti.removeFromTop (2);
            auto fr = ti.removeFromTop (20);
            lblTimbreRepeats.setJustificationType (juce::Justification::centredLeft);
            lblTimbreRepeats.setBounds (fr.removeFromLeft (38));
            timbreRepeatsFader.setBounds (fr);

            gReference.setBounds (colBox.reduced (3));
            auto ri = colBox.reduced (3).reduced (8, 4); ri.removeFromTop (15);
            auto rrow2 = ri.removeFromTop (24);
            lblRefA.setJustificationType (juce::Justification::centredLeft);
            lblRefA.setBounds (rrow2.removeFromLeft (32));
            refAFader.setBounds (rrow2);
        }
    }

    // Floating "silent settings" explainer, centred over everything.
    warningOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (580, 200));
    warningOverlay.toFront (false);
    noticeOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (560, 220));
}

void FractalAdditiveAudioProcessorEditor::mouseDown (const juce::MouseEvent&) { grabKeyboardFocus(); }

bool FractalAdditiveAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    const int  period = juce::jmax (1, processorRef.getScaleDegrees());
    if (c == 'z' || c == 'Z') { baseKey = juce::jlimit (0, 112, baseKey - period); return true; }
    if (c == 'x' || c == 'X') { baseKey = juce::jlimit (0, 112, baseKey + period); return true; }
    return false;
}

bool FractalAdditiveAudioProcessorEditor::keyStateChanged (bool)
{
    pollComputerKeyboard();
    return false; // never swallow keys - let shortcuts etc. through
}

// Reconcile the sounding computer-keyboard notes with the ACTUAL physical key
// state (isKeyCurrentlyDown -> GetAsyncKeyState on Windows). Called both on key
// events (low latency) and on the 30 Hz timer (safety net): a KEYUP missed while
// the editor was unfocused, or a phantom key from keyboard ghosting, otherwise
// leaves a stuck note - and since the top row is an octave up, that stuck note
// appears "in another octave". If we do not have focus, everything is released
// so global key presses in other apps never play notes.
void FractalAdditiveAudioProcessorEditor::pollComputerKeyboard()
{
    // No note-playing while unfocused OR while typing in a text field (the Period
    // box, a preset name...): letters there must never trigger notes. This is
    // folded PER KEY into `active` rather than a mass clear+return, so heldKeys
    // always mirrors exactly what we triggered - a stale entry from a mass-clear
    // was one way a key could later re-fire and leave a note stuck in memory.
    const bool typing = dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;
    const bool active = hasKeyboardFocus (true) && ! typing;

    for (const auto& m : kKeyMap)
    {
        const bool wantOn = active && juce::KeyPress::isKeyCurrentlyDown (m.ch);
        const auto it = heldKeys.find (m.ch);
        if (wantOn && it == heldKeys.end())
        {
            const int note = juce::jlimit (0, 127, baseKey + m.off);
            heldKeys[m.ch] = note;
            // Guard against a SECOND note-on for a note that is already sounding
            // (from an overlapping key/source or a reconcile race): a duplicate
            // note-on starts a second voice, which is exactly the "press one key
            // and two sound at once" bug. Only ring it if it is not already on.
            if (! processorRef.keyboardState.isNoteOn (1, note))
                processorRef.keyboardState.noteOn (1, note, 0.8f);
        }
        else if (! wantOn && it != heldKeys.end())
        {
            processorRef.keyboardState.noteOff (1, it->second, 0.0f);
            heldKeys.erase (it);
        }
    }
}

void FractalAdditiveAudioProcessorEditor::timerCallback()
{
    pollComputerKeyboard(); // safety net for missed KEYUPs / ghost keys / focus loss

    // Re-adapt the colour keyboard's visible window whenever the active tuning
    // changes (a scale loaded/generated, or the A reference moved), so a wide
    // period never leaves sub-audio/ultrasonic keys on screen.
    {
        int kbLo = 36, kbNum = 0;
        processorRef.getKeyboardKeyRange (kbLo, kbNum);
        if (kbLo != lastKbLo || kbNum != lastKbNum)
        {
            lastKbLo = kbLo; lastKbNum = kbNum;
            keyboard.setRange (kbLo, kbNum);
        }
    }

    const int  rendered  = processorRef.getPartialCount();
    const int  templ     = processorRef.getTemplatePartialCount();
    const bool truncated = processorRef.isSpectrumTruncated();
    juce::String meter;
    if (templ == 0)
    {
        // Empty template = the selection + prime rules (intersected) left nothing.
        meter = tr ("0 partials \xe2\x80\x94 the selection / prime filters leave nothing to sound",
                    "0 parcials \xe2\x80\x94 la selecci\xc3\xb3 / els filtres de primers no deixen sonar res");
        meterLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
    }
    else
    {
        meter = juce::String (rendered) + tr (" partials audible", " parcials audibles")
              + "   \xe2\x80\x94   " + juce::String (templ) + tr (" in the full timbre", " al timbre complet");
        if (truncated) meter += tr ("   (safety cap)", "   (l\xc3\xadmit de seguretat)");
        meterLabel.setColour (juce::Label::textColourId, truncated ? juce::Colours::orange : juce::Colour (0xff7ff59a));
    }
    meterLabel.setText (meter, juce::dontSendNotification);

    updateWarning (templ);

    // Localised tuning status - a dim prefix + a highlighted scale name. The
    // A=440 reference now has its own control, so 12-TET shows just "12-TET".
    juce::String prefix, sname;
    switch (processorRef.getTuningMode())
    {
        case 1:  if (processorRef.tuningHasMaster()) { prefix = "MTS-ESP:"; sname = processorRef.tuningScaleName(); }
                 else prefix = tr ("MTS-ESP (no master)", "MTS-ESP (sense mestre)"); break;
        case 2:  if (processorRef.tuningHasScala())  { prefix = tr ("Loaded:", "Carregat:"); sname = processorRef.tuningScaleName(); }
                 else prefix = tr ("Scala (no file)", "Scala (sense fitxer)"); break;
        default: prefix = "12-TET"; break;
    }
    tuningStatusLabel.setText (prefix, juce::dontSendNotification);
    tuningNameLabel.setText (sname, juce::dontSendNotification);

    juce::String tms = processorRef.isTimbreActive()
        ? "Timbre: " + processorRef.getTimbreName() + "  (" + juce::String (processorRef.getTimbreNoteCount())
          + tr (" notes/period)", " notes/per\xc3\xadode)")
        : tr ("Timbre off (harmonic engine)", "Timbre apagat (motor harm\xc3\xb2nic)");
    timbreStatusLabel.setText (tms, juce::dontSendNotification);

    // Recent .scl buttons.
    const auto recents = processorRef.getRecentSclPaths();
    for (int i = 0; i < 3; ++i)
    {
        const bool has = i < recents.size();
        recentBtn[i].setVisible (has);
        if (has) recentBtn[i].setButtonText (juce::File (recents[i]).getFileNameWithoutExtension());
    }

    // Hide the "undo" buttons when there is nothing to undo: Reset only matters
    // away from plain 12-TET, Off only when Timbre mode is actually on. Relayout
    // so the Load button expands to fill the freed space.
    const bool resetVis = processorRef.getTuningMode() != 0 || processorRef.tuningHasScala();
    const bool offVis   = processorRef.isTimbreActive();
    const int  packed   = (resetVis ? 1 : 0) | (offVis ? 2 : 0);
    if (packed != lastResetOffState) { lastResetOffState = packed; resized(); }

    const int mode = (int) (processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f);
    if (mode != lastPrimeMode) { lastPrimeMode = mode; resized(); }

    if (processorRef.getDissGeneration() != lastDissGeneration)
    {
        lastDissGeneration = processorRef.getDissGeneration();
        refreshDegreeList();
    }

    const bool timbre = processorRef.isTimbreActive();
    if (timbre != lastTimbreActive)
    {
        if (timbre)
        {
            forceDefault ("baseN");     forceDefault ("baseMode"); forceDefault ("selection");
            forceDefault ("phaseMode"); forceDefault ("primeMode"); forceDefault ("primeLimit");
            for (int i = 0; i < 9; ++i) forceDefault ("primeWL" + juce::String (i));
        }
        applyTimbreEnablement (timbre);
        lastTimbreActive = timbre;
    }
}
