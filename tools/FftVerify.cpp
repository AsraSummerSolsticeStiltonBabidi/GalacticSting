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

    std::printf ("\n%s\n", failures == 0 ? "no partials dropped" : "PARTIALS DROPPED");
    return failures == 0 ? 0 : 1;
}
