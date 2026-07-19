#include "PluginEditor.h"

namespace
{
    const juce::Colour cBase      { 0xff5aa9e6 };
    const juce::Colour cWhitelist { 0xff6ee7b7 };
    const juce::Colour cSlope     { 0xffa7f3d0 };
    const juce::Colour cFractal   { 0xfffb923c };
    const juce::Colour cPhase     { 0xff93c5fd };
    const juce::Colour cPrime     { 0xfffcd34d };
    const juce::Colour cFilter    { 0xff67e8f9 };
    const juce::Colour cEnv       { 0xfff9a8a8 };
    const juce::Colour cTuning    { 0xffc4b5fd };
    const juce::Colour cTimbre    { 0xfff0abfc };
    const juce::Colour cGain      { 0xffe879f9 };

    struct KeyNote { int ch; int off; };
    const KeyNote kKeyMap[] = {
        {'a',0},{'w',1},{'s',2},{'e',3},{'d',4},{'f',5},{'t',6},{'g',7},
        {'y',8},{'h',9},{'u',10},{'j',11},{'k',12},{'o',13},{'l',14},{'p',15}
    };
}

//==============================================================================
FractalAdditiveAudioProcessorEditor::FractalAdditiveAudioProcessorEditor (
        FractalAdditiveAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      keyboard (p.keyboardState, [this] { return processorRef.getScaleDegrees(); })
{
    configGroup (gBase, "Base", cBase);
    configFader (baseNFader,   "baseN",    baseNAtt,     cBase, "Base harmonic N");
    configCombo (basePitchBox, "baseMode", basePitchAtt, cBase, "Original = harmonics at natural positions (raise N -> brighter).  Transposed down = lowest partial at the played pitch (raise N -> denser).");
    makeLabel (lblBaseN, "N", cBase);
    makeLabel (lblBasePitch, "Pitch", cBase);

    configGroup (gWhitelist, "Harmonics", cWhitelist);
    configCombo (whitelistBox, "selection", whitelistAtt, cWhitelist, "Which harmonics may sound: All / Even / Odd / Prime");
    configGroup (gSlope, "Decay Slope", cSlope);
    configFader (slopeFader, "curveSlope", slopeAtt, cSlope, "Amplitude roll-off. 0 = flat/bright, 1 = saw, 2 = triangle.  (Also thins the highs in Timbre mode.)");
    configGroup (gFractal, "Fractal", cFractal);
    configFader (fractalFader, "fractalLevel", fractalAtt, cFractal, "Fractal self-similarity level (1 = off)");
    configGroup (gPhase, "Phase", cPhase);
    configCombo (phaseBox, "phaseMode", phaseAtt, cPhase, "Aligned: in phase -> tonal pulse, never clips (dense = quiet).  Scattered: random phases -> louder, noisier.");
    configGroup (gGain, "Gain", cGain);
    configFader (gainFader, "masterGain", gainAtt, cGain, "Master output gain");

    configGroup (gPrime, "Prime Control", cPrime);
    configCombo (primeModeBox,  "primeMode",  primeModeAtt,  cPrime, "Limit = drop harmonics with a prime factor above the value.  Whitelist = only allow the ticked primes.");
    configCombo (primeLimitBox, "primeLimit", primeLimitAtt, cPrime, "Largest prime factor allowed (Limit mode).");
    makeLabel (lblPrimeLimit, "Limit", cPrime);
    const char* pn[] = { "2","3","5","7","11","13","17","19","23" };
    for (int i = 0; i < 9; ++i)
    {
        primeToggle[i].setButtonText ({}); // number is drawn in a label below, so 2-digit primes are not clipped
        primeToggle[i].setColour (juce::ToggleButton::tickColourId, cPrime);
        primeToggle[i].setMouseClickGrabsKeyboardFocus (false);
        primeToggle[i].setTooltip ("Allow prime " + juce::String (pn[i]));
        primeToggle[i].onClick = [this] { refocus(); };
        addAndMakeVisible (primeToggle[i]);
        primeToggleAtt[i] = std::make_unique<ButtonAttachment> (processorRef.apvts, "primeWL" + juce::String (i), primeToggle[i]);
        makeLabel (primeLbl[i], pn[i], cPrime);
    }

    // Filter = cutoff / resonance / env-amount + its A D S R.
    configGroup (gFilter, "Filter", cFilter);
    configFader (cutoffFader,  "filterCutoff",    cutoffAtt,  cFilter, "Low-pass cutoff");
    configFader (resFader,     "filterResonance", resAtt,     cFilter, "Resonance");
    configFader (fEnvAmtFader, "filterEnvAmount", fEnvAmtAtt, cFilter, "How far the filter envelope moves the cutoff (octaves, can be negative)");
    makeLabel (lblCutoff, "Cut", cFilter); makeLabel (lblRes, "Res", cFilter); makeLabel (lblEnvAmt, "Env", cFilter);

    configGroup (gAmpEnv, "Volume Envelope", cEnv);
    const char* adsr[]   = { "A","D","S","R" };
    const char* ampIds[] = { "ampAttack","ampDecay","ampSustain","ampRelease" };
    const char* fltIds[] = { "filterAttack","filterDecay","filterSustain","filterRelease" };
    for (int i = 0; i < 4; ++i)
    {
        configFader (ampAdsr[i],  ampIds[i], ampAdsrAtt[i],  cEnv,    "Amplitude envelope");
        configFader (filtAdsr[i], fltIds[i], filtAdsrAtt[i], cFilter, "Filter envelope");
        makeLabel (ampAdsrLbl[i],  adsr[i], cEnv);
        makeLabel (filtAdsrLbl[i], adsr[i], cFilter);
    }

    // Tuning (scale): choose the source + load a .scl scale.
    configGroup (gTuning, "Tuning (scale)", cTuning);
    configCombo (tuningBox, "tuningMode", tuningAtt, cTuning,
                 "12-TET = standard.  MTS-ESP = follow a connected ODDSound master.  Scala = the loaded .scl scale.");
    loadSclButton.setButtonText ("Load .scl");
    loadSclButton.setMouseClickGrabsKeyboardFocus (false);
    loadSclButton.setTooltip ("Load a Scala scale file (retunes the notes). 1/1 anchored to middle C.");
    loadSclButton.onClick = [this] { openScalaChooser(); };
    addAndMakeVisible (loadSclButton);
    clearSclButton.setButtonText ("Reset");
    clearSclButton.setMouseClickGrabsKeyboardFocus (false);
    clearSclButton.setTooltip ("Back to 12-TET.");
    clearSclButton.onClick = [this] { processorRef.clearScala(); refocus(); };
    addAndMakeVisible (clearSclButton);
    tuningStatusLabel.setJustificationType (juce::Justification::centredLeft);
    tuningStatusLabel.setColour (juce::Label::textColourId, cTuning);
    tuningStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (tuningStatusLabel);

    // Timbre (.scl as spectrum): the scale's intervals become the partials.
    configGroup (gTimbre, "Timbre (.scl)", cTimbre);
    loadTimbreButton.setButtonText ("Load .scl");
    loadTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    loadTimbreButton.setTooltip ("Use a Scala file's intervals AS the timbre (partials). Slope + fractal + repeats shape it; the harmonic controls turn off.");
    loadTimbreButton.onClick = [this] { openTimbreChooser(); };
    addAndMakeVisible (loadTimbreButton);
    clearTimbreButton.setButtonText ("Off");
    clearTimbreButton.setMouseClickGrabsKeyboardFocus (false);
    clearTimbreButton.setTooltip ("Back to the harmonic engine.");
    clearTimbreButton.onClick = [this] { processorRef.clearTimbre(); refocus(); };
    addAndMakeVisible (clearTimbreButton);
    configFader (timbreRepeatsFader, "timbreRepeats", timbreRepeatsAtt, cTimbre,
                 "How many times the scale's period stacks into the timbre. More = reaches higher in the spectrum.");
    makeLabel (lblTimbreRepeats, "Reps", cTimbre);
    timbreStatusLabel.setJustificationType (juce::Justification::centredLeft);
    timbreStatusLabel.setColour (juce::Label::textColourId, cTimbre);
    timbreStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (timbreStatusLabel);

    meterLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (meterLabel);
    helpLabel.setJustificationType (juce::Justification::centred);
    helpLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    helpLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    helpLabel.setText ("Click the colour bar or type A S D F...  W E T Y U...   Z / X = down / up one scale period.  "
                       "3-note PC-keyboard chords need an n-key-rollover keyboard (or use MIDI).",
                       juce::dontSendNotification);
    addAndMakeVisible (helpLabel);

    addAndMakeVisible (keyboard);

    applyTimbreEnablement (processorRef.isTimbreActive());
    lastTimbreActive = processorRef.isTimbreActive();

    setWantsKeyboardFocus (true);
    setSize (1180, 620);
    startTimerHz (15);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });
}

FractalAdditiveAudioProcessorEditor::~FractalAdditiveAudioProcessorEditor() { stopTimer(); }

//==============================================================================
void FractalAdditiveAudioProcessorEditor::refocus() { grabKeyboardFocus(); }

void FractalAdditiveAudioProcessorEditor::forceDefault (const juce::String& paramID)
{
    if (auto* prm = processorRef.apvts.getParameter (paramID))
        prm->setValueNotifyingHost (prm->getDefaultValue());
}

void FractalAdditiveAudioProcessorEditor::applyTimbreEnablement (bool active)
{
    // In Timbre mode the harmonic engine is bypassed, so its controls are disabled.
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

void FractalAdditiveAudioProcessorEditor::makeLabel (juce::Label& l, const juce::String& t, juce::Colour c)
{
    l.setText (t, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, c.withAlpha (0.9f));
    l.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (l);
}

void FractalAdditiveAudioProcessorEditor::configGroup (juce::GroupComponent& g, const juce::String& title, juce::Colour col)
{
    g.setText (title);
    g.setColour (juce::GroupComponent::outlineColourId, col);
    g.setColour (juce::GroupComponent::textColourId, col);
    addAndMakeVisible (g);
}

void FractalAdditiveAudioProcessorEditor::configFader (juce::Slider& s, const juce::String& paramID,
                                                       std::unique_ptr<SliderAttachment>& att, juce::Colour col,
                                                       const juce::String& tooltip)
{
    s.setSliderStyle (juce::Slider::LinearVertical);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 14);
    s.setColour (juce::Slider::thumbColourId, col);
    s.setColour (juce::Slider::trackColourId, col.withAlpha (0.5f));
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
    b.setColour (juce::ComboBox::outlineColourId, col.withAlpha (0.6f));
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
    auto in = area.reduced (3).reduced (5, 4); in.removeFromTop (8);
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
    g.fillAll (juce::Colour (0xff16161c));
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (17.0f)));
    g.drawText ("Fractal Additive", getLocalBounds().removeFromTop (28), juce::Justification::centred);
}

void FractalAdditiveAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (6, 0);
    area.removeFromTop (28);

    {
        auto kb = area.removeFromBottom (98).reduced (4, 6);
        keyboard.setRange (36);
        keyboard.setBounds (kb);
    }
    helpLabel.setBounds (area.removeFromBottom (16));
    meterLabel.setBounds (area.removeFromBottom (18));

    auto compact = [] (juce::Component& c, juce::Rectangle<int> r)
    { c.setBounds (r.withSizeKeepingCentre (r.getWidth(), juce::jmin (24, r.getHeight()))); };

    const int rowH = (area.getHeight() - 4) / 2;

    // A labelled section holding either a compact combo or a single fader.
    auto oneFader = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Slider& s, juce::Label* lbl)
    {
        grp.setBounds (b.reduced (3));
        auto r = b.reduced (3).reduced (5, 4); r.removeFromTop (8);
        if (lbl) { lbl->setBounds (r.removeFromTop (13)); }
        s.setBounds (r);
    };
    auto oneCombo = [&] (juce::Rectangle<int> b, juce::GroupComponent& grp, juce::Component& c)
    {
        grp.setBounds (b.reduced (3));
        auto r = b.reduced (3).reduced (6, 4); r.removeFromTop (8);
        compact (c, r);
    };

    //== Row 1: Base | Harmonics | Slope | Fractal | Phase | Gain | Prime =======
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int u = row.getWidth() / 10;

        // Base (2u): N fader + pitch combo.
        {
            auto bb = row.removeFromLeft (u * 2);
            gBase.setBounds (bb.reduced (3));
            auto bi = bb.reduced (3).reduced (6, 4); bi.removeFromTop (8);
            auto left = bi.removeFromLeft (bi.getWidth() / 2);
            lblBaseN.setBounds (left.removeFromTop (13)); baseNFader.setBounds (left);
            lblBasePitch.setBounds (bi.removeFromTop (13)); compact (basePitchBox, bi);
        }
        oneCombo (row.removeFromLeft (u), gWhitelist, whitelistBox);
        oneFader (row.removeFromLeft (u), gSlope,   slopeFader,   nullptr);
        oneFader (row.removeFromLeft (u), gFractal, fractalFader, nullptr);
        oneCombo (row.removeFromLeft (u), gPhase,   phaseBox);
        oneFader (row.removeFromLeft (u), gGain,    gainFader,    nullptr);

        // Prime (rest, ~3u): mode combo + limit combo OR 9 tick boxes with numbers below.
        {
            auto pbox = row;
            const bool wl = processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f;
            primeLimitBox.setVisible (! wl); lblPrimeLimit.setVisible (! wl);
            for (int i = 0; i < 9; ++i) { primeToggle[i].setVisible (wl); primeLbl[i].setVisible (wl); }

            gPrime.setBounds (pbox.reduced (3));
            auto pi = pbox.reduced (3).reduced (6, 4); pi.removeFromTop (8);
            compact (primeModeBox, pi.removeFromTop (24));
            pi.removeFromTop (4);
            if (wl)
            {
                const int tw = pi.getWidth() / 9;
                for (int i = 0; i < 9; ++i)
                {
                    auto cell = pi.removeFromLeft (tw);
                    primeLbl[i].setBounds (cell.removeFromBottom (14));
                    primeToggle[i].setBounds (cell.withSizeKeepingCentre (26, juce::jmin (26, cell.getHeight())));
                }
            }
            else
            {
                lblPrimeLimit.setBounds (pi.removeFromLeft (44).withSizeKeepingCentre (44, 22));
                compact (primeLimitBox, pi.removeFromLeft (120));
            }
        }
    }

    //== Row 2: Filter | Volume Env | Tuning | Timbre ===========================
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int u = row.getWidth() / 10;

        layoutFaders (gFilter, row.removeFromLeft (u * 4), cFilter,
            { &cutoffFader, &resFader, &fEnvAmtFader, &filtAdsr[0], &filtAdsr[1], &filtAdsr[2], &filtAdsr[3] },
            { &lblCutoff, &lblRes, &lblEnvAmt, &filtAdsrLbl[0], &filtAdsrLbl[1], &filtAdsrLbl[2], &filtAdsrLbl[3] });

        layoutFaders (gAmpEnv, row.removeFromLeft (u * 2), cEnv,
            { &ampAdsr[0], &ampAdsr[1], &ampAdsr[2], &ampAdsr[3] },
            { &ampAdsrLbl[0], &ampAdsrLbl[1], &ampAdsrLbl[2], &ampAdsrLbl[3] });

        // Tuning (2u): mode combo + Load/Reset + status.
        {
            auto tb = row.removeFromLeft (u * 2);
            gTuning.setBounds (tb.reduced (3));
            auto ti = tb.reduced (3).reduced (8, 6); ti.removeFromTop (8);
            compact (tuningBox, ti.removeFromTop (24));
            ti.removeFromTop (6);
            auto brow = ti.removeFromTop (24);
            loadSclButton.setBounds (brow.removeFromLeft (brow.getWidth() / 2).reduced (2, 0));
            clearSclButton.setBounds (brow.reduced (2, 0));
            ti.removeFromTop (4);
            tuningStatusLabel.setBounds (ti.removeFromTop (20));
        }

        // Timbre (2u): Load/Off + Reps fader + status.
        {
            auto tb = row;
            gTimbre.setBounds (tb.reduced (3));
            auto ti = tb.reduced (3).reduced (8, 6); ti.removeFromTop (8);
            auto brow = ti.removeFromTop (24);
            loadTimbreButton.setBounds (brow.removeFromLeft (brow.getWidth() / 2).reduced (2, 0));
            clearTimbreButton.setBounds (brow.reduced (2, 0));
            ti.removeFromTop (4);
            auto fr = ti.removeFromRight (56);
            lblTimbreRepeats.setBounds (fr.removeFromTop (13));
            timbreRepeatsFader.setBounds (fr);
            timbreStatusLabel.setBounds (ti.removeFromTop (20));
        }
    }
}

void FractalAdditiveAudioProcessorEditor::mouseDown (const juce::MouseEvent&) { grabKeyboardFocus(); }

bool FractalAdditiveAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    // Z / X shift the playable range by one PERIOD of the active scale (its number
    // of degrees per period), not a fixed octave -> in 19-EDO one step = 19 keys.
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
    juce::String text = juce::String (rendered) + " partials audible   -   " + juce::String (templ) + " in the full timbre";
    if (truncated) text += "   (safety cap reached)";
    meterLabel.setColour (juce::Label::textColourId, truncated ? juce::Colours::orange : juce::Colours::lightgreen);
    meterLabel.setText (text, juce::dontSendNotification);

    tuningStatusLabel.setText (processorRef.getTuningStatus(), juce::dontSendNotification);
    timbreStatusLabel.setText (processorRef.getTimbreStatus(), juce::dontSendNotification);

    const int mode = (int) (processorRef.apvts.getRawParameterValue ("primeMode")->load() > 0.5f);
    if (mode != lastPrimeMode) { lastPrimeMode = mode; resized(); }

    const bool timbre = processorRef.isTimbreActive();
    if (timbre != lastTimbreActive)
    {
        if (timbre) // entering timbre mode: reset the bypassed harmonic controls to default
        {
            forceDefault ("baseN");     forceDefault ("baseMode"); forceDefault ("selection");
            forceDefault ("phaseMode"); forceDefault ("primeMode"); forceDefault ("primeLimit");
            for (int i = 0; i < 9; ++i) forceDefault ("primeWL" + juce::String (i));
        }
        applyTimbreEnablement (timbre);
        lastTimbreActive = timbre;
    }
}
