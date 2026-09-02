// Offline FFT diagnostics of the rendered synth output.
//   - "sounding": how many partials actually render (matches the on-screen meter)
//   - "lowest":   frequency of the lowest sounding partial (the played pitch, if anchored)
//   - RMS / peak dBFS (level consistency + clipping)

#include "PluginProcessor.h"
#include "dsp/Spectrum.h"

#include <juce_dsp/juce_dsp.h>

#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>

using namespace juce;

static void setP (AudioProcessorValueTreeState& apvts, const char* id, float realValue)
{
    if (auto* p = apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (realValue));
}

static constexpr int    fftOrder = 15;
static constexpr int    fftSize  = 1 << fftOrder;
static constexpr double sampleRate = 48000.0;
static constexpr int    blockSize = 512;

static std::vector<float> renderWindow (FractalAdditiveAudioProcessor& proc, int note)
{
    std::vector<float> mono;
    const int total = fftSize + (int) (sampleRate * 0.5);
    AudioBuffer<float> buf (2, blockSize);
    MidiBuffer noteMidi; noteMidi.addEvent (MidiMessage::noteOn (1, note, (uint8) 110), 0);
    mono.reserve ((size_t) total + blockSize);
    bool first = true;
    while ((int) mono.size() < total)
    {
        buf.clear();
        MidiBuffer mb; if (first) { mb = noteMidi; first = false; }
        proc.processBlock (buf, mb);
        for (int i = 0; i < blockSize; ++i) mono.push_back (buf.getSample (0, i));
    }
    return std::vector<float> (mono.end() - fftSize, mono.end());
}

static std::vector<float> magnitudeOf (std::vector<float> w)
{
    for (int i = 0; i < fftSize; ++i)
        w[(size_t) i] *= 0.5f * (1.0f - std::cos (2.0f * MathConstants<float>::pi * (float) i / (float) (fftSize - 1)));
    std::vector<float> fftData ((size_t) fftSize * 2, 0.0f);
    for (int i = 0; i < fftSize; ++i) fftData[(size_t) i] = w[(size_t) i];
    dsp::FFT fft (fftOrder);
    fft.performRealOnlyForwardTransform (fftData.data());
    std::vector<float> mag ((size_t) (fftSize / 2 + 1));
    for (int b = 0; b <= fftSize / 2; ++b)
    {
        const float re = fftData[(size_t) (2 * b)], im = fftData[(size_t) (2 * b + 1)];
        mag[(size_t) b] = std::sqrt (re * re + im * im);
    }
    return mag;
}

static float peakNear (const std::vector<float>& mag, double freq, int halfWin = 5)
{
    const double binWidth = sampleRate / fftSize;
    const int c = (int) std::lround (freq / binWidth);
    float best = 0.0f;
    for (int b = c - halfWin; b <= c + halfWin; ++b)
        if (b >= 0 && b < (int) mag.size()) best = std::max (best, mag[(size_t) b]);
    return best;
}

static int failures = 0;

static void report (const char* name, std::function<void (AudioProcessorValueTreeState&)> setParams,
                    const spectrum::SpectrumSettings& expected, int note)
{
    FractalAdditiveAudioProcessor proc;
    setP (proc.apvts, "ampAttack", 0.001f);
    setP (proc.apvts, "ampDecay", 0.001f);
    setP (proc.apvts, "ampSustain", 1.0f);
    setP (proc.apvts, "masterGain", 0.0f);
    setParams (proc.apvts);
    proc.prepareToPlay (sampleRate, blockSize);
    // prepareToPlay builds the spectrum from the parameters as they stand, so setting
    // them BEFORE it (as above) is what makes this correct. The explicit poll keeps it
    // correct if that order is ever disturbed: outside a host there is no message loop
    // to run the parameter poll, and a spectrum still on its startup values would make
    // every partial check here meaningless.
    proc.pollParametersForTests();

    auto win = renderWindow (proc, note);
    double sumSq = 0.0; float peak = 0.0f;
    for (float s : win) { sumSq += (double) s * s; peak = std::max (peak, std::abs (s)); }
    const double rmsDb = 20.0 * std::log10 (std::max (1e-9, std::sqrt (sumSq / (double) win.size())));
    const double pkDb  = 20.0 * std::log10 (std::max (1e-9f, peak));

    auto mag = magnitudeOf (win);
    const float maxMag = *std::max_element (mag.begin(), mag.end());
    const double f0  = MidiMessage::getMidiNoteInHertz (note);
    const double nyq = sampleRate * 0.5;
    auto tmpl = spectrum::compileSpectrum (expected);
    const float thresh = maxMag * 3.16e-4f;
    float maxTemplateAmp = 0.0f;
    for (const auto& p : tmpl.partials) maxTemplateAmp = std::max (maxTemplateAmp, p.amp);

    // The processor's default per-voice low-pass sits at 20 kHz, so template
    // partials in its stop-band (and above ~human hearing) are legitimately
    // attenuated below the FFT floor. Only an ABSENT partial that is BOTH audible
    // (>-60 dB in the template) AND below 18 kHz is a genuine dropped partial.
    constexpr double kAudibleCeilingHz = 18000.0;
    int sounding = 0, realMissing = 0, ultrasonicMissing = 0;
    double lowestHz = 0.0, minRealMissHz = 1e18, maxRealMissHz = 0.0;
    for (const auto& p : tmpl.partials)
    {
        const double f = (double) p.ratio * f0;
        if (f >= nyq - 20.0) continue;
        if (peakNear (mag, f) > thresh) { ++sounding; if (lowestHz == 0.0) lowestHz = f; }
        else if (p.amp > maxTemplateAmp * 1.0e-3f)
        {
            if (f < kAudibleCeilingHz)
            {
                ++realMissing;
                minRealMissHz = std::min (minRealMissHz, f);
                maxRealMissHz = std::max (maxRealMissHz, f);
            }
            else ++ultrasonicMissing; // in the filter stop-band -> expected, not a drop
        }
    }

    std::printf ("  %-24s sounding %4d   lowest %7.1f Hz   RMS %6.1f dB   peak %6.1f dBFS%s\n",
                 name, sounding, lowestHz, rmsDb, pkDb, realMissing > 0 ? "  <-- DROPPED!" : "");
    if (realMissing > 0)
    {
        std::printf ("        %d AUDIBLE partial(s) missing in %.0f..%.0f Hz\n",
                     realMissing, minRealMissHz, maxRealMissHz);
        ++failures;
    }
    else if (ultrasonicMissing > 0)
    {
        std::printf ("        (%d ultrasonic >18 kHz partial(s) attenuated by the 20 kHz filter — not a drop)\n",
                     ultrasonicMissing);
    }
}

// Magnitude (dB below the spectrum peak) in a proportional band around freq.
static double envAt (const std::vector<float>& mag, double freq, float maxMag)
{
    const double binWidth = sampleRate / fftSize;
    const int lo = (int) ((freq * 0.94) / binWidth), hi = (int) ((freq * 1.06) / binWidth);
    float best = 0.0f;
    for (int b = lo; b <= hi && b < (int) mag.size(); ++b) if (b >= 0) best = std::max (best, mag[(size_t) b]);
    return 20.0 * std::log10 (std::max (1e-9f, best) / std::max (1e-9f, maxMag));
}

static void reportEnvelope (const char* name, std::function<void (AudioProcessorValueTreeState&)> setParams, int note)
{
    FractalAdditiveAudioProcessor proc;
    setP (proc.apvts, "ampAttack", 0.001f); setP (proc.apvts, "ampDecay", 0.001f);
    setP (proc.apvts, "ampSustain", 1.0f);  setP (proc.apvts, "masterGain", 0.0f);
    setParams (proc.apvts);
    proc.prepareToPlay (sampleRate, blockSize);
    // prepareToPlay builds the spectrum from the parameters as they stand, so setting
    // them BEFORE it (as above) is what makes this correct. The explicit poll keeps it
    // correct if that order is ever disturbed: outside a host there is no message loop
    // to run the parameter poll, and a spectrum still on its startup values would make
    // every partial check here meaningless.
    proc.pollParametersForTests();
    auto mag = magnitudeOf (renderWindow (proc, note));
    const float maxMag = *std::max_element (mag.begin(), mag.end());
    std::printf ("  %-16s spectral tilt:  400Hz %5.1f   1.5k %5.1f   5k %5.1f   15k %5.1f  (dB below peak)\n",
                 name, envAt (mag, 400, maxMag), envAt (mag, 1500, maxMag),
                 envAt (mag, 5000, maxMag), envAt (mag, 15000, maxMag));
}

int main()
{
    ScopedJuceInitialiser_GUI juceInit;
    using B = spectrum::BaseMode; using Se = spectrum::Selection;
    auto mk = [] (int n, B m, Se sel, float slope, int fr, int pl)
    { spectrum::SpectrumSettings s; s.baseN = n; s.baseMode = m; s.selection = sel; s.curveSlope = slope; s.fractalLevel = (float) fr; s.primeLimit = pl; return s; };
    // set(): baseMode idx 0=Transposed 1=Original;  selection idx 0=All..3=Prime (the
    // legacy order, mapped onto the continuous Harmonics axis);  primeLimit idx 0=off
    auto set = [] (int n, int mode, int sel, float slope, int fr, int plIdx, int phase = 0)
    { return [=] (AudioProcessorValueTreeState& a) {
        static const float selAxis[4] = { (float) spectrum::All, (float) spectrum::Even,
                                          (float) spectrum::Odd, (float) spectrum::Prime };
        setP (a, "baseN", (float) n); setP (a, "baseMode", (float) mode);
        setP (a, "harmonics", selAxis[juce::jlimit (0, 3, sel)]);
        setP (a, "curveSlope", slope); setP (a, "fractalLevel", (float) fr); setP (a, "primeLimit", (float) plIdx);
        setP (a, "phaseMode", (float) phase); }; };

    std::printf ("== Fractal at C2 (All, slope1): sounding count should GROW with level ==\n");
    report ("fractal 1", set (1,0,0,1.0f,1,0), mk (1,B::Transposed,Se::All,1.0f,1,0), 36);
    report ("fractal 2", set (1,0,0,1.0f,2,0), mk (1,B::Transposed,Se::All,1.0f,2,0), 36);
    report ("fractal 3", set (1,0,0,1.0f,3,0), mk (1,B::Transposed,Se::All,1.0f,3,0), 36);
    report ("fractal 4", set (1,0,0,1.0f,4,0), mk (1,B::Transposed,Se::All,1.0f,4,0), 36);

    std::printf ("== Slope at C2 (All): RMS should stay constant ==\n");
    report ("slope 0", set (1,0,0,0.0f,1,0), mk (1,B::Transposed,Se::All,0.0f,1,0), 36);
    report ("slope 1", set (1,0,0,1.0f,1,0), mk (1,B::Transposed,Se::All,1.0f,1,0), 36);
    report ("slope 2", set (1,0,0,2.0f,1,0), mk (1,B::Transposed,Se::All,2.0f,1,0), 36);

    std::printf ("== Base + Prime + Transposed at C4 (261.6 Hz): does lowest partial move? ==\n");
    report ("N=1 Prime", set (1,0,3,1.0f,1,0), mk (1,B::Transposed,Se::Prime,1.0f,1,0), 60);
    report ("N=2 Prime", set (2,0,3,1.0f,1,0), mk (2,B::Transposed,Se::Prime,1.0f,1,0), 60);
    report ("N=3 Prime", set (3,0,3,1.0f,1,0), mk (3,B::Transposed,Se::Prime,1.0f,1,0), 60);
    report ("N=5 Prime", set (5,0,3,1.0f,1,0), mk (5,B::Transposed,Se::Prime,1.0f,1,0), 60);

    std::printf ("== Screenshot (Prime, Transposed, slope 0.666, Fractal 4): raising N at C4 ==\n");
    for (int n : { 1, 3, 5, 10, 20, 40, 64 })
    {
        char nm[32]; std::snprintf (nm, sizeof nm, "N=%d", n);
        report (nm, set (n,0,3,0.666f,4,0), mk (n,B::Transposed,Se::Prime,0.666f,4,0), 60);
    }

    std::printf ("== N=8 default (the noise case): Aligned vs Scattered, peak must stay < 0 dBFS ==\n");
    report ("N=8 Aligned",   set (8,0,0,1.0f,1,0,0), mk (8,B::Transposed,Se::All,1.0f,1,0), 60);
    report ("N=8 Scattered", set (8,0,0,1.0f,1,0,1), mk (8,B::Transposed,Se::All,1.0f,1,0), 60);

    std::printf ("== Spectral tilt vs N (All, slope1, Aligned): is the high end really lost? ==\n");
    reportEnvelope ("N=1",  set (1, 0,0,1.0f,1,0,0), 60);
    reportEnvelope ("N=4",  set (4, 0,0,1.0f,1,0,0), 60);
    reportEnvelope ("N=8",  set (8, 0,0,1.0f,1,0,0), 60);
    reportEnvelope ("N=20", set (20,0,0,1.0f,1,0,0), 60);

    std::printf ("== Aligned, high N (no fractal): are partials PRESENT or cancelled? ==\n");
    report ("N=20 aligned",   set (20,0,0,1.0f,1,0,0), mk (20,B::Transposed,Se::All,1.0f,1,0), 60);
    report ("N=40 aligned",   set (40,0,0,1.0f,1,0,0), mk (40,B::Transposed,Se::All,1.0f,1,0), 60);
    report ("N=64 aligned",   set (64,0,0,1.0f,1,0,0), mk (64,B::Transposed,Se::All,1.0f,1,0), 60);
    report ("N=64 scattered", set (64,0,0,1.0f,1,0,1), mk (64,B::Transposed,Se::All,1.0f,1,0), 60);

    std::printf ("== Spectral tilt vs N (ORIGINAL mode): raising N should remove lows -> brighter ==\n");
    reportEnvelope ("N=1 Orig",  set (1, 1,0,1.0f,1,0,0), 60);
    reportEnvelope ("N=4 Orig",  set (4, 1,0,1.0f,1,0,0), 60);
    reportEnvelope ("N=8 Orig",  set (8, 1,0,1.0f,1,0,0), 60);

    // == INDEPENDENT check: the expectation is generated HERE, not by the engine ==
    // Everything above compares the rendered audio against compileSpectrum(expected)
    // - the very function the synth renders from. That catches anything the RENDERER
    // loses (a voice not sounding a partial, the filter, Nyquist, normalisation), but
    // it cannot catch a partial the COMPILER itself drops: the partial would vanish
    // from the measurement and the expectation together and the comparison would
    // still agree. Verified by deliberately deleting a partial inside
    // compileSpectrum: every check above still passed.
    // So here the expected ratios are written out by hand. A plain harmonic spectrum
    // (base 1, All, no fractal, no prime limit) must contain EVERY integer multiple
    // of the played pitch up to the audible ceiling - no exceptions, no thinning.
    {
        std::printf ("== INDEPENDENT: plain harmonic series, expectation not from the engine ==\n");
        FractalAdditiveAudioProcessor proc;
        setP (proc.apvts, "ampAttack", 0.001f);
        setP (proc.apvts, "ampDecay", 0.001f);
        setP (proc.apvts, "ampSustain", 1.0f);
        setP (proc.apvts, "masterGain", 0.0f);
        set (1, 0, 0, 1.0f, 1, 0, 0) (proc.apvts);   // N=1, All, slope 1, no fractal, no limit
        setP (proc.apvts, "spectrumRange", 10.0f);   // no octave cut
        setP (proc.apvts, "filterCutoff", 20000.0f); // keep the filter out of the way
        proc.prepareToPlay (sampleRate, blockSize);
        proc.pollParametersForTests();

        constexpr int note = 36;                     // C2 - room for many harmonics
        const double f0 = MidiMessage::getMidiNoteInHertz (note);
        auto mag = magnitudeOf (renderWindow (proc, note));
        const float maxMag = *std::max_element (mag.begin(), mag.end());
        const float thresh = maxMag * 3.16e-4f;      // -70 dB below the peak

        int missing = 0, firstMissing = 0;
        // Up to 18 kHz, and staying clear of Nyquist.
        for (int h = 1; h * f0 < std::min (18000.0, sampleRate * 0.5 - 20.0); ++h)
            if (peakNear (mag, (double) h * f0) <= thresh)
            { ++missing; if (firstMissing == 0) firstMissing = h; }

        if (missing > 0)
        {
            std::printf ("  %d harmonic(s) of the plain series ABSENT (first: harmonic %d at %.1f Hz)\n",
                         missing, firstMissing, (double) firstMissing * f0);
            ++failures;
        }
        else
            std::printf ("  every harmonic of the plain series is present, 1..%d\n",
                         (int) (std::min (18000.0, sampleRate * 0.5 - 20.0) / f0));
    }

    // == A PENCIL EDIT MUST BE HEARD: armManualPartialAmpsForEdit really arms it ==
    // The defect this guards: "partialManual" ships OFF, and with it off
    // SpectrumEngine::fillGains throws the stored per-partial edits away - so the
    // pencil accepted a +12 dB drag, showed it on the knob and in the undo stack, and
    // the audio moved 0.00 dB (the user's "cambiar el volumen de 2 no produce ningun
    // cambio perceptible"). The fix routes every grid gesture through
    // armManualPartialAmpsForEdit(), and THIS is the only check in the suite that
    // hears the result: the unit tests never build the processor, so without it the
    // arming could silently stop working again and every verifier would stay green.
    // Measured on the 2*f0-vs-f0 dB difference, not on 2*f0 alone, so the engine's
    // level normalisation (which rescales everything when one partial grows) cannot
    // hide or fake the change.
    {
        std::printf ("== Pencil edit: +12 dB at 2*f0 is INAUDIBLE stored-only, AUDIBLE once armed ==\n");
        FractalAdditiveAudioProcessor proc;
        setP (proc.apvts, "ampAttack", 0.001f);
        setP (proc.apvts, "ampDecay", 0.001f);
        setP (proc.apvts, "ampSustain", 1.0f);
        setP (proc.apvts, "masterGain", 0.0f);
        set (1, 0, 0, 1.0f, 1, 0, 0) (proc.apvts);   // plain harmonic series
        setP (proc.apvts, "spectrumRange", 10.0f);
        setP (proc.apvts, "filterCutoff", 20000.0f);
        proc.prepareToPlay (sampleRate, blockSize);
        proc.pollParametersForTests();

        constexpr int note = 48;                     // C3
        const double f0 = MidiMessage::getMidiNoteInHertz (note);
        const auto relDb = [&]                       // 2*f0 relative to f0, in dB
        {
            auto mag = magnitudeOf (renderWindow (proc, note));
            const float a1 = peakNear (mag, f0), a2 = peakNear (mag, 2.0 * f0);
            return 20.0 * std::log10 (std::max (1.0e-12, (double) a2 / std::max (1.0e-12f, a1)));
        };
        const double before = relDb();

        // The stored-but-off state: exactly what the editor produced before the fix.
        proc.setPartialGains ({ { spectrum::gainKey (2.0), 12.0f } }, false);
        proc.pollParametersForTests();
        const double storedOnly = relDb();

        // The fix's path: what PartialAmpPanel's onGestureStart now calls first.
        const bool armed = proc.armManualPartialAmpsForEdit();
        proc.pollParametersForTests();
        const double armedDb = relDb();
        const bool paramOn = proc.apvts.getRawParameterValue ("partialManual")->load() > 0.5f;

        std::printf ("  2*f0 vs f0: baseline %+.2f dB, edit stored (manual off) %+.2f dB, armed %+.2f dB  (param %s)\n",
                     before, storedOnly, armedDb, paramOn ? "ON" : "off");
        // Stored-only must not move (that silence is the DELIBERATE meaning of the
        // pill being off), and arming must make the edit clearly audible. The measured
        // effect is ~+6 dB (the normaliser absorbs the rest of the +12); 3 dB is the
        // line between "audible change" and "the arming quietly stopped working".
        if (std::abs (storedOnly - before) > 0.5) { std::printf ("        stored-only edit CHANGED the audio - the pill no longer gates\n"); ++failures; }
        if (! armed || ! paramOn)                 { std::printf ("        armManualPartialAmpsForEdit did not flip the parameter\n"); ++failures; }
        if (armedDb - before < 3.0)               { std::printf ("        armed edit still inaudible (%.2f dB < 3 dB) - the defect is back\n", armedDb - before); ++failures; }
    }

    // == "Phase" turned from a Choice into a fader: does yesterday's project still ==
    // == sound the same today?  The parameter ID is REUSED, so an old chunk lands in ==
    // == the new parameter with no name to tell them apart.  If the two shapes ever ==
    // == disagree about what 1.0 means, a user reopening a saved project hears their ==
    // == Scattered patch come back Aligned - a 4.5x-32x crest change, i.e. a ==
    // == completely different sound, with nothing on screen to explain it. ==
    //
    // The blob is written by a REAL AudioParameterChoice object, not by our belief
    // about one: APVTS stores the DENORMALISED value (ParameterAdapter::flushToTree
    // writes unnormalisedValue = convertFrom0to1 (parameter.getValue())), so that is
    // exactly the arithmetic reproduced here. Asserting our assumption instead of
    // measuring it is how a migration passes its own test and fails a user's project.
    {
        std::printf ("== Phase: a project saved by the OLD Choice parameter, opened by the fader ==\n");

        AudioParameterChoice oldShape ({ "phaseMode", 1 }, "Phase",
                                       StringArray { "Aligned", "Scattered" }, 0);

        // TWO shapes of old chunk, because the user has BOTH on disk and neither may
        // refuse to open:
        //   stale = 0  a PRE-fader project: no "phaseGlide" child at all.
        //   stale = 1  a project saved by YESTERDAY'S build, which still had the
        //              phaseGlide parameter and therefore wrote a child for it. That
        //              parameter has since been deleted, so this chunk now names one
        //              the plugin does not own. APVTS reads state by asking each
        //              parameter it OWNS for its child, so the extra one should simply
        //              never be looked at - but "should" is the word that ships bugs,
        //              and a user whose set does not load is not consoled by a design
        //              argument. Measured here instead.
        for (int idx = 0; idx <= 1; ++idx)
        for (int stale = 0; stale <= 1; ++stale)
        {
            // What the old build actually wrote into the chunk for this choice.
            const float stored = oldShape.convertFrom0to1 (oldShape.convertTo0to1 ((float) idx));

            FractalAdditiveAudioProcessor proc;
            auto tree = proc.apvts.copyState();

            // Whichever shape we are testing, START from a tree with no phaseGlide child
            // (the parameter is gone, so a fresh state has none) and put one back only
            // for the stale case. Written the way the deleted AudioParameterChoice wrote
            // it - a PARAM child carrying the denormalised index - so this is yesterday's
            // chunk rather than our idea of one.
            tree.removeChild (tree.getChildWithProperty ("id", "phaseGlide"), nullptr);
            if (stale == 1)
            {
                ValueTree glide ("PARAM");
                glide.setProperty ("id", "phaseGlide", nullptr);
                glide.setProperty ("value", 1.0f, nullptr); // "Dry/Wet", the non-default
                tree.appendChild (glide, nullptr);
            }
            auto ph = tree.getChildWithProperty ("id", "phaseMode");
            ph.setProperty ("value", stored, nullptr);

            MemoryBlock blob;
            if (auto xml = tree.createXml())
                FractalAdditiveAudioProcessor::copyXmlToBinary (*xml, blob);
            proc.setStateInformation (blob.getData(), (int) blob.getSize());
            proc.pollParametersForTests();

            const float got = proc.apvts.getRawParameterValue ("phaseMode")->load();
            const float want = idx == 0 ? 0.0f : 1.0f;   // 0 = Aligned, 1 = Scattered
            std::printf ("  %-9s %-18s old chunk value %.3f  ->  fader %.3f (want %.3f)\n",
                         idx == 0 ? "Aligned" : "Scattered",
                         stale == 1 ? "(+ dead phaseGlide)" : "(no phaseGlide)",
                         stored, got, want);
            if (std::abs (got - want) > 1.0e-4f)
            { std::printf ("        OLD PROJECT OPENS ON A DIFFERENT SOUND\n"); ++failures; }

            // The deleted parameter must not come back to life as a ghost: if anything
            // ever re-registers it, the "it is simply ignored" argument above stops
            // holding and this catches that rather than the user's next session.
            if (proc.apvts.getParameter ("phaseGlide") != nullptr)
            { std::printf ("        phaseGlide still exists as a parameter\n"); ++failures; }

            // The same number is what a saved AUTOMATION LANE holds, because a host
            // stores normalised values: a two-choice parameter normalises index 1 to
            // 1.0, and so must the fader, or every drawn lane is rescaled silently.
            if (auto* p = proc.apvts.getParameter ("phaseMode"))
                if (std::abs (p->convertTo0to1 (want) - oldShape.convertTo0to1 ((float) idx)) > 1.0e-4f)
                { std::printf ("        automation lane at %d no longer means the same phase\n", idx); ++failures; }
        }

        // And the other half of the migration: the parameter must now actually be
        // CONTINUOUS. If someone reverts it to a Choice the two checks above still
        // pass - they only look at the ends - and the whole feature is gone.
        {
            FractalAdditiveAudioProcessor proc;
            setP (proc.apvts, "phaseMode", 0.5f);
            const float mid = proc.apvts.getRawParameterValue ("phaseMode")->load();
            std::printf ("  midpoint 0.500 -> %.3f\n", mid);
            if (std::abs (mid - 0.5f) > 1.0e-3f)
            { std::printf ("        Phase is not continuous - it snapped to %.3f\n", mid); ++failures; }
        }
    }

    // == The TRANSITION now defaults to Dry/Wet - and old projects must not follow it ==
    //
    // The panel asks ONE question ("TRANSITION: Off / Morph / Dry/Wet") and its default
    // answer is Dry/Wet, so the "dryWet" flag ships DEFAULTING TO ON. That default is a
    // trap for every project older than the feature: APVTS restores state by asking each
    // parameter it owns for its <PARAM> child, and a parameter whose child is ABSENT is
    // left holding whatever it already had - which, on the fresh instance a host builds
    // before handing over a chunk, is that new default. Without the migration in
    // restoreState, every patch saved before bf0f768 would reopen with a 0.3 s crossfade
    // under every spectrum change, on a sound whose author never had the control at all.
    //
    // Three chunks, because all three exist on disk and each must come back as itself.
    {
        std::printf ("== Transition: what an OLD project opens as, now that Dry/Wet is the default ==\n");

        // A brand new instance, first: this is the half the user asked for, and if it
        // ever goes back to OFF the migration below would still pass while the feature
        // it protects has quietly gone.
        {
            FractalAdditiveAudioProcessor fresh;
            const float dw = fresh.apvts.getRawParameterValue ("dryWet")->load();
            const float mo = fresh.apvts.getRawParameterValue ("morph")->load();
            std::printf ("  a NEW instance: dryWet %.0f, morph %.0f (want 1, 0 - the Dry/Wet default)\n", dw, mo);
            if (dw < 0.5f || mo > 0.5f)
            { std::printf ("        a new instance no longer starts on Dry/Wet\n"); ++failures; }
        }

        // 0: no "dryWet" child at all - a project older than the transition itself.
        // 1: a child saying OFF, written by a build that had the control.
        // 2: a child saying ON.
        for (int shape = 0; shape <= 2; ++shape)
        {
            FractalAdditiveAudioProcessor proc;
            auto tree = proc.apvts.copyState();
            auto dwChild = tree.getChildWithProperty ("id", "dryWet");
            if (shape == 0) tree.removeChild (dwChild, nullptr);
            else            dwChild.setProperty ("value", shape == 2 ? 1.0f : 0.0f, nullptr);

            MemoryBlock blob;
            if (auto xml = tree.createXml())
                FractalAdditiveAudioProcessor::copyXmlToBinary (*xml, blob);
            proc.setStateInformation (blob.getData(), (int) blob.getSize());
            proc.pollParametersForTests();

            const float got  = proc.apvts.getRawParameterValue ("dryWet")->load();
            const float want = shape == 2 ? 1.0f : 0.0f;
            static const char* what[3] = { "no dryWet child (pre-feature project)",
                                           "child says OFF", "child says ON" };
            std::printf ("  %-38s -> dryWet %.0f (want %.0f)\n", what[shape], got, want);
            if (std::abs (got - want) > 1.0e-4f)
            { std::printf ("        OLD PROJECT OPENS ON A DIFFERENT TRANSITION\n"); ++failures; }
        }
    }

    // == A project older than the Harmonics fader: Prime Limit came back INVERTED ==
    //
    // Two parameters changed shape on 2026-07-25 and both are recognised by ONE piece of
    // evidence - the chunk having no "harmonics" child:
    //   selection  -> harmonics   the Choice {All,Even,Odd,Prime} became the continuous
    //                             axis {Even,All,Odd,Prime}. An old chunk has no child for
    //                             it, and a missing child does NOT leave the fader where it
    //                             stood: the reconnect pass appends a valueless <PARAM>,
    //                             the append fires valueTreeChildAdded -> setNewState, and
    //                             setNewState with no "value" property writes the DEFAULT.
    //                             The saved choice is simply lost, and the fader reads All.
    //   primeLimit                the Choice list was REVERSED, {Off,2,3,..,23} ->
    //                             {Off,23,19,..,2}. This one DOES have a child, and APVTS
    //                             stores a Choice denormalised, i.e. as its index - so the
    //                             old index lands in the new order and means its opposite.
    //                             Saved "23" (the mildest limit) reopens as limit 2 and
    //                             keeps only the powers of two; saved "2" reopens as 23
    //                             and filters nothing. Only "Off" survives, because 0 is 0
    //                             in both orders - so a test that tried only Off would
    //                             pass on the single index that is its own image.
    //
    // The migration for this existed but sat AFTER apvts.replaceState, where the evidence
    // is already gone (replaceState shares the tree and its reconnect pass appends a child
    // for every parameter the chunk was missing), so it had never run. This is the check
    // that says whether it runs now, and it is written against a HAND-BUILT chunk in the
    // old shape rather than against a belief about one.
    {
        std::printf ("== A project older than the Harmonics fader: Prime Limit must not invert ==\n");

        static const int   oldPrimeOrder[10] = { 0, 2, 3, 5, 7, 11, 13, 17, 19, 23 };
        static const int   newPrimeOrder[10] = { 0, 23, 19, 17, 13, 11, 7, 5, 3, 2 };
        static const float wantAxis[4] = { (float) spectrum::All,  (float) spectrum::Even,
                                           (float) spectrum::Odd,  (float) spectrum::Prime };
        static const char* oldSelName[4] = { "All", "Even", "Odd", "Prime" };

        // Every old prime index, with the selection cycling so all four choices are
        // covered too.
        for (int oldPl = 0; oldPl < 10; ++oldPl)
        {
            const int oldSel = oldPl % 4;

            FractalAdditiveAudioProcessor proc;

            // Move BOTH faders somewhere distinctive BEFORE handing over the chunk, so a
            // pass can never be the instance simply never having moved. It also pins the
            // mechanism down: with the migration dead this prints Harmonics 1.00 for every
            // row - the DEFAULT, not the 3.00 set one line up - which is the measured proof
            // that a missing child is reset rather than left alone, and therefore that the
            // saved Even/Odd/Prime choice is unrecoverable without this migration.
            setP (proc.apvts, "harmonics",  (float) spectrum::Prime);
            setP (proc.apvts, "primeLimit", 4.0f);
            proc.pollParametersForTests();

            auto tree = proc.apvts.copyState();
            tree.removeChild (tree.getChildWithProperty ("id", "harmonics"), nullptr);
            ValueTree sel ("PARAM");
            sel.setProperty ("id", "selection", nullptr);
            sel.setProperty ("value", (float) oldSel, nullptr);
            tree.appendChild (sel, nullptr);
            tree.getChildWithProperty ("id", "primeLimit").setProperty ("value", (float) oldPl, nullptr);

            MemoryBlock blob;
            if (auto xml = tree.createXml())
                FractalAdditiveAudioProcessor::copyXmlToBinary (*xml, blob);
            proc.setStateInformation (blob.getData(), (int) blob.getSize());
            proc.pollParametersForTests();

            const float gotAxis = proc.apvts.getRawParameterValue ("harmonics")->load();
            const int   gotIdx  = jlimit (0, 9, (int) std::lround (proc.apvts.getRawParameterValue ("primeLimit")->load()));
            const int   gotLim  = newPrimeOrder[gotIdx];

            std::printf ("  old chunk  selection=%-5s primeLimit=%-2s  ->  Harmonics %.2f (want %.2f), limit %2d (want %2d)\n",
                         oldSelName[oldSel], oldPrimeOrder[oldPl] == 0 ? "Off" : String (oldPrimeOrder[oldPl]).toRawUTF8(),
                         gotAxis, wantAxis[oldSel], gotLim, oldPrimeOrder[oldPl]);

            if (std::abs (gotAxis - wantAxis[oldSel]) > 1.0e-3f)
            { std::printf ("        the saved Harmonics choice was thrown away\n"); ++failures; }
            if (gotLim != oldPrimeOrder[oldPl])
            { std::printf ("        THE PRIME LIMIT INVERTED - the patch reopens on a different spectrum\n"); ++failures; }
        }

        // What that costs in partials, which is the half a user would actually hear. An
        // old patch on limit 23 (barely any filtering) reopening under the reversed order
        // lands on limit 2 and keeps only the powers of two.
        {
            const auto countFor = [] (bool oldShape) -> int
            {
                FractalAdditiveAudioProcessor proc;
                setP (proc.apvts, "baseN", 1.0f);
                setP (proc.apvts, "fractalLevel", 4.0f);
                setP (proc.apvts, "harmonics", (float) spectrum::All);
                setP (proc.apvts, "primeLimit", oldShape ? 0.0f : 1.0f); // 1 = limit 23 today
                proc.pollParametersForTests();

                auto tree = proc.apvts.copyState();
                if (oldShape)
                {
                    tree.removeChild (tree.getChildWithProperty ("id", "harmonics"), nullptr);
                    ValueTree s ("PARAM");
                    s.setProperty ("id", "selection", nullptr);
                    s.setProperty ("value", 0.0f, nullptr);      // "All", in the old order
                    tree.appendChild (s, nullptr);
                    tree.getChildWithProperty ("id", "primeLimit").setProperty ("value", 9.0f, nullptr); // old "23"
                    MemoryBlock blob;
                    if (auto xml = tree.createXml())
                        FractalAdditiveAudioProcessor::copyXmlToBinary (*xml, blob);
                    proc.setStateInformation (blob.getData(), (int) blob.getSize());
                    proc.pollParametersForTests();
                }
                // DRAIN THE SPECTRUM WORKER BEFORE MEASURING, or this check reports a
                // number that has nothing to do with the migration.
                //
                // The polls above each queue a rebuild on the Priority::low SpectrumBuilder
                // thread, and the FIRST of them was queued with primeLimit still Off. If
                // the worker has already picked that one up, buildNow()'s synchronous
                // publish is simply overwritten by the stale one a moment later - measured
                // here as 1024 partials (Off, nothing filtered) instead of 375 (limit 23),
                // on 1 run in 5 while another build was loading the machine. Waiting for
                // isSpectrumBuildPending() to clear is what RhythmVerify's awaitSpectrumIdle
                // does for the same reason; a sleep would only move the race.
                const auto settle = [&proc]
                {
                    const double t0 = Time::getMillisecondCounterHiRes();
                    while (proc.isSpectrumBuildPending()
                           && Time::getMillisecondCounterHiRes() - t0 < 15000.0)
                        Thread::sleep (1);
                };
                settle();
                proc.prepareToPlay (sampleRate, blockSize);   // buildNow() is synchronous
                settle();
                return proc.getTemplatePartialCount();
            };

            const int viaOldChunk = countFor (true);
            const int direct      = countFor (false);
            std::printf ("  the same patch, limit 23, Fractal 4: %d partials via the old chunk, %d set directly\n",
                         viaOldChunk, direct);
            if (viaOldChunk != direct)
            { std::printf ("        %d partials LOST to the migration alone\n", direct - viaOldChunk); ++failures; }
        }

        // THE OTHER HALF, and the one that makes deleting the gate unsafe: a chunk that
        // DOES carry a harmonics child is a chunk from a build that already had the new
        // prime order, so its primeLimit index must be taken at face value. Remap that and
        // every project saved since 09c47e9 opens on the wrong limit - a far bigger blast
        // radius than the one this migration repairs.
        for (int idx : { 1, 5, 9 })
        {
            FractalAdditiveAudioProcessor proc;
            auto tree = proc.apvts.copyState();
            tree.getChildWithProperty ("id", "primeLimit").setProperty ("value", (float) idx, nullptr);
            tree.getChildWithProperty ("id", "harmonics") .setProperty ("value", (float) spectrum::Odd, nullptr);
            // A stale "selection" child as well: some chunks carry one from a build that
            // still wrote it, and its mere presence must not start a migration.
            ValueTree sel ("PARAM");
            sel.setProperty ("id", "selection", nullptr);
            sel.setProperty ("value", 3.0f, nullptr);
            tree.appendChild (sel, nullptr);

            MemoryBlock blob;
            if (auto xml = tree.createXml())
                FractalAdditiveAudioProcessor::copyXmlToBinary (*xml, blob);
            proc.setStateInformation (blob.getData(), (int) blob.getSize());
            proc.pollParametersForTests();

            const int   got     = jlimit (0, 9, (int) std::lround (proc.apvts.getRawParameterValue ("primeLimit")->load()));
            const float gotAxis = proc.apvts.getRawParameterValue ("harmonics")->load();
            std::printf ("  a CURRENT chunk (has harmonics) primeLimit=%d -> %d, Harmonics %.2f (want %d, %.2f)\n",
                         idx, got, gotAxis, idx, (float) spectrum::Odd);
            if (got != idx || std::abs (gotAxis - (float) spectrum::Odd) > 1.0e-3f)
            { std::printf ("        the migration fired on a CURRENT project\n"); ++failures; }
        }
    }

    // == The fader has to be a RAMP, not a switch with a ramp painted on it. ==
    //
    // phaseMode used to select the normalisation law by a threshold at 0.5:
    //   aligned ? min(rmsN, peakN) : rmsN.  Those two laws are up to 20.70 dB apart
    // (PhaseSetTests measures the gap), so a continuous control crossing 0.5 while a
    // chord is held would step every sounding voice by that much IN ONE SAMPLE. The
    // gesture the user asked for is exactly the gesture that would trigger it.
    //
    // So: sweep the fader and look at the LEVEL. A threshold shows up as one adjacent
    // pair miles apart while every other pair barely moves; a blend shows up as a
    // staircase with no step much bigger than the others. 3.0 dB is the line - the
    // whole span here is about 16 dB over 20 steps, so a working blend averages 0.8 dB
    // a step, and the smallest thing a re-introduced threshold could produce is the
    // several-dB remainder of a 16 dB gap.
    {
        std::printf ("== Phase fader: the level must RAMP across the travel, not step ==\n");

        FractalAdditiveAudioProcessor proc;
        setP (proc.apvts, "baseN", 8.0f);
        setP (proc.apvts, "harmonics", (float) spectrum::All);
        setP (proc.apvts, "curveSlope", 1.0f);
        setP (proc.apvts, "fractalLevel", 1.0f);
        proc.prepareToPlay (sampleRate, blockSize);

        constexpr int kSteps = 21;
        double db[kSteps] = {};
        for (int s = 0; s < kSteps; ++s)
        {
            setP (proc.apvts, "phaseMode", (float) s / (float) (kSteps - 1));
            proc.pollParametersForTests();
            auto w = renderWindow (proc, 60);
            double sum = 0.0;
            for (float v : w) sum += (double) v * (double) v;
            db[s] = 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (sum / (double) w.size())));
        }

        double worstStep = 0.0; int worstAt = 0;
        for (int s = 1; s < kSteps; ++s)
            if (std::abs (db[s] - db[s - 1]) > worstStep) { worstStep = std::abs (db[s] - db[s - 1]); worstAt = s; }

        std::printf ("  RMS across the travel: %.1f dB at 0.00 -> %.1f dB at 1.00 (span %.1f dB)\n",
                     db[0], db[kSteps - 1], db[kSteps - 1] - db[0]);
        std::printf ("  biggest single step %.2f dB, between u=%.2f and u=%.2f\n",
                     worstStep, (double) (worstAt - 1) / (kSteps - 1), (double) worstAt / (kSteps - 1));
        if (worstStep > 3.0)
        { std::printf ("        the fader STEPS - a held chord bangs mid-gesture\n"); ++failures; }
        // ...and it must actually GO somewhere, or a "blend" that returns a constant
        // would pass the step test by doing nothing at all.
        if (db[kSteps - 1] - db[0] < 6.0)
        { std::printf ("        the fader barely moves the level (%.1f dB) - it is not blending the two laws\n",
                       db[kSteps - 1] - db[0]); ++failures; }
    }

    std::printf ("\n%s\n", failures == 0 ? "no partials dropped" : "PARTIALS DROPPED");
    return failures == 0 ? 0 : 1;
}
