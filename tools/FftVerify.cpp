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

        for (int idx = 0; idx <= 1; ++idx)
        {
            // What the old build actually wrote into the chunk for this choice.
            const float stored = oldShape.convertFrom0to1 (oldShape.convertTo0to1 ((float) idx));

            FractalAdditiveAudioProcessor proc;
            auto tree = proc.apvts.copyState();

            // A pre-fader chunk has no "phaseGlide" child at all - that parameter did
            // not exist. Removing it is what makes this blob genuinely OLD rather than
            // a new one with one property edited.
            tree.removeChild (tree.getChildWithProperty ("id", "phaseGlide"), nullptr);
            auto ph = tree.getChildWithProperty ("id", "phaseMode");
            ph.setProperty ("value", stored, nullptr);

            MemoryBlock blob;
            if (auto xml = tree.createXml())
                FractalAdditiveAudioProcessor::copyXmlToBinary (*xml, blob);
            proc.setStateInformation (blob.getData(), (int) blob.getSize());
            proc.pollParametersForTests();

            const float got = proc.apvts.getRawParameterValue ("phaseMode")->load();
            const float want = idx == 0 ? 0.0f : 1.0f;   // 0 = Aligned, 1 = Scattered
            std::printf ("  %-9s old chunk value %.3f  ->  fader %.3f (want %.3f)\n",
                         idx == 0 ? "Aligned" : "Scattered", stored, got, want);
            if (std::abs (got - want) > 1.0e-4f)
            { std::printf ("        OLD PROJECT OPENS ON A DIFFERENT SOUND\n"); ++failures; }

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

    std::printf ("\n%s\n", failures == 0 ? "no partials dropped" : "PARTIALS DROPPED");
    return failures == 0 ? 0 : 1;
}
