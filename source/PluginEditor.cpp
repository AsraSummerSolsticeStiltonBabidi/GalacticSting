#include "PluginEditor.h"

namespace
{
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
    const juce::Colour cTuning    { 0xffb9a4ff };
    const juce::Colour cTimbre    { 0xfff58bff };
    const juce::Colour cGain      { 0xffef7dff };
    const juce::Colour cLoad      { 0xff2fb155 }; // green = Load buttons
    const juce::Colour cReset     { 0xffe0453f }; // red   = Reset / Off buttons

    struct KeyNote { int ch; int off; };
    const KeyNote kKeyMap[] = {
        {'a',0},{'w',1},{'s',2},{'e',3},{'d',4},{'f',5},{'t',6},{'g',7},
        {'y',8},{'h',9},{'u',10},{'j',11},{'k',12},{'o',13},{'l',14},{'p',15}
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
            return juce::Font (juce::FontOptions (l.getFont().getHeight()).withStyle ("Bold"));
        }

        void drawGroupComponentOutline (juce::Graphics& g, int w, int h, const juce::String& text,
                                        const juce::Justification&, juce::GroupComponent& group) override
        {
            const auto col = group.findColour (juce::GroupComponent::outlineColourId);
            constexpr float titleH = 17.0f;
            auto frame = juce::Rectangle<float> (1.0f, titleH * 0.5f,
                                                 (float) w - 2.0f, (float) h - titleH * 0.5f - 1.0f);
            g.setColour (col.withAlpha (0.9f));
            g.drawRoundedRectangle (frame, 6.0f, 1.8f);

            g.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            const int tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 14;
            auto chip = juce::Rectangle<float> (11.0f, 0.0f, (float) tw, titleH);
            g.setColour (juce::Colour (0xff0c0c12));
            g.fillRoundedRectangle (chip, 3.0f);
            g.setColour (col);
            g.drawText (text, chip, juce::Justification::centred, false);
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
    const char* ampIds[]  = { "ampAttack","ampDecay","ampSustain","ampRelease" };
    const char* fltIds[]  = { "filterAttack","filterDecay","filterSustain","filterRelease" };
    const char* adsrTxt[] = { "A","D","S","R" };
    for (int i = 0; i < 4; ++i)
    {
        configFader (ampAdsr[i],  ampIds[i], ampAdsrAtt[i],  cEnv,    "Amplitude envelope");
        configFader (filtAdsr[i], fltIds[i], filtAdsrAtt[i], cFilter, "Filter envelope");
        makeLabel (ampAdsrLbl[i],  cEnv);    ampAdsrLbl[i].setText  (adsrTxt[i], juce::dontSendNotification);
        makeLabel (filtAdsrLbl[i], cFilter); filtAdsrLbl[i].setText (adsrTxt[i], juce::dontSendNotification);
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
    tuningStatusLabel.setColour (juce::Label::textColourId, cTuning);
    tuningStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    addAndMakeVisible (tuningStatusLabel);

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
    makeLabel (lblTimbreRepeats, cTimbre);
    timbreStatusLabel.setJustificationType (juce::Justification::centredLeft);
    timbreStatusLabel.setColour (juce::Label::textColourId, cTimbre);
    timbreStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    addAndMakeVisible (timbreStatusLabel);

    // Language toggle (top-right).
    langButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff33334a));
    langButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    langButton.setMouseClickGrabsKeyboardFocus (false);
    langButton.setTooltip ("Switch language: English / Català");
    langButton.onClick = [this] { catalan = ! catalan; applyLanguage(); refocus(); };
    addAndMakeVisible (langButton);

    meterLabel.setJustificationType (juce::Justification::centred);
    meterLabel.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    addAndMakeVisible (meterLabel);
    helpLabel.setJustificationType (juce::Justification::centred);
    helpLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0b0));
    helpLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (helpLabel);

    addAndMakeVisible (keyboard);

    applyLanguage();
    applyTimbreEnablement (processorRef.isTimbreActive());
    lastTimbreActive = processorRef.isTimbreActive();

    setWantsKeyboardFocus (true);
    setSize (1280, 648);
    startTimerHz (15);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });
}

FractalAdditiveAudioProcessorEditor::~FractalAdditiveAudioProcessorEditor()
{
    stopTimer();
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

    gBase.setText          (tr ("Base", "Base"));
    gWhitelist.setText     (tr ("Harmonics", "Harm\xc3\xb2nics"));
    gSlope.setText         (tr ("Decay Slope", "Pendent"));
    gSpectrumRange.setText (tr ("Spectrum Range", "Rang espectral"));
    gFractal.setText       (tr ("Fractal", "Fractal"));
    gPhase.setText         (tr ("Phase", "Fase"));
    gGain.setText          (tr ("Gain", "Guany"));
    gPrime.setText         (tr ("Prime Control", "Control primers"));
    gFilter.setText        (tr ("Filter", "Filtre"));
    gAmpEnv.setText        (tr ("Volume Envelope", "Envolupant volum"));
    gTuning.setText        (tr ("Tuning (scale)", "Afinaci\xc3\xb3 (escala)"));
    gTimbre.setText        (tr ("Timbre (.scl)", "Timbre (.scl)"));

    lblBaseN.setText     ("N", dontSendNotification);
    lblBasePitch.setText (tr ("Pitch", "To"), dontSendNotification);
    lblRange.setText     (tr ("Oct", "Oct"), dontSendNotification);
    lblPrimeLimit.setText(tr ("Limit", "L\xc3\xadmit"), dontSendNotification);
    lblCutoff.setText    (tr ("Cut", "Tall"), dontSendNotification);
    lblRes.setText       (tr ("Res", "Res"), dontSendNotification);
    lblEnvAmt.setText    (tr ("Env", "Env"), dontSendNotification);
    lblTimbreRepeats.setText (tr ("Reps", "Reps"), dontSendNotification);
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);

    loadSclButton.setButtonText    (tr ("Load .scl", "Obrir .scl"));
    clearSclButton.setButtonText   (tr ("Reset", "Reinicia"));
    loadTimbreButton.setButtonText (tr ("Load .scl", "Obrir .scl"));
    clearTimbreButton.setButtonText(tr ("Off", "Apaga"));

    helpLabel.setText (tr ("Click the colour bar or type A S D F...  W E T Y U...   Z / X = down / up one scale period.   Chords of 3+ PC keys need an n-key-rollover keyboard (or use MIDI).",
                           "Clica la barra de colors o escriu A S D F...  W E T Y U...   Z / X = avall / amunt un per\xc3\xadode.   Els acords de 3+ tecles al PC depenen del teclat (o fes servir MIDI)."),
                       dontSendNotification);

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

    repaint(); // title uses the language
}

void FractalAdditiveAudioProcessorEditor::openScalaChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a Scala scale (.scl)", juce::File(), "*.scl");
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
    fileChooser = std::make_unique<juce::FileChooser> ("Load a Scala file as timbre (.scl)", juce::File(), "*.scl");
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

    auto titleArea = getLocalBounds().removeFromTop (52);
    g.setColour (juce::Colour (0xff2a2a3c));
    g.fillRect (0, titleArea.getBottom() - 1, getWidth(), 1);

    auto t = titleArea.reduced (16, 8);
    juce::ColourGradient grad (juce::Colour (0xff7dd3fc), (float) t.getX(), 0.0f,
                               juce::Colour (0xfff58bff), (float) juce::jmin (t.getRight(), t.getX() + 430), 0.0f, false);
    g.setGradientFill (grad);
    g.setFont (juce::Font (juce::FontOptions (32.0f).withStyle ("Bold")));
    g.drawText (catalan ? juce::String::fromUTF8 ("FIBL\xc3\x93 GAL\xc3\x80" "CTIC") : juce::String ("GALACTIC STING"),
                t, juce::Justification::centredLeft, false);
}

void FractalAdditiveAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto titleBar = full.removeFromTop (52);
    langButton.setBounds (titleBar.removeFromRight (86).withSizeKeepingCentre (66, 26));

    auto area = full.reduced (6, 4);

    {
        auto kb = area.removeFromBottom (96).reduced (2, 6);
        keyboard.setRange (36);
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

    const int rowH = (area.getHeight() - 4) / 2;

    //== Row 1 ==================================================================
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int u = row.getWidth() / 10;

        {
            auto bb = row.removeFromLeft (u * 2);
            gBase.setBounds (bb.reduced (3));
            auto bi = bb.reduced (3).reduced (6, 4); bi.removeFromTop (16);
            auto left = bi.removeFromLeft (bi.getWidth() / 2);
            lblBaseN.setBounds (left.removeFromTop (13)); baseNFader.setBounds (left);
            lblBasePitch.setBounds (bi.removeFromTop (13)); compact (basePitchBox, bi);
        }
        oneCombo (row.removeFromLeft (u), gWhitelist, whitelistBox);
        oneFader (row.removeFromLeft (u), gSlope,         slopeFader,         nullptr);
        oneFader (row.removeFromLeft (u), gSpectrumRange, spectrumRangeFader, &lblRange);
        oneFader (row.removeFromLeft (u), gFractal,       fractalFader,       nullptr);
        oneCombo (row.removeFromLeft (u), gPhase,         phaseBox);
        oneFader (row.removeFromLeft (u), gGain,          gainFader,          nullptr);

        {
            auto pbox = row;
            const bool wl = processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
            primeLimitBox.setVisible (! wl); lblPrimeLimit.setVisible (! wl);
            for (int i = 0; i < 9; ++i) { primeToggle[i].setVisible (wl); primeLbl[i].setVisible (wl); }

            gPrime.setBounds (pbox.reduced (3));
            auto pi = pbox.reduced (3).reduced (6, 4); pi.removeFromTop (16);
            compact (primeModeBox, pi.removeFromTop (24));
            pi.removeFromTop (4);
            if (wl)
            {
                const int tw = pi.getWidth() / 9;
                for (int i = 0; i < 9; ++i)
                {
                    auto cell = pi.removeFromLeft (tw);
                    primeLbl[i].setBounds (cell.removeFromBottom (14));
                    primeToggle[i].setBounds (cell.withSizeKeepingCentre (juce::jmin (24, tw), juce::jmin (24, cell.getHeight())));
                }
            }
            else
            {
                lblPrimeLimit.setBounds (pi.removeFromLeft (42).withSizeKeepingCentre (42, 22));
                compact (primeLimitBox, pi.removeFromLeft (110));
            }
        }
    }

    //== Row 2: Filter | Volume Env | Tuning(+Recent) | Timbre ==================
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int W = row.getWidth();

        layoutFaders (gFilter, row.removeFromLeft (W * 4 / 10), cFilter,
            { &cutoffFader, &resFader, &fEnvAmtFader, &filtAdsr[0], &filtAdsr[1], &filtAdsr[2], &filtAdsr[3] },
            { &lblCutoff, &lblRes, &lblEnvAmt, &filtAdsrLbl[0], &filtAdsrLbl[1], &filtAdsrLbl[2], &filtAdsrLbl[3] });

        layoutFaders (gAmpEnv, row.removeFromLeft (W * 2 / 10), cEnv,
            { &ampAdsr[0], &ampAdsr[1], &ampAdsr[2], &ampAdsr[3] },
            { &ampAdsrLbl[0], &ampAdsrLbl[1], &ampAdsrLbl[2], &ampAdsrLbl[3] });

        {
            auto tb = row.removeFromLeft (W * 25 / 100);
            gTuning.setBounds (tb.reduced (3));
            auto ti = tb.reduced (3).reduced (8, 5); ti.removeFromTop (15);
            compact (tuningBox, ti.removeFromTop (24)); ti.removeFromTop (4);
            auto brow = ti.removeFromTop (23);
            loadSclButton.setBounds (brow.removeFromLeft (brow.getWidth() / 2).reduced (2, 0));
            clearSclButton.setBounds (brow.reduced (2, 0));
            ti.removeFromTop (4);
            recentLabel.setBounds (ti.removeFromTop (14));
            auto rrow = ti.removeFromTop (22);
            const int rw = rrow.getWidth() / 3;
            for (int i = 0; i < 3; ++i) recentBtn[i].setBounds (rrow.removeFromLeft (rw).reduced (2, 0));
            ti.removeFromTop (3);
            tuningStatusLabel.setBounds (ti.removeFromTop (18));
        }

        {
            auto tb = row;
            gTimbre.setBounds (tb.reduced (3));
            auto ti = tb.reduced (3).reduced (8, 5); ti.removeFromTop (15);
            auto brow = ti.removeFromTop (23);
            loadTimbreButton.setBounds (brow.removeFromLeft (brow.getWidth() * 6 / 10).reduced (2, 0));
            clearTimbreButton.setBounds (brow.reduced (2, 0));
            ti.removeFromTop (4);
            auto fr = ti.removeFromRight (54);
            lblTimbreRepeats.setBounds (fr.removeFromTop (13));
            timbreRepeatsFader.setBounds (fr);
            ti.removeFromRight (6);
            timbreStatusLabel.setBounds (ti.removeFromTop (40));
        }
    }
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
    bool consumed = false;
    for (const auto& m : kKeyMap)
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown (m.ch);
        const auto it = heldKeys.find (m.ch);
        if (down && it == heldKeys.end())
        {
            const int note = juce::jlimit (0, 127, baseKey + m.off);
            processorRef.keyboardState.noteOn (1, note, 0.8f);
            heldKeys[m.ch] = note; consumed = true;
        }
        else if (! down && it != heldKeys.end())
        {
            processorRef.keyboardState.noteOff (1, it->second, 0.0f);
            heldKeys.erase (it); consumed = true;
        }
    }
    return consumed;
}

void FractalAdditiveAudioProcessorEditor::timerCallback()
{
    const int  rendered  = processorRef.getPartialCount();
    const int  templ     = processorRef.getTemplatePartialCount();
    const bool truncated = processorRef.isSpectrumTruncated();
    juce::String meter = juce::String (rendered) + tr (" partials audible", " parcials audibles")
                       + "   \xe2\x80\x94   " + juce::String (templ) + tr (" in the full timbre", " al timbre complet");
    if (truncated) meter += tr ("   (safety cap)", "   (l\xc3\xadmit de seguretat)");
    meterLabel.setColour (juce::Label::textColourId, truncated ? juce::Colours::orange : juce::Colour (0xff7ff59a));
    meterLabel.setText (meter, juce::dontSendNotification);

    // Localised tuning status.
    juce::String ts;
    switch (processorRef.getTuningMode())
    {
        case 1:  ts = processorRef.tuningHasMaster() ? "MTS-ESP: " + processorRef.tuningScaleName()
                                                     : tr ("MTS-ESP (no master)", "MTS-ESP (sense mestre)"); break;
        case 2:  ts = processorRef.tuningHasScala()  ? "Scala: " + processorRef.tuningScaleName()
                                                     : tr ("Scala (no file)", "Scala (sense fitxer)"); break;
        default: ts = tr ("12-TET (A=440)", "12-TET (La=440)"); break;
    }
    tuningStatusLabel.setText (ts, juce::dontSendNotification);

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

    const int mode = (int) (processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f);
    if (mode != lastPrimeMode) { lastPrimeMode = mode; resized(); }

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
