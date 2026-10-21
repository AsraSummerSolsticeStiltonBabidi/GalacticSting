#pragma once

//==============================================================================
// PhaseSets: the start phase of every partial, from "all aligned" to "as scattered
// as we can honestly measure", as one deterministic pure function of the partial
// list. This is the source the ALIGNED <-> SCATTERED FADER stands on.
//
// WHY THIS EXISTS. The voice has exactly two phase modes today: Aligned (every
// partial at 0.25, i.e. a cosine, so the whole spectrum fires as one pulse) and
// Scattered (a fixed sin/frac hash, AdditiveVoice::seedPhase). Aligned is a
// 4.5x-32x crest factor, so the per-voice normaliser has to pull the whole voice
// down by up to 20 dB to keep that one sample legal, and everything else in the
// patch gets quieter with it. The hash is much better but it is a coincidence, not
// an answer, and there is nothing in between the two.
//
//==============================================================================
// THE GUARANTEE, STATED EXACTLY. Read the next twenty lines before anything else;
// three previous versions of this file shipped a guarantee that was true of
// something other than what a listener hears, and each time the same sentence is
// what would have caught it.
//
//   For every prefix length p from 1 to PhaseReport::analysed, the phases that
//   PhasePlan::phasesFor(p) returns have a peak-to-RMS ratio at least
//   kPreferMarginDb BETTER than the shipping hash's on the same prefix, measured
//   on the TRUE UNQUANTISED RATIOS, AT EVERY OBSERVATION LENGTH IN THE SPAN
//   LADDER - not at one. Wherever that was not achieved at every length, the hash
//   IS what is returned, bit for bit.
//
//   The ladder reaches PhaseReport::verifiedSpanPeriods fundamental periods, which
//   is PhaseReport::verifiedSpanPeriods / f0 seconds at the played pitch. Two
//   classes, and which one a template is in is a PROVEN property, not a guess:
//
//     EXACTLY PERIODIC (PhaseReport::exactlyPeriodic == true). Every analysed
//     ratio times the analysis window W is an integer, so the rendered waveform
//     REPEATS with period W and one span is the whole of every note that will ever
//     be played. verifiedSpanPeriods is infinite in the only sense that matters and
//     is reported as kUnboundedSpan. Every template the harmonic engine compiles is
//     in this class (its ratios are integers by construction, fractal merges
//     included), and this is where the search of version 2 keeps its win.
//
//     APERIODIC (exactlyPeriodic == false). A Scala-as-timbre template: the ratios
//     are irrational, the waveform never repeats, and its peak keeps CREEPING UP
//     the longer the note is held. Here the ladder measures kSpansAperiodic
//     consecutive windows and a candidate must win at all of them. Past the end of
//     the ladder we do not claim anything - and we return the hash rather than
//     claim it.
//
//   For p > analysed the hash is returned unconditionally, so those pitches are
//   exactly today's Scattered mode. PhaseReport::lowestFullyCoveredHz says which
//   pitch that starts at and the UI should paint it.
//
//   Every measurement is taken on the VERIFY GRID, whose sample points are a strict
//   superset of the ones any construction looked at (Options::verifyOversample). A
//   denser grid can only find a peak the sparser one missed, so the decision cannot
//   be an artefact of a search's own grid.
//
//   THE TWO PRECONDITIONS, stated here because a guarantee with unstated
//   preconditions is how the previous three versions were refuted:
//
//     1. THE AMPLITUDES. The guarantee is for the amplitude list handed to
//        buildPlan. It is NOT for a different one, and two things in this plugin
//        hand the voice a different one - a per-partial gain edit and slope
//        modulation. See FAILURE 5 below, which is the fifth refutation and the one
//        that lives on the caller's side of the line.
//
//     2. THE SAMPLE GRID AT VERY SHORT NOTES. On an exactly-periodic template the
//        guarantee is over one whole period of the CONTINUOUS waveform - the
//        shortest complete observation that exists. A real 48 kHz render at a low
//        pitch samples that period at a non-integer rate (65.406 Hz is 733.9 samples
//        per period), so a note only one or two periods long sees a sparse, shifting
//        subset of it and its SAMPLED peak has not converged. MEASURED, FLAT n=512
//        at MIDI 36, plan peak against hash peak on a real 48 kHz grid: +0.22 dB at
//        1 period, +0.26 dB at 2, -0.82 dB at 3.3, -1.73 dB at 16. One note of
//        eighty-eight, at one length, on one spectrum, and it converges by the third
//        period - a 46 ms note at the bottom of the keyboard. It is pinned by
//        testShortNoteSampling so it cannot be quietly widened.
//
//==============================================================================
// WHAT THE INTEGRATOR MUST DO. Every limit below changes what is heard, so none of
// them may be silent.
//
//   WHERE TO CALL IT. buildPlan belongs beside compileSpectrum on the background
//   compile thread, and its PhasePlan is cached per compiled template. It is NOT a
//   per-frame call: pass kWorkBudgetDrag while a playground control is being
//   dragged, and re-run once with kWorkBudgetCompile when the drag settles. A late
//   upgrade is inaudible on notes already sounding, because a plan only reaches a
//   voice at note-on.
//
//   WHAT TO HAND IT. The AMPLITUDES the voice will actually render, not the ones
//   compileSpectrum returned, whenever those differ: multiply the per-partial gain
//   vector in first, and rebuild the plan when that vector changes. This is a
//   PRECONDITION of the guarantee, not a refinement of it - see FAILURE 5. A stale
//   plan measured 2.13 dB WORSE than today's hash after a single pencil edit.
//
//   AND AT WHICH SLOPE. The one the template was COMPILED at - param +
//   modulationSpan_LO, the reachable floor PluginProcessor.cpp:574-580 already uses.
//   Do NOT re-centre it on the modulation span, and do not compile a second template
//   to centre it on. This is a choice between two imperfect rules, not a clean win:
//   once Slope is modulated NEITHER compile point keeps the guarantee (FAILURE 5(b)),
//   and the low end is chosen because over the whole reachable parameter its worst case
//   is -5.07 dB where centring's is -15.27 dB, both on the default patch. Centring is
//   genuinely BETTER over some narrow ranges - on the default patch at [0.0, 1.0] it
//   wins on mean, worst case and share-of-cases at once - and 5(c) names where. Read it
//   before you re-litigate this: two revisions of this header have now been wrong about
//   it in opposite directions. testSlopeCompilePosition pins the mid range, the floor
//   range AND the whole parameter, because the answer differs at all three.
//
//   AT NOTE-ON, AND THESE ARE TWO DIFFERENT NUMBERS:
//
//       seedFromPlan (plan,
//                     RenderedPrefix { renderedPrefix (partials, n, f0Hz, sampleRate) },
//                     FillSpan       { seedSpan },
//                     scatter, out);
//
//   RenderedPrefix is the voice's own sub-Nyquist partial count at THIS pitch, and it
//   chooses WHICH PHASE SET PLAYS - the entire answer to Failure 2 below. FillSpan is
//   how many oscillator slots to write - a BANK SPAN, oscCap() or a stale activeCount,
//   routinely ten times larger. Collapsing the two into one argument is FAILURE 6 at
//   seedFromPlan, which disconnected the guarantee from every voice in the plugin and
//   measured 636 of 1851 cases WORSE than today's hash. The types will not let you do
//   it again; renderedPrefix() computes the first by the same rule the voice uses, so
//   do not write a second copy of that rule either.
//
//   AND IF YOU MUST SEED MORE SLOTS THAN THE PLAN COVERS, JUST ASK FOR THEM.
//   seedFromPlan is total in the FILL SPAN: past plan.n it continues today's Scattered
//   mode at the same scatter, so a span of oscCap() - 1638 with Shepard on, 16384 with
//   it off - or a stale, larger activeCount is a defined sound at every index, not a
//   boundary. You do not have to clamp, and you must not: clamping and filling the rest
//   yourself is how the 0.25 pulse got there. hashPhaseAt(index) is exposed for the one
//   case seedFromPlan cannot serve - seeding a Shepard copy slot by slot - and it is
//   the same value the plan carries inside [0, n).
//
//   WHAT THAT TAIL COSTS: nothing you are not already paying. Measured on this box,
//   scatter 1 over the full 16384-slot span, seedFromPlan 173.4 us against today's
//   own seedPhase loop at 178.0 us over the same span - because past plan.n it IS
//   today's loop. The old kAlignedPhase fill took 1.8 us; if someone offers you that
//   1.7 us back, they are offering to put every partial past plan.n at its peak.
//
//   THE FADER. Apply the taper yourself: x = pow(u, kTaperExponent). Pass x as
//   `scatter`. Without it the first half of the travel is dead - see the table at
//   kTaperExponent, which is the same complaint the user already made about the
//   fractal fader.
//
//   PAINT, in order of how much a listener would notice it:
//     lowestFullyCoveredHz  Below this pitch the note renders more partials than
//                           the analysis reached, so it gets today's Scattered mode
//                           unchanged. Paint it as a keyboard range, not a number.
//     exactlyPeriodic / verifiedSpanPeriods
//                           On an aperiodic (Scala) timbre the guarantee reaches
//                           verifiedSpanPeriods / f0 seconds and no further. Paint
//                           it as a time if you paint it at all.
//     outcome               Aligned / Hash / Chosen / Empty. `Hash` is a RESULT, not
//                           a failure - it means we measured and today's phases won -
//                           so do not paint it as an error.
//     improvedPrefixes, firstImproved, lastImproved
//                           The only honest claim of improvement in this header. A
//                           badge saying "improved" must read this, never `outcome`
//                           alone and never a boolean that says a search ran.
//     reachReduced          The budget, not the pitch, cut the reach. Offer the
//                           bigger budget rather than hiding it.
//
//   NEVER DIVIDE WITHOUT CHECKING >= 0.0. Unmeasured fields are exactly
//   kUnmeasured (-1.0), never 0.0; that is deliberate, and it is why v1's silent
//   normGain = 0 cannot happen here.
//
//   DO NOT DRIVE A NORMALISATION LAW FROM crestChosen. It is a measurement of one
//   template and it steps across a playground blend when the union's reach changes.
//   Drive the normaliser from something continuous in the blend, or hold it for the
//   duration of a drag. (AdditiveVoice's own phaseMode-selected normalisation law is
//   a separate trap - see the integration notes.)
//
//==============================================================================
// THE FOUR REFUTATIONS THIS FILE IS THE ANSWER TO. Every one of them had the same
// shape: THE GUARANTEE WAS SCOPED TO SOMETHING THAT IS NOT WHAT THE LISTENER HEARS.
// They are written out because the next person to have one of these ideas deserves
// the measurement rather than having to rediscover it.
//
//   FAILURE 1 - THE ANALYSIS GRID WAS A SURROGATE. v1 snapped every partial onto an
//     integer bin, llround(ratio * W). On integer harmonics that is exact. On any
//     INHARMONIC set - every Scala-as-timbre patch, this instrument's headline
//     feature - the bin is up to half a bin off, which is up to 0.49 CYCLES of
//     accumulated phase error across the window. Measured: a 22-EDO timbre over 5
//     periods, n=110, reported crest 2.3213 against a hash of 3.0318 - a claimed
//     2.32 dB win - actually rendered (direct std::sin, true ratios) at 3.4409
//     against a hash of 2.8953. The "win" was a 1.50 dB LOSS.
//     FIXED, and the fix is kept: there is no bin. Each partial gets a 32-BIT
//     FIXED-POINT PHASE INCREMENT, so its frequency is represented to 2^-32 cycles
//     per sample - about 5e-7 cycles of drift across a 4096-sample window, six
//     orders of magnitude better than v1 - while keeping v1's table-lookup speed and
//     its exact-integer determinism. See detail::Grid.
//
//   FAILURE 2 - THE TARGET WAS BUILT FOR THE TEMPLATE; THE VOICE RENDERS A PREFIX.
//     AdditiveVoice renders only the partials below Nyquist at the played pitch (its
//     `newN`). A phase set is a property of the TEMPLATE, the rendered list is a
//     property of the PITCH. Measured: a default patch reported a 4.14 dB win at
//     n=128 and was 3.04 dB WORSE than the hash at MIDI 96, where only 11 of those
//     128 partials sound; 34 of 88 notes were worse.
//     FIXED, and the fix is kept: EVERY PREFIX IS DECIDED SEPARATELY. One cumulative
//     pass measures all n prefixes for the price of one synthesis, and the plan
//     carries a per-prefix index. A prefix that has nothing better gets the hash.
//
//   FAILURE 3 - THE NOISE CONSTRAINT WAS A PULSE DETECTOR. v1 constrained the
//     derivative crest, which is blind to a CHIRP: a flat-spectrum Schroeder set has
//     a flat envelope and a flat |dx/dt| envelope while sweeping from 7 kHz to 54 kHz
//     inside every period - an audible rasp at the note rate, the exact opposite of
//     the noise this control is for. It passed a set 501/512 phases identical to
//     Schroeder whose short-time RMS frequency ramped x7.8 per period.
//     FIXED, and the fix is kept: detail::sweepCorrelation, a SWEEP detector, runs
//     beside the derivative gate. Both are kept because each is blind where the
//     other sees - see the table at kDerivConstructionHeadroom, where squeezing the
//     derivative gate harder pushes the construction INTO a chirp.
//
//   FAILURE 4 - THE GUARANTEE WAS SCOPED TO THE WINDOW WE CHOSE. This is the one
//     that killed version 2, and it is the reason this file exists. v2 measured over
//     ONE window - the window the partial spacing demanded, which on a Scala timbre
//     is tens of fundamental periods - and a held note is not obliged to be shorter
//     than a window we chose. MEASURED on the skeptics' independent true-ratio rig,
//     53-EDO x3 (159 partials), MIDI 48, engine window 76 periods = 581 ms, v2's
//     returned set against today's hash:
//
//        note length   x window   dB vs hash   plan.improved() said
//          0.10 s        0.17x      +1.30          TRUE
//          0.30 s        0.52x      -0.09          TRUE
//          0.58 s        1.00x      +1.96          TRUE
//          1.00 s        1.72x      -1.15          TRUE      <-- audibly worse
//          2.00 s        3.44x      -0.66          TRUE
//          4.00 s        6.88x      -0.49          TRUE
//
//     At 1.0 s that set was beaten by 238 of 256 independent random phase draws.
//     FIXED HERE, and this is the whole change: A CANDIDATE IS MEASURED AT EVERY
//     LENGTH IN A LADDER OF CONSECUTIVE SPANS AND MUST WIN AT ALL OF THEM. Where the
//     ratios prove the waveform repeats, one span IS every length and the ladder
//     costs nothing. Where they do not, the ladder is what the budget is spent on -
//     which is why the expensive search is not run there at all.
//
//   FAILURE 5 - THE GUARANTEE WAS SCOPED TO THE AMPLITUDES WE WERE HANDED, AND THE
//     VOICE RENDERS DIFFERENT ONES. This one is not fixed here, because it cannot
//     be: it is the caller's to avoid, and it is written out at this length so that
//     it is avoided rather than rediscovered. Two paths reach it, both read off the
//     plugin source rather than imagined.
//
//     (a) A PER-PARTIAL GAIN EDIT. Spectrum.h:137 says the gain vector is NEVER
//         written by compileSpectrum; AdditiveVoice.h:714-719 multiplies it in
//         afterwards. So a pencil edit changes what sounds and does not change what
//         buildPlan saw. MEASURED on the skeptics' rig, true continuous peak over one
//         exact period, one partial pulled to silence:
//           FLAT n=512    MIDI 99, 9 partials sounding   +0.75 dB -> 2.13 dB WORSE
//           fractal L3    MIDI 96, 11 sounding           +0.97 dB -> 2.01 dB WORSE
//         THE REMEDY, and it works completely: REBUILD THE PLAN ON THE GAINED
//         AMPLITUDES. Same cases, plan rebuilt on what actually sounds: +0.00 dB and
//         +0.00 dB - the engine correctly returns the hash - and +1.69 dB where a set
//         still wins. testAmplitudesAreAPrecondition asserts both halves. The gain
//         vector is a settled, user-driven edit, so this is one more compile, on the
//         thread buildPlan already lives on.
//
//     (b) SLOPE MODULATION, which a rebuild CANNOT fix. PluginProcessor compiles the
//         template at slope = param + modulationSpan_LO and AdditiveVoice rescales
//         every amplitude by ratio^-dS per voice at render time, so dS is exactly 0
//         when Slope carries no modulation and grows to the width of the modulation
//         span when it does. A per-voice, continuously moving quantity cannot be met
//         by a cached plan. MEASURED (default n=1024, mean over the notes the plan
//         claims improved):
//           dS      0      0.25    0.5     1.0     2.0
//           dB   +1.96   +1.67   +1.26   +0.48   -0.09
//         and the worst single case found anywhere was FLAT n=512 at dS = 1.0,
//         MIDI 47: 5.07 dB worse than the hash.
//
//         AND THE FIX THAT WAS TRIED AND REJECTED, because it is the obvious one:
//         make the ladder cover dS as well - require a candidate to beat the hash at
//         every dS in {0, .25, .5, 1, 2, 4} the way it must beat it at every span.
//         MEASURED: of the notes the plan improves, the number that survive that
//         requirement is 0 of 70 on the default patch, 0 of 71 on fractal L3, 0 of 65
//         on FLAT, 0 of 48 on odd, 0 of 60 on prime-limit 7. ZERO EVERYWHERE. A
//         slope ladder does not make the feature honest, it deletes it. So the
//         answer is the precondition, stated - and NOT the caller-side instruction a
//         previous revision of this header gave, which is refuted immediately below.
//
//     (c) WHERE TO COMPILE THE PLAN IN THE MODULATION SPAN. This section has been
//         wrong twice in opposite directions and is now written from a sweep that
//         reaches the parameter's real floor. Read the verdict before the numbers.
//
//         THE RULE THAT SHIPS, UNCHANGED: BUILD THE PLAN ON EXACTLY THE AMPLITUDES
//         THE TEMPLATE WAS COMPILED AT - the reachable minimum, param +
//         modulationSpan_LO, which is already what PluginProcessor.cpp:574-580 hands
//         compileSpectrum. Do not re-centre it, and do not compile a second template
//         to centre it on.
//
//         BUT THE REASONS THAT WERE GIVEN FOR IT WERE HALF WRONG, AND THE HALF THAT
//         WAS WRONG IS THE HALF A READER WOULD HAVE LEANED ON. Two revisions ago this
//         header told the integrator to compile at the CENTRE of the span; the
//         previous revision withdrew that on a four-range sweep and gave five
//         arguments. A skeptic then pointed out that all four of those ranges compile
//         at 0.5 or higher and NONE of them reaches curveSlope's actual floor.
//         PluginProcessor.cpp:126-127 declares NormalisableRange(0.0, 4.0, 0.001)
//         default 1.0, and PluginProcessor.cpp:574-580 compiles at param + lo where
//         ModEngine.h:317-318 sums only NEGATIVE modulator ranges into lo - so ONE
//         modulator routed at curveSlope with its range at -0.25 normalised puts the
//         default patch's compile point exactly at 0.0. That is an ordinary patch, and
//         the old sweep never went there.
//
//         THE SWEEP RE-RUN WITH THE FLOOR REACHED. Same rig, same method, same three
//         rules, MIDI 21-108, 5 voice slopes per range, each rule compiling its own
//         plan and scored against the hash on its own sounding amplitudes; four more
//         reachable ranges added, every one of them touching 0.0:
//           [0.0,1.0] param 1.0 with one modulator at -0.25 normalised (the skeptic's)
//           [0.0,0.5] param 0.5 with one at -0.125
//           [0.0,2.0] param 1.0 with -0.25 and +0.25
//           [0.0,4.0] param 2.0 with -0.5 and +0.5 - the whole parameter
//
//         AND AT EVERY SEMITONE, WHICH TURNED OUT TO MATTER AS MUCH AS THE RANGES.
//         Both the old sweep and the first re-run of it stepped MIDI in threes. On the
//         default patch at [0.0,1.0] the three-step grid reports the shipped rule's
//         worst case as -0.35 dB; every semitone reports -5.07 dB. The bad case is ONE
//         note. A coarse note grid is the same defect as a short range list - a sample
//         reported as a result - and every number below is at step 1 for that reason.
//
//         WHICH OF THE FIVE ARGUMENTS SURVIVED:
//
//         1. "The shipped rule's worst regression anywhere is -1.23 dB, centring's is
//            -3.78 dB."  HALF FALSE, AND THE FALSE HALF WAS OURS. Over 40 harmonic
//            (family, range) rows at every semitone:
//              SHIPPED     worst  -5.07 dB  (THE DEFAULT PATCH, [0.0,1.0] / [0.0,2.0]
//                                            / [0.0,4.0] - all three floor ranges)
//              CENTRE-AMP  worst -15.27 dB  (THE DEFAULT PATCH, [0.0,4.0])
//              CENTRE-ALL  worst -15.27 dB
//            The ORDERING survives and widens. The shipped rule's own number does not:
//            it is four times worse than published, it is on the DEFAULT PATCH rather
//            than on an unusual one, and the case that produces it was outside the old
//            sweep in two independent ways at once. -1.23 dB was never the shipped
//            rule's worst case; it was the worst case of what we happened to look at.
//
//         2. "Centring breaks the guarantee on families where the shipped rule never
//            breaks it."  TRUE WHERE IT WAS MEASURED AND FALSE MORE OFTEN AT THE
//            FLOOR, so as a general argument it does not survive. The three rows the
//            previous revision cited still read as published. The floor rows read the
//            other way (share of (note, slope) cases worse than the hash, SHIPPED ->
//            CENTRE-AMP, with each rule's worst):
//              default,    [0.0,1.0]   8.0% -> 5.7%   -5.07 dB -> -1.26 dB
//              default,    [0.0,0.5]   2.5% -> 0.9%   -3.24 dB -> -1.30 dB
//              default,    [0.0,2.0]  34.8% ->13.9%   -5.07 dB -> -3.15 dB
//              odd P512,   [0.0,1.0]  40.2% ->25.5%   -4.94 dB -> -2.61 dB
//              odd P512,   [0.0,0.5]  19.1% -> 8.0%   -3.84 dB -> -1.43 dB
//              fractal L3, [0.0,1.0]   6.7% -> 1.6%   -2.88 dB -> -0.23 dB
//              fractal L3, [0.0,2.0]  24.4% -> 1.9%   -3.21 dB -> -0.35 dB
//              fractal L4, [0.0,1.0]   5.0% -> 1.1%   -1.41 dB -> -0.69 dB
//            Counted over all 40 harmonic rows the shipped rule has the better worst
//            case on 19, centring on 14, and 7 tie. It leads; it does not dominate.
//            Anyone who reads argument 2 as "centring breaks things and the low end
//            does not" is reading a sample, not a result.
//
//         3. "The shipped mean is already positive nearly everywhere and the negative
//            rows are all `odd maxP512`."  SURVIVES. 34 of the 40 harmonic rows have a
//            positive shipped mean and all 6 negative rows are still `odd maxP512`
//            (-0.008 / -0.088 / -0.200 / -0.239 / -0.257 / -0.468 dB). The floor makes
//            that family worse, not different.
//
//            THE SUB-CLAIM ATTACHED TO IT IS FALSE, and it was false in the previous
//            revision's OWN published data, not only at the floor. It said centring
//            rescues `odd` only by switching the feature off - "meanImproved is exactly
//            0.000 because there are no improved notes at all". On odd maxP512 at
//            [1.0,1.5] the centred rule has meanImproved +0.522 dB with 0.0% of cases
//            worse: it claims, and it is right. Same at [0.0,1.0] (+0.251, 25.5% worse)
//            and at [0.0,0.5] (+1.164, 8.0% worse). Two of the six odd rows do read
//            +0.000; four do not. The sentence generalised from two rows.
//
//         4. "The damage is not a function of |dS|, and the sign of the asymmetry is
//            not the same on every family."  SURVIVES, at every compile anchor. The
//            previous revision took this table at ONE compile slope, 1.0, which is
//            itself the coverage gap in miniature: the shape of the curve belongs to
//            the template the dS is measured FROM. Re-taken at three anchors (mean dB
//            vs hash over the notes the plan claims improved; a voice slope below 0.0
//            is unreachable and is left blank):
//
//              compiled at 0.0    -0.50  -0.25  +0.00  +0.25  +0.50  +1.00  +2.00
//                default              .      .  +1.53  +1.27  +0.97  +0.14  -0.48
//                odd P512             .      .  +2.02  +0.74  -0.69  -2.21  -0.89
//                fractal L3           .      .  +1.35  +1.18  +0.88  -0.11  -0.21
//                fractal L4           .      .  +1.71  +1.62  +1.37  +0.59  +1.20
//                prime-lim 7          .      .  +2.43  +2.14  +1.43  +0.44  -0.42
//
//              compiled at 0.5    -0.50  -0.25  +0.00  +0.25  +0.50  +1.00  +2.00
//                default          +0.55  +0.98  +1.95  +1.73  +1.32  +0.75  -0.22
//                odd P512         +0.29  +0.62  +1.39  +0.26  -0.85  -0.93  -0.44
//                fractal L3       +0.97  +1.27  +1.69  +1.80  +1.73  +1.80  +1.33
//                fractal L4       +1.19  +1.41  +1.59  +1.32  +1.09  +0.61  +1.96
//                prime-lim 7      +1.41  +1.83  +2.34  +2.19  +1.95  +1.15  -0.14
//
//              compiled at 1.0    -0.50  -0.25  +0.00  +0.25  +0.50  +1.00  +2.00
//                default          +0.74  +1.39  +1.96  +1.69  +1.29  +0.52  -0.09
//                odd P512         -1.02  +0.35  +0.69  -0.14  -0.42  -0.44  -0.22
//                fractal L3       +1.06  +1.37  +1.62  +1.27  +1.06  +0.46  -0.44
//                fractal L4       +1.27  +1.36  +1.67  +1.31  +1.25  +1.82  +1.69
//                prime-lim 7      +0.79  +1.46  +2.26  +1.90  +1.35  +0.59  -0.09
//
//            At anchor 0.5, three families lose more from brightening and two gain from
//            it - so buildPlan still cannot know which side a template sits on, which
//            is the argument. But note what the anchor-0.0 row also shows, because it
//            is the mechanism behind argument 2 failing: at the FLOOR the shipped rule
//            has to survive the full positive width of the span (dS 0..+1 on [0.0,1.0])
//            while centring survives only half of it in each direction. On odd P512
//            that is +2.02 -> -2.21 across the shipped range against +1.39 -> -0.85
//            across the centred one, and the sign advantage does not pay for the
//            doubled magnitude. THE ASYMMETRY IS REAL AND IS NOT SUFFICIENT.
//
//         5. "Recompiling at the centre deletes partials the bright half needs."
//            TRUE, AND SILENT ON EVERY FLOOR RANGE. The count only starts falling
//            above about slope 2.0:
//
//              compile slope   0.0   0.25   0.5   1.0   1.5   2.0   2.5   3.0   4.0
//              default n=1024 1024   1024  1024  1024  1024  1000   251   100    31
//              odd maxP512     512    512   512   512   512   500   126    50    16
//              fractal L3      384    384   384   384   384   384   384   235    53
//              fractal L4     1024   1024  1024  1024  1024  1024   761   305    62
//              prime-limit 7   143    143   143   143   143   141    75    46    22
//
//            So for a patch reaching [2.0, 3.0] the shipped compile keeps 1000 partials
//            and a centred one keeps 251, and PluginProcessor.cpp:560-562 exists to
//            stop exactly that. But every floor-touching range centres at 0.25, 0.5,
//            1.0 or 2.0, where the count is unchanged - which is why CENTRE-AMP and
//            CENTRE-ALL are numerically IDENTICAL on all of them. This argument does
//            not apply where argument 2 fails, so it cannot be used to cover for it.
//
//         SO WHY DOES THE RULE STILL SHIP? Not because centring loses on the average -
//         it does not - and not because it always breaks the guarantee where the low
//         end does not, which is the reading the previous revision invited. It ships
//         for one reason that the extended sweep makes stronger rather than weaker:
//         THE TAIL, OVER THE WHOLE REACHABLE PARAMETER. Compare the two rules on the
//         default patch at [0.0, 4.0], which is the widest thing a user can actually
//         route:
//              SHIPPED     mean +0.05 dB   worst  -5.07 dB   49.8% of cases worse
//              CENTRE-AMP  mean -0.99 dB   worst -15.27 dB   61.6% of cases worse
//         Centring is worse there on all three columns at once, and 15 dB is not a
//         degradation, it is a different sound. The shipped rule's own worst case is
//         -5.07 dB and that is not good; it is simply three times shallower, and this
//         file's contract is about the bad case, not the average.
//
//         BE HONEST ABOUT WHAT IS BEING TRADED. Over a NARROW range at the floor -
//         [0.0, 1.0], one ordinary modulator - centring is better on the default patch
//         on every column (mean +0.98 vs +0.62, worst -1.26 vs -5.07, 5.7% vs 8.0%).
//         The shipped rule is chosen anyway, because buildPlan is handed one compile
//         point and cannot know which range the patch will end up reaching, and the
//         rule that is merely behind on narrow ranges is preferable to the rule that
//         loses 15 dB on wide ones. The fully honest statement of the trade is: with
//         Slope modulated, NEITHER compile point restores the guarantee - Failure 5(b)
//         already established that no cached plan can - and the low end is chosen
//         because its failures are bounded where centring's are not.
//
//         ON APERIODIC TEMPLATES THE QUESTION DOES NOT ARISE. On 19-EDO x6, 31-EDO x4
//         and 53-EDO x3 the plan returns the hash at every note, so all three rules
//         score exactly +0.000 dB at every slope in every reachable range measured -
//         floor ranges included. There is nothing to trade there in either direction.
//
//         testSlopeCompilePosition pins this at BOTH a mid range and a floor range, so
//         neither the conclusion nor the coverage can silently narrow again. With Slope
//         unmodulated - the default - dS is 0, the reachable range is a point, and none
//         of this applies.
//
//==============================================================================
// WHAT VERSION 2 GOT RIGHT AND IS KEPT WITHOUT CHANGE:
//   - determinism: bit-identical across nine compiler configurations including the
//     plugin's own, 2/4/8/16 threads, 20 processes and a REVERSED traversal order;
//     the tie-sensitivity gap is designed out (the sort key is (ratio, amp), so two
//     partials that tie in both are interchangeable) rather than merely tested for;
//   - the work budget is a COUNT of visits, never a clock, so a busy machine takes
//     longer and returns THE SAME PHASES;
//   - the two-descriptor noise gate;
//   - the greedy construction, on the templates where its guarantee is exact.
//
//==============================================================================

#include "Spectrum.h"

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <limits>

namespace spectrum
{
namespace phaseset
{
    //==========================================================================
    // Constants the integrator needs.

    // Today's Aligned constant. sin(2*pi*0.25) = 1, so every partial starts at its
    // peak. scatter == 0 must reproduce this EXACTLY or every existing patch
    // changes; interpolatePhases and seedFromPlan both write it unconditionally
    // before they look at anything else, and testScatterZeroIsSacred asserts it on
    // every spectrum in the suite including the ones where no plan was built.
    inline constexpr double kAlignedPhase = 0.25;

    // TODAY'S SCATTERED PHASE AT ONE INDEX, index for index the same value
    // AdditiveVoice::seedPhase(j, aligned = false) writes:
    //     h = sin(j * 12.9898) * 43758.5453;  phase = h - floor(h)
    //
    // WHY THIS IS PUBLIC AND WHY THE PLAN IS NOT ENOUGH. plan.phase carries the hash
    // for [0, plan.n) only, and the note-on path is routinely asked for MORE indices
    // than that:
    //   AdditiveVoice.h:519 and :646  seedSpan = min(max(hi, activeCount), oscCap())
    //   AdditiveVoice.h:914           span     = max(n, activeCount)
    // where activeCount is still the PREVIOUS, larger template's slot count, and the
    // alignPhase re-seed at :603-606 walks j < seedSpan. Each Shepard copy is seeded
    // over a span oscCap() gives it - 1638 with Shepard on, 16384 with it off. Every
    // one of those routinely exceeds plan.n.
    //
    // Before this existed, a caller past plan.n had exactly two options and both were
    // wrong: write kAlignedPhase - every partial at its peak, which is the OPPOSITE of
    // Scattered and the loudest possible crest - or leave the oscillator bank holding
    // whatever the last note left in it, which is not deterministic. seedFromPlan now
    // uses this itself, so the boundary at plan.n is inaudible and the caller does not
    // have to know where it is; the accessor is exposed anyway because a caller that
    // seeds a Shepard copy by hand needs the same value for the same index.
    //
    // Deterministic, allocation-free, noexcept, and the ONLY copy of this formula in
    // the header (detail::closedHash calls it) - a second, subtly different copy of a
    // rule is how Failure 2 hid for a whole version.
    inline double hashPhaseAt (int index) noexcept
    {
        const double h = std::sin ((double) index * 12.9898) * 43758.5453;
        return h - std::floor (h);
    }

    // FADER TAPER, applied by the CALLER, never here: x = pow(u, kTaperExponent).
    //
    // MEASURED (testFader, printed on every run): 40 evenly spaced fader positions u
    // on all twelve reference spectra, x = pow(u, e), the REAL crest at each position
    // on the independent rig, and two questions asked of each exponent - how much of
    // the total dB span the fader has bought by its half-way point, and the worst
    // deviation of the dB curve from a straight ramp.
    //
    //   exponent   share of the dB span by u = 0.5   worst deviation from a straight ramp
    //     1.00           18.1% of 10.29 dB                      5.90 dB
    //     0.70           31.3%                                  5.58 dB
    //     0.50           46.2%                                  5.20 dB
    //     0.45           50.7%                                  5.07 dB   <- shipped
    //     0.35           61.1%                                  5.46 dB
    //
    // 0.45 is the minimum of the worst-deviation column, and the test asserts that
    // identity rather than the number - so an engine change that moves the curve
    // moves the constant with it instead of leaving a stale comment behind. At 1.00
    // the first HALF of the travel buys 18% of the span, which is the dead zone the
    // user has already complained about on the fractal fader.
    //
    // The taper is a UI decision, so it is a constant here and an instruction to the
    // caller, not something this header applies behind its back - a caller that
    // already has its own curve would otherwise be tapered twice.
    inline constexpr float kTaperExponent = 0.45f;

    // THE SENTINEL. A field that was not measured is exactly this, never 0.0. A
    // crest is >= 1 by definition, so -1.0 cannot be mistaken for one, and a caller
    // that divides by it gets an obviously wrong negative gain instead of the
    // SILENCE that v1's 0.0 produced. Callers must test `>= 0.0`.
    inline constexpr double kUnmeasured = -1.0;

    // PhaseReport::verifiedSpanPeriods when the waveform provably repeats inside the
    // window, so every note length is covered and no finite number is the truth.
    inline constexpr double kUnboundedSpan = std::numeric_limits<double>::infinity();

    // How much better than the hash a candidate must measure, ON EVERY SPAN, before
    // a prefix is allowed to use it. MEASURED against a brute-force std::sin rig on
    // true ratios (testTrueRatioAgreement, printed by --tables): over the twelve
    // reference spectra the engine's hash-to-candidate dB gap is optimistic by at
    // most 0.09 dB and pessimistic by at most 0.23 dB. 0.25 dB clears the optimistic
    // side with room, and where it does not, the cost of being wrong is that we keep
    // today's sound.
    inline constexpr double kPreferMarginDb = 0.25;

    //--------------------------------------------------------------------------
    // THE SPAN LADDER - the answer to Failure 4.
    //
    // A span is one analysis window of W fundamental periods, taken at an OFFSET of
    // o windows from the note's start: span o covers [o*W, (o+1)*W). The
    // implementation is a phase offset, not a longer grid: span o starts each
    // partial's fixed-point accumulator at inc_i * o * T, which is bit-exactly where
    // the accumulator would be after o*T samples. So a span three thousand windows
    // into a held note costs one pass and no extra memory, and the streamed arrays
    // never fall out of L2 (see kMaxGridPoints).
    //
    // A candidate must beat the hash at EVERY span in the ladder, on BOTH readings:
    //   - the crest of that span ALONE - what a listener hears during that stretch;
    //   - the crest of the UNION of every span up to it - peak = max of the spans'
    //     peaks, energy = their sum, which is the statistic of a note held that long.
    // Failing either one at any span means the hash is returned for that prefix.
    //
    // HOW LONG THE LADDER IS. Two classes, and which one a template is in is proven,
    // not guessed:
    //
    //   EXACTLY PERIODIC templates need ONE span, because span 1 is bit-identical to
    //   span 0 - the waveform has repeated. testSpanLadderExactness asserts that
    //   identity to the last bit on every integer-ratio spectrum in the suite, which
    //   is what makes "one span" a proof rather than an economy.
    //
    //   APERIODIC templates get kSpansAperiodic spans spread from the note's start
    //   out to kLadderReachPeriods fundamental periods: offsets 0 and 1 (the exact
    //   continuation), then geometrically out to the far end, so five orders of
    //   magnitude of note length are checked for the price of eight passes.
    //
    // WHY THE REACH IS IN PERIODS AND NOT IN WINDOWS, which is a mistake this file
    // made and a measurement caught. A first version of this ladder used eight
    // CONSECUTIVE windows, i.e. a reach of 8*W. On 53-EDO x3 (W = 76) that is 608
    // periods and it fixed the case that killed v2. On 17-of-2.4 x4 (W = 5) it is
    // FORTY periods - 11 ms at MIDI 105 - and the skeptics' rig found the returned
    // set 0.66 dB WORSE than the hash on a 4.00 s note at that pitch. Same disease,
    // one layer down: the reach was scoped to a window we chose. Measured on the
    // skeptics' independent rig, notes worse than the hash over MIDI 21-108 x twelve
    // reference spectra x held-note lengths 0.05/0.25/1.00/4.00 s:
    //
    //   ladder                          worse @0.05s  @0.25s  @1.00s  @4.00s
    //   8 consecutive windows (8*W)          0          0       0       1
    //   8 spans out to 16384 periods         0          0       0       0
    //
    // 16384 periods is 3.9 s at the top of the keyboard (MIDI 108, 4186 Hz) and
    // 596 s at the bottom (MIDI 21, 27.5 Hz), so the shortest promise the ladder
    // makes anywhere on the keyboard is a four-second note. Past that we claim
    // nothing, and PhaseReport::verifiedSpanPeriods is the number to paint.
    //
    //==========================================================================
    // THE BOUNDARY MEASUREMENT THE APERIODIC RULE IS JUSTIFIED BY, because "gate the
    // optimiser where its guarantee is defensible" is only an instruction until
    // somebody measures where that is.
    //
    // THE SWEEP: held-note lengths from a 20 ms pluck to an 8 s pad, 30 notes
    // (MIDI 24-111), on the skeptics' independent rig - a real 48 kHz render, direct
    // std::sin on the true float ratios, nothing from this engine's measurement path
    // - with the construction FORCED ON everywhere (Options::optimiseAperiodic) so
    // the question can actually be asked. Each cell is notes WORSE than today's hash
    // by more than 0.02 dB, out of the notes the plan claims improved.
    //
    //  ONE WINDOW (Options::spansOverride = 1 - this IS version 2):
    //   spectrum        W    0.02s 0.05s 0.10s 0.25s 0.50s 1.00s 2.00s 4.00s 8.00s
    //   HARM default    1      0     0     0     0     0     0     0     0     0
    //   HARM fractalL3  1      0     0     0     0     0     0     0     0     0
    //   HARM FLAT512    1      1     0     0     0     0     0     0     0     0
    //   SCL 19EDO x6   27      0     1     5    11    14    12    12    10     9
    //   SCL 31EDO x4   45      2     7    11    14    14    14    17    15    11
    //   SCL 53EDO x3   76     12     7     7     5     4     3     6     7     9
    //   SCL 22EDO x5   32     13     9     5     3     4     5     9     5     9
    //   SCL 17-of-2.4  19     11    10     6     5     9    12    13    16    18
    //
    //  THE EIGHT-SPAN LADDER (what ships):
    //   every harmonic row      0 at every length except FLAT512 at 0.02s (see below)
    //   every Scala row         0 at every length, and 0 prefixes claimed improved
    //
    // READ THE TWO HALVES SEPARATELY, because they are two different answers.
    //
    //   ON A HARMONIC TEMPLATE THE BOUNDARY DOES NOT EXIST. The window is ONE
    //   fundamental period and the waveform provably repeats in it, so a 20 ms note
    //   and an 8 s note are the same measurement. The search keeps its whole win
    //   there: +1.5 to +1.9 dB mean, zero worse, at every length tested. There is no
    //   "safe region" to find - the guarantee is a proof, not a range.
    //
    //   ON AN APERIODIC TEMPLATE THERE IS NO SAFE REGION AT ALL. Not a small one - a
    //   none. Read the one-window rows again: the regression is not confined to long
    //   notes, it is worst at 0.02 s on three of the five Scala families and worst at
    //   0.50 s on another. There is no note length at which the one-window claim
    //   holds, so there is no boundary to cut at, and "use the optimised set where
    //   the window covers the musical case" cannot be implemented - the window never
    //   covers it. The only honest rule is the one that ships: on an aperiodic
    //   template the search does not run, the closed forms are still measured and are
    //   still allowed to win, and when none of them beats the hash at every span the
    //   hash is returned bit for bit.
    //
    // What the ladder buys on a Scala timbre is therefore NOT improvement - it is
    // zero improvement, correctly labelled, in place of version 2's 106-153 prefixes
    // of improvement that were the window rather than the phases. That is the whole
    // trade, and it is worth making: a fader whose top end is today's sound is a
    // fader that works; a fader whose top end is 1.3 dB worse than today's sound on a
    // held Scala note is the thing that got refuted three times.
    inline constexpr int kSpansAperiodic = 8;

    // How far out the aperiodic ladder's last span sits, in fundamental periods. See
    // the table above for why this is in periods rather than in windows.
    inline constexpr int kLadderReachPeriods = 16384;

    // An aperiodic template whose budget will not stretch to this many spans gets
    // the hash everywhere, because a shorter ladder is exactly the claim that was
    // refuted. Cutting the REACH to afford the ladder is fine and is reported;
    // cutting the LADDER to afford the reach is how Failure 4 happened.
    inline constexpr int kMinSpansAperiodic = 8;

    // Two ratios closer than this to the same integer multiple of 1/W are treated as
    // the same, i.e. the window is the true period. Compiled harmonic templates carry
    // exact integer ratios (float represents every integer below 2^24 exactly, and
    // kMaxPartials is 16384), so on those the residual is exactly 0.0. A Scala degree
    // is 2^(k/edo) and its residual is order 1e-1. Nothing measured lands between
    // 0 and 1e-9, so this threshold is not a tuning knob, it is a type test.
    inline constexpr double kPeriodicityTolerance = 1.0e-9;

    // THE SWEEP GATE. |Pearson correlation between block index and block RMS
    // frequency| over 16 blocks of one fundamental period. MEASURED, engine reading
    // first and the independent rig's in brackets (testSweepDetector --tables):
    //   Schroeder gamma=1, FLAT n=512          0.971  (0.854)  the rasp v1 shipped
    //   Schroeder gamma=1, fractal L3          0.877  (0.844)
    //   Schroeder gamma=1, default maxP128     0.846  (0.856)
    //   Schroeder gamma=1, harmonic slope 1    0.418  (0.186)  does NOT sweep
    //   what this engine returns, all four     0.30 - 0.46 (0.009 - 0.35)
    //   shipping hash, all four                    -   (0.004 - 0.489)
    //   random draw,   all four                    -   (0.060 - 0.479)
    // 0.60 sits in the gap. It is deliberately not tighter: a random draw is
    // noise-like by construction, and rejecting one because it happened to read 0.55
    // would cost quality for nothing. The harmonic row is why the derivative gate is
    // still here as well - see Failure 3.
    inline constexpr double kSweepCorrLimit = 0.60;

    // The construction's phase ladder: kGreedyCoarse evenly spaced phases, then
    // kGreedyRefine steps either side of the winner at 1/(coarse*(refine+1)).
    // MEASURED (brute-force-verified gain over the hash, seven reference spectra):
    //   16 coarse, no refine   +1.09 .. +4.55 dB   9.8 grid passes
    //    8 coarse, 2 refine    +1.01 .. +4.02 dB   7.7 grid passes
    //    8 coarse, no refine   +1.02 .. +4.88 dB   5.6 grid passes
    //    6 coarse, 2 refine    unstable: -0.63 dB on harmonic slope 1 N=128
    // Below 8 coarse steps the sweep can miss the feasible basin entirely, which is
    // why 6+2 goes backwards. 8+2 is the knee.
    inline constexpr int kGreedyCoarse = 8;
    inline constexpr int kGreedyRefine = 2;

    // The construction holds the derivative gate at this fraction of the ceiling the
    // verification will apply, since the denser verify grid reads a peak a few
    // percent higher than the grid the construction searched.
    //
    // MEASURED (improved prefixes and the brute-force-verified gain at the full
    // prefix, over seven reference spectra):
    //   headroom  refused by deriv   refused by SWEEP   gain range
    //     0.60      36 - 51            0                 +0.00 .. +1.80 dB
    //     0.70       0 - 36            0                 +0.00 .. +1.67 dB
    //     0.75       0 - 12            306 (!)           +0.00 .. +3.34 dB
    //     0.85       0 - 49            0                 +0.00 .. +2.75 dB
    //     0.95       1 - 57            0                 +1.26 .. +3.50 dB
    // 0.95 is the only setting with no zero row. The 0.75 line is the interesting
    // one and it is why BOTH gates exist: squeezing the derivative harder does not
    // make the result more noise-like, it pushes the construction toward a CHIRP -
    // constant envelope and constant |dx/dt| envelope is exactly what a sweep is -
    // and on a harmonic slope-1 set at that setting the sweep detector refused 306
    // of 366 prefixes. A pulse detector alone would have shipped every one of them.
    inline constexpr double kDerivConstructionHeadroom = 0.95;

    // A HARD CEILING ON THE ANALYSIS GRID, in samples. Two streamed double arrays of
    // this length are 256 kB, which stays in a typical L2; past that a work unit
    // silently costs three to five times as much and the work budget stops bounding
    // wall time at all. MEASURED: a wide, finely spaced blended template asked for
    // T = 59000, and a call the counter had bounded at 16 ms took 115 ms.
    // When T would exceed this the ANALYSIS WINDOW is shortened, which is reported
    // (PhaseReport::windowCapped), never the partial list.
    //
    // NOTE THAT THE SPAN LADDER DOES NOT PUSH AGAINST THIS. A longer observation is
    // more spans, not a bigger T, so the arrays stay the same size however long the
    // note we are covering. That is the second reason the ladder is affordable.
    inline constexpr int kMaxGridPoints = 16384;

    //==========================================================================
    // THE WORK BUDGET. A wall-clock cutoff would make the OUTPUT depend on machine
    // load, which breaks determinism outright: the same preset would compile to
    // different phases on a busy machine and the morph would jump. So the budget is
    // a COUNT of (partial, grid-sample) visits - the inner loop of every synthesis
    // and of every candidate the construction tries - and the engine shrinks its
    // ANALYSIS REACH, deterministically, until the fixed work of the whole call fits
    // inside it. workUsed <= workBudget holds as an exact inequality, because the
    // reach is chosen from the WORST CASE (every candidate phase scanned to the end
    // of the grid); the early abort then makes the real cost lower, never higher.
    //
    // CALIBRATION. One visit is two table lookups, two multiply-adds, two compares
    // and two streamed double arrays. Measured here (single-threaded, /O2 x64, MSVC
    // 14.51, twelve real compiled templates, minimum of nine runs):
    //
    //   machine state                                        ns per work unit
    //   fresh process, idle core, burst clock                 1.25 - 1.58
    //   same process, after this suite's own brute-force rig   2.2 - 2.9
    //   after 10 s of saturated scalar FP                      4.5 - 6.7
    //   after 40 s of saturated scalar FP                      3.5 - 4.9
    //   box fully saturated by a second build                   24 - 56
    //
    // The SAME buildPlan call on the SAME binary read 7.13 ms when the cost test ran
    // first, 18.84 ms after the suite's rig, and 189.71 ms with another job
    // saturating the machine - a 28x spread with the work done identical to the
    // unit. So the rate below is quoted at the SLOW END of the states this engine is
    // meant to run in, near 5 ns, and every millisecond figure in this header is a
    // WORST-CASE that an idle core beats by about three times.
    //
    // A CONSEQUENCE WORTH STATING PLAINLY: no wall-clock number here is a promise,
    // and PhaseSetTests asserts none of them. What it asserts is workUsed <=
    // workBudget exactly, and that budget x THIS rate fits the frame the budget is
    // named for. That arithmetic is what makes "8 ms" mean 8 ms.
    inline constexpr long long kWorkUnitsPerMillisecond = 200000LL;
    inline constexpr long long kWorkBudget2ms  = 2LL  * kWorkUnitsPerMillisecond;
    inline constexpr long long kWorkBudget4ms  = 4LL  * kWorkUnitsPerMillisecond;
    inline constexpr long long kWorkBudget8ms  = 8LL  * kWorkUnitsPerMillisecond;
    inline constexpr long long kWorkBudget16ms = 16LL * kWorkUnitsPerMillisecond;
    inline constexpr long long kWorkBudget32ms = 32LL * kWorkUnitsPerMillisecond;

    //--------------------------------------------------------------------------
    // TWO BUDGETS, BECAUSE ONE NUMBER CANNOT SERVE BOTH CALLERS.
    //
    //   kWorkBudgetDrag    - a LIVE PLAYGROUND DRAG, which posts a freshly blended
    //     template every 16.7 ms. The frame also owes blendTemplates 0.02-2.24 ms,
    //     so the phase build gets at most ~14 ms of it. 8.0 ms at the quoted rate.
    //
    //   kWorkBudgetCompile - a SETTLED TEMPLATE: the user let go of the control, or
    //     changed a setting, and one compile is due. 48.0 ms at the quoted rate,
    //     under half of the 100 ms realtime-adaptation budget. It does NOT fit a
    //     60 Hz frame, which is why it is not what a drag uses.
    //
    // WHY THE COMPILE BUDGET IS BIGGER THAN VERSION 2's 28 ms: the span ladder costs
    // kSpansAperiodic verification passes instead of one on aperiodic templates, and
    // a budget that cannot afford the ladder returns the hash (kMinSpansAperiodic).
    // The trade is stated where it is paid: on an aperiodic template the budget buys
    // HONESTY - how long a note the answer is true for - and only then reach.
    // MEASURED wall clock over the twelve reference templates, inside the full test
    // suite (the slow machine state above): 2.4 - 14.6 ms at the drag budget,
    // 6.1 - 41.9 ms at the compile budget.
    inline constexpr long long kWorkBudgetDrag    = kWorkBudget8ms;                  // 1,600,000
    inline constexpr long long kWorkBudgetCompile = 48LL * kWorkUnitsPerMillisecond; // 9,600,000

    // The default is the COMPILE budget, because that is where buildPlan lives. A
    // caller that has put it on a 60 Hz path must say so by passing the drag budget.
    inline constexpr long long kWorkBudgetDefault = kWorkBudgetCompile;

    //==========================================================================
    struct Options
    {
        // The rate the voice renders at. With lowestNoteHz it fixes the highest
        // partial ratio that can ever sound, which is what the analysis has to
        // reach: ratioCap = 0.5 * sampleRate / lowestNoteHz.
        double sampleRate  = 48000.0;

        // The lowest fundamental the analysis is ASKED to cover. Partials above
        // 0.5*sampleRate/lowestNoteHz cannot sound at or above this pitch, so they
        // keep the hash phase and are excluded - and that is REPORTED (analysed<n,
        // lowestFullyCoveredHz), never silent. The budget may shrink it further,
        // which is also reported. C2 by default.
        double lowestNoteHz = 65.406;

        // Hard, deterministic work bound. The engine shrinks its reach to fit; it
        // never returns without measuring.
        long long workBudget = kWorkBudgetDefault;

        // CONSTRUCTION grid: samples per cycle of the top analysed partial.
        // MEASURED: at 2 the construction overfits its own grid badly on shallow
        // spectra - the greedy set reads 1.9996 on flat N=128 and renders at 2.6537
        // (33% out). At 4 the same set reads 2.2680 and renders at 2.4847 (8.7% out)
        // and beats the hash by 1.10 dB brute-force-verified. 4 is the first honest
        // setting; 8 buys 0.1-0.4 dB more for double the cost.
        int oversample = 4;

        // VERIFY grid: samples per cycle of the top analysed partial, used for the
        // measurement that actually decides the plan. Forced to an integer multiple
        // of `oversample`, so its sample points are a strict SUPERSET of the ones
        // the construction searched - that is what makes the verification
        // non-circular rather than a re-run of the search's own approximation.
        int verifyOversample = 8;

        // Fixed random-phase draws on the construction grid. They are the noise
        // reference for the derivative gate AND weak candidates in their own right.
        int randomDraws = 2;

        // What share of the work budget the VERIFICATION may claim, the rest going to
        // the construction. On an aperiodic template the verification is the whole
        // job, so this share is raised internally to 1.0 there - there is nothing to
        // construct that could be verified over the ladder (see kSpansAperiodic).
        double verifyShare = 0.65;

        // How far below the verification's derivative ceiling the construction aims.
        double derivHeadroom = kDerivConstructionHeadroom;

        // Measure Schroeder gamma 1 and 0.5 as candidates. THIS IS NOT A REPORTING
        // KNOB. On a flat spectrum Schroeder is worth up to 5.7 dB over the hash at
        // some prefixes while being 9.3 dB WORSE at others, so it only pays as a
        // PER-PREFIX candidate - which is what it is here. Turning it off costs the
        // whole closed-form half of the answer.
        bool measureSchroeder = true;

        // Run the greedy construction at all. SHIPS ON, and this knob is how the
        // question "how much of this is the CHOOSER and how much is the SEARCH" gets
        // an answer instead of an opinion.
        //
        // THE ANSWER, AND IT OVERRULES THE ARCHITECTURE THIS FILE WAS ASKED TO
        // EVALUATE. The proposal was that three closed forms chosen per spectrum
        // would be "most of the available win". MEASURED (testChooserAlone, and the
        // skeptics' own contrib rig agrees), improved PREFIXES with the construction
        // off and on, and the notes of MIDI 21-108 that get a better-than-hash set on
        // a real 48 kHz render:
        //
        //   spectrum              chooser alone        shipped (chooser + search)
        //   odd maxP512            99 pfx,  5/30 notes   172 pfx, 16/30 notes
        //   FLAT n=512             16 pfx,  0/30         337 pfx, 22/30
        //   default maxP128 s0.3   12 pfx,  3/30         123 pfx, 29/30
        //   fractal L4             13 pfx,  2/30         334 pfx, 22/30
        //   default n=1024          0 pfx,  0/30         328 pfx, 25/30
        //   harmonic s1 n=128       0 pfx,  0/30         124 pfx
        //   prime-limit 7           0 pfx,  0/30          75 pfx
        //   every Scala family      0 pfx,  0/30           0 pfx
        //
        // So the closed forms are a REAL floor - they win on five of ten harmonic
        // spectra, by up to 2.35 dB, and they cost three passes - but they are not
        // most of the win. The search is. 140 improved prefixes against 1294 over
        // the six spectra the test sweeps. The chooser is what cannot be wrong; the
        // search is what is worth having. Both are gated by the same ladder, and 15
        // of the shipped prefixes on FLAT n=512 are still won by a Schroeder form
        // that the search did not beat, which is what pays for keeping all three.
        bool useConstruction = true;

        // Run the greedy construction on an APERIODIC template as well. SHIPS OFF,
        // and the table at kSpansAperiodic is why: over the span ladder its sets do
        // not survive on any Scala family in the suite, and on an aperiodic template
        // the ladder is what the budget has to buy. It is an Option so the trade can
        // be re-measured without editing the header (testAperiodicOptimiser prints
        // the comparison), not because it is a close call.
        bool optimiseAperiodic = false;

        // MEASUREMENT KNOB, NOT A PRODUCTION SETTING. 0 means "as the periodicity
        // class says". Any other value forces that many spans, which is how the
        // boundary table at kSpansAperiodic was produced: setting it to 1 reproduces
        // version 2's one-window claim exactly, so the regression it shipped can be
        // measured rather than described. A test pins the default at 0.
        int spansOverride = 0;

        // The analysis window, in fundamental periods, is 1/(closest partial
        // spacing) so the slowest beat in the set completes one cycle inside it -
        // capped here, because a set with a near-coincident pair would otherwise ask
        // for thousands of periods.
        //
        // 128 is chosen so the EDO timbres this instrument is built for are never
        // capped by THIS limit: 19-EDO asks for 27 periods, 22-EDO for 32, 31-EDO
        // for 45, 53-EDO for 77. A capped window is a window that cannot see the
        // beat. MEASURED, 31-EDO over 4 periods with the cap at 32 instead of its
        // required 45: brute-force-verified over four times the analysis window, 28
        // of 30 notes came out WORSE than the hash, mean -2.15 dB.
        int maxWindowPeriods = 128;

        // OPTIONAL per-note decorrelation, OFF by default because switching it on
        // changes every existing Scattered patch. The salt is a pure TIME SHIFT of
        // the whole partial set (phase += s * ratio), so a single voice's waveform is
        // only delayed and its crest is mathematically unchanged; what it changes is
        // what happens when several voices ADD. Derived from the NOTE NUMBER, never
        // the voice slot, so "the same note twice sounds identical" still holds. It
        // is applied to EVERY set in the plan, so crossing a prefix boundary cannot
        // introduce a discontinuity the un-salted plan did not have.
        bool perNoteSalt = false;
        int  noteNumber  = 60;
    };

    //==========================================================================
    // What actually happened. `outcome` is the first thing a UI should read.
    enum class Outcome
    {
        Empty = 0,   // n <= 0. Nothing was written; every measurement is kUnmeasured.
        Aligned,     // scatter == 0: every phase is kAlignedPhase, bit-identical to
                     // today's Aligned mode. crestChosen is the EXACT closed-form
                     // aligned crest - not a fallback, and not a zero.
        Hash,        // measured, and nothing beat the shipping hash on every span.
                     // What you get is today's Scattered mode, and we can prove it
                     // was the right call rather than assuming it.
        Chosen       // measured, and at least one prefix uses something better.
    };

    // Which family a prefix's phases came from. Reported per plan set so a UI can
    // say WHAT it is playing, and so the tests can assert the chooser picked what
    // the measurement says it should have.
    enum class Family
    {
        Hash = 0,        // set 0, always present, always the floor
        Schroeder1,      // closed form, gamma = 1
        SchroederHalf,   // closed form, gamma = 0.5
        Constructed      // the greedy search (exactly-periodic templates only)
    };

    struct PhaseReport
    {
        Outcome outcome = Outcome::Empty;
        bool measured   = false;      // false -> every crest field is kUnmeasured

        int  n          = 0;          // partials handed in
        int  analysed   = 0;          // prefixes 1..analysed got a measured decision.
                                      // Past it the plan hands back the hash:
                                      // VISIBLE, never silent.
        int  built      = 0;          // partials the CONSTRUCTION shaped (<= analysed);
                                      // 0 on an aperiodic template by design.
        double ratioCap = kUnmeasured;// highest partial ratio the analysis reached
        double lowestFullyCoveredHz = kUnmeasured; // below this pitch the rendered
                                      // prefix runs past `analysed` and the plan
                                      // hands back the hash. PAINT THIS.
        bool   reachReduced = false;  // the budget forced a shorter reach than
                                      // Options::lowestNoteHz asked for

        //-- THE ANSWER TO FAILURE 4 -------------------------------------------
        bool   exactlyPeriodic = false;  // every analysed ratio times the window is an
                                         // integer: the waveform REPEATS and one span
                                         // covers a note of any length
        int    spans = 0;                // observation lengths the decision was taken at
        double verifiedSpanPeriods = kUnmeasured; // spans * window, in fundamental
                                         // periods; kUnboundedSpan when exactlyPeriodic.
                                         // PAINT THIS as a time: value / f0 seconds.
        bool   ladderUnaffordable = false; // an aperiodic template whose budget would
                                         // not stretch to kMinSpansAperiodic spans, so
                                         // the hash was returned rather than a claim
                                         // that could not be checked

        int    gridPoints       = 0;  // construction grid T
        int    verifyGridPoints = 0;  // verify grid T
        double gridWindow       = kUnmeasured; // construction window, fundamental periods
        double verifyWindow     = kUnmeasured; // ONE span, in fundamental periods
        bool   windowCapped     = false; // the spacing asked for a longer window than
                                         // the grid ceiling allowed
        int    windowBlindPeriods = 0;   // the window the partial spacing demands with
                                         // nothing capping it. Kept from v2 so an
                                         // existing UI still reads; on an aperiodic
                                         // template verifiedSpanPeriods is the number
                                         // that now matters.

        // Crests at the FULL analysed prefix, on the verify grid, over span 0 unless
        // marked. All on true ratios.
        double crestChosen     = kUnmeasured;  // what phasesFor(analysed) gets
        double crestHash       = kUnmeasured;
        double crestAligned    = kUnmeasured;  // exact closed form, no synthesis
        double crestGreedy     = kUnmeasured;  // the construction, before the gates
        double crestBestRandom = kUnmeasured;  // construction grid
        double crestSchroeder1    = kUnmeasured;
        double crestSchroederHalf = kUnmeasured;

        // The same two, at the LAST span of the ladder - the number that refutes a
        // one-window claim. On an exactly-periodic template these equal the span-0
        // pair to the last bit.
        double crestChosenLastSpan = kUnmeasured;
        double crestHashLastSpan   = kUnmeasured;

        // Noise descriptors of the returned set and of what it was judged against.
        double sweepChosen     = kUnmeasured;  // |corr(block index, block RMS freq)|
        double sweepHash       = kUnmeasured;
        double sweepGreedy     = kUnmeasured;
        double sweepSchroeder1 = kUnmeasured;
        double sweepSchroederHalf = kUnmeasured;
        double sweepLimit      = kSweepCorrLimit;
        double derivChosen     = kUnmeasured;
        double derivHash       = kUnmeasured;
        double derivGreedy     = kUnmeasured;
        double derivSchroeder1 = kUnmeasured;
        double derivReference  = kUnmeasured;  // the gate: max(hash, median of draws)

        // WHY prefixes did not get a challenger, counted rather than flagged: a
        // boolean cannot tell an integrator whether one prefix or all of them were
        // refused, and the difference is the difference between a working feature
        // and a dead one.
        int    refusedBySweep  = 0;
        int    refusedByDeriv  = 0;
        int    refusedByMargin = 0;
        int    refusedByLadder = 0;   // won on span 0 and lost on a later one. THIS
                                      // COUNT IS FAILURE 4, measured per prefix: on
                                      // 53-EDO x3 it is 121 of 159.
        bool   challengerRefusedBySweep = false;
        bool   challengerRefusedByDeriv = false;

        // The plan. improvedPrefixes == 0 is the ONLY honest way to say "we measured
        // and improved nothing": there is no separate boolean that could claim a win
        // that did not happen.
        int    improvedPrefixes = 0;  // prefixes 1..analysed that use a non-hash set
        int    firstImproved    = 0;  // 0 = none
        int    lastImproved     = 0;
        int    planSets         = 1;  // phase arrays in the plan (1 = hash only)
        double bestGainDb       = 0.0;// best improvement over the hash, over prefixes,
                                      // at the ladder's WORST span
        double worstGainDb      = 0.0;// smallest improvement AMONG IMPROVED prefixes
                                      // (>= kPreferMarginDb by construction)
        int    prefixesSchroeder1    = 0;  // how many prefixes each family won
        int    prefixesSchroederHalf = 0;
        int    prefixesConstructed   = 0;

        long long workUsed      = 0;
        long long workProjected = 0;  // the worst case the reach was chosen from
        long long workBudget    = 0;
    };

    //==========================================================================
    // THE TWO COUNTS AT NOTE-ON, WHICH ARE NOT THE SAME COUNT.
    //
    // These exist because a single int called `prefixLen` was doing both jobs and the
    // guarantee silently came apart - see FAILURE 6 at seedFromPlan for the measurement.
    // They are deliberately NOT interconvertible and their constructors are explicit,
    // so a bare int compiles at neither position and swapping them does not compile.
    // That is the point: this is the one confusion in this file that no amount of
    // comment ever prevented.
    struct RenderedPrefix
    {
        // How many partials are BELOW NYQUIST at the pitch about to sound. Chooses
        // WHICH PHASE SET IS PLAYED, and nothing else. Get it from renderedPrefix().
        int value = 0;
        explicit constexpr RenderedPrefix (int v) noexcept : value (v) {}
    };

    struct FillSpan
    {
        // How many oscillator SLOTS to write - the voice's seedSpan / oscCap(). Decides
        // only how far the writing goes, and NEVER which set is played.
        int value = 0;
        explicit constexpr FillSpan (int v) noexcept : value (v) {}
    };

    //==========================================================================
    // THE PLAN. One phase array per distinct set that some prefix wants, plus a
    // per-prefix index into them. `pick` is indexed by (prefixLength - 1).
    //
    // Set 0 is ALWAYS the shipping hash, so a prefix with nothing better costs one
    // byte to express and sounds exactly like today.
    //
    // phasesFor() is the note-on entry point: O(1), allocation-free, noexcept.
    struct PhasePlan
    {
        int n        = 0;
        int analysed = 0;
        int numSets  = 0;

        // numSets * n doubles; set s starts at phase[s * n]. In the CALLER'S partial
        // order, so a voice can copy straight into its oscillator bank.
        std::vector<double>  phase;
        // pick[p - 1] = which set prefix length p must use. Sized n, so a prefix past
        // `analysed` is covered too (it holds 0 = the hash).
        std::vector<uint8_t> pick;
        // What each set is. family[0] is always Family::Hash.
        std::vector<Family>  family;

        bool valid() const noexcept { return n > 0 && numSets > 0; }

        // The phases to seed a note whose RENDERED partial count - partials below
        // Nyquist at the played pitch - is renderedPrefix. Never null when valid().
        //
        // THE CLAMP IS A BOUNDS CHECK, NOT AN ANSWER, AND FEEDING IT A SPAN IS
        // FAILURE 6. It clamps to n so a Shepard copy asking past the template cannot
        // read out of range. That is all it is for. Hand it a BANK SPAN - 1638, 16384,
        // a stale activeCount - and the clamp quietly turns every pitch into
        // pick[n - 1], which is precisely how the pitch-dependent choice of set stopped
        // reaching any voice; the measurement is at seedFromPlan. Prefer the
        // RenderedPrefix overload below, which a span cannot be passed to at all.
        //
        // And do not seed from this pointer directly in any case: it is an array of n,
        // so a caller who asked for more than n and copies n of them has silently
        // seeded the wrong count. Call seedFromPlan, which is total in the fill span
        // and continues the shipping hash past n, or read hashPhaseAt(index) yourself.
        const double* phasesFor (int renderedPrefixLen) const noexcept
        {
            if (n <= 0 || numSets <= 0) return nullptr;
            const int p = renderedPrefixLen < 1 ? 1 : (renderedPrefixLen > n ? n : renderedPrefixLen);
            const int s = pick.empty() ? 0 : (int) pick[(size_t) (p - 1)];
            return phase.data() + (size_t) s * (size_t) n;
        }

        // The spelling a fill span cannot be written into. Same function, typed.
        const double* phasesFor (RenderedPrefix rendered) const noexcept
        { return phasesFor (rendered.value); }

        // True when this prefix is getting something other than today's hash.
        bool improved (int renderedPrefixLen) const noexcept
        {
            if (n <= 0 || pick.empty()) return false;
            const int p = renderedPrefixLen < 1 ? 1 : (renderedPrefixLen > n ? n : renderedPrefixLen);
            return pick[(size_t) (p - 1)] != 0;
        }

        // Which family this prefix is playing. Hash when nothing beat it.
        Family familyFor (int renderedPrefixLen) const noexcept
        {
            if (n <= 0 || pick.empty() || family.empty()) return Family::Hash;
            const int p = renderedPrefixLen < 1 ? 1 : (renderedPrefixLen > n ? n : renderedPrefixLen);
            const size_t s = (size_t) pick[(size_t) (p - 1)];
            return s < family.size() ? family[s] : Family::Hash;
        }

        // THE TYPED SPELLINGS, for the same reason phasesFor has one. These two are
        // what the UI paints "improved" and the family badge from, and a span passed to
        // either of them lights the badge at every pitch on a plan that improves only
        // some - the visual twin of Failure 6. Prefer these at every call site that has
        // a pitch in hand.
        bool   improved  (RenderedPrefix r) const noexcept { return improved  (r.value); }
        Family familyFor (RenderedPrefix r) const noexcept { return familyFor (r.value); }
    };

    //==========================================================================
    // How many partials the voice renders at a given pitch: the same rule
    // AdditiveVoice uses (ratio <= Nyquist / f0, list sorted ascending). Exposed
    // because the tests, the UI and the note-on path must all agree on it - a
    // second, subtly different copy of this rule is exactly how Failure 2 hid.
    inline int renderedPrefix (const Partial* partials, int n,
                               double f0Hz, double sampleRate) noexcept
    {
        if (n <= 0 || f0Hz <= 0.0 || sampleRate <= 0.0) return 0;
        const double maxRatio = 0.5 * sampleRate / f0Hz;
        int p = 0;
        for (int i = 0; i < n; ++i)
        {
            if ((double) partials[i].ratio > maxRatio) break; // sorted ascending
            p = i + 1;
        }
        return p;
    }

    //==========================================================================
    // Internals. Everything below is deterministic integer/double arithmetic in a
    // fixed order; nothing consults a clock, an unordered container or a thread.
    namespace detail
    {
        inline constexpr double kTwoPi = 6.283185307179586476925286766559;

        inline uint64_t splitmix64 (uint64_t x) noexcept
        {
            x += 0x9E3779B97F4A7C15ull;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        inline double unitFrom (uint64_t h) noexcept
        {
            // 53 bits -> [0,1). Exact in double, so bit-identical everywhere.
            return (double) (h >> 11) * (1.0 / 9007199254740992.0);
        }
        inline double wrap01 (double p) noexcept { return p - std::floor (p); }

        //----------------------------------------------------------------------
        // THE SINE TABLE. One shared table, built once, indexed by the top bits of a
        // 32-bit phase accumulator. Two properties matter:
        //   - the index arithmetic is EXACT INTEGER, so a synthesis is
        //     bit-reproducible on any machine and under any optimiser setting, and a
        //     span offset is exact rather than an accumulated float error;
        //   - the cosine the derivative stream needs is the same table read a quarter
        //     turn along, so the derivative costs one extra load, not a second
        //     transcendental.
        // 8192 entries quantise phase to 1/8192 cycle, i.e. a per-partial amplitude
        // error of 2*pi/8192 = 7.7e-4 relative. Against a peak that is several times
        // the RMS of hundreds of partials that is ~1e-4 of the measured value - two
        // orders below kPreferMarginDb - and it is the SAME error for every
        // candidate, so it cannot tip a comparison.
        //
        // The entries are FLOAT, not double, and that is a speed decision with a
        // measured reason: two indexed reads per visit are the only random access in
        // the inner loop, and 8192 doubles is 64 kB, which does not fit L1. At double
        // the whole engine ran at 3.5 ns per work unit; at float (32 kB, and it does
        // fit) it runs at the rate the budget is calibrated for.
        inline constexpr int kTableBits  = 13;
        inline constexpr int kTableSize  = 1 << kTableBits;      // 8192
        inline constexpr int kTableMask  = kTableSize - 1;
        inline constexpr int kQuarter    = kTableSize / 4;
        inline constexpr int kPhaseShift = 32 - kTableBits;      // 19

        struct SineTable
        {
            std::vector<float> t;
            SineTable() : t ((size_t) kTableSize)
            {
                for (int k = 0; k < kTableSize; ++k)
                    t[(size_t) k] = (float) std::sin (kTwoPi * (double) k / (double) kTableSize);
            }
        };
        inline const SineTable& sineTable()
        {
            static const SineTable s;   // thread-safe, built once, never rebuilt
            return s;
        }

        inline uint32_t phaseToFixed (double ph) noexcept
        {
            // 4294967296.0 == 2^32. wrap01 first, so a phase of exactly 1.0 lands on
            // 0 rather than on an out-of-range llround.
            const double u = wrap01 (ph);
            return (uint32_t) (int64_t) std::llround (u * 4294967296.0);
        }

        //----------------------------------------------------------------------
        // THE ANALYSIS GRID. T samples spanning W fundamental periods.
        //
        // v1's grid snapped each partial to an integer bin so the index arithmetic
        // was exact - and that snapping was Failure 1. Here the index arithmetic is
        // still exact integer, but what is quantised is the PER-SAMPLE PHASE
        // INCREMENT at 2^-32 cycles, not the frequency at one part in W. A partial
        // drifts by at most T * 2^-33 cycles across the whole window: 5e-7 cycles at
        // T = 4096, against v1's 0.49.
        //
        // W is the reciprocal of the CLOSEST partial spacing, so the slowest beat in
        // the set completes at least one cycle inside the window. For integer
        // harmonics that is W = 1, the true period. For an inharmonic Scala timbre it
        // is tens of periods - and there it is only the FIRST rung of the span
        // ladder, never the whole claim.
        struct Grid
        {
            int    T = 0;
            int    n = 0;
            double W = 1.0;
            std::vector<uint32_t> inc;    // per-sample phase step, 2^32 = one cycle
            std::vector<double>   amp;
            std::vector<double>   ampR;   // amp * ratio: the derivative's weight
            int    periodSamples = 0;     // samples in one fundamental period (T / W)
        };

        // The window the partial spacing demands: one over the closest spacing
        // between two partials, so the slowest beat that can move the waveform
        // completes one cycle inside it.
        inline int requiredWindowPeriods (const double* ratio, int n) noexcept
        {
            double minGap = 0.0;
            double prev = 0.0; bool havePrev = false;
            for (int i = 0; i < n; ++i)
            {
                if (havePrev)
                {
                    const double d = ratio[i] - prev;
                    if (d > 1.0e-9 && (minGap <= 0.0 || d < minGap)) minGap = d;
                }
                prev = ratio[i]; havePrev = true;
            }
            if (minGap <= 0.0) minGap = 1.0;
            const double w = std::ceil (1.0 / minGap);
            if (! (w >= 1.0)) return 1;
            return w > 1.0e7 ? 10000000 : (int) w;
        }

        // Samples per fundamental period for a grid at this oversample. Kept separate
        // so the verify grid can be handed an EXACT INTEGER MULTIPLE of the
        // construction grid's value: only then are the construction's sample times a
        // strict SUBSET of the verify grid's, which is the whole basis for "a denser
        // grid can only find a peak the sparser one missed". ceil() applied
        // independently to each oversample does NOT give that - at topR = 22.6 it
        // gives 91 and 181, and 181 is not a multiple of 91.
        inline int periodSamplesFor (const double* ratio, int n, int oversample) noexcept
        {
            double topR = 0.0;
            for (int i = 0; i < n; ++i) topR = std::max (topR, ratio[i]);
            if (topR <= 0.0) topR = 1.0;
            const int os = oversample < 2 ? 2 : oversample;
            long long ps = (long long) std::ceil ((double) os * topR);
            // One period must hold at least 32 samples or the 16-block sweep detector
            // has nothing to measure.
            if (ps < 32) ps = 32;
            return (int) ps;
        }

        inline Grid buildGrid (const double* ratio, const double* amp, int n,
                               int periodSamples, int W)
        {
            Grid g;
            if (n <= 0) return g;
            if (W < 1) W = 1;
            if (periodSamples < 32) periodSamples = 32;
            const long long T = (long long) periodSamples * (long long) W;

            g.T = (int) T;
            g.n = n;
            g.W = (double) W;
            g.periodSamples = periodSamples;

            g.inc .resize ((size_t) n);
            g.amp .resize ((size_t) n);
            g.ampR.resize ((size_t) n);
            const double cyclesPerSample = (double) W / (double) T; // per unit ratio
            for (int i = 0; i < n; ++i)
            {
                g.inc [(size_t) i] = (uint32_t) (int64_t)
                                     std::llround (ratio[i] * cyclesPerSample * 4294967296.0);
                g.amp [(size_t) i] = amp[i];
                g.ampR[(size_t) i] = amp[i] * ratio[i];
            }
            return g;
        }

        //----------------------------------------------------------------------
        // IS THE WINDOW THE TRUE PERIOD? This one predicate is what separates a
        // guarantee that holds for every note length from one that holds for
        // 581 milliseconds. If every ratio times W is an integer, the waveform
        // repeats with period W and span 1 is bit-identical to span 0 - so one
        // measurement IS every measurement.
        //
        // The test is on the DOUBLE ratios the plan was built from, not on the grid's
        // fixed-point increments, because it is a statement about the sound the voice
        // renders (direct std::sin on the float ratios), not about our grid.
        //
        // MEASURED residual max|r*W - round(r*W)| over the twelve reference spectra:
        //   default n=1024, fractal L3, FLAT n=512, prime-limit 7, odd, slope 0.5
        //                                            exactly 0.0   -> periodic
        //   scala 19/22/31/53-EDO, 12-EDO, BP 13-of-3   0.10 - 0.50 -> aperiodic
        // Nothing lands between. This is a type test, not a tuning knob.
        inline bool exactlyPeriodicOverWindow (const double* ratio, int n, int W) noexcept
        {
            if (n <= 0 || W < 1) return false;
            const double w = (double) W;
            for (int i = 0; i < n; ++i)
            {
                const double x = ratio[i] * w;
                if (std::fabs (x - std::floor (x + 0.5)) > kPeriodicityTolerance) return false;
            }
            return true;
        }

        //----------------------------------------------------------------------
        // THE SWEEP DETECTOR, and the reason it exists: derivative crest is a PULSE
        // detector. A flat-spectrum Schroeder set has a flat envelope - low
        // derivative crest - while sweeping from 7 kHz to 54 kHz inside every period.
        // That is an audible rasp at the note rate, the exact opposite of the noise
        // this control is for, and v1 shipped it with 2.8 dB to spare.
        //
        // Split ONE fundamental period into 16 blocks; each block's RMS frequency is
        // sqrt(sum g^2 / sum x^2) where g is the ratio-weighted quadrature sum, so
        // the value is directly "mean harmonic number of the energy here". Correlate
        // that with the block index. A linear chirp reads |r| ~ 0.98; noise, the hash
        // and any random draw read near zero, because their spectral centre of
        // gravity does not move.
        inline constexpr int kSweepBlocks = 16;

        inline double sweepCorrelation (const double* x, const double* g,
                                        int periodSamples) noexcept
        {
            const int B = kSweepBlocks;
            const int len = periodSamples / B;
            if (len < 2) return 0.0;
            double f[kSweepBlocks];
            int used = 0;
            for (int b = 0; b < B; ++b)
            {
                double sx = 0.0, sg = 0.0;
                const int k0 = b * len;
                for (int k = k0; k < k0 + len; ++k) { sx += x[k] * x[k]; sg += g[k] * g[k]; }
                if (sx <= 0.0) return 0.0;          // silent block: no opinion
                f[used++] = std::sqrt (sg / sx);
            }
            if (used < 4) return 0.0;
            double mb = 0.0, mf = 0.0;
            for (int b = 0; b < used; ++b) { mb += (double) b; mf += f[b]; }
            mb /= (double) used; mf /= (double) used;
            double num = 0.0, db = 0.0, df = 0.0;
            for (int b = 0; b < used; ++b)
            {
                const double eb = (double) b - mb, ef = f[b] - mf;
                num += eb * ef; db += eb * eb; df += ef * ef;
            }
            if (db <= 0.0 || df <= 0.0) return 0.0;
            return std::fabs (num / std::sqrt (db * df));
        }

        // The prefix lengths the sweep correlation is taken at. Log spaced, so a
        // short prefix - where a chirp is most likely to survive truncation - is
        // checked as densely as a long one, and always ending at n.
        inline std::vector<int> sweepLadder (int n)
        {
            std::vector<int> l;
            for (int p = 8; p < n; p = std::max (p + 1, (p * 3) / 2)) l.push_back (p);
            l.push_back (n);
            return l;
        }

        //----------------------------------------------------------------------
        // WHERE THE SPANS SIT, in windows from the note's start. Offset 0 is the
        // note's beginning and offset 1 is its exact continuation - the two that
        // matter for a short note - and the rest are geometric out to the far end of
        // the ladder, so a four-second note at the top of the keyboard is checked as
        // well as a fifty-millisecond one. Strictly increasing and deduplicated, so
        // a template whose window is already long enough simply gets consecutive
        // spans and the ladder degenerates to the exact contiguous case.
        //
        // Pure integer/double arithmetic in a fixed order: the offsets are part of
        // the answer, so they have to be bit-identical on every machine.
        inline std::vector<int> spanOffsets (int spans, int windowPeriods)
        {
            std::vector<int> o;
            if (spans < 1) spans = 1;
            o.push_back (0);
            if (spans == 1) return o;
            const int maxOff = std::max (1, kLadderReachPeriods / std::max (1, windowPeriods));
            o.push_back (1);
            if (spans == 2 || maxOff <= 1) return o;
            // Geometric from 2 to maxOff over the remaining rungs.
            const int rungs = spans - 2;
            for (int k = 0; k < rungs; ++k)
            {
                const double f = (rungs == 1) ? 1.0 : (double) k / (double) (rungs - 1);
                const double v = std::exp (std::log (2.0) + (std::log ((double) std::max (2, maxOff))
                                                             - std::log (2.0)) * f);
                const int off = (int) std::floor (v + 0.5);
                if (off > o.back()) o.push_back (off);
            }
            return o;
        }

        //----------------------------------------------------------------------
        // ONE CANDIDATE, MEASURED AT EVERY PREFIX AND AT EVERY OBSERVATION LENGTH.
        //
        // The partials are added in ratio order and the running waveform's peak and
        // energy are recorded after each one, so all n prefixes cost exactly what one
        // full synthesis costs. That is what makes the per-pitch guarantee affordable
        // and it is half the answer to Failure 2.
        //
        // The span loop is the answer to Failure 4. Two readings come out of it:
        //   crest[s][p]      the crest of span s ALONE - what is heard during that
        //                    stretch of the note;
        //   unionCrest[s][p] the crest of spans 0..s together - peak = max of their
        //                    peaks, energy = the sum - which is the statistic of a
        //                    note held from the start out to span s.
        // A candidate has to win on both at every s, so neither "good early, bad
        // later" nor "bad in one stretch, hidden by the average" can get through.
        //
        // The two noise descriptors are taken on span 0 only. They describe what
        // happens inside ONE period (a chirp, a pulse), which by definition does not
        // need a longer observation; paying for them S times would buy nothing.
        struct SpanResult
        {
            int spans = 0, n = 0;
            std::vector<double> crest;       // [span * n + prefix], that span alone
            std::vector<double> unionCrest;  // [span * n + prefix], spans 0..span
            std::vector<double> derivCrest;  // [prefix], span 0
            std::vector<double> sweep;       // [prefix], span 0
            std::vector<double> gsumsq;      // [prefix], span 0 (the deriv gate's scale)
            double sweepWorst = 0.0;

            double crestAt (int span, int prefix) const noexcept
            { return crest[(size_t) span * (size_t) n + (size_t) (prefix - 1)]; }
            double unionAt (int span, int prefix) const noexcept
            { return unionCrest[(size_t) span * (size_t) n + (size_t) (prefix - 1)]; }
        };

        inline void measureSpans (const Grid& gr, const double* ph,
                                  const std::vector<int>& offsets,
                                  const std::vector<int>& ladder,
                                  std::vector<double>& x, std::vector<double>& g,
                                  SpanResult& out, long long& work)
        {
            const int T = gr.T, n = gr.n;
            const float* tbl = sineTable().t.data();
            const int spans = std::max (1, (int) offsets.size());

            out.spans = spans; out.n = n;
            out.crest     .assign ((size_t) spans * (size_t) n, kUnmeasured);
            out.unionCrest.assign ((size_t) spans * (size_t) n, kUnmeasured);
            out.derivCrest.assign ((size_t) n, kUnmeasured);
            out.sweep     .assign ((size_t) n, 0.0);
            out.gsumsq    .assign ((size_t) n, 0.0);
            out.sweepWorst = 0.0;

            std::vector<double> runPeak ((size_t) n, 0.0), runSS ((size_t) n, 0.0);

            for (int s = 0; s < spans; ++s)
            {
                const bool wantDeriv = (s == 0);
                x.assign ((size_t) T, 0.0);
                if (wantDeriv) g.assign ((size_t) T, 0.0);

                // Where each partial's accumulator stands at the start of this span.
                // uint32 arithmetic wraps mod 2^32, which is exactly one cycle, so
                // this is the same value the accumulator would hold after offset*T
                // steps - no drift, no float, and identical on every machine.
                const uint32_t stride = (uint32_t) ((uint64_t) T * (uint64_t) offsets[(size_t) s]);

                size_t nextLadder = 0;
                int    ladderFrom = 0;
                for (int i = 0; i < n; ++i)
                {
                    const double a  = gr.amp [(size_t) i];
                    const double ar = gr.ampR[(size_t) i];
                    const uint32_t inc = gr.inc[(size_t) i];
                    uint32_t acc = phaseToFixed (ph[i]) + inc * stride;

                    double pk = 0.0, ss = 0.0, gpk = 0.0, gss = 0.0;
                    if (wantDeriv)
                    {
                        for (int k = 0; k < T; ++k)
                        {
                            const uint32_t idx = acc >> kPhaseShift;
                            acc += inc;
                            const double xv = x[(size_t) k] + a  * tbl[idx];
                            const double gv = g[(size_t) k] + ar * tbl[(idx + (uint32_t) kQuarter)
                                                                       & (uint32_t) kTableMask];
                            x[(size_t) k] = xv;
                            g[(size_t) k] = gv;
                            const double ax = std::fabs (xv); if (ax > pk)  pk  = ax;
                            const double ag = std::fabs (gv); if (ag > gpk) gpk = ag;
                            ss  += xv * xv;
                            gss += gv * gv;
                        }
                    }
                    else
                    {
                        for (int k = 0; k < T; ++k)
                        {
                            const uint32_t idx = acc >> kPhaseShift;
                            acc += inc;
                            const double xv = x[(size_t) k] + a * tbl[idx];
                            x[(size_t) k] = xv;
                            const double ax = std::fabs (xv); if (ax > pk) pk = ax;
                            ss += xv * xv;
                        }
                    }
                    work += T;

                    out.crest[(size_t) s * (size_t) n + (size_t) i] =
                        ss > 0.0 ? pk / std::sqrt (ss / (double) T) : kUnmeasured;

                    if (pk > runPeak[(size_t) i]) runPeak[(size_t) i] = pk;
                    runSS[(size_t) i] += ss;
                    const double nObs = (double) T * (double) (s + 1);
                    out.unionCrest[(size_t) s * (size_t) n + (size_t) i] =
                        runSS[(size_t) i] > 0.0
                            ? runPeak[(size_t) i] / std::sqrt (runSS[(size_t) i] / nObs)
                            : kUnmeasured;

                    if (wantDeriv)
                    {
                        out.gsumsq[(size_t) i] = gss;
                        out.derivCrest[(size_t) i] = gss > 0.0
                            ? gpk / std::sqrt (gss / (double) T) : kUnmeasured;

                        if (nextLadder < ladder.size() && ladder[nextLadder] == i + 1)
                        {
                            const double c = sweepCorrelation (x.data(), g.data(), gr.periodSamples);
                            if (c > out.sweepWorst) out.sweepWorst = c;
                            // Every prefix from the previous ladder point up to this
                            // one is judged by THIS reading. Attributing it forward
                            // keeps the sweep gate from being an all-or-nothing
                            // switch: a single short prefix reading 0.61 used to
                            // collapse the whole plan back to the hash.
                            for (int p = ladderFrom; p <= i; ++p) out.sweep[(size_t) p] = c;
                            ladderFrom = i + 1;
                            ++nextLadder;
                        }
                    }
                }
                if (wantDeriv)
                    for (int p = ladderFrom; p < n; ++p) out.sweep[(size_t) p] = out.sweepWorst;
            }
        }

        //----------------------------------------------------------------------
        // THE CONSTRUCTION. Greedy in RATIO ORDER: partial i's phase is chosen to
        // minimise the peak of the waveform of partials 1..i, and nothing later ever
        // moves it. Two properties fall out of that one choice:
        //
        //   - EVERY PREFIX IS ITS OWN ANSWER. The set the voice renders at MIDI 96 is
        //     not a truncation of a set optimised for 128 partials; it is the
        //     construction's own result for those 11 partials. That is why the
        //     per-pitch guarantee can be met at every pitch instead of at one.
        //   - THE OBJECTIVE AND THE CONSTRAINT ARE BOTH PEAK COMPARISONS, so a
        //     candidate phase is abandoned the moment it cannot beat the incumbent,
        //     which is what makes the whole thing affordable.
        //
        // IT RUNS ONLY WHERE ITS ANSWER IS TRUE FOR EVERY NOTE LENGTH - that is, on
        // an exactly-periodic template, where the window it searched is the
        // waveform's own period. On an aperiodic template it is skipped and the
        // budget goes to the span ladder instead; the table at kSpansAperiodic is the
        // measurement that settled that.
        //
        // gLimitPeak[i] is the absolute derivative-peak ceiling at prefix i+1: the
        // noise gate, pre-multiplied by the prefix's derivative RMS so the inner loop
        // compares two peaks and nothing else. Without it the construction converges
        // on a SAWTOOTH - measured, harmonic slope 1 N=128: crest 1.65 (excellent)
        // with a derivative crest of 7.0 against a 2.59 reference, which is a buzz at
        // the note rate, not scatter.
        //
        // WHEN NOTHING IS FEASIBLE, REPAIR RATHER THAN GIVE UP. The peak of g only
        // ever grows as partials are added, so a partial placed to minimise the
        // X-peak while the gate is already violated pushes g further over and every
        // partial after it inherits the violation. MEASURED before the repair,
        // harmonic slope 1 n=1024: 271 of 272 prefixes refused by the derivative gate
        // at a construction headroom of 0.60, 22 at 0.80, 246 at 0.85 - a bistable,
        // chaotic response to a constant that should have traded smoothly. So an
        // infeasible step falls back to the phase with the SMALLEST derivative peak,
        // which lets the next partial be feasible again.
        inline void buildGreedy (const Grid& gr, const double* gLimitPeak,
                                 double* ph, long long& work)
        {
            const int T = gr.T, n = gr.n;
            const float* tbl = sineTable().t.data();
            std::vector<double> x ((size_t) T, 0.0), g ((size_t) T, 0.0);

            for (int i = 0; i < n; ++i)
            {
                const double a  = gr.amp [(size_t) i];
                const double ar = gr.ampR[(size_t) i];
                const uint32_t inc = gr.inc[(size_t) i];
                const double gLim = gLimitPeak[i];

                double bestPk  = 1.0e300, bestPhi  = 0.0;  // min x-peak among FEASIBLE
                double bestGPk = 1.0e300, bestGPhi = 0.0;  // min g-peak overall (repair)
                bool   anyFeasible = false;

                auto tryPhi = [&] (double phi)
                {
                    uint32_t acc = phaseToFixed (phi);
                    double pk = 0.0, gpk = 0.0;
                    int k = 0;
                    for (; k < T; ++k)
                    {
                        const uint32_t idx = acc >> kPhaseShift;
                        acc += inc;
                        const double xv = x[(size_t) k] + a  * tbl[idx];
                        const double gv = g[(size_t) k] + ar * tbl[(idx + (uint32_t) kQuarter)
                                                                   & (uint32_t) kTableMask];
                        const double ax = std::fabs (xv); if (ax > pk)  pk  = ax;
                        const double ag = std::fabs (gv); if (ag > gpk) gpk = ag;
                        // Both running peaks only grow, so a phase that has already
                        // lost the x race AND the g race can never win either.
                        if (pk >= bestPk && gpk >= bestGPk) { ++k; break; }
                    }
                    work += k;
                    if (k < T) return;                      // beaten; nothing learned
                    if (gpk < bestGPk) { bestGPk = gpk; bestGPhi = phi; }
                    if (gpk <= gLim && pk < bestPk) { bestPk = pk; bestPhi = phi; anyFeasible = true; }
                };

                for (int c = 0; c < kGreedyCoarse; ++c)
                    tryPhi ((double) c / (double) kGreedyCoarse);

                // Refine around the winner. A fixed schedule, so the search path is
                // reproducible and the work is bounded by construction.
                if (kGreedyRefine > 0)
                {
                    const double base = anyFeasible ? bestPhi : bestGPhi;
                    const double step = 1.0 / (double) (kGreedyCoarse * (kGreedyRefine + 1));
                    for (int r = 1; r <= kGreedyRefine; ++r)
                    {
                        tryPhi (wrap01 (base + (double) r * step));
                        tryPhi (wrap01 (base - (double) r * step));
                    }
                }

                ph[i] = anyFeasible ? bestPhi : bestGPhi;

                uint32_t acc = phaseToFixed (ph[i]);
                for (int k = 0; k < T; ++k)
                {
                    const uint32_t idx = acc >> kPhaseShift;
                    acc += inc;
                    x[(size_t) k] += a  * tbl[idx];
                    g[(size_t) k] += ar * tbl[(idx + (uint32_t) kQuarter) & (uint32_t) kTableMask];
                }
                work += T;
            }
        }

        //----------------------------------------------------------------------
        // Closed forms. All take the partials already sorted by ratio.

        // The hash the voice ships today (AdditiveVoice::seedPhase), reproduced
        // EXACTLY - index for index - so "Scattered, as it sounds right now" stays
        // available and stays the floor we are never allowed to fall below.
        inline void closedHash (double* ph, int n)
        {
            // One formula, one place. hashPhaseAt is the public accessor seedFromPlan
            // uses past plan.n; if the two ever diverged, the phases inside the plan
            // and the phases past it would be different sounds.
            for (int i = 0; i < n; ++i) ph[i] = hashPhaseAt (i);
        }

        // Schroeder, with gamma as the exponent on the POWER weighting:
        //   p_m ~ amp_m^(2*gamma),  phi_n = phi_1 - sum over m<n of (n-m) * p_m
        // gamma = 1 is textbook Schroeder (power weighted); gamma = 0.5 weights by
        // amplitude. On a FLAT spectrum the two are identical by construction, which
        // is the cross-check that this is the right reading of gamma.
        //
        // O(n) to build and it is a REAL candidate, not a straw man: per prefix it
        // beats the hash by up to 5.69 dB on a flat spectrum. It is also 9.28 dB
        // WORSE at other prefixes on the same spectrum, and 3.71 dB worse on a Scala
        // timbre, which is precisely why the chooser is per prefix and per span.
        inline void closedSchroeder (const double* amp, int n, double gamma, double* ph)
        {
            if (n <= 0) return;
            std::vector<double> p ((size_t) n, 0.0);
            double tot = 0.0;
            for (int i = 0; i < n; ++i)
            {
                p[(size_t) i] = std::pow (std::max (1.0e-30, amp[i]), 2.0 * gamma);
                tot += p[(size_t) i];
            }
            if (tot <= 0.0) { for (int i = 0; i < n; ++i) ph[i] = kAlignedPhase; return; }
            for (int i = 0; i < n; ++i) p[(size_t) i] /= tot;
            double run = 0.0, acc = 0.0;
            ph[0] = kAlignedPhase;
            for (int i = 1; i < n; ++i)
            {
                run += p[(size_t) i - 1];   // sum of p over m < i
                acc += run;                 // sum_{m<i} (i-m) p_m
                ph[i] = wrap01 (kAlignedPhase - acc);
            }
        }

        // A fixed splitmix64 draw. Never a clock: these are candidates AND the noise
        // reference, and both have to be reproducible for ever.
        inline void closedRandom (int n, uint64_t seed, double* ph)
        {
            for (int i = 0; i < n; ++i)
                ph[i] = unitFrom (splitmix64 (seed * 0x100000001B3ull
                                              + (uint64_t) i * 0x9E3779B97F4A7C15ull));
        }

        // Per-note salt: a pure TIME SHIFT of s fundamental periods, applied to every
        // set in the plan so crossing a prefix boundary cannot introduce a jump the
        // un-salted plan did not have.
        inline void applySalt (double* ph, const double* ratio, int n, int noteNumber)
        {
            const double s = unitFrom (splitmix64 (0x5EED0000ull + (uint64_t) (noteNumber & 0x7F)));
            for (int i = 0; i < n; ++i) ph[i] = wrap01 (ph[i] + s * ratio[i]);
        }

        inline double dbGain (double from, double to) noexcept
        {
            // How many dB BETTER `to` is than `from`.
            if (from <= 0.0 || to <= 0.0) return 0.0;
            return 20.0 * std::log10 (from / to);
        }

        // BUILDS BOTH GRIDS AT ONCE, because their relationship is the load-bearing
        // part and splitting it across two call sites is how it would rot.
        //
        //   verify period samples = construction period samples * (OSv / OSc)
        //   both grids span the SAME window
        //
        // Together those make the construction grid's sample TIMES a strict subset of
        // the verify grid's, over the same span. That is the entire justification for
        // treating the verification as independent evidence rather than as a re-run
        // of the search's own approximation.
        inline void planGrids (const double* ratio, const double* amp, int n,
                               int oversample, int verifyOversample,
                               int maxWindowPeriods, Grid& outC, Grid& outV)
        {
            if (n <= 0) { outC = Grid(); outV = Grid(); return; }
            const int osC = oversample < 2 ? 2 : oversample;
            int osV = verifyOversample < osC ? osC : verifyOversample;
            osV = osC * std::max (1, osV / osC);          // an exact integer multiple

            const int psC = periodSamplesFor (ratio, n, osC);
            const int psV = psC * (osV / osC);

            // Three things bound the window, and the tightest wins:
            //   reqW              the window the spacing asks for (the fastest beat);
            //   maxWindowPeriods  a musical ceiling on how long a beat is worth
            //                     shaping at all;
            //   kMaxGridPoints    a ceiling on T, so a work unit keeps costing what
            //                     the calibration says it costs.
            const int maxW  = std::max (1, maxWindowPeriods);
            const int reqW  = requiredWindowPeriods (ratio, n);
            const int fitW  = std::max (1, kMaxGridPoints / std::max (1, psV));
            const int W     = std::max (1, std::min (reqW, std::min (maxW, fitW)));

            outC = buildGrid (ratio, amp, n, psC, W);
            outV = buildGrid (ratio, amp, n, psV, W);
        }

        // WORST-CASE work, in grid-sample visits. The two reaches are chosen from
        // these, so workUsed <= workBudget is an exact inequality: the construction's
        // early abort can only make the real cost lower, never higher.
        //
        // VERIFICATION, on the verify grid: one pass per candidate PER SPAN, plus one
        // random draw on span 0 for the derivative gate's noise reference.
        //   candidates = hash + Schroeder1 + SchroederHalf (+ the construction)
        inline long long verifyWork (int analysed, int Tv, int candidates, int spans) noexcept
        {
            if (analysed <= 0) return 0;
            const long long passes = (long long) std::max (1, candidates) * (long long) std::max (1, spans)
                                     + 1LL;   // the span-0 random draw
            return passes * (long long) analysed * (long long) Tv;
        }

        // CONSTRUCTION, on the construction grid, per pass = built * Tc:
        //   1  hash            (also the derivative gate's anchor and its RMS scale)
        //   d  random draws    (the gate's noise reference, and candidates)
        //   1  the finished construction, measured
        //   kGreedyCoarse + 2*kGreedyRefine  the construction itself, no early abort
        inline long long buildWork (int built, int Tc, int randomDraws) noexcept
        {
            if (built <= 0) return 0;
            const long long passes = 2LL + (long long) std::max (0, randomDraws)
                                     + (long long) (kGreedyCoarse + 2 * kGreedyRefine);
            return passes * (long long) built * (long long) Tc;
        }
    } // namespace detail

    //==========================================================================
    // EXACT ALIGNED CREST, in closed form: every partial is a cosine so the peak is
    // the sum of the amplitudes at t = 0, and the long-run mean square is half the
    // sum of squares with any partials that share a ratio added coherently first.
    // O(n), no synthesis, exact rather than sampled - so the one number a caller is
    // most likely to divide by is never an estimate and never a zero.
    inline double alignedCrest (const Partial* partials, int n)
    {
        if (n <= 0) return kUnmeasured;
        std::vector<int> ord ((size_t) n);
        for (int i = 0; i < n; ++i) ord[(size_t) i] = i;
        std::stable_sort (ord.begin(), ord.end(), [partials] (int a, int b)
                          { return partials[a].ratio < partials[b].ratio; });
        double pk = 0.0, ss = 0.0, run = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const int idx = ord[(size_t) i];
            const double a = (double) partials[idx].amp;
            pk  += a;
            run += a;
            const bool endOfGroup = (i + 1 >= n)
                                    || partials[ord[(size_t) i + 1]].ratio != partials[idx].ratio;
            if (endOfGroup) { ss += 0.5 * run * run; run = 0.0; }
        }
        return ss > 0.0 ? pk / std::sqrt (ss) : kUnmeasured;
    }

    //==========================================================================
    // What the analysis will cost for this partial list at the requested reach,
    // BEFORE running it, in the same units as Options::workBudget. The UI can size a
    // budget with it and predict whether the reach will be cut. Public because a
    // limit that changes what is heard must be predictable from outside.
    inline long long projectedWork (const Partial* partials, int n, const Options& opt)
    {
        if (n <= 0) return 0;
        std::vector<double> ratio ((size_t) n), amp ((size_t) n, 1.0);
        for (int i = 0; i < n; ++i) ratio[(size_t) i] = (double) partials[i].ratio;
        std::sort (ratio.begin(), ratio.end());
        const double cap = 0.5 * opt.sampleRate / std::max (1.0e-6, opt.lowestNoteHz);
        int analysed = 0;
        for (int i = 0; i < n; ++i) { if (ratio[(size_t) i] > cap) break; analysed = i + 1; }
        if (analysed <= 0) return 0;
        detail::Grid gc, gv;
        detail::planGrids (ratio.data(), amp.data(), analysed, opt.oversample,
                           opt.verifyOversample, opt.maxWindowPeriods, gc, gv);
        const bool per = detail::exactlyPeriodicOverWindow (ratio.data(), analysed, (int) gv.W);
        const int spans = opt.spansOverride > 0 ? opt.spansOverride : (per ? 1 : kSpansAperiodic);
        const int cands = opt.measureSchroeder ? 3 : 1;
        const bool build = opt.useConstruction && (per || opt.optimiseAperiodic);
        return detail::verifyWork (analysed, gv.T, cands + (build ? 1 : 0), spans)
               + (build ? detail::buildWork (analysed, gc.T, opt.randomDraws) : 0);
    }

    //==========================================================================
    // buildPlan: THE EXPENSIVE CALL. Cache its PhasePlan per compiled template.
    //
    // Never returns without measuring something, and never returns a plan that is
    // worse than today's Scattered mode at any prefix AT ANY OBSERVATION LENGTH IN
    // THE LADDER.
    inline PhaseReport buildPlan (const Partial* partials, int n,
                                  const Options& opt, PhasePlan& plan)
    {
        PhaseReport rep;
        plan = PhasePlan();
        if (n <= 0) return rep;                    // Outcome::Empty, all kUnmeasured

        rep.n = n;
        rep.workBudget = opt.workBudget;

        // ---- order-independence: sort by (ratio, amp) and remember the way back, so
        // the returned array is a pure function of the partial MULTISET. (Compiled
        // templates arrive sorted; a blended or hand-built one need not be.)
        //
        // stable_sort is belt and braces here, NOT load-bearing, and that is
        // deliberate. v1 had a sort whose tie order reached the output - a free
        // AMPLITUDE sort, where two partials could tie in amplitude and differ in
        // ratio - so swapping stable_sort for sort changed the result while its suite
        // stayed green. This key is (ratio, amp): two partials that tie in BOTH are
        // interchangeable, so no tie order can reach the output, and everything that
        // follows walks in ratio order with no sort of its own.
        std::vector<int> order ((size_t) n);
        for (int i = 0; i < n; ++i) order[(size_t) i] = i;
        std::stable_sort (order.begin(), order.end(), [partials] (int a, int b)
        {
            if (partials[a].ratio != partials[b].ratio) return partials[a].ratio < partials[b].ratio;
            return partials[a].amp < partials[b].amp;
        });
        std::vector<double> ratio ((size_t) n), amp ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            ratio[(size_t) i] = (double) partials[order[(size_t) i]].ratio;
            amp  [(size_t) i] = (double) partials[order[(size_t) i]].amp;
        }

        rep.crestAligned = alignedCrest (partials, n);

        // ---- SET 0 IS ALWAYS THE HASH, for every partial, whatever happens next.
        std::vector<double> phHash ((size_t) n);
        detail::closedHash (phHash.data(), n);

        auto emit = [&] (const std::vector<std::vector<double>>& sets,
                         const std::vector<uint8_t>& pick,
                         const std::vector<Family>& fam)
        {
            plan.n = n;
            plan.analysed = rep.analysed;
            plan.numSets = (int) sets.size();
            plan.phase.resize (sets.size() * (size_t) n);
            for (size_t s = 0; s < sets.size(); ++s)
            {
                std::vector<double> tmp = sets[s];
                if (opt.perNoteSalt) detail::applySalt (tmp.data(), ratio.data(), n, opt.noteNumber);
                for (int i = 0; i < n; ++i)
                    plan.phase[s * (size_t) n + (size_t) order[(size_t) i]] = tmp[(size_t) i];
            }
            plan.pick   = pick;
            plan.family = fam;
        };
        auto hashOnly = [&] ()
        {
            emit ({ phHash }, std::vector<uint8_t> ((size_t) n, 0), { Family::Hash });
        };

        // ---- THE REACH. Partials above ratioCap cannot sound at or above
        // Options::lowestNoteHz, so they keep the hash and are excluded.
        const double askCap = 0.5 * opt.sampleRate / std::max (1.0e-6, opt.lowestNoteHz);
        int capCount = 0;
        for (int i = 0; i < n; ++i) { if (ratio[(size_t) i] > askCap) break; capCount = i + 1; }
        if (capCount <= 0)
        {
            rep.outcome = Outcome::Hash; rep.measured = false; rep.analysed = 0;
            hashOnly();
            return rep;
        }

        // ---- WHICH CLASS IS THIS TEMPLATE IN? The predicate is taken at the FULL
        // requested reach, not at whatever the budget ends up affording, so the
        // answer - and therefore how long a note the guarantee covers - does not
        // depend on the budget. It cannot change with the reach anyway: a prefix of
        // an integer-ratio list is still integer, and the window is 1 either way.
        {
            detail::Grid tc, tv;
            detail::planGrids (ratio.data(), amp.data(), capCount, opt.oversample,
                               opt.verifyOversample, opt.maxWindowPeriods, tc, tv);
            rep.exactlyPeriodic = detail::exactlyPeriodicOverWindow (ratio.data(), capCount, (int) tv.W);
        }
        const int  spans     = opt.spansOverride > 0 ? opt.spansOverride
                                                    : (rep.exactlyPeriodic ? 1 : kSpansAperiodic);
        const bool doBuild   = opt.useConstruction && (rep.exactlyPeriodic || opt.optimiseAperiodic);
        const int  numCands  = 1 + (opt.measureSchroeder ? 2 : 0) + (doBuild ? 1 : 0);
        // Everything measured on the CONSTRUCTION grid is a within-window question -
        // the derivative gate's anchor, the sweep reading, what the greedy search
        // sees. The ladder is a property of the DECISION, not of the search, and it
        // lives on the verify grid alone.
        const std::vector<int> oneSpan { 0 };

        // On an aperiodic template the construction is not run, so the verification
        // is the whole job and there is nothing to hold budget back for.
        const double share = doBuild ? std::min (0.95, std::max (0.05, opt.verifyShare)) : 1.0;
        const long long verifyCeiling = (long long) ((double) opt.workBudget * share);

        auto costVerify = [&] (int count, detail::Grid& outC, detail::Grid& outV) -> long long
        {
            detail::planGrids (ratio.data(), amp.data(), count, opt.oversample,
                               opt.verifyOversample, opt.maxWindowPeriods, outC, outV);
            return detail::verifyWork (count, outV.T, numCands, spans);
        };
        auto costBuild = [&] (int count) -> long long
        {
            detail::Grid c, v;
            detail::planGrids (ratio.data(), amp.data(), count, opt.oversample,
                               opt.verifyOversample, opt.maxWindowPeriods, c, v);
            return detail::buildWork (count, c.T, opt.randomDraws);
        };

        // THE REACH IS CUT TO AFFORD THE LADDER, NEVER THE OTHER WAY ROUND. Cutting
        // the reach costs keyboard coverage and is reported (lowestFullyCoveredHz,
        // reachReduced); cutting the ladder would cost the guarantee, which is how
        // Failure 4 happened. The cut is a BINARY SEARCH for the longest prefix that
        // fits: worstCaseWork is monotone non-decreasing in the prefix length (a
        // longer prefix has a top ratio at least as high and a closest spacing no
        // wider, so neither T nor W can fall), so the search is exact.
        int analysed = 0;
        {
            detail::Grid tc, tv;
            if (costVerify (capCount, tc, tv) <= verifyCeiling) analysed = capCount;
            else
            {
                rep.reachReduced = true;
                int lo = 1, hi = capCount;
                while (lo < hi)
                {
                    const int mid = lo + (hi - lo + 1) / 2;
                    if (costVerify (mid, tc, tv) <= verifyCeiling) lo = mid; else hi = mid - 1;
                }
                analysed = (costVerify (lo, tc, tv) <= verifyCeiling && lo >= 8) ? lo : 0;
            }
        }
        if (analysed <= 0)
        {
            // Not even a handful of partials fits the budget at the ladder length this
            // template needs. Today's hash, everywhere, and the report says the reach
            // is zero rather than pretending: measured=false, every crest kUnmeasured.
            rep.outcome  = Outcome::Hash;
            rep.measured = false;
            rep.analysed = 0;
            rep.ladderUnaffordable = ! rep.exactlyPeriodic;
            hashOnly();
            return rep;
        }

        detail::Grid gcAtAnalysed, gv;
        const long long verifyCost = costVerify (analysed, gcAtAnalysed, gv);

        // The construction reach: the longest prefix whose build cost fits what the
        // verification left over. Zero on an aperiodic template by design.
        int built = 0;
        if (doBuild)
        {
            const long long buildBudget = opt.workBudget - verifyCost;
            int lo = 0, hi = analysed;
            while (lo < hi)
            {
                const int mid = lo + (hi - lo + 1) / 2;
                if (costBuild (mid) <= buildBudget) lo = mid; else hi = mid - 1;
            }
            built = lo;
            if (built < 8) built = 0;    // too short to shape anything
            if (built < analysed) rep.reachReduced = true;
        }

        detail::Grid gc, gvOfBuilt;
        if (built > 0)
            detail::planGrids (ratio.data(), amp.data(), built, opt.oversample,
                               opt.verifyOversample, opt.maxWindowPeriods, gc, gvOfBuilt);

        rep.analysed         = analysed;
        rep.built            = built;
        rep.ratioCap         = ratio[(size_t) analysed - 1];
        rep.lowestFullyCoveredHz = 0.5 * opt.sampleRate / rep.ratioCap;
        rep.gridPoints       = gc.T;
        rep.verifyGridPoints = gv.T;
        rep.gridWindow       = built > 0 ? gc.W : kUnmeasured;
        rep.verifyWindow     = gv.W;
        rep.windowCapped     = detail::requiredWindowPeriods (ratio.data(), analysed) > (int) gv.W;
        rep.windowBlindPeriods = detail::requiredWindowPeriods (ratio.data(), analysed);
        // THE LADDER, fixed now that the window is known. Its last offset is what
        // makes verifiedSpanPeriods a real number rather than an aspiration.
        const std::vector<int> offsets = detail::spanOffsets (spans, (int) gv.W);
        const int usedSpans = (int) offsets.size();
        rep.spans            = usedSpans;
        rep.verifiedSpanPeriods = rep.exactlyPeriodic
                                    ? kUnboundedSpan
                                    : gv.W * (double) (offsets.back() + 1);
        rep.workProjected    = verifyCost + (built > 0 ? costBuild (built) : 0);
        rep.measured         = true;

        std::vector<double> x, g;
        long long work = 0;

        //---------------------------------------------------------------------
        // THE CANDIDATES. Each is a full-length phase array over the analysed prefix.
        // The hash is candidate 0 and is the floor; nothing else is used unless it
        // beats the hash at every span.
        std::vector<std::vector<double>> cand;
        std::vector<Family>              candFam;
        std::vector<double>              hashPrefix (phHash.begin(), phHash.begin() + analysed);
        cand.push_back (hashPrefix);      candFam.push_back (Family::Hash);

        if (opt.measureSchroeder)
        {
            std::vector<double> s1 ((size_t) analysed), sh ((size_t) analysed);
            detail::closedSchroeder (amp.data(), analysed, 1.0, s1.data());
            detail::closedSchroeder (amp.data(), analysed, 0.5, sh.data());
            cand.push_back (s1); candFam.push_back (Family::Schroeder1);
            cand.push_back (sh); candFam.push_back (Family::SchroederHalf);
        }

        //---------------------------------------------------------------------
        // THE CONSTRUCTION, where it is allowed to run. Its set becomes one more
        // candidate and is verified by exactly the same rule as the closed forms -
        // there is no path by which a search result reaches the output without
        // beating the hash on every span.
        if (built > 0)
        {
            const std::vector<int> ladderC = detail::sweepLadder (built);
            detail::SpanResult rHash;
            std::vector<double> hashBuilt (phHash.begin(), phHash.begin() + built);
            detail::measureSpans (gc, hashBuilt.data(), oneSpan, ladderC, x, g, rHash, work);

            const int draws = std::max (0, opt.randomDraws);
            std::vector<detail::SpanResult>   rDraw ((size_t) draws);
            std::vector<std::vector<double>>  phDraw ((size_t) draws);
            for (int d = 0; d < draws; ++d)
            {
                phDraw[(size_t) d].resize ((size_t) built);
                detail::closedRandom (built, 0xA5A5A5A5ull + (uint64_t) d, phDraw[(size_t) d].data());
                detail::measureSpans (gc, phDraw[(size_t) d].data(), oneSpan, ladderC, x, g,
                                      rDraw[(size_t) d], work);
            }

            // THE DERIVATIVE GATE the construction builds to. The ceiling is the
            // larger of today's hash and the median random draw. Using the draws
            // alone would refuse today's Scattered mode itself on some spectra
            // (MEASURED: harmonic slope 1 N=128, hash 2.705 against a 2.590 median),
            // and a gate that refuses the thing it is calibrated to imitate has the
            // wrong number in it.
            std::vector<double> gLimitPeak ((size_t) built, 0.0);
            for (int i = 0; i < built; ++i)
            {
                double med = kUnmeasured;
                if (draws > 0)
                {
                    std::vector<double> v;
                    for (int d = 0; d < draws; ++d)
                        if (rDraw[(size_t) d].derivCrest[(size_t) i] >= 0.0)
                            v.push_back (rDraw[(size_t) d].derivCrest[(size_t) i]);
                    if (! v.empty()) { std::sort (v.begin(), v.end()); med = v[v.size() / 2]; }
                }
                const double lim = std::max (med, rHash.derivCrest[(size_t) i]);
                const double gRms = rHash.gsumsq[(size_t) i] > 0.0
                                    ? std::sqrt (rHash.gsumsq[(size_t) i] / (double) gc.T) : 0.0;
                gLimitPeak[(size_t) i] = (lim > 0.0 ? lim * opt.derivHeadroom : 1.0e30) * gRms;
            }
            rep.derivReference = std::max (kUnmeasured, rHash.derivCrest[(size_t) built - 1]);

            std::vector<double> phGreedy ((size_t) built);
            detail::buildGreedy (gc, gLimitPeak.data(), phGreedy.data(), work);
            {
                detail::SpanResult rG;
                detail::measureSpans (gc, phGreedy.data(), oneSpan, ladderC, x, g, rG, work);
                rep.crestGreedy = rG.crestAt (0, built);
                rep.sweepGreedy = rG.sweepWorst;
                rep.derivGreedy = rG.derivCrest[(size_t) built - 1];
            }
            for (int d = 0; d < draws; ++d)
            {
                const double c = rDraw[(size_t) d].crestAt (0, built);
                if (c >= 0.0 && (rep.crestBestRandom < 0.0 || c < rep.crestBestRandom))
                    rep.crestBestRandom = c;
            }

            // The construction shaped `built` partials; above that the candidate
            // keeps the hash, so a prefix past `built` still gets the better-phased
            // low partials wherever the verification says that actually wins.
            std::vector<double> full = hashPrefix;
            for (int i = 0; i < built; ++i) full[(size_t) i] = phGreedy[(size_t) i];
            cand.push_back (full); candFam.push_back (Family::Constructed);
        }

        // ================= THE VERIFY GRID, SPAN BY SPAN ======================
        // Everything above chose candidates on the grid they were built on. What
        // follows DECIDES on a grid whose sample points are a strict superset of that
        // one, over `spans` consecutive observation lengths. This is the whole
        // difference between a guarantee and v1's circular suite - and, with the span
        // loop, between a guarantee and v2's 581-millisecond one.
        const std::vector<int> ladderV = detail::sweepLadder (analysed);
        std::vector<detail::SpanResult> res (cand.size());
        for (size_t c = 0; c < cand.size(); ++c)
            detail::measureSpans (gv, cand[c].data(), offsets, ladderV, x, g, res[c], work);

        detail::SpanResult vRand;
        {
            // One random draw on the verify grid: the noise half of the derivative
            // gate's reference, measured on the grid the gate is applied on. Span 0
            // only - the derivative crest is a within-period descriptor.
            std::vector<double> r0 ((size_t) analysed);
            detail::closedRandom (analysed, 0xA5A5A5A5ull, r0.data());
            detail::measureSpans (gv, r0.data(), oneSpan, ladderV, x, g, vRand, work);
        }
        rep.workUsed = work;

        const detail::SpanResult& vHash = res[0];
        rep.crestHash = vHash.crestAt (0, analysed);
        rep.sweepHash = vHash.sweepWorst;
        rep.derivHash = vHash.derivCrest[(size_t) analysed - 1];
        rep.crestHashLastSpan = vHash.unionAt (usedSpans - 1, analysed);
        for (size_t c = 1; c < cand.size(); ++c)
        {
            if (candFam[c] == Family::Schroeder1)
            { rep.crestSchroeder1 = res[c].crestAt (0, analysed);
              rep.sweepSchroeder1 = res[c].sweepWorst;
              rep.derivSchroeder1 = res[c].derivCrest[(size_t) analysed - 1]; }
            else if (candFam[c] == Family::SchroederHalf)
            { rep.crestSchroederHalf = res[c].crestAt (0, analysed);
              rep.sweepSchroederHalf = res[c].sweepWorst; }
        }

        //---------------------------------------------------------------------
        // THE CHOOSER. Per prefix, per span, per candidate. A candidate is admitted
        // at prefix p only if:
        //   - it is not a chirp (sweep gate) and not a pulse (derivative gate), both
        //     judged at prefix p on the verify grid;
        //   - it beats the hash by kPreferMarginDb AT EVERY SPAN in the ladder, on
        //     BOTH readings: that span alone, and the union of every span up to it.
        // Among the admitted, the one with the largest WORST-SPAN gain wins - the
        // most conservative reading, so a candidate that is spectacular at one
        // observation length and marginal at another does not outrank a candidate
        // that is solidly better at all of them. Ties go to the lower candidate
        // index, which is a fixed order, so the choice is deterministic.
        const double marginRatio = std::pow (10.0, -kPreferMarginDb / 20.0); // < 1
        std::vector<uint8_t> pick ((size_t) n, 0);
        std::vector<uint8_t> usedSet (cand.size(), 0);
        usedSet[0] = 1;
        int improved = 0, first = 0, last = 0;
        double bestDb = 0.0, worstDb = 1.0e30;

        for (int p = 1; p <= analysed; ++p)
        {
            const double chSpan0 = vHash.crestAt (0, p);
            if (chSpan0 <= 0.0) continue;

            int    bestC = 0;
            double bestWorstDb = 0.0;
            bool   anySweepRefusal = false, anyDerivRefusal = false;
            bool   anyLadderRefusal = false, anyMarginRefusal = false;

            for (size_t c = 1; c < cand.size(); ++c)
            {
                const detail::SpanResult& r = res[c];
                if (r.sweep[(size_t) p - 1] > kSweepCorrLimit) { anySweepRefusal = true; continue; }
                // The derivative gate, on the verify grid, against a reference
                // measured ON THE VERIFY GRID TOO. Mixing a construction-grid
                // reference with a verify-grid reading made the gate a knife edge:
                // the two grids disagree by a few percent and a candidate built right
                // against the line then flipped between accepted and refused on
                // neighbouring blend frames - up to 3.5 dB of crest step in a frame.
                const double dRef = std::max (vHash.derivCrest[(size_t) p - 1],
                                              vRand.derivCrest[(size_t) p - 1]);
                if (r.derivCrest[(size_t) p - 1] > dRef) { anyDerivRefusal = true; continue; }

                // THE LADDER. Every span, or nothing. `wonSpan0` separates the two
                // reasons for a refusal so refusedByLadder counts exactly the
                // prefixes where a one-window engine would have shipped a regression.
                bool   ok = true, wonSpan0 = false;
                double worstSpanDb = 1.0e30;
                for (int s = 0; s < usedSpans && ok; ++s)
                {
                    for (int which = 0; which < 2; ++which)
                    {
                        const double ch = which ? vHash.unionAt (s, p) : vHash.crestAt (s, p);
                        const double cc = which ? r.unionAt (s, p)     : r.crestAt (s, p);
                        if (ch <= 0.0 || cc <= 0.0) { ok = false; break; }
                        const bool wins = (cc <= ch * marginRatio);
                        if (s == 0 && which == 0) wonSpan0 = wins;
                        if (! wins) { ok = false; break; }
                        worstSpanDb = std::min (worstSpanDb, detail::dbGain (ch, cc));
                    }
                }
                if (! ok)
                {
                    if (wonSpan0) anyLadderRefusal = true; else anyMarginRefusal = true;
                    continue;
                }
                if (worstSpanDb > bestWorstDb) { bestWorstDb = worstSpanDb; bestC = (int) c; }
            }

            if (bestC == 0)
            {
                if (anyLadderRefusal)      ++rep.refusedByLadder;
                else if (anySweepRefusal)  ++rep.refusedBySweep;
                else if (anyDerivRefusal)  ++rep.refusedByDeriv;
                else if (anyMarginRefusal) ++rep.refusedByMargin;
                if (anySweepRefusal) rep.challengerRefusedBySweep = true;
                if (anyDerivRefusal) rep.challengerRefusedByDeriv = true;
                continue;
            }

            pick[(size_t) p - 1] = (uint8_t) bestC;
            usedSet[(size_t) bestC] = 1;
            ++improved;
            if (first == 0) first = p;
            last = p;
            if (bestWorstDb > bestDb)  bestDb  = bestWorstDb;
            if (bestWorstDb < worstDb) worstDb = bestWorstDb;
            switch (candFam[(size_t) bestC])
            {
                case Family::Schroeder1:    ++rep.prefixesSchroeder1;    break;
                case Family::SchroederHalf: ++rep.prefixesSchroederHalf; break;
                case Family::Constructed:   ++rep.prefixesConstructed;   break;
                default: break;
            }
        }

        //---------------------------------------------------------------------
        // Emit only the sets some prefix actually asked for, so a plan that improved
        // nothing is one array and costs one byte per prefix to express.
        std::vector<int> remap (cand.size(), 0);
        std::vector<std::vector<double>> sets;
        std::vector<Family> fams;
        for (size_t c = 0; c < cand.size(); ++c)
        {
            if (! usedSet[c]) continue;
            remap[c] = (int) sets.size();
            std::vector<double> full = phHash;             // hash above `analysed`
            for (int i = 0; i < analysed; ++i) full[(size_t) i] = cand[c][(size_t) i];
            sets.push_back (full);
            fams.push_back (candFam[c]);
        }
        for (int p = 0; p < n; ++p) pick[(size_t) p] = (uint8_t) remap[(size_t) pick[(size_t) p]];

        rep.improvedPrefixes = improved;
        rep.firstImproved    = first;
        rep.lastImproved     = last;
        rep.planSets         = (int) sets.size();
        rep.bestGainDb       = bestDb;
        rep.worstGainDb      = (improved > 0) ? worstDb : 0.0;
        rep.outcome          = (improved > 0) ? Outcome::Chosen : Outcome::Hash;

        {
            const size_t sel = (size_t) pick[(size_t) analysed - 1];
            // Map back to the candidate index so the reported crests describe the set
            // the full prefix is actually playing.
            size_t candIdx = 0;
            for (size_t c = 0; c < cand.size(); ++c) if (usedSet[c] && (size_t) remap[c] == sel) { candIdx = c; break; }
            rep.crestChosen         = res[candIdx].crestAt (0, analysed);
            rep.crestChosenLastSpan = res[candIdx].unionAt (usedSpans - 1, analysed);
            rep.sweepChosen         = res[candIdx].sweepWorst;
            rep.derivChosen         = res[candIdx].derivCrest[(size_t) analysed - 1];
        }

        emit (sets, pick, fams);
        return rep;
    }

    //==========================================================================
    // The cheap half: aligned -> target along the SHORTEST SIGNED delta, per partial.
    // Allocation-free, lock-free and O(n), so this is the piece that may run on a
    // note-on.
    //
    // MEASURED monotone in crest over 40 steps on nine reference spectra: every step
    // of the fader moves the crest in one direction, so the control has no fold-back.
    //
    // `scatter` is the ALREADY-TAPERED amount. The caller applies
    // pow(u, kTaperExponent); the taper is a UI decision and does not belong in the
    // signal maths, and putting it here would silently re-taper anything that already
    // had its own curve.
    //==========================================================================
    // THE SHORTEST SIGNED PATH FROM THE ALIGNED PHASE TO A TARGET PHASE.
    //
    // Three operations, and they were written out three times: in interpolatePhases, in
    // the scatterDeltas() table, and - once a caller has a PLAN rather than the hash - in
    // whatever asks "how far does this partial still have to travel". Three copies of one
    // rule is Failure 2 in miniature, so there is one.
    //
    // The identity every caller relies on:
    //     phase (target, s)  ==  wrap01 (kAlignedPhase + s * scatterDeltaTo (target))
    inline double scatterDeltaTo (double targetPhase) noexcept
    {
        double d = targetPhase - kAlignedPhase;
        d -= std::floor (d);            // -> [0,1)
        if (d > 0.5) d -= 1.0;          // -> shortest signed delta in [-0.5, 0.5)
        return d;
    }

    inline void interpolatePhases (const double* target, int n, double scatter,
                                   double* out) noexcept
    {
        const double t = scatter < 0.0 ? 0.0 : (scatter > 1.0 ? 1.0 : scatter);
        // SCATTER 0 IS SACRED and is written FIRST, before `target` is even read, so
        // a null or short target cannot stop today's Aligned mode from coming out
        // bit-identical. A skeptic found the previous version returning uninitialised
        // memory here whenever the plan was invalid; testScatterZeroIsSacred now
        // covers exactly that path.
        if (t <= 0.0) { for (int i = 0; i < n; ++i) out[i] = kAlignedPhase; return; }
        if (target == nullptr) { for (int i = 0; i < n; ++i) out[i] = kAlignedPhase; return; }
        for (int i = 0; i < n; ++i)
        {
            if (t >= 1.0) { out[i] = target[i]; continue; }
            out[i] = detail::wrap01 (kAlignedPhase + t * scatterDeltaTo (target[i]));
        }
    }

    //==========================================================================
    // THE FADER'S PER-PARTIAL SLOPE, TABULATED ONCE FOR THE WHOLE PLUGIN.
    //
    // WHAT IT IS. interpolatePhases carries partial `index` from kAlignedPhase toward its
    // hash along the SHORTEST SIGNED path, so the phase it writes at fader position s is
    //
    //     phase (index, s)  ==  wrap01 (kAlignedPhase + s * scatterDeltas()[index])
    //
    // for every s in [0, 1] - the same three operations, in the same order, so it is an
    // identity and not an approximation. One number per partial is therefore the WHOLE
    // curve, and it depends on nothing but the index: not on the note, not on the
    // template, not on which voice is asking.
    //
    // WHY IT EXISTS. A LIVE re-phase - the fader moved while a note is HELD, so its
    // partials have to walk to their new phases instead of waiting for the next note-on -
    // has to ask "how far does partial j still have to travel" ON THE AUDIO THREAD, and
    // re-ask it every block for the length of a drag. Deriving that from seedScattered
    // costs one std::sin per partial per Shepard copy per voice: at 512 partials, 8
    // copies and 32 voices that is 131072 sines in a block, which is not a rounding error
    // on the budget, it IS the budget. Read from here it is one multiply.
    //
    // ...AND THE NOTE-ON PATH READS IT TOO NOW, which is why the table stands ABOVE
    // seedScattered instead of below it. seedScattered was the last caller still paying
    // for the very sines this table exists to abolish; the note inside it has the
    // measurement.
    //
    // WHY THERE ARE TWO COLUMNS. `delta` is the slope. `hash` is hashPhaseAt(index)
    // ITSELF, which is the fader's TOP END - and interpolatePhases does not compute the
    // top end, it hands the target back VERBATIM (its t >= 1 branch). seedScattered has to
    // spell it the same way or it is not the same function, and without this column the
    // only way to spell it would have been std::sin at exactly the fader position - full
    // Scattered - that a user is most likely to park on.
    //
    // AND NOT BECAUSE THE ROUND TRIP IS KNOWN TO DIFFER. It was written here that
    // wrap01 (kAlignedPhase + 1.0 * delta[i]) "lands within an ulp of hash[i] rather than
    // ON it", and that was a guess: sabotaging seedScattered's endpoint into exactly that
    // spelling and re-running the bit-identity test gave 0 differences in 440496
    // comparisons, because kAlignedPhase is 0.25 and h - 0.25 is exact for every h in
    // [0,1). The round trip is not proved across the whole 16384-entry range, though, and
    // an identity that holds by luck is the kind this header exists to stop relying on.
    // The column costs nothing to fill - the delta is computed FROM the hash anyway - and
    // it makes the endpoint true by construction instead of by coincidence.
    //
    // WHY IT CANNOT DRIFT AWAY FROM THE CURVE. It is built out of hashPhaseAt and
    // kAlignedPhase - the same two things interpolatePhases is built out of - and
    // testScatterDeltaIsTheCurve re-derives interpolatePhases from this table at 41 fader
    // positions across the whole index range and demands the two agree bit for bit. A
    // second copy of an interpolation curve that quietly stops matching the first is the
    // exact failure this header was written to end (Failure 2), so the table is CHECKED
    // against the curve rather than trusted to stay next to it.
    //
    // NO ALLOCATION, EVER: the storage is a function-local static, so it lives in BSS and
    // the only cost of the first call is filling it. Call warmScatterDeltas() once from
    // prepareToPlay - the audio thread must never be the one to meet the initialisation
    // guard, because that first call is 16384 sines and a lock.
    inline constexpr int kScatterDeltaCount = spectrum::kMaxPartials;

    namespace detail
    {
        struct ScatterTable
        {
            double delta[kScatterDeltaCount];   // scatterDeltaTo (hashPhaseAt (i)): the slope
            double hash [kScatterDeltaCount];   // hashPhaseAt (i) itself: the fader's top end
            ScatterTable() noexcept
            {
                for (int i = 0; i < kScatterDeltaCount; ++i)
                {
                    hash[i]  = hashPhaseAt (i);
                    delta[i] = scatterDeltaTo (hash[i]);
                }
            }
        };
        inline const ScatterTable& scatterTable() noexcept
        {
            static const ScatterTable t;
            return t;
        }
    }

    inline const double* scatterDeltas() noexcept { return detail::scatterTable().delta; }
    // Build the table on a thread that is allowed to be slow. Idempotent.
    inline void warmScatterDeltas() noexcept { (void) scatterDeltas(); }

    //==========================================================================
    // TODAY'S SCATTERED MODE, SEEDED FROM AN INDEX INSTEAD OF FROM A PLAN.
    //
    // Fills [first, first + count) with hashPhaseAt(index) carried toward
    // kAlignedPhase by the SAME interpolation the plan gets - read out of the
    // scatterDeltas table above, which IS that interpolation with the index's own hash
    // substituted in, so there is still no second copy of the curve to drift.
    // Allocation-free and noexcept.
    //
    // scatter 0  -> kAlignedPhase exactly, so scatter 0 stays sacred here too.
    // scatter 1  -> hashPhaseAt(index) exactly = AdditiveVoice::seedPhase(index, false).
    // in between -> the same shortest-signed-delta blend as inside the plan.
    inline void seedScattered (int first, int count, double scatter, double* out) noexcept
    {
        if (count <= 0 || out == nullptr) return;
        // Scatter 0 needs no hash at all, and it is the default on most patches: skip
        // `count` std::sin calls that interpolatePhases would only throw away. Same
        // bits it would have written (its own t <= 0 branch does exactly this), and
        // testSeedPastThePlan asserts that at every span the traps name.
        if (! (scatter > 0.0)) { for (int i = 0; i < count; ++i) out[i] = kAlignedPhase; return; }

        // ...AND SCATTER ABOVE 0 NEEDS NO std::sin EITHER, WHICH IS THE WHOLE POINT OF
        // THE TABLE ABOVE AND IT WAS NOT BEING USED HERE.
        //
        // The chunked hash loop this replaces called hashPhaseAt - one std::sin - for
        // every slot of every copy of every voice, ON THE NOTE-ON PATH. With the Phase
        // fader anywhere above 0 a cold note-on seeds both banks over oscCap() slots:
        // 16384 sines with Shepard off, and with it on eight copies of 1638. A six-note
        // chord therefore spent 0.6-1.4 ms in std::sin before it rendered its first
        // sample, which lands on the ear as a click on the attack - the one place a
        // synth may not be late.
        //
        // Nothing about the SOUND changes, and the identity that says so is the table's
        // own reason for existing (see scatterDeltas() above):
        //     phase (index, s) == wrap01 (kAlignedPhase + s * scatterDeltas()[index])
        // which is interpolatePhases' middle branch with `target` = hashPhaseAt(index),
        // spelled with the same three operations in the same order. The two ENDPOINTS are
        // spelled the way interpolatePhases spells them rather than derived from that
        // identity: t <= 0 writes kAlignedPhase, and t >= 1 hands back hashPhaseAt(index)
        // ITSELF, out of the table's second column. Bit-identity is asserted rather than
        // argued - the seedScattered test at the end of RhythmVerify re-derives this whole
        // function from the old chunked hash path at 21 fader positions across 72 spans
        // and demands agreement to the LAST BIT.
        const double t = scatter > 1.0 ? 1.0 : scatter;
        if (first >= 0 && count <= kScatterDeltaCount && first <= kScatterDeltaCount - count)
        {
            const auto& tab = detail::scatterTable();
            if (t >= 1.0)
            {
                for (int i = 0; i < count; ++i) out[i] = tab.hash[(size_t) (first + i)];
                return;
            }
            for (int i = 0; i < count; ++i)
                out[i] = detail::wrap01 (kAlignedPhase + t * tab.delta[(size_t) (first + i)]);
            return;
        }

        // PAST THE TABLE, which is sized kMaxPartials and is therefore as long as any
        // oscillator bank in this synth: seedRange clamps first+count to kMaxPartials and
        // seedFromPlan's span is an oscCap(). So this branch is unreachable from the
        // shipping call sites - but it is kept, and kept EXACT, because the alternative
        // to a slow answer here is a wrong one, and "the index was past a table nobody
        // told you about" is precisely the kind of silent cut this file forbids.
        constexpr int kChunk = 64;
        double buf[kChunk];
        for (int i = 0; i < count; i += kChunk)
        {
            const int m = (count - i) < kChunk ? (count - i) : kChunk;
            for (int k = 0; k < m; ++k) buf[k] = hashPhaseAt (first + i + k);
            interpolatePhases (buf, m, scatter, out + i);
        }
    }

    //==========================================================================
    // NOTE-ON, in one call. Allocation-free: everything expensive is already in the
    // cached plan.
    //
    // FAILURE 6 - ONE ARGUMENT WAS DOING TWO JOBS, AND IT DISCONNECTED THE WHOLE
    // GUARANTEE. This is the sixth refutation and it is the reason this function now
    // takes two counts instead of one. Read it before you touch either.
    //
    //   Up to the previous revision the signature was
    //       seedFromPlan (plan, int prefixLen, scatter, out)
    //   and that single int was used for TWO different quantities:
    //     (i)  how many oscillator slots to FILL - a BANK SPAN; and
    //     (ii) through PhasePlan::phasesFor's clamp p = min(prefixLen, plan.n),
    //          WHICH PHASE SET TO PLAY - the RENDERED PREFIX, how many partials are
    //          actually below Nyquist at the played pitch.
    //   Those are not the same number and they are not even close. The pitch-dependent
    //   choice of set is, in this header's own capitals, the entire answer to Failure 2
    //   and the basis of the never-worse-at-every-prefix guarantee.
    //
    //   Every real call site passes a BANK SPAN, because that is how many slots the
    //   voice has to fill: seedSpan = min(max(hi, activeCount), oscCap()) is 1638 with
    //   Shepard on and kMaxPartials = 16384 with it off, and both are far larger than
    //   plan.n. With any bank span the clamp collapses p to plan.n, so EVERY PITCH
    //   PLAYS pick[plan.n - 1] and the pitch-dependent choice never reaches a voice at
    //   all. The suite did not catch it because the suite measured plan.phasesFor(p)
    //   directly and never once went through seedFromPlan with a real span.
    //
    //   MEASURED on the skeptics' rig with the old signature and a bank span, 70
    //   templates, MIDI 21-108, at spans 0.05 / 0.25 / 1.00 s, hash hand-transcribed
    //   from AdditiveVoice.h:1456-1457 and no engine scoring anywhere: across the 53
    //   templates where pick[n-1] != 0, 636 of 1851 cases are WORSE than today's hash.
    //   The never-worse guarantee was not weakened, it was simply not connected.
    //
    //   THE FIX, and why it is a type and not a sentence in this comment. The previous
    //   revision HAD a comment here telling the caller to pass the rendered prefix, in
    //   capitals, twice. Both integration call sites passed a span anyway - because a
    //   span is what the surrounding code has in hand. So the two counts now have two
    //   DIFFERENT TYPES with explicit constructors. MEASURED, one translation unit per
    //   row, MSVC /std:c++17:
    //
    //     seedFromPlan (plan, RenderedPrefix{p}, FillSpan{span}, x, out)   compiles
    //     seedFromPlan (plan, span, x, out)              does NOT - deleted overload
    //     seedFromPlan (plan, FillSpan{span}, RenderedPrefix{p}, x, out)   does NOT
    //     seedFromPlan (plan, p, span, x, out)                             does NOT
    //     plan.phasesFor (FillSpan{span})                                  does NOT
    //     plan.improved  (FillSpan{span})                                  does NOT
    //
    //   The deleted overload exists so that a stale four-argument call names THIS
    //   comment in the compiler's own output ("function was explicitly deleted", with
    //   the declaration site) instead of reporting a bare "no matching overload".
    //   testSetChosenByRenderedPrefix asserts the property itself - that what comes out
    //   of here is the set the RENDERED PREFIX chose. Put the old line back and 13
    //   checks fail across four fixtures, the never-worse guarantee among them.
    //
    // WHAT EACH ARGUMENT MEANS:
    //
    //   rendered  RenderedPrefix - the voice's newN at THIS pitch, the partial count
    //             below Nyquist. It chooses WHICH SET IS PLAYED and nothing else.
    //             renderedPrefix() computes it by the same rule the voice uses; use it
    //             rather than writing a second copy. A cached count from another pitch,
    //             or the template's n, throws the guarantee away exactly as the span did.
    //
    //   fill      FillSpan - how many oscillator slots to WRITE. It decides only how
    //             far the writing goes, never which set is chosen. Pass the voice's
    //             real seedSpan; you do not have to clamp it and you must not.
    //
    // THE TWO EDGES, so neither is a surprise. A rendered prefix ABOVE plan.n is a
    // Shepard copy asking for more partials than the template has: it clamps to n,
    // which is the bounds check, and the head is still the whole plan. A rendered
    // prefix of 0 or less means this pitch renders nothing from the base bank at all -
    // it clamps to 1, the shortest verified prefix. Neither clamp is an answer to
    // anything; they exist so a wrong count cannot read out of range. The fill span is
    // clamped nowhere, because past plan.n it is defined.
    //
    // EVERY entry [0, fill) is written on every path, and the WHOLE range is a defined
    // sound - not just the part the plan covers.
    //
    // THE BOUNDARY AT plan.n, AND WHY IT NO LONGER EXISTS FOR THE LISTENER. A fill span
    // past plan.n is ORDINARY, not exotic: AdditiveVoice.h:519 and :646 take
    // seedSpan = min(max(hi, activeCount), oscCap()) where activeCount is still the
    // PREVIOUS template's larger slot count, :914 takes span = max(n, activeCount), and
    // each Shepard copy is seeded over whatever oscCap() gives it. An older version
    // filled that remainder with kAlignedPhase at EVERY scatter. At scatter 0 that is
    // right. At scatter 1 - full Scattered, the mode this file exists to improve - it
    // was writing every one of those partials at its PEAK, which is the loudest
    // arrangement there is and the exact opposite of what the caller asked for. The
    // tail gets the shipping hash carried by the same scatter, so crossing plan.n is
    // inaudible.
    //
    // An invalid plan takes the same path for the whole range: today's mode, at the
    // requested scatter. Filling it with kAlignedPhase instead would have made "no
    // plan" sound like Aligned no matter where the fader was.
    inline void seedFromPlan (const PhasePlan& plan, RenderedPrefix rendered,
                              FillSpan fill, double scatter, double* out) noexcept
    {
        const int span = fill.value;
        if (span <= 0 || out == nullptr) return;
        if (! plan.valid()) { seedScattered (0, span, scatter, out); return; }

        // THE ONE LINE FAILURE 6 IS ABOUT. The set is chosen by `rendered`. `span`
        // appears nowhere in this call and must never be allowed to.
        const double* const set = plan.phasesFor (rendered);

        // ...and `span` decides only how far we write. The head is however much of the
        // plan's n the span reaches; past that it is the shipping hash at this scatter.
        const int head = span > plan.n ? plan.n : span;
        interpolatePhases (set, head, scatter, out);
        seedScattered (head, span - head, scatter, out + head);
    }

    // THE OLD SIGNATURE, DELETED ON PURPOSE. If your build stopped here you are on a
    // call site written before Failure 6 above, and it was passing a BANK SPAN into the
    // argument that chooses the phase set. Do not "fix" it by wrapping the same number
    // in both types. Split it: RenderedPrefix { renderedPrefix (partials, n, f0Hz, sr) }
    // for the set, FillSpan { seedSpan } for the slots.
    inline void seedFromPlan (const PhasePlan&, int, double, double*) = delete;

    //==========================================================================
    // One-shot convenience for callers with no prefix to worry about (the tests, and
    // any offline use). Use buildPlan + seedFromPlan on any path that changes
    // `scatter` or the pitch more often than it changes the partial list - which is
    // every path that has a fader or a keyboard on it.
    inline PhaseReport buildPhases (const Partial* partials, int n, double scatter,
                                    const Options& opt, double* outPhase)
    {
        PhaseReport rep;
        if (n <= 0) return rep;                       // Outcome::Empty
        if (scatter <= 0.0)
        {
            // Free, allocation-free, and bit-identical to today's Aligned mode. NOT a
            // fallback: outcome says Aligned and the crest is the exact closed-form
            // aligned crest, so a caller cannot confuse "the fader is at zero" with
            // "we gave up".
            for (int i = 0; i < n; ++i) outPhase[i] = kAlignedPhase;
            rep.outcome      = Outcome::Aligned;
            rep.measured     = true;
            rep.n            = n;
            rep.crestAligned = alignedCrest (partials, n);
            rep.crestChosen  = rep.crestAligned;
            return rep;
        }
        PhasePlan plan;
        rep = buildPlan (partials, n, opt, plan);
        // The one case where the two counts genuinely coincide, and it still says so
        // twice: this entry point renders the whole template at no particular pitch,
        // so the rendered prefix IS n and the span to fill IS n. A voice is never in
        // this position - see FAILURE 6.
        seedFromPlan (plan, RenderedPrefix { n }, FillSpan { n }, scatter, outPhase);
        return rep;
    }
} // namespace phaseset
} // namespace spectrum
