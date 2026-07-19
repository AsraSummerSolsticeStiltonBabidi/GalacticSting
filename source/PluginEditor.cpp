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
            return juce::Font (juce::FontOptions (juce::jmax (12.5f, l.getFont().getHeight())).withStyle ("Bold"));
        }

        void drawGroupComponentOutline (juce::Graphics& g, int w, int h, const juce::String& text,
                                        const juce::Justification&, juce::GroupComponent& group) override
        {
            const auto col = group.findColour (juce::GroupComponent::outlineColourId);
            constexpr float titleH = 18.0f;
            auto frame = juce::Rectangle<float> (1.3f, titleH * 0.5f,
                                                 (float) w - 2.6f, (float) h - titleH * 0.5f - 1.3f);

            // Interior panel: lighter than the page (0xff0c0c12) and subtly tinted by the
            // section colour, so each box reads as a raised, distinct panel.
            g.setColour (juce::Colour (0xff17171f).interpolatedWith (col, 0.05f));
            g.fillRoundedRectangle (frame, 7.0f);
            g.setColour (col.withAlpha (0.95f));
            g.drawRoundedRectangle (frame, 7.0f, 2.6f); // thicker outline

            g.setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));
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
    langButton.onClick = [this] { catalan = ! catalan; applyLanguage(); refocus(); };
    addAndMakeVisible (langButton);

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

    addAndMakeVisible (keyboard);

    applyLanguage();
    applyTimbreEnablement (processorRef.isTimbreActive());
    lastTimbreActive = processorRef.isTimbreActive();

    setWantsKeyboardFocus (true);
    setSize (1280, 566); // stay well within a 720p screen so the standalone's Options/preset bar stays visible
    startTimerHz (15);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
    { if (safe != nullptr) safe->grabKeyboardFocus(); });
}

FractalAdditiveAudioProcessorEditor::~FractalAdditiveAudioProcessorEditor()
{
    stopTimer();
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
    lblBend.setText      (tr ("Bend", "Bend"), dontSendNotification);
    recentLabel.setText  (tr ("Recent", "Recents"), dontSendNotification);

    loadSclButton.setButtonText    (tr ("Load .scl", "Obrir .scl"));
    clearSclButton.setButtonText   (tr ("Reset", "Reinicia"));
    loadTimbreButton.setButtonText (tr ("Load .scl", "Obrir .scl"));
    clearTimbreButton.setButtonText(tr ("Off", "Apaga"));
    savePresetButton.setButtonText (tr ("Save", "Desa"));
    presetBox.setTextWhenNothingSelected (tr ("Presets...", "Presets..."));

    helpLabel.setText (tr ("Click the colour bar or type A S D F...  W E T Y U...   Z / X = down / up one scale period.   Chords of 3+ PC keys need an n-key-rollover keyboard (or use MIDI).",
                           "Clica la barra de colors o escriu A S D F...  W E T Y U...   Z / X = avall / amunt un per\xc3\xadode.   Els acords de 3+ tecles al PC depenen del teclat (o fes servir MIDI)."),
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

    const auto nStr = juce::String (baseN);
    juce::String why;
    if (sel == 3 && wl)
        why = tr ("'Harmonics = Prime' combined with the prime Whitelist: a prime harmonic can only pass if it IS one of the ticked primes (max 23), and the series starts at N = ",
                  "'Harm\xc3\xb2nics = Primers' amb la Llista de primers: un harm\xc3\xb2nic primer nom\xc3\xa9s passa si \xc3\x89S un dels primers marcats (m\xc3\xa0xim 23), i la s\xc3\xa8rie comen\xc3\xa7""a a N = ")
            + nStr
            + tr (". Set Prime Control to Limit/Off, or Harmonics to All.",
                  ". Posa el Control de primers en L\xc3\xadmit/No, o Harm\xc3\xb2nics en Tots.");
    else if (wl && mask == 0)
        why = tr ("The prime Whitelist has no primes ticked, so no harmonic can pass. Tick some primes or switch to Limit mode.",
                  "La Llista de primers no t\xc3\xa9 cap primer marcat, aix\xc3\xad que cap harm\xc3\xb2nic pot passar. Marca'n alguns o canvia al mode L\xc3\xadmit.");
    else if (sel == 3 && ! wl && limitIdx > 0)
        why = tr ("'Harmonics = Prime' with a low Prime Limit: every prime harmonic above the limit is removed, and the series starts at N = ",
                  "'Harm\xc3\xb2nics = Primers' amb un L\xc3\xadmit de primers baix: tot harm\xc3\xb2nic primer per sobre del l\xc3\xadmit s'elimina, i la s\xc3\xa8rie comen\xc3\xa7""a a N = ")
            + nStr
            + tr (". Raise the limit, set it to Off, or lower N.",
                  ". Puja el l\xc3\xadmit, posa'l en No, o abaixa N.");
    else
        why = tr ("The Harmonics selection and the Prime filter together exclude every harmonic of the current Base N. Relax one of the two filters or change N.",
                  "La selecci\xc3\xb3 d'Harm\xc3\xb2nics i el filtre de Primers junts exclouen tots els harm\xc3\xb2nics de la Base N actual. Relaxa un dels dos filtres o canvia N.");

    warningOverlay.title = tr ("Why is there no sound?", "Per qu\xc3\xa8 no sona res?");
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

    auto titleArea = getLocalBounds().removeFromTop (42);
    g.setColour (juce::Colour (0xff2a2a3c));
    g.fillRect (0, titleArea.getBottom() - 1, getWidth(), 1);

    // Title centred in the window; the gradient spans the text width around the centre.
    auto t = titleArea.reduced (16, 6);
    const float cx = (float) getWidth() * 0.5f;
    juce::ColourGradient grad (juce::Colour (0xff7dd3fc), cx - 215.0f, 0.0f,
                               juce::Colour (0xfff58bff), cx + 215.0f, 0.0f, false);
    g.setGradientFill (grad);
    g.setFont (juce::Font (juce::FontOptions (28.0f).withStyle ("Bold")));
    g.drawText (catalan ? juce::String::fromUTF8 ("FIBL\xc3\x93 GAL\xc3\x80" "CTIC") : juce::String ("GALACTIC STING"),
                t, juce::Justification::centred, false);
}

void FractalAdditiveAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto titleBar = full.removeFromTop (42);
    langButton.setBounds (titleBar.removeFromRight (86).withSizeKeepingCentre (66, 24));
    auto presetArea = titleBar.removeFromLeft (272).reduced (10, 0);
    presetBox.setBounds (presetArea.removeFromLeft (190).withSizeKeepingCentre (190, 24));
    presetArea.removeFromLeft (6);
    savePresetButton.setBounds (presetArea.withSizeKeepingCentre (juce::jmin (56, presetArea.getWidth()), 24));

    auto area = full.reduced (6, 4);

    {
        auto kb = area.removeFromBottom (78).reduced (2, 5);
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

    //== Row 1: Base | (Harmonics over Prime Control) | Slope | Range | Fractal | Phase | Gain
    {
        auto row = area.removeFromTop (rowH).reduced (0, 2);
        const int u = row.getWidth() / 9;

        {
            auto bb = row.removeFromLeft (u * 2);
            gBase.setBounds (bb.reduced (3));
            auto bi = bb.reduced (3).reduced (6, 4); bi.removeFromTop (16);
            auto left = bi.removeFromLeft (bi.getWidth() / 2);
            lblBaseN.setBounds (left.removeFromTop (13)); baseNFader.setBounds (left);
            lblBasePitch.setBounds (bi.removeFromTop (13)); compact (basePitchBox, bi);
        }

        // Harmonics stacked on top of Prime Control, sharing one 2u-wide column.
        {
            auto colBox = row.removeFromLeft (u * 2);
            auto hb = colBox.removeFromTop (colBox.getHeight() * 2 / 5);
            oneCombo (hb, gWhitelist, whitelistBox);

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

        oneFader (row.removeFromLeft (u), gSlope,         slopeFader,         nullptr);
        oneFader (row.removeFromLeft (u), gSpectrumRange, spectrumRangeFader, &lblRange);
        oneFader (row.removeFromLeft (u), gFractal,       fractalFader,       nullptr);
        oneCombo (row.removeFromLeft (u), gPhase,         phaseBox);
        oneFader (row,                    gGain,          gainFader,          nullptr);
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
            ti.removeFromTop (2);
            auto bendRow = ti.removeFromTop (22);
            lblBend.setBounds (bendRow.removeFromLeft (44));
            compact (bendBox, bendRow.removeFromLeft (juce::jmin (130, bendRow.getWidth())));
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

    // Floating "silent settings" explainer, centred over everything.
    warningOverlay.setBounds (getLocalBounds().withSizeKeepingCentre (540, 160));
    warningOverlay.toFront (false);
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
